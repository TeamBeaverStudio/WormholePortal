// Copyright 2026 Team Beaver. All Rights Reserved.


#include "Transit/WPTransitComponent.h"

#include "WormholePortalActor.h"
#include "WPLog.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

#include "Subsystem/WPTransitSubsystem.h"
#include "Transit/Handler/WPCharacterHandler.h"
#include "Transit/Handler/WPTransitHandler.h"
#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitTypeResolver.h"
#include "Transit/WPVisualClip.h"
#include "Transit/WPTransitTags.h"
#include "Voxel/WPVoxelData.h"


UWPTransitComponent::UWPTransitComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	
	// 생성자에서 bReplicated를 수정하는 함수.
	SetIsReplicatedByDefault(true);
}

void UWPTransitComponent::BeginPlay()
{
	Super::BeginPlay();
	
	RefreshFromOwner();
}

void UWPTransitComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CancelTransit();
	
	if (ClientTransitState.bApplied)
	{
		UWPTransitComponent* MasterComponent = ClientTransitState.Run.TransitComponent.Get();
		const uint64 Sequence = ClientTransitState.Run.Sequence;
		if (IsValid(MasterComponent) && MasterComponent != this)
		{
			MasterComponent->ClearClientTransit(Sequence, false, true);
		}
		else
		{
			ClearClientTransit(Sequence, false, true);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void UWPTransitComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	RemainingIgnoreTime = FMath::Max(RemainingIgnoreTime - DeltaTime, 0.0f);
	if (RemainingIgnoreTime > 0.0f) return;

	SetPhase(EWPTransitPhase::Idle);
	SetComponentTickEnabled(false);
}

void UWPTransitComponent::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	
	DOREPLIFETIME(UWPTransitComponent, Phase);
	DOREPLIFETIME(UWPTransitComponent, TransitRepState);
}

EWPTransitPhase UWPTransitComponent::GetPhase() const
{
	const FWPTransitRun* Run = FindActiveRun();
	if (Run != nullptr)
	{
		const UWPTransitComponent* MasterComponent = Run->TransitComponent.Get();
		if (IsValid(MasterComponent)) return MasterComponent->Phase;
	}

	return Phase;
}

EWPTransitRole UWPTransitComponent::GetTransitRole() const
{
	const UWorld* World = GetWorld();
	const UWPTransitSubsystem* TransitSubsystem = IsValid(World) ? World->GetSubsystem<UWPTransitSubsystem>() : nullptr;
	return IsValid(TransitSubsystem) ? TransitSubsystem->GetTransitRole(GetOwner()) : EWPTransitRole::None;
}

AActor* UWPTransitComponent::GetCounterPartActor() const
{
	const UWorld* World = GetWorld();
	const UWPTransitSubsystem* TransitSubsystem = IsValid(World) ? World->GetSubsystem<UWPTransitSubsystem>() : nullptr;
	return IsValid(TransitSubsystem) ? TransitSubsystem->GetCounterpartActor(GetOwner()) : nullptr;
}

AActor* UWPTransitComponent::GetMasterActor() const
{
	const FWPTransitRun* Run = FindActiveRun();
	AActor* MasterActor = Run != nullptr ? Run->MasterActor.Get() : nullptr;
	return IsValid(MasterActor) ? MasterActor : nullptr;
}

AActor* UWPTransitComponent::GetTwinActor() const
{
	const FWPTransitRun* Run = FindActiveRun();
	AActor* TwinActor = (Run != nullptr ? Run->TwinActor.Get() : nullptr);
	return IsValid(TwinActor) ? TwinActor : nullptr;
}

AWormholePortalActor* UWPTransitComponent::GetSourcePortal() const
{
	const FWPTransitRun* Run = FindActiveRun();
	return Run != nullptr ? Run->Source.Get() : nullptr;
}

AWormholePortalActor* UWPTransitComponent::GetDestPortal() const
{
	const FWPTransitRun* Run = FindActiveRun();
	return Run != nullptr ? Run->Dest.Get() : nullptr;
}

EWPTransitType UWPTransitComponent::GetTransitType() const
{
	const FWPTransitRun* Run = FindActiveRun();
	if (Run == nullptr) return ResolvedType;

	return Run->TransitType;
}

