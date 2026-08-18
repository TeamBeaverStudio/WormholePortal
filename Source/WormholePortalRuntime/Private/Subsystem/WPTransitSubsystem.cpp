// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPTransitSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformTime.h"

#include "WPLog.h"
#include "WormholePortalActor.h"
#include "Voxel/WPVoxel.h"
#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitTags.h"
#include "Transit/Handler/WPTransitHandler.h"
#include "Transit/WPTransitShape.h"


void UWPTransitSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LastTransitSequence = 0;
}

void UWPTransitSubsystem::Deinitialize()
{
	// 진행중인 Transit은 Cancel 경로 사용해 원본 물리 상태와 Phase를 정상적으로 복구
	for (FWPTransitRun& Run : Runs)
	{
		Cancel(Run, EWPTransitFailReason::SubsystemClosed);
	}
	
	// 모든 Runtime 상태 제거
	Runs.Reset();
	LastTransitSequence = 0;
	
	// Delegate 초기화
	TransitStartedDelegate.Clear();
	TransitCommittedDelegate.Clear();
	TransitCancelledDelegate.Clear();
	TransitRejectedDelegate.Clear();
	
	Super::Deinitialize();
}

void UWPTransitSubsystem::Tick(float DeltaTime)
{
	// 서버에서만 Tick 동작 보장
	if (!HasAuthority()) return;
	
	// 역순 순회 안전하게 배열에서 제거 
	for (int32 RunIndex = Runs.Num() - 1; RunIndex >= 0; --RunIndex)
	{
		UpdateRun(Runs[RunIndex]);
		if (!Runs[RunIndex].TransitComponent.IsValid())
		{
			Runs.RemoveAtSwap(RunIndex);
		}
	}
	// 외부에서 Actor와 Component가 제거된 경우 남은 Run을 정리합니다.
	CleanRuns();
}

TStatId UWPTransitSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWPTransitSubsystem, STATGROUP_Tickables);
}

bool UWPTransitSubsystem::IsTickable() const
{
	return !HasAnyFlags(RF_ClassDefaultObject);
}

EWPTransitRole UWPTransitSubsystem::GetTransitRole(const AActor* Actor) const
{
	const FWPTransitRun* Run = FindRun(Actor);
	if (Run == nullptr) return EWPTransitRole::None;

	return GetMaster(*Run) == Actor ? EWPTransitRole::Master : EWPTransitRole::Twin;
}

AActor* UWPTransitSubsystem::GetCounterpartActor(const AActor* Actor) const
{
	const FWPTransitRun* Run = FindRun(Actor);
	if (Run == nullptr) return nullptr;

	AActor* MasterActor = GetMaster(*Run);
	AActor* TwinActor = Run->TwinActor;
	if (MasterActor == Actor) return IsValid(TwinActor) ? TwinActor : nullptr;
	return IsValid(MasterActor) ? MasterActor : nullptr;
}

bool UWPTransitSubsystem::IsPointAcross(const AActor* Actor, const FVector& SourceWorldPoint) const
{
	if (SourceWorldPoint.ContainsNaN()) return false;

	const FWPTransitRun* Run = FindRun(Actor);
	if (Run == nullptr || Run->Sequence == 0 || !Run->bMappingValid) return false;

	const FVector EntryNormal = (Run->EntryPoint - Run->Mapping.SourceCenter).GetSafeNormal();
	if (EntryNormal.IsNearlyZero()) return false;

	return FVector::DotProduct(SourceWorldPoint - Run->EntryPoint, EntryNormal) < 0.0;
}