int64 UWPTransitComponent::GetTransitSequence() const
{
	const FWPTransitRun* Run = FindActiveRun();
	return Run != nullptr ? static_cast<int64>(Run->Sequence) : 0;
}

bool UWPTransitComponent::GetEntryData(OUT FVector& OutEntryPoint,OUT FVector& OutEntryNormal) const
{
	OutEntryPoint = FVector::ZeroVector;
	OutEntryNormal = FVector::ZeroVector;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	OutEntryPoint = Run->EntryPoint;
	OutEntryNormal = (OutEntryPoint - Mapping.SourceCenter).GetSafeNormal();
	return !OutEntryNormal.IsNearlyZero();
}

bool UWPTransitComponent::GetExitData(OUT FVector& OutExitPoint, OUT FVector& OutExitNormal) const
{
	OutExitPoint = FVector::ZeroVector;
	OutExitNormal = FVector::ZeroVector;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	OutExitPoint = Mapping.MapExit(Run->EntryPoint, Run->SelectedPlane);
	OutExitNormal = (OutExitPoint - Mapping.DestCenter).GetSafeNormal();
	return !OutExitNormal.IsNearlyZero();
}

bool UWPTransitComponent::MapPoint(const FVector& SourcePoint, OUT FVector& OutDestPoint) const
{
	OutDestPoint = SourcePoint;
	if (SourcePoint.ContainsNaN()) return false;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	const FVector ExitPoint = Mapping.MapExit(Run->EntryPoint, Run->SelectedPlane);
	OutDestPoint = ExitPoint + Mapping.MapDir(SourcePoint - Run->EntryPoint);
	return !OutDestPoint.ContainsNaN();
}

bool UWPTransitComponent::MapDirection(const FVector& SourceDirection, OUT FVector& OutDestDirection) const
{
	OutDestDirection = SourceDirection;
	if (SourceDirection.ContainsNaN()) return false;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	OutDestDirection = Mapping.MapDir(SourceDirection);
	return !OutDestDirection.ContainsNaN();
}

bool UWPTransitComponent::MapRotation(const FRotator& SourceRotation, OUT FRotator& OutDestRotation) const
{
	OutDestRotation = SourceRotation;
	if (SourceRotation.ContainsNaN()) return false;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	OutDestRotation = Mapping.MapRot(SourceRotation.Quaternion()).Rotator();
	return !OutDestRotation.ContainsNaN();
}

bool UWPTransitComponent::MapTransform(const FTransform& SourceTransform, OUT FTransform& OutDestTransform) const
{
	OutDestTransform = SourceTransform;
	if (SourceTransform.ContainsNaN()) return false;

	const FWPTransitRun* Run = FindActiveRun();
	FWPTransform Mapping;
	if (Run == nullptr || !TryGetRunMapping(*Run, OUT Mapping)) return false;

	OutDestTransform = Mapping.MapTransform(SourceTransform, Run->EntryPoint, Run->SelectedPlane);
	return !OutDestTransform.ContainsNaN();
}

bool UWPTransitComponent::GetMapping(FWPTransform& OutMapping) const
{
	OutMapping = FWPTransform();
	const FWPTransitRun* Run = FindActiveRun();
	return Run != nullptr && TryGetRunMapping(*Run, OUT OutMapping);
}

bool UWPTransitComponent::IsPointInsidePortal(const FVector& SourceWorldPoint) const
{
	const UWorld* World = GetWorld();
	const UWPTransitSubsystem* TransitSubsystem = IsValid(World) ? World->GetSubsystem<UWPTransitSubsystem>() : nullptr;
	return IsValid(TransitSubsystem) && TransitSubsystem->IsPointAcross(GetOwner(), SourceWorldPoint);
}

bool UWPTransitComponent::RefreshFromOwner()
{
	// Transit Phase가 Idle이 아닌 경우에는 Refresh를 거절한다.
	if (GetPhase() != EWPTransitPhase::Idle)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose, TEXT("Refresh Failed: Actor Transit Phase isn't Idle"));
		#endif
		return false;
	}

	ResolveFromOwner();
	return true;
}