bool UWPTransitSubsystem::TryStart(UWPTransitComponent* TransitComp, AWormholePortalActor* SourcePortal, UPrimitiveComponent* OverlappedComponent)
{
#if !UE_BUILD_SHIPPING
	const double TryStartStartSeconds = FPlatformTime::Seconds();
#endif

	// Client의 Phase 변경은 무시
	if (!HasAuthority()) return false;
	
	// Transit Component & Source 유효성 검사
	if (!IsValid(TransitComp) || !IsValid(SourcePortal)) return false;

	AActor* MasterActor = TransitComp->GetOwner();
	if (!IsValid(MasterActor) || MasterActor->ActorHasTag(WPTransitTags::Generated))
	{
		return false;
	}
	
	// 진행 중인 Run의 고정된 실행 구성이 바뀌지 않도록 Idle 상태에서만 Resolver를 다시 실행합니다.
	if (TransitComp->GetPhase() != EWPTransitPhase::Idle || FindRun(MasterActor) != nullptr)
	{
		return false;
	}
	
	// 실제 전이 직전에 Resolver가 갱신한 Primitive 캐시를 사용합니다.
	TransitComp->ResolveFromOwner();

	const EWPTransitResolveFailReason ResolveFailReason = TransitComp->ResolveFailReason;
	if (ResolveFailReason != EWPTransitResolveFailReason::None && ResolveFailReason != EWPTransitResolveFailReason::TransitDisabled)
	{
		const EWPTransitFailReason FailReason = ResolveFailReason == EWPTransitResolveFailReason::UnsupportedActor ?
		EWPTransitFailReason::UnsupportedActor : EWPTransitFailReason::InvalidSetup;

		Reject(TransitComp, MasterActor, SourcePortal, FailReason);
		return false;
	}

	if (!TransitComp->GetTransitPrimitives().Contains(OverlappedComponent) || TransitComp->GetHandler() == nullptr)
	{
		return false;
	}
	
	// 실제 Transit 조건 검사 시작
	TransitComp->SetPhase(EWPTransitPhase::Check);
	// Component 정책, Portal Link 또는 목적지 World Partition 준비 상태가 조건을 만족하지 않으면 Reject한다.
	if (!TransitComp->bTransitEnabled || !CanUseGate(SourcePortal))
	{
		EWPTransitFailReason FailReason = EWPTransitFailReason::PortalUnavailable;
		if (!TransitComp->bTransitEnabled)
		{
			FailReason = EWPTransitFailReason::TransitDisabled;
		}
		else if (IsValid(SourcePortal->GetLinkedPortal()) && !SourcePortal->IsLinkedPortalAreaReady())
		{
			FailReason = EWPTransitFailReason::NotReady;
		}

		TransitComp->SetPhase(EWPTransitPhase::Idle);
		Reject(TransitComp, MasterActor, SourcePortal, FailReason);
		return false;
	}
	
	// Source와 Destination Portal의 현재 상태로 전이 Mapping을 생성합니다.
	FWPTransform Mapping;
	if (!FWPTransform::Build(SourcePortal, SourcePortal->GetLinkedPortal(), OUT Mapping))
	{
		TransitComp->SetPhase(EWPTransitPhase::Idle);
		Reject(TransitComp, MasterActor, SourcePortal, EWPTransitFailReason::PortalUnavailable);
		return false;
	}
	
	// Twin과 내부 Transit Run이 정상적으로 생성된 경우에만 Crossing 전환
	EWPTransitFailReason FailReason = EWPTransitFailReason::None;
	const bool bStarted = Start(TransitComp, MasterActor, SourcePortal, Mapping, OUT FailReason);
	
	TransitComp->SetPhase(bStarted ? EWPTransitPhase::Crossing: EWPTransitPhase::Idle);

	if (!bStarted)
	{
		Reject(TransitComp, MasterActor, SourcePortal, FailReason);
	}

	if (bStarted && !Runs.IsEmpty())
	{
		const FWPTransitRun& StartedRun = Runs.Last();
		if (StartedRun.TransitComponent.Get() == TransitComp && StartedRun.Sequence != 0)
		{
			FWPTransitEvent Event = BuildTransitEvent(StartedRun, EWPTransitResult::None);
			Event.TransitElapsedMs = 0.0;

#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose,
				TEXT("[Transit][Started] Sequence=%llu TimestampSeconds=%.6f TransitElapsedMs=%.3f SetupCpuMs=%.3f Actor=%s SourcePortal=%s DestinationPortal=%s TransitType=%s PathMappingValid=%d EntryPointWorld=%s MoveDirectionWorld=%s"),
				static_cast<unsigned long long>(Event.Sequence), Event.TimestampSeconds, Event.TransitElapsedMs,
				(FPlatformTime::Seconds() - TryStartStartSeconds) * 1000.0,
				*GetNameSafe(Event.Actor.Get()), *GetNameSafe(Event.SourcePortal.Get()),
				*GetNameSafe(Event.DestinationPortal.Get()), *UEnum::GetValueAsString(Event.TransitType),
				Event.bPathMappingValid ? 1 : 0, *Event.EntryPointWorld.ToCompactString(),
				*Event.MoveDirectionWorld.ToCompactString());
#endif

			if (AActor* TwinActor = Event.TwinActor.Get())
			{
				TransitComp->OnTwinCreated.Broadcast(TwinActor);
			}

			TransitStartedDelegate.Broadcast(Event);
		}
	}
	
	return bStarted;
}

bool UWPTransitSubsystem::CancelTransit(UWPTransitComponent* TransitComponent)
{
	if (!HasAuthority() || !IsValid(TransitComponent)) return false;

	for (int32 RunIndex = Runs.Num() - 1; RunIndex >= 0; --RunIndex)
	{
		if (Runs[RunIndex].TransitComponent.Get() != TransitComponent) continue;

		Cancel(Runs[RunIndex], EWPTransitFailReason::CancelRequested);
		Runs.RemoveAtSwap(RunIndex);
		return true;
	}
	return false;
}

bool UWPTransitSubsystem::HasAuthority() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_Client;
}

AActor* UWPTransitSubsystem::GetMaster(const FWPTransitRun& Run)
{
	AActor* MasterActor = Run.MasterActor.Get();
	return IsValid(MasterActor) ? MasterActor : nullptr;
}

const FWPTransitRun* UWPTransitSubsystem::FindRun(const AActor* Actor) const
{
	if (!IsValid(Actor)) return nullptr;

	for (const FWPTransitRun& Run : Runs)
	{
		if (GetMaster(Run) == Actor || Run.TwinActor == Actor) return &Run;
	}

	if (const UWorld* World = GetWorld(); World != nullptr && World->GetNetMode() == NM_Client)
	{
		UWPTransitComponent* TransitComp = Actor->FindComponentByClass<UWPTransitComponent>();

		if (IsValid(TransitComp) && TransitComp->ClientTransitState.bApplied && TransitComp->ClientTransitState.Run.Sequence != 0)
		{
			return &TransitComp->ClientTransitState.Run;
		}
	}

	return nullptr;
}

FWPTransitEvent UWPTransitSubsystem::BuildTransitEvent(const FWPTransitRun& Run,
	const EWPTransitResult Result, const EWPTransitFailReason FailReason) const
{
	FWPTransitEvent Event;
	Event.Actor = GetMaster(Run);
	Event.TwinActor = Run.TwinActor;
	Event.SourcePortal = Run.Source;
	Event.DestinationPortal = Run.Dest;
	Event.EntryPointWorld = Run.EntryPoint;
	Event.MoveDirectionWorld = Run.MoveDir.GetSafeNormal();
	Event.SelectedPlane = Run.SelectedPlane;
	const bool bSelectedPlaneValid = IsValidTransitPlane(Event.SelectedPlane);
	const bool bPathInputsValid = !Event.EntryPointWorld.ContainsNaN() && bSelectedPlaneValid;
	Event.Mapping = Run.Mapping;
	Event.bPathMappingValid = bPathInputsValid && Run.bMappingValid;
	Event.Result = Result;
	Event.FailReason = FailReason;
	Event.TransitType = Run.TransitType;
	Event.Sequence = Run.Sequence;
	Event.TimestampSeconds = FPlatformTime::Seconds();
	Event.TransitElapsedMs = Run.StartTimestampSeconds > 0.0 ? FMath::Max((Event.TimestampSeconds - Run.StartTimestampSeconds) * 1000.0, 0.0) : 0.0;
	
	return Event;
}

void UWPTransitSubsystem::Reject(const UWPTransitComponent* TransitComponent, AActor* MasterActor,
	AWormholePortalActor* SourcePortal, const EWPTransitFailReason FailReason)
{
	FWPTransitEvent Event;
	Event.Actor = MasterActor;
	Event.SourcePortal = SourcePortal;
	Event.DestinationPortal = IsValid(SourcePortal) ? SourcePortal->GetLinkedPortal() : nullptr;
	Event.Result = EWPTransitResult::Rejected;
	Event.FailReason = FailReason;
	Event.TransitType = IsValid(TransitComponent) ? TransitComponent->GetResolvedType() : EWPTransitType::Auto;
	Event.TimestampSeconds = FPlatformTime::Seconds();

	if (IsValid(TransitComponent))
	{
		Event.ResolveFailReason = TransitComponent->ResolveFailReason;
		Event.FailedComponents = TransitComponent->FailedComponents;
	}

#if !UE_BUILD_SHIPPING
	TArray<FString> FailedComponentNames;
	FailedComponentNames.Reserve(Event.FailedComponents.Num());
	for (const TWeakObjectPtr<UActorComponent>& FailComponent : Event.FailedComponents)
	{
		FailedComponentNames.Add(GetNameSafe(FailComponent.Get()));
	}
	const FString FailedComponentsText = FailedComponentNames.IsEmpty()
		? FString(TEXT("None"))
		: FString::Join(FailedComponentNames, TEXT(","));

	WP_LOG(this, Verbose,
		TEXT("[Transit][Rejected] TimestampSeconds=%.6f RuntimeReason=%s ResolveReason=%s FailedComponents=%s Actor=%s SourcePortal=%s DestinationPortal=%s TransitType=%s"),
		Event.TimestampSeconds, *UEnum::GetValueAsString(Event.FailReason),
		*UEnum::GetValueAsString(Event.ResolveFailReason), *FailedComponentsText,
		*GetNameSafe(Event.Actor.Get()), *GetNameSafe(Event.SourcePortal.Get()),
		*GetNameSafe(Event.DestinationPortal.Get()), *UEnum::GetValueAsString(Event.TransitType));
#endif

	TransitRejectedDelegate.Broadcast(Event);
}