void UWPTransitComponent::ResolveFromOwner()
{
	// 이전 판정 결과를 먼저 비워 Actor 구성 변경 뒤에도 오래된 Component 포인터가 남지 않게 합니다.
	TransitPrimitives.Reset();
	MovementComponent = nullptr;
	TransitHandler = nullptr;
	ResolvedType = EWPTransitType::Auto;
	
	// 현재 Owner 구성을 하나의 Resolver기준으로 다시 판정
	FWPTransitResolveResult Result = FWPTransitTypeResolver::Resolve(GetOwner(), this, TransitType);
	ResolveFailReason = Result.FailReason;
	FailedComponents = MoveTemp(Result.FailedComponents);
	TransitPrimitives = MoveTemp(Result.TransitPrimitives);
	MovementComponent = Result.MovementComponent;
	
	// 비활성화는 구성 오류가 아니므로 종류와 Handler는 유지
	// Editor에서 다시 활성화 할 때 동일한 판정 결과를 사용하고, 실제 전이 시작은 bTransitEnabled가 별도로 차단
	if (!Result.IsPassed() && Result.FailReason != EWPTransitResolveFailReason::TransitDisabled) return;

	ResolvedType = Result.TransitType;
	TransitHandler = FWPTransitHandlers::Get(ResolvedType);
}

bool UWPTransitComponent::CancelTransit()
{
	UWorld* World = GetWorld();
	UWPTransitSubsystem* TransitSubsystem = IsValid(World) ? World->GetSubsystem<UWPTransitSubsystem>() : nullptr;
	
	return IsValid(TransitSubsystem) && TransitSubsystem->CancelTransit(this);
}

const UWPVoxelData* UWPTransitComponent::FindVoxelData(const UPrimitiveComponent* Primitive) const
{
	for (const UWPVoxelData* Data : VoxelData)
	{
		if (IsValid(Data) && Data->IsValidFor(Primitive))
		{
			return Data;
		}
	}
	return nullptr;
}

void UWPTransitComponent::StartTransitIgnore()
{
	RemainingIgnoreTime = FMath::Max(IgnoreTime, 0.0f);
	SetPhase(RemainingIgnoreTime > 0.0f ? EWPTransitPhase::Cooldown : EWPTransitPhase::Idle);
	SetComponentTickEnabled(RemainingIgnoreTime > 0.0f);
}

void UWPTransitComponent::SetPhase(EWPTransitPhase NewPhase)
{
	if (Phase == NewPhase) return;
	
	const EWPTransitPhase PreviousPhase = Phase;
	Phase = NewPhase;
	OnPhaseChanged.Broadcast(PreviousPhase, Phase);
}

const FWPTransitRun* UWPTransitComponent::FindActiveRun() const
{
	// Client에서 복원한 Run이 있다면 해당 Run을 반환합니다.
	if (ClientTransitState.bApplied && ClientTransitState.Run.Sequence != 0)
	{
		return &ClientTransitState.Run;
	}

	// Client Run이 없다면 World Subsystem에서 Owner의 Run을 찾습니다.
	const UWorld* World = GetWorld();
	const UWPTransitSubsystem* Subsystem = IsValid(World) ? World->GetSubsystem<UWPTransitSubsystem>() : nullptr;
	
	if (!IsValid(Subsystem))
	{
		return nullptr;
	}
	
	const FWPTransitRun* Run = Subsystem->FindRun(GetOwner());
	return Run!= nullptr && Run->Sequence != 0 ? Run : nullptr;
}

bool UWPTransitComponent::TryGetRunMapping(const FWPTransitRun& Run, FWPTransform& OutMapping)
{
	OutMapping = FWPTransform();
	if (!Run.bMappingValid || Run.EntryPoint.ContainsNaN() || !IsValidTransitPlane(Run.SelectedPlane)) return false;

	OutMapping = Run.Mapping;
	return true;
}

void UWPTransitComponent::OnRep_Phase(EWPTransitPhase PreviousPhase) const
{
	if (PreviousPhase == Phase) return;
	
	OnPhaseChanged.Broadcast(PreviousPhase, Phase);
}