bool UWPTransitSubsystem::Start(UWPTransitComponent* TransitComp, AActor* MasterActor, AWormholePortalActor* SourcePortal,
	const FWPTransform& Mapping, OUT EWPTransitFailReason& OutFailReason)
{
	OutFailReason = EWPTransitFailReason::InternalError;

	if (!IsValid(TransitComp) || !IsValid(MasterActor) || !CanUseGate(SourcePortal) || GetWorld() == nullptr)
	{
		return false;
	}

	if (!HasAuthority())
	{
		OutFailReason = EWPTransitFailReason::NotReady;
		return false;
	}

	const IWPTransitHandler* Handler = TransitComp->GetHandler();
	const EWPTransitType TransitType = TransitComp->GetResolvedType();
	if (Handler == nullptr || TransitType == EWPTransitType::Auto)
	{
		OutFailReason = EWPTransitFailReason::UnsupportedActor;
		return false;
	}
	
	// 1. 이번 시작 시도에서 얻은 Resolver의 Primitive를 사용
	// 실제 전이 직전에 Resolver가 갱신한 Component 캐시를 사용합니다.
	const TArray<TObjectPtr<UPrimitiveComponent>>& TransitPrimitives = TransitComp->GetTransitPrimitives();
	if (TransitPrimitives.IsEmpty())
	{
		OutFailReason = EWPTransitFailReason::MissingPrimitives;
		return false;
	}

	// Shape 판정 함수가 사용하는 Weak Pointer 배열로 한 번만 변환합니다.
	TArray<TWeakObjectPtr<UPrimitiveComponent>> Parts;
	Parts.Reserve(TransitPrimitives.Num());

	for (UPrimitiveComponent* Part : TransitPrimitives)
	{
		Parts.Add(Part);
	}

	// 2. 각 Type에 맞는 Handler에서 Velocity 및 MoveDir 검수
	const FVector Velocity = Handler->GetVelocity(TransitComp);

	const FVector MoveDir = Velocity.GetSafeNormal();
	if (MoveDir.IsNearlyZero())
	{
		OutFailReason = EWPTransitFailReason::InvalidVelocity;
		return false;
	}
	
	// 3. 설정된 기준으로 접평면 계산할 master 중심 결정
	FVector MasterCenter = FVector::ZeroVector;
	if (!FWPTransitShape::GetMasterCenter(MasterActor, Parts, TransitComp->CenterMode, OUT MasterCenter))
	{
		OutFailReason = EWPTransitFailReason::InvalidCenter;
		return false;
	}
	
	// 4. Source 중심에서 master 중심으로 향하는 방향을 진입 접평면의 법선으로 사용한다.
	const FVector TangentPlaneNormal = (MasterCenter - Mapping.SourceCenter).GetSafeNormal();
	if (TangentPlaneNormal.IsNearlyZero())
	{
		OutFailReason = EWPTransitFailReason::InvalidCenter;
		return false;
	}
	
	// 접평면 법선을 Source 구 표면까지 확장해 실제 진입 지점을 계산한다.
	const FVector EntryPoint = Mapping.SourceCenter + TangentPlaneNormal * Mapping.SourceCoreRadius;
	
	// 5. 진행 축에 수직인 투영 단면만 Gate 반경과 비교한다.
	if (!FWPTransitShape::FitsGate(Parts, Mapping.SourceCenter, MoveDir, Mapping.SourceCoreRadius))
	{
		OutFailReason = EWPTransitFailReason::DoesNotFitGate;
		return false;
	}
	
	// 6. 진입 접평면, 이동 방향 및 전역 설정으로 위치 반사 평면을 한 번 선택한다.
	EWPTransitPlane SelectedPlane = EWPTransitPlane::YZ;
	if (!FWPTransitShape::SelectPlane(SourcePortal->GetActorQuat(), TangentPlaneNormal, MoveDir, OUT SelectedPlane))
	{
		OutFailReason = EWPTransitFailReason::InvalidPlane;
		return false;
	}

	// 7. 물체 전이 상태 생성
	FWPTransitRun& Run = Runs.AddDefaulted_GetRef();
	Run.MasterActor = MasterActor;
	Run.Source = SourcePortal;
	Run.Dest = SourcePortal->GetLinkedPortal();
	Run.TransitComponent = TransitComp;
	Run.SelectedPlane = SelectedPlane;
	Run.EntryPoint = EntryPoint;
	Run.MoveDir = MoveDir;
	Run.TransitType = TransitType;
	Run.Mapping = Mapping;
	Run.bMappingValid = true;

	// 8. Master와 같은 클래스로 Twin을 생성합니다.
	if (!CreateTwinActor(Run, MasterActor, Mapping))
	{
		OutFailReason = EWPTransitFailReason::TwinCreationFailed;
		Runs.RemoveAtSwap(Runs.Num() - 1);
		return false;
	}

	// 9. Voxel 기능이 활성화된 경우 필요한 Static Mesh Pair를 직접 수집합니다.
	if (TransitComp->IsVoxelCollisionEnabled() && !FWPVoxel::GatherPairs(Run))
	{
		OutFailReason = EWPTransitFailReason::MissingVoxelData;
		Cancel(Run, OutFailReason);
		Runs.RemoveAtSwap(Runs.Num() - 1);
		return false;
	}
	
	// 10. Actor 종류에 맞는 Transit 상태 준비
	if (!Handler->Begin(GetWorld(), Run, Mapping))
	{
		OutFailReason = EWPTransitFailReason::BeginFailed;
		Cancel(Run, OutFailReason);
		Runs.RemoveAtSwap(Runs.Num() - 1);
		return false;
	}

	// 11. Physics 준비가 끝난 Master와 Twin을 Voxel Body로 교체
	if (TransitComp->IsVoxelCollisionEnabled() && !FWPVoxel::Begin(Run))
	{
		OutFailReason = EWPTransitFailReason::VoxelBeginFailed;
		Cancel(Run, OutFailReason);
		Runs.RemoveAtSwap(Runs.Num() - 1);
		return false;
	}
	
	// 첫 Physics Frame 전에 양쪽 Voxel Shape를 현재 Portal 평면에 맞춘다.
	const FVector SourceSurface = Run.EntryPoint;
	const FVector DestSurface = Mapping.MapExit(Run.EntryPoint, Run.SelectedPlane);
	const FVector SourceNormal = (SourceSurface - Mapping.SourceCenter).GetSafeNormal();
	const FVector DestNormal = (DestSurface - Mapping.DestCenter).GetSafeNormal();
	
	if (TransitComp->IsVoxelCollisionEnabled())
	{
		FWPVoxel::Update(Run, SourceSurface, SourceNormal, DestSurface, DestNormal);
	}
	
	TransitComp->ApplyVisualClip(Run);
	
	if (!Handler->Update(GetWorld(), Run, Mapping))
	{
		OutFailReason = EWPTransitFailReason::UpdateFailed;
		Cancel(Run, OutFailReason);
		Runs.RemoveAtSwap(Runs.Num() - 1);
		return false;
	}
	
	Run.Sequence = ++LastTransitSequence;
	Run.StartTimestampSeconds = FPlatformTime::Seconds();
	if (MasterActor->GetIsReplicated())
	{
		// Dormancy 상태에서도 이번 시작 snapshot이 다음 복제 갱신 대상이 되도록 먼저 깨웁니다.
		// 상태를 여러 번 강제 전송하지 않고 시작 경계에서 한 번만 갱신을 요청합니다.
		MasterActor->ForceNetUpdate();
		TransitComp->PublishTransitRepState(Run);
		Run.bNetworkStatePublished = true;
	}
	OutFailReason = EWPTransitFailReason::None;
	return true;
}

void UWPTransitSubsystem::UpdateRun(FWPTransitRun& Run)
{
	UWPTransitComponent* TransitComponent = Run.TransitComponent.Get();
	AActor* MasterActor = GetMaster(Run);
	AWormholePortalActor* SourcePortal = Run.Source.Get();
	AWormholePortalActor* DestPortal = Run.Dest.Get();

	// Runtime State is invalid
	if (!IsValid(TransitComponent) || !IsValid(MasterActor) || !IsValid(Run.TwinActor))
	{
		Cancel(Run, EWPTransitFailReason::RuntimeStateLost);
		return;
	}

	// Source or Dest Portal is invalid
	if (!IsValid(SourcePortal) || !IsValid(DestPortal))
	{
		Cancel(Run, EWPTransitFailReason::PortalDestroyed);
		return;
	}

	// Source or Dest Portal is invalid
	if (!CanUseGate(SourcePortal) || SourcePortal->GetLinkedPortal() != DestPortal)
	{
		Cancel(Run, EWPTransitFailReason::PortalUnavailable);
		return;
	}
	// Source-Dest Portal Mapping is Invalid
	if (!Run.bMappingValid)
	{
		Cancel(Run, EWPTransitFailReason::MappingInvalid);
		return;
	}

	const FWPTransform& Mapping = Run.Mapping;
	const IWPTransitHandler* Handler = TransitComponent->GetHandler();
	// Invalid Handler
	if (Handler == nullptr)
	{
		Cancel(Run, EWPTransitFailReason::RuntimeStateLost);
		return;
	}
	// Handler Update Failed
	if (!Handler->Update(GetWorld(), Run, Mapping))
	{
		Cancel(Run, EWPTransitFailReason::UpdateFailed);
		return;
	}
	
	const FVector SourceSurface = Run.EntryPoint;
	const FVector DestSurface = Mapping.MapExit(Run.EntryPoint, Run.SelectedPlane);
	const FVector EntryNormal = (SourceSurface - Mapping.SourceCenter).GetSafeNormal();
	const FVector ExitNormal = (DestSurface - Mapping.DestCenter).GetSafeNormal();
	
	// MoveDir, Entry Normal, ExitNormal is Nearly Zero
	if (Run.MoveDir.IsNearlyZero() || EntryNormal.IsNearlyZero() || ExitNormal.IsNearlyZero())
	{
		Cancel(Run, EWPTransitFailReason::InvalidRunState);
		return;
	}
	
	// 물체 이동에 따라 Portal 평면을 지난 Voxel Shape의 Query와 Physics를 함께 끕니다.
	if (TransitComponent->IsVoxelCollisionEnabled())
	{
		FWPVoxel::Update(Run, SourceSurface, EntryNormal, DestSurface, ExitNormal);
	}
	
	// Master가 진입 전에 외부로 돌아오면 Transit 취소
	if (FWPTransitShape::MasterOutside(Run, SourceSurface, EntryNormal))
	{
		Cancel(Run, EWPTransitFailReason::ReturnedToSource);
		return;
	}
	
	// Actor 종류에 맞는 통과 완료 조건을 확인합니다.
	const bool bPassed = Handler->HasPassed(Run, SourceSurface, EntryNormal,
		DestSurface, ExitNormal);
	
	if (bPassed)
	{
		Commit(Run, Mapping);
		return;
	}
	
	if (IsValid(TransitComponent) && TransitComponent->bDrawDebug)
	{
		FWPVoxel::DrawDebug(GetWorld(), Run);
	}
}