void UWPTransitComponent::OnRep_TransitRepState()
{
	// Client에서만 동작을 허용한다
	if (GetOwner() == nullptr || GetOwner()->HasAuthority()) return;
	
	// Replicated된 RepState의 Sequence를 체크한다.
	const uint64 IncomingSequence = TransitRepState.Sequence;
	
	// 전이중인 상태가 아니다
	if (!TransitRepState.bActive)
	{
		if (IncomingSequence == 0) return;
		
		// Client에서 더 최신 전이를 처리하고 있는가?
		if (ClientTransitState.bApplied && ClientTransitState.Run.Sequence > IncomingSequence)
		{
			return;
		}

		ClearClientTransit(IncomingSequence, true, true);
		return;
	}

	// 오래된 Sequence 무시
	if (IncomingSequence == 0 || IncomingSequence <= LastClosedTransitSequence)
	{
		return;
	}
	
	// Object Ref 검사
	if (!IsValid(TransitRepState.CounterpartActor) || !IsValid(TransitRepState.SourcePortal) || !IsValid(TransitRepState.DestinationPortal))
	{
		// Waiting for replicated object references.
		return;
	}
	
	// TransitRepState 유효성 검사
	if (!IsTransitRepStateValid(TransitRepState))
	{
		WP_LOG(this, Warning, TEXT("Invalid replicated state ignored. Actor=%s Sequence=%llu"), *GetNameSafe(GetOwner()), static_cast<unsigned long long>(IncomingSequence));
		return;
	}

	UWPTransitComponent* CounterpartComponent = TransitRepState.CounterpartActor->FindComponentByClass<UWPTransitComponent>();
	if (!IsValid(CounterpartComponent))
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, VeryVerbose, TEXT("Waiting for counterpart component. Actor=%s Counterpart=%s Sequence=%llu"),
			*GetNameSafe(GetOwner()), *GetNameSafe(TransitRepState.CounterpartActor),
			static_cast<unsigned long long>(IncomingSequence));
		#endif
		return;
	}

	if (ClientTransitState.bApplied)
	{
		if (ClientTransitState.Run.Sequence > IncomingSequence)
		{
			return;
		}
		if (ClientTransitState.Run.Sequence == IncomingSequence && ClientTransitState.Run.TwinActor.Get() == TransitRepState.CounterpartActor)
		{
			return;
		}

		ClearClientTransit(ClientTransitState.Run.Sequence, false, true);
	}

	TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>> VisualPairs;
	GatherVisualPairs(CounterpartComponent, OUT VisualPairs);

	ApplyClientTransit(CounterpartComponent, VisualPairs);
}

void UWPTransitComponent::PublishTransitRepState(const FWPTransitRun& Run)
{
	FWPTransitRepState NewState;
	NewState.CounterpartActor = Run.TwinActor;
	NewState.SourcePortal = Run.Source.Get();
	NewState.DestinationPortal = Run.Dest.Get();
	NewState.EntryPoint = Run.EntryPoint;
	NewState.SourceCenter = Run.Mapping.SourceCenter;
	NewState.DestinationCenter = Run.Mapping.DestCenter;
	NewState.SourceRotation = Run.Mapping.SourceRotation;
	NewState.TransportRotation = Run.Mapping.TransportRotation;
	NewState.SourceCoreRadius = Run.Mapping.SourceCoreRadius;
	NewState.DestinationCoreRadius = Run.Mapping.DestCoreRadius;
	NewState.Sequence = Run.Sequence;
	NewState.SelectedPlane = Run.SelectedPlane;
	NewState.TransitType = Run.TransitType;
	NewState.bActive = true;

	TransitRepState = MoveTemp(NewState);
}

void UWPTransitComponent::PublishTransitEnd(const uint64 Sequence)
{
	FWPTransitRepState NewState;
	NewState.Sequence = Sequence;
	NewState.bActive = false;

	TransitRepState = MoveTemp(NewState);
}

void UWPTransitComponent::ResetReplicatedStateForTwin()
{
	Phase = EWPTransitPhase::Idle;
	TransitRepState = FWPTransitRepState();
}