void UWPTransitSubsystem::Cancel(FWPTransitRun& Run, const EWPTransitFailReason FailReason)
{
#if !UE_BUILD_SHIPPING
	const double CleanupStartSeconds = FPlatformTime::Seconds();
#endif
	const bool bWasStarted = (Run.Sequence != 0);
	const FWPTransitEvent Event = bWasStarted ? BuildTransitEvent(Run, EWPTransitResult::Cancelled, FailReason) : FWPTransitEvent();
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();

	// Actor 종류별 상태를 먼저 정리한 뒤 원본 Static Mesh Body로 복원
	const IWPTransitHandler* Handler = FWPTransitHandlers::Get(Run.TransitType);
	if (Handler != nullptr)
	{
		Handler->Cancel(Run);
	}

	FWPVoxel::Reset(Run);
	
	// Twin과 활성 목록 정리 -> Component Idle로 복구
	CloseRun(Run);
	
	if (IsValid(TransitComp))
	{
		TransitComp->SetPhase(EWPTransitPhase::Idle);
	}

	// Sequence가 없는 Run은 Start 실패 정리용입니다.
	// 호출자인 TryStart가 최종 Reject 로그와 이벤트를 남기므로 여기서는 중복 기록하지 않는다
	if (!bWasStarted) return;

#if !UE_BUILD_SHIPPING
	const double CleanupCpuMs = (FPlatformTime::Seconds() - CleanupStartSeconds) * 1000.0;
	const FString ActorName = GetNameSafe(Event.Actor.Get());
	const FString SourcePortalName = GetNameSafe(Event.SourcePortal.Get());
	const FString DestinationPortalName = GetNameSafe(Event.DestinationPortal.Get());

	WP_LOG(this, Verbose,
		TEXT("[Transit][Cancelled] Sequence=%llu TimestampSeconds=%.6f TransitElapsedMs=%.3f Reason=%s Actor=%s SourcePortal=%s DestinationPortal=%s PathMappingValid=%d EntryPointWorld=%s MoveDirectionWorld=%s CleanupCpuMs=%.3f"),
		static_cast<unsigned long long>(Event.Sequence), Event.TimestampSeconds, Event.TransitElapsedMs,
		*UEnum::GetValueAsString(FailReason), *ActorName, *SourcePortalName, *DestinationPortalName,
		Event.bPathMappingValid ? 1 : 0, *Event.EntryPointWorld.ToCompactString(),
		*Event.MoveDirectionWorld.ToCompactString(), CleanupCpuMs);
#endif

	TransitCancelledDelegate.Broadcast(Event);
}