bool UWPTransitComponent::IsTransitRepStateValid(const FWPTransitRepState& State)
{
	const uint8 TransitTypeValue = static_cast<uint8>(State.TransitType);
	const bool bTransitTypeValid = State.TransitType != EWPTransitType::Auto && TransitTypeValue <= static_cast<uint8>(EWPTransitType::Pawn);
	// Mapping을 Build할 수 있는지 검증 목적으로 테스트.
	FWPTransform Mapping;

	return State.bActive
		&& State.Sequence != 0
		&& IsValid(State.CounterpartActor)
		&& IsValid(State.SourcePortal)
		&& IsValid(State.DestinationPortal)
		&& State.SourcePortal != State.DestinationPortal
		&& !State.EntryPoint.ContainsNaN()
		&& IsValidTransitPlane(State.SelectedPlane)
		&& bTransitTypeValid
		&& BuildReplicatedMapping(State, OUT Mapping);
}

bool UWPTransitComponent::BuildReplicatedMapping(const FWPTransitRepState& State, FWPTransform& OutMapping)
{
	if (State.SourceCenter.ContainsNaN()
		|| State.DestinationCenter.ContainsNaN()
		|| State.SourceRotation.ContainsNaN()
		|| State.TransportRotation.ContainsNaN()
		|| !FMath::IsFinite(State.SourceCoreRadius)
		|| !FMath::IsFinite(State.DestinationCoreRadius)
		|| State.SourceCoreRadius <= KINDA_SMALL_NUMBER
		|| State.DestinationCoreRadius <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	FQuat SourceRotation = State.SourceRotation;
	FQuat TransportRotation = State.TransportRotation;
	if (SourceRotation.SizeSquared() <= KINDA_SMALL_NUMBER || TransportRotation.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	SourceRotation.Normalize();
	TransportRotation.Normalize();

	FWPTransform Mapping;
	Mapping.SourceCenter = State.SourceCenter;
	Mapping.DestCenter = State.DestinationCenter;
	Mapping.SourceRotation = SourceRotation;
	Mapping.TransportRotation = TransportRotation;
	Mapping.SourceCoreRadius = State.SourceCoreRadius;
	Mapping.DestCoreRadius = State.DestinationCoreRadius;
	OutMapping = Mapping;
	return true;
}

void UWPTransitComponent::GatherVisualPairs(UWPTransitComponent* CounterpartComponent,
	OUT TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& OutPairs)
{
	OutPairs.Reset();
	ClipParts.Reset();
	
	UWorld* World = GetWorld();
	AActor* MasterActor = GetOwner();
	AActor* CounterpartActor = IsValid(CounterpartComponent) ? CounterpartComponent->GetOwner() : nullptr;
	
	if (World == nullptr || World->GetNetMode() == NM_DedicatedServer || !IsValid(MasterActor) || !IsValid(CounterpartActor))
	{
		return;
	}
	
	const bool bNeedsMaterialClip = IsMaterialClipEnabled();
	
	// Collect Counterpart Primitives
	TArray<UPrimitiveComponent*> CounterpartPrimitives;
	CounterpartActor->GetComponents<UPrimitiveComponent>(OUT CounterpartPrimitives);
	
	TMap<FName, UPrimitiveComponent*> CounterpartByName;
	for (UPrimitiveComponent* Primitive : CounterpartPrimitives)
	{
		if (IsValid(Primitive) && Primitive->GetOwner() == CounterpartActor)
		{
			CounterpartByName.Add(Primitive->GetFName(), Primitive);
		}
	}
	
	// Collect Master Primitives
	TArray<UPrimitiveComponent*> MasterPrimitives;
	MasterActor->GetComponents<UPrimitiveComponent>(OUT MasterPrimitives);
	
	OutPairs.Reserve(MasterPrimitives.Num());
	if (bNeedsMaterialClip) ClipParts.Reserve(MasterPrimitives.Num());
	
	for (UPrimitiveComponent* Primitive : MasterPrimitives)
	{
		if (!IsValid(Primitive) || Primitive->GetOwner() != MasterActor ||
			Primitive->GetMobility() != EComponentMobility::Movable ||
			Primitive->IsA<UInstancedStaticMeshComponent>() ||
			Primitive->ComponentHasTag(WPTransitTags::Generated) ||
			Primitive->ComponentHasTag(WPTransitTags::Ignore))
		{
			continue;
		}

#if WITH_EDITORONLY_DATA
		if (Primitive->IsEditorOnly() || Primitive->IsVisualizationComponent())
		{
			continue;
		}
#endif
		
		const UStaticMeshComponent* SM = Cast<UStaticMeshComponent>(Primitive);
		const USkeletalMeshComponent* SKM = Cast<USkeletalMeshComponent>(Primitive);
		
		const bool bStaticVisual = IsValid(SM) && IsValid(SM->GetStaticMesh());
		const bool bSkeletalVisual = IsValid(SKM) && IsValid(SKM->GetSkeletalMeshAsset());
		
		// SkeletalMesh -> LeaderPose | Static Mesh -> Material Clip
		if (!bSkeletalVisual && !(bNeedsMaterialClip && bStaticVisual))
		{
			continue;
		}
		UPrimitiveComponent* CounterpartPrimitive = CounterpartByName.FindRef(Primitive->GetFName());
		
		if (!IsValid(CounterpartPrimitive) || CounterpartPrimitive->GetClass() != Primitive->GetClass())
		{
			continue;
		}
		
		if (bNeedsMaterialClip)
		{
			ClipParts.Add(Primitive);
		}
		
		OutPairs.Emplace(Primitive, CounterpartPrimitive);
	}
}

void UWPTransitComponent::ApplyTwinVisualPairs(FWPTransitRun& Run,
	const TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& VisualPairs) const
{
	Run.TwinVisualParts.Reset();
	Run.TwinVisualParts.Reserve(VisualPairs.Num());
	
	UWorld* World = GetWorld();
	const bool bApplyLeaderPose = World != nullptr && World->GetNetMode() != NM_DedicatedServer;
	
	for (const TPair<UPrimitiveComponent*, UPrimitiveComponent*>& Pair : VisualPairs)
	{
		Run.TwinVisualParts.Add(Pair.Value);
		if (!bApplyLeaderPose) continue;
		
		USkeletalMeshComponent* MasterSKM = Cast<USkeletalMeshComponent>(Pair.Key);
		USkeletalMeshComponent* TwinSKM = Cast<USkeletalMeshComponent>(Pair.Value);
		
		if (IsValid(MasterSKM) && IsValid(TwinSKM))
		{
			TwinSKM->SetLeaderPoseComponent(MasterSKM, true, false);
		}
	}
}

void UWPTransitComponent::ApplyVisualClip(const FWPTransitRun& Run) const
{
	const UWorld* World = GetWorld();
	if (!IsMaterialClipEnabled() || ClipParts.IsEmpty() || World == nullptr || World->GetNetMode() == NM_DedicatedServer || !IsValid(Run.TwinActor))
	{
		return;
	}
	
	const FWPTransform& Mapping = Run.Mapping;
	const FVector DestSurface = Mapping.MapExit(Run.EntryPoint, Run.SelectedPlane);
	const FVector EntryNormal = (Run.EntryPoint - Mapping.SourceCenter).GetSafeNormal();
	const FVector ExitNormal = (DestSurface - Mapping.DestCenter).GetSafeNormal();
	
	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitivePtr : ClipParts)
	{
		FWPVisualClip::SetSource(PrimitivePtr.Get(), Mapping.SourceCenter, EntryNormal, Mapping.SourceCoreRadius);
	}
	
	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitivePtr : Run.TwinVisualParts)
	{
		FWPVisualClip::SetTwin(PrimitivePtr.Get(), Mapping.DestCenter, ExitNormal, Mapping.DestCoreRadius);
	}
}