void UWPTransitSubsystem::Commit(FWPTransitRun& Run, const FWPTransform& Mapping)
{
#if !UE_BUILD_SHIPPING
	const double CommitStartSeconds = FPlatformTime::Seconds();
#endif
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	const IWPTransitHandler* Handler = IsValid(TransitComp) ? TransitComp->GetHandler() : nullptr;
	const bool bCommitted = Handler != nullptr && Handler->Commit(Run, Mapping);
	
	if (!bCommitted)
	{
		Cancel(Run, EWPTransitFailReason::CommitFailed);
		return;
	}

	// Commit된 Transform과 Velocity를 유지하면서 원본 Static Mesh Body로 복원
	FWPVoxel::Reset(Run);

	// CloseRun이 Actor/Portal weak reference를 reset하기 전에 delegate payload를 값으로 복사한다.
	const FWPTransitEvent Event = BuildTransitEvent(Run, EWPTransitResult::Committed);
#if !UE_BUILD_SHIPPING
	const FString ActorName = GetNameSafe(Event.Actor.Get());
	const FString SourcePortalName = GetNameSafe(Event.SourcePortal.Get());
	const FString DestinationPortalName = GetNameSafe(Event.DestinationPortal.Get());
#endif
	
	CloseRun(Run);
	
	if (IsValid(TransitComp))
	{
		TransitComp->StartTransitIgnore();
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Transit][Committed] Sequence=%llu TimestampSeconds=%.6f TransitElapsedMs=%.3f Actor=%s SourcePortal=%s DestinationPortal=%s PathMappingValid=%d EntryPointWorld=%s MoveDirectionWorld=%s CommitCpuMs=%.3f"),
		static_cast<unsigned long long>(Event.Sequence), Event.TimestampSeconds, Event.TransitElapsedMs,
		*ActorName, *SourcePortalName, *DestinationPortalName, Event.bPathMappingValid ? 1 : 0,
		*Event.EntryPointWorld.ToCompactString(), *Event.MoveDirectionWorld.ToCompactString(),
		(FPlatformTime::Seconds() - CommitStartSeconds) * 1000.0);
#endif

	TransitCommittedDelegate.Broadcast(Event);
}

void UWPTransitSubsystem::CloseRun(FWPTransitRun& Run)
{
	UWPTransitComponent* TransitComponent = Run.TransitComponent.Get();
	if (Run.bNetworkStatePublished && IsValid(TransitComponent))
	{
		AActor* MasterActor = GetMaster(Run);
		if (IsValid(MasterActor) && MasterActor->GetIsReplicated())
		{
			// 시작과 같은 방식으로 종료 경계에서만 다음 복제 갱신을 요청합니다.
			MasterActor->ForceNetUpdate();
			TransitComponent->PublishTransitEnd(Run.Sequence);
		}
		Run.bNetworkStatePublished = false;
	}

	if (IsValid(TransitComponent))
	{
		TransitComponent->ClearVisualClip(Run);
	}

	if (Run.Sequence != 0)
	{
		if (IsValid(TransitComponent) && IsValid(Run.TwinActor))
		{
			TransitComponent->OnTwinRemoving.Broadcast(Run.TwinActor);
		}
	}
	
	RemoveTwinActor(Run);
	
	Run.MasterActor.Reset();
	Run.TransitComponent.Reset();
	Run.Source.Reset();
	Run.Dest.Reset();
}