void UWPTransitComponent::ClearVisualClip(const FWPTransitRun& Run)
{
	const UWorld* World = GetWorld();
	if (!IsMaterialClipEnabled() || World == nullptr || World->GetNetMode() == NM_DedicatedServer || ClipParts.IsEmpty())
	{
		ClipParts.Reset();
		return;
	}

	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitivePtr : ClipParts)
	{
		FWPVisualClip::Clear(PrimitivePtr.Get());
	}

	for (const TWeakObjectPtr<UPrimitiveComponent>& PrimitivePtr : Run.TwinVisualParts)
	{
		FWPVisualClip::Clear(PrimitivePtr.Get());
	}
	ClipParts.Reset();
}

void UWPTransitComponent::ApplyClientTransit(UWPTransitComponent* CounterpartComponent,
                                             const TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& VisualPairs)
{
	if (!IsValid(CounterpartComponent)) return;

	// 복제된 정보로 Client Run을 만든다.
	FWPTransform ClientMapping;
	if (!BuildReplicatedMapping(TransitRepState, OUT ClientMapping))
	{
		// Failed to build client mapping from replicated Transit snapshot
		return;
	}

	FWPTransitRun LocalRun;
	LocalRun.Sequence = TransitRepState.Sequence;
	LocalRun.TransitType = TransitRepState.TransitType;
	LocalRun.MasterActor = GetOwner();
	LocalRun.Mapping = ClientMapping;
	LocalRun.bMappingValid = true;
	LocalRun.TwinActor = TransitRepState.CounterpartActor;
	LocalRun.Source = TransitRepState.SourcePortal;
	LocalRun.Dest = TransitRepState.DestinationPortal;
	LocalRun.SelectedPlane = TransitRepState.SelectedPlane;
	LocalRun.EntryPoint = TransitRepState.EntryPoint;
	LocalRun.TransitComponent = this;

	ApplyTwinVisualPairs(LocalRun, VisualPairs);

	// Master와 Twin에 같은 전이 상태를 저장한다.
	ClientTransitState.Run = LocalRun;
	ClientTransitState.Role = EWPTransitRole::Master;
	ClientTransitState.bApplied = true;

	CounterpartComponent->ClientTransitState.Run = LocalRun;
	CounterpartComponent->ClientTransitState.Role = EWPTransitRole::Twin;
	CounterpartComponent->ClientTransitState.bApplied = true;

	// Server와 Client가 동일한 Material Clip 적용 경로를 사용하도록 합니다.
	ApplyVisualClip(LocalRun);

	// Client의 전이 준비가 끝났음을 알린다.
	OnTwinCreated.Broadcast(TransitRepState.CounterpartActor);
}

void UWPTransitComponent::ClearClientTransit(const uint64 Sequence, const bool bMarkClosed,
	const bool bBroadcastRemoving)
{
	UWPTransitComponent* MasterComponent = ClientTransitState.Run.TransitComponent.Get();
	if (IsValid(MasterComponent) && MasterComponent != this)
	{
		MasterComponent->ClearClientTransit(Sequence, bMarkClosed, bBroadcastRemoving);
		return;
	}

	if (!ClientTransitState.bApplied)
	{
		if (bMarkClosed)
		{
			LastClosedTransitSequence = FMath::Max(LastClosedTransitSequence, Sequence);
		}
		return;
	}

	const uint64 AppliedSequence = ClientTransitState.Run.Sequence;
	if (AppliedSequence > Sequence) return;

	AActor* CounterpartActor = ClientTransitState.Run.TwinActor.Get();
	UWPTransitComponent* CounterpartComp = IsValid(CounterpartActor) ? CounterpartActor->FindComponentByClass<UWPTransitComponent>() : nullptr;
	if (bBroadcastRemoving && CounterpartActor != nullptr)
	{
		OnTwinRemoving.Broadcast(CounterpartActor);
	}
	
	ClearVisualClip(ClientTransitState.Run);
	
	for (const TWeakObjectPtr<UPrimitiveComponent>& TwinPrimitivePtr : ClientTransitState.Run.TwinVisualParts)
	{
		if (USkeletalMeshComponent* TwinSkeletalMesh = Cast<USkeletalMeshComponent>(TwinPrimitivePtr.Get()))
		{
			TwinSkeletalMesh->SetLeaderPoseComponent(nullptr, false, false);
		}
	}

	ClientTransitState = FWPTransitClientState();
	if (bMarkClosed)
	{
		LastClosedTransitSequence = FMath::Max(LastClosedTransitSequence, Sequence);
	}

	if (IsValid(CounterpartComp) && CounterpartComp->ClientTransitState.bApplied && CounterpartComp->ClientTransitState.Run.Sequence <= Sequence)
	{
		CounterpartComp->ClientTransitState = FWPTransitClientState();
		if (bMarkClosed)
		{
			CounterpartComp->LastClosedTransitSequence = FMath::Max(CounterpartComp->LastClosedTransitSequence, Sequence);
		}
	}
}

void UWPTransitComponent::ClientSyncChar_Implementation(FVector DestLocation, FRotator DestRotation, FVector DestVelocity, FRotator DestControl)
{
	FWPCharacterHandler::SyncClient(this, DestLocation, DestRotation, DestVelocity, DestControl);
}