bool UWPTransitSubsystem::CreateTwinActor(FWPTransitRun& Run, AActor* Master, const FWPTransform& Mapping)
{
	Run.TwinActor = nullptr;
	Run.TwinVisualParts.Reset();
	
	UWorld* World = GetWorld();
	UWPTransitComponent* TransitComponent = Run.TransitComponent.Get();
	if (World == nullptr || !IsValid(Master) || !IsValid(TransitComponent)) return false;

	const FTransform TwinTransform = Mapping.MapTransform(Master->GetActorTransform(), Run.EntryPoint, Run.SelectedPlane);
	
	FActorSpawnParameters Params;
	Params.Template = Master;
	Params.bDeferConstruction = true;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	
	// Master의 Root Transform이 TwinTransform에 다시 곱해지지 않도록 합니다.
	Params.TransformScaleMethod = ESpawnActorScaleMethod::OverrideRootScale;
	Params.CustomPreSpawnInitialization = [](AActor* SpawnedActor)
	{
		// Master의 외부 AttachParent가 Twin에 이어지지 않도록 합니다.
		SpawnedActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		
		if (APawn* SpawnedPawn = Cast<APawn>(SpawnedActor))
		{
			// Clears the Master's ownership references duplicated by Template Spawn from the Twin.
			SpawnedPawn->SetController(nullptr);
			SpawnedPawn->SetPlayerState(nullptr);
			SpawnedPawn->AutoPossessPlayer = EAutoReceiveInput::Disabled;
			SpawnedPawn->AutoPossessAI = EAutoPossessAI::Disabled;
		}
		SpawnedActor->Tags.AddUnique(WPTransitTags::Generated);
	};
	
	AActor* Twin = World->SpawnActor<AActor>(Master->GetClass(), TwinTransform, Params);
	
	if (!IsValid(Twin)) return false;
	
	// Pawn Controller, PlayerState, Auto Possess 같은 게임별 정책은 사용자가 결정한다.
	// FinishSpawning 전에 필요한 Twin 설정을 적용할 수 있도록 알립니다.
	TransitComponent->OnTwinPreparing.Broadcast(Twin);
	if (!IsValid(Twin)) return false;

	UWPTransitComponent* TwinTransitComponent = Twin->FindComponentByClass<UWPTransitComponent>();
	if (!IsValid(TwinTransitComponent))
	{
		Twin->Destroy();
		return false;
	}
	// Template Spawn은 Master의 Check Phase와 이전 복제 snapshot까지 복사. 따라서 FinishSpawning 전에 기본 상태로 되돌립니다.
	TwinTransitComponent->ResetReplicatedStateForTwin();
	
	Twin->FinishSpawning(TwinTransform, true, nullptr, ESpawnActorScaleMethod::OverrideRootScale);
	if (!IsValid(Twin)) return false;

	TArray<TPair<UPrimitiveComponent* , UPrimitiveComponent*>> VisualPairs;
	TransitComponent->GatherVisualPairs(TwinTransitComponent, OUT VisualPairs);
	TransitComponent->ApplyTwinVisualPairs(Run, VisualPairs);

	Run.TwinActor = Twin;
	return true;
}

void UWPTransitSubsystem::RemoveTwinActor(FWPTransitRun& Run)
{
	if (IsValid(Run.TwinActor))
	{
		Run.TwinActor->Destroy();
	}
	Run.TwinActor = nullptr;
	Run.TwinVisualParts.Reset();
}

void UWPTransitSubsystem::CleanRuns()
{
	for (int32 RunIndex = Runs.Num() - 1; RunIndex >= 0; --RunIndex)
	{
		if (!Runs[RunIndex].TransitComponent.IsValid())
		{
			Cancel(Runs[RunIndex], EWPTransitFailReason::RuntimeStateLost);
			Runs.RemoveAtSwap(RunIndex);
		}
	}

}

bool UWPTransitSubsystem::CanUseGate(const AWormholePortalActor* Gate) const
{
	return IsValid(Gate)										// 1. Gate is Valid
		&& IsValid(Gate->GetLinkedPortal())				// 2. Linked Gate is Valid
		&& Gate->GetLinkedPortal() != Gate						// 3. Linked Gate isn't itself
		&& Gate->GetWorld() == GetWorld()						// 4. Gate is in UWorld
		&& Gate->GetLinkedPortal()->GetWorld() == GetWorld()	// 5. Linked Gate is in UWorld
		&& Gate->IsLinkedPortalAreaReady();						// 6. World Partition Case
}
