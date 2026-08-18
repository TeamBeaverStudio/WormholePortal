// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPPortalLightCollectionSubsystem.h"
#include "Lighting/WPPortalLightTags.h"

#include "Subsystem/WPRegistrySubsystem.h"
#include "WormholePortalActor.h"
#include "WPLog.h"

#include "Components/LightComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "UObject/UObjectIterator.h"

namespace WPPortalLightCollectionPrivate
{
	/**
	 * OnActorSpawned로 잡지 못한 runtime AddComponent를 보정하는 주기입니다.
	 * World 전체 TObjectIterator 검색은 이 주기로만 수행합니다.
	 */
	constexpr double FullLightReconcileIntervalSeconds = 1.0;
}

bool UWPPortalLightCollectionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	
	const UWorld* World = Cast<UWorld>(Outer);
	
	return IsValid(World) && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer && FApp::CanEverRender();
}

void UWPPortalLightCollectionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	
	Super::Initialize(Collection);
	
	bDeinitializing = false;
	
	TrackedLights.Reset();
	TrackedLightKeys.Reset();
	PortalStates.Reset();
	PendingSpawnedActors.Reset();
	
	UWorld* World = GetWorld();
	check(World);
	
	RegistrySubsystem = Collection.InitializeDependency<UWPRegistrySubsystem>();
	
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		/**
		 * Registry bootstrap 전에 delegate를 먼저 연결합니다.
		 * 이미 등록된 Portal과 initialization 중 등록되는 Portal 사이의 누락을 막습니다.
		 */
		PortalRegisteredHandle = Registry->OnPortalRegistered().AddUObject(this, &UWPPortalLightCollectionSubsystem::HandlePortalRegistered);
		PortalUnregisteredHandle = Registry->OnPortalUnregistered().AddUObject(this, &UWPPortalLightCollectionSubsystem::HandlePortalUnregistered);
	}
	
	const FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateUObject(
		this, &UWPPortalLightCollectionSubsystem::HandleActorSpawned);
	
	ActorSpawnedHandle = World->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UWPPortalLightCollectionSubsystem::HandleWorldPostActorTick);
	
	/**
	 * 초기 World Light를 한 번 수집합니다.
	 * 이후 매 프레임에는 이 TrackedLights 목록만 순회합니다.
	 */
	DiscoverAllLights();
	
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		TArray<AWormholePortalActor*> BootstrapPortals;
		Registry->GetRegisteredPortals(BootstrapPortals);
		
		for (AWormholePortalActor* Portal : BootstrapPortals)
		{
			RegisterPortal(Portal);
		}
	}
	
	/**
	 * 첫 외부 query 전에 초기 결과가 있도록 즉시 한 번 계산합니다.
	 */
	RefreshAllPortalLights();
	
	NextFullLightReconcileRealSeconds = FPlatformTime::Seconds() + WPPortalLightCollectionPrivate::FullLightReconcileIntervalSeconds;
	
#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalLightCollection][Initialize] World=%s TrackedLights=%d TrackedPortals=%d ActorSpawnDelegate=%d PostActorTickDelegate=%d RegistryDelegates=%d CpuMs=%.3f"),
		*GetNameSafe(World),
		TrackedLights.Num(),
		PortalStates.Num(),
		ActorSpawnedHandle.IsValid() ? 1 : 0,
		WorldPostActorTickHandle.IsValid() ? 1 : 0,
		PortalRegisteredHandle.IsValid()
			&& PortalUnregisteredHandle.IsValid()
				? 1
				: 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalLightCollectionSubsystem::Deinitialize()
{
	check(IsInGameThread());
	
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 ReleasedLightCount = TrackedLights.Num();
	const int32 ReleasedPortalCount = PortalStates.Num();
#endif

	bDeinitializing = true;
	
	if (WorldPostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
		WorldPostActorTickHandle.Reset();
	}
	
	if (UWorld* World = GetWorld())
	{
		if (ActorSpawnedHandle.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
			ActorSpawnedHandle.Reset();
		}
	}
	
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		if (PortalRegisteredHandle.IsValid())
		{
			Registry->OnPortalRegistered().Remove(PortalRegisteredHandle);
		}
		
		if (PortalUnregisteredHandle.IsValid())
		{
			Registry->OnPortalUnregistered().Remove(PortalUnregisteredHandle);
		}
	}
	
	PortalRegisteredHandle.Reset();
	PortalUnregisteredHandle.Reset();
	
	PendingSpawnedActors.Reset();
	
	PortalStates.Reset();
	TrackedLightKeys.Reset();
	TrackedLights.Reset();
	
	RegistrySubsystem.Reset();
	
	NextFullLightReconcileRealSeconds = 0.0;
	
#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalLightCollection][Deinitialize] ReleasedLights=%d ReleasedPortals=%d CpuMs=%.3f"),
		ReleasedLightCount,
		ReleasedPortalCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif

	Super::Deinitialize();
}

int32 UWPPortalLightCollectionSubsystem::GetTrackedLightCount() const
{
	check(IsInGameThread());
	return TrackedLights.Num();
}

int32 UWPPortalLightCollectionSubsystem::GetTrackedPortalCount() const
{
	check(IsInGameThread());
	return PortalStates.Num();
}

bool UWPPortalLightCollectionSubsystem::GetAffectingLights(const AWormholePortalActor* Portal,
	TArray<TWeakObjectPtr<ULightComponent>>& OutLights) const
{
	check(IsInGameThread());
	
	OutLights.Reset();
	
	if (!Portal)
	{
		return false;
	}
	
	const FWPPortalAffectingLightState* PortalState = PortalStates.Find(TObjectKey<AWormholePortalActor>(Portal));
	
	if (!PortalState)
	{
		return false;
	}
	
	OutLights.Reserve(PortalState->AffectingLights.Num());
	
	for (const TWeakObjectPtr<ULightComponent>& WeakLight : PortalState->AffectingLights)
	{
		if (WeakLight.IsValid())
		{
			OutLights.Add(WeakLight);
		}
	}
	
	return true;
}

bool UWPPortalLightCollectionSubsystem::GetPortalLightSnapshot(const AWormholePortalActor* Portal,
	FWPPortalAffectingLightSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	
	OutSnapshot = FWPPortalAffectingLightSnapshot{};
	
	if (!Portal)
	{
		return false;
	}
	
	const FWPPortalAffectingLightState* PortalState = PortalStates.Find(TObjectKey<AWormholePortalActor>(Portal));
	
	if (!PortalState)
	{
		return false;
	}
	
	OutSnapshot.Portal = PortalState->Portal;
	OutSnapshot.Revision = PortalState->Revision;
	
	OutSnapshot.AffectingLights.Reserve(PortalState->AffectingLights.Num());
	
	for (const TWeakObjectPtr<ULightComponent>& WeakLight : PortalState->AffectingLights)
	{
		if (WeakLight.IsValid())
		{
			OutSnapshot.AffectingLights.Add(WeakLight);
		}
	}
	
	return true;
}

void UWPPortalLightCollectionSubsystem::DiscoverAllLights()
{
	check(IsInGameThread());
	
	if (bDeinitializing)
	{
		return;
	}
	
	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		ULightComponent* LightComponent = *It;
		
		if (IsSupportedLight(LightComponent))
		{
			TrackLight(LightComponent);
		}
	}
}

void UWPPortalLightCollectionSubsystem::DiscoverLightsOnActor(AActor* Actor)
{
	check(IsInGameThread());
	
	if (bDeinitializing || !IsValid(Actor) || Actor->GetWorld() != GetWorld())
	{
		return;
	}
	
	TInlineComponentArray<ULightComponent*> LightComponents;
	Actor->GetComponents(LightComponents);
	
	for (ULightComponent* LightComponent : LightComponents)
	{
		TrackLight(LightComponent);
	}
}

void UWPPortalLightCollectionSubsystem::ProcessPendingSpawnedActors()
{
	check(IsInGameThread());
	
	TArray<TWeakObjectPtr<AActor>> PendingActors;
	Swap(PendingActors, PendingSpawnedActors);
	
	for (const TWeakObjectPtr<AActor>& WeakActor : PendingActors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			DiscoverLightsOnActor(Actor);
		}
	}
}

void UWPPortalLightCollectionSubsystem::TrackLight(ULightComponent* LightComponent)
{
	check(IsInGameThread());
	
	if (!IsSupportedLight(LightComponent))
	{
		return;
	}
	
	const TObjectKey<ULightComponent> ObjectKey(LightComponent);
	
	if (TrackedLightKeys.Contains(ObjectKey))
	{
		return;
	}
	
	FWPTrackedLightRecord Record;
	Record.ObjectKey = ObjectKey;
	Record.LightComponent = LightComponent;
	
	TrackedLightKeys.Add(ObjectKey);
	TrackedLights.Add(MoveTemp(Record));
	
#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		VeryVerbose,
		TEXT("[PortalLightCollection][LightTracked] Light=%s Type=%d TrackedLightCount=%d"),
		*GetNameSafe(LightComponent),
		static_cast<int32>(LightComponent->GetLightType()),
		TrackedLights.Num());
#endif
}

void UWPPortalLightCollectionSubsystem::CompactTrackedLights()
{
	check(IsInGameThread());
	
	for (int32 Index = TrackedLights.Num() - 1; Index >= 0; --Index)
	{
		const FWPTrackedLightRecord& Record = TrackedLights[Index];
		
		ULightComponent* LightComponent = Record.LightComponent.Get();
		
		if (IsSupportedLight(LightComponent))
		{
			continue;
		}
		
		TrackedLightKeys.Remove(Record.ObjectKey);
		
		TrackedLights.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	}
}

void UWPPortalLightCollectionSubsystem::RegisterPortal(AWormholePortalActor* Portal)
{
	check(IsInGameThread());
	
	if (bDeinitializing || !IsValid(Portal) || Portal->GetWorld() != GetWorld())
	{
		return;
	}
	
	const TObjectKey<AWormholePortalActor> ObjectKey(Portal);
	
	if (FWPPortalAffectingLightState* ExistingState = PortalStates.Find(ObjectKey))
	{
		ExistingState->Portal = Portal;
		return;
	}
	
	FWPPortalAffectingLightState NewState;
	NewState.Portal = Portal;
	
	FWPPortalAffectingLightState& AddedState = PortalStates.Add(ObjectKey, MoveTemp(NewState));
	
	RefreshPortalLights(AddedState);
	
#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalLightCollection][PortalTracked] Portal=%s TrackedPortalCount=%d"),
		*GetNameSafe(Portal),
		PortalStates.Num());
#endif
}

void UWPPortalLightCollectionSubsystem::UnregisterPortal(AWormholePortalActor* Portal)
{
	check(IsInGameThread());

	if (!Portal)
	{
		return;
	}

	const int32 RemovedCount = PortalStates.Remove(TObjectKey<AWormholePortalActor>(Portal));

	if (RemovedCount > 0)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(
			this,
			Verbose,
			TEXT("[PortalLightCollection][PortalUntracked] Portal=%s TrackedPortalCount=%d"),
			*GetNameSafe(Portal),
			PortalStates.Num());
#endif
	}
}

void UWPPortalLightCollectionSubsystem::CompactInvalidPortals()
{
	check(IsInGameThread());

	for (TMap<TObjectKey<AWormholePortalActor>, FWPPortalAffectingLightState>::TIterator It(PortalStates); It; ++It)
	{
		AWormholePortalActor* Portal = It.Value().Portal.Get();

		if (IsValid(Portal) && Portal->GetWorld() == GetWorld())
		{
			continue;
		}

		It.RemoveCurrent();
	}
}

void UWPPortalLightCollectionSubsystem::RefreshAllPortalLights()
{
	check(IsInGameThread());

	for (TPair<TObjectKey<AWormholePortalActor>, FWPPortalAffectingLightState>& Entry : PortalStates)
	{
		RefreshPortalLights(Entry.Value);
	}
}

void UWPPortalLightCollectionSubsystem::RefreshPortalLights(FWPPortalAffectingLightState& PortalState)
{
	check(IsInGameThread());
	
	AWormholePortalActor* Portal = PortalState.Portal.Get();
	
	if (!IsValid(Portal) || Portal->GetWorld() != GetWorld())
	{
		return;
	}
	
	const float PortalRadius = FMath::Max(Portal->GetPortalRadius(), 1.0f);
	
	/**
	 * 현재 Portal Actor는 unit scale을 전제로 합니다.
	 * 따라서 PortalRadius를 world-space seam radius로 그대로 사용합니다.
	 */
	const FBoxSphereBounds PortalBounds(Portal->GetActorLocation(), FVector(PortalRadius), PortalRadius);
	
	TArray<TWeakObjectPtr<ULightComponent>> NewLights;
	TSet<TObjectKey<ULightComponent>> NewLightKeys;
	
	NewLights.Reserve(TrackedLights.Num());
	NewLightKeys.Reserve(TrackedLights.Num());
	
	for (const FWPTrackedLightRecord& Record : TrackedLights)
	{
		ULightComponent* LightComponent = Record.LightComponent.Get();
		
		if (!IsEligibleLight(LightComponent))
		{
			continue;
		}
		
		const bool bAffectsPortal = LightComponent->GetLightType() == LightType_Directional || LightComponent->AffectsBounds(PortalBounds);
		
		if (!bAffectsPortal)
		{
			continue;
		}
		
		NewLights.Add(LightComponent);
		NewLightKeys.Add(Record.ObjectKey);
	}
	
	const bool bMembershipChanges = !PortalState.bInitialized || !AreLightKeySetsEqual(PortalState.AffectingLightKeys, NewLightKeys);
	
	if (!bMembershipChanges)
	{
		return;
	}
	
#if !UE_BUILD_SHIPPING
	const int32 PreviousLightCount = PortalState.AffectingLights.Num();
#endif
	PortalState.AffectingLights = MoveTemp(NewLights);
	PortalState.AffectingLightKeys = MoveTemp(NewLightKeys);
	PortalState.bInitialized = true;
	++PortalState.Revision;
	
	if (PortalState.Revision == 0)
	{
		PortalState.Revision = 1;
	}
	
#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalLightCollection][MembershipChanged] Portal=%s PreviousLights=%d CurrentLights=%d Revision=%u"),
		*GetNameSafe(Portal),
		PreviousLightCount,
		PortalState.AffectingLights.Num(),
		PortalState.Revision);
#endif
}

bool UWPPortalLightCollectionSubsystem::IsSupportedLight(const ULightComponent* LightComponent) const
{
	if (!IsValid(LightComponent) || LightComponent->IsTemplate() || LightComponent->GetWorld() != GetWorld())
	{
		return false;
	}
	
	if (LightComponent->ComponentHasTag(WPPortalLightTags::Disabled) 
		|| LightComponent->ComponentHasTag(WPPortalLightTags::Generated))
	{
		return false;
	}
	
	if (const AActor* Owner = LightComponent->GetOwner())
	{
		if (Owner->ActorHasTag(WPPortalLightTags::Disabled) 
			|| Owner->ActorHasTag(WPPortalLightTags::Generated))
		{
			return false;
		}
	}
	
	return IsSupportedLightType(*LightComponent);
}

bool UWPPortalLightCollectionSubsystem::IsEligibleLight(const ULightComponent* LightComponent) const
{
	if (!IsSupportedLight(LightComponent))
	{
		return false;
	}
	
	return LightComponent->IsRegistered()
	&& LightComponent->bAffectsWorld
	&& LightComponent->ShouldComponentAddToScene()
	&& LightComponent->ShouldRender()
	&& LightComponent->GetColoredLightBrightness().GetMax() > KINDA_SMALL_NUMBER;
}

bool UWPPortalLightCollectionSubsystem::IsSupportedLightType(const ULightComponent& LightComponent)
{
	switch (LightComponent.GetLightType())
	{
	case LightType_Point:
	case LightType_Spot:
	case LightType_Directional:
		return true;
		
	default:
		return false;
	}
}

bool UWPPortalLightCollectionSubsystem::AreLightKeySetsEqual(const TSet<TObjectKey<ULightComponent>>& A,
	const TSet<TObjectKey<ULightComponent>>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	
	for (const TObjectKey<ULightComponent>& Key : A)
	{
		if (!B.Contains(Key))
		{
			return false;
		}
	}
	
	return true;
}

void UWPPortalLightCollectionSubsystem::HandleActorSpawned(AActor* SpawnedActor)
{
	check(IsInGameThread());
	
	if (bDeinitializing || !IsValid(SpawnedActor) || SpawnedActor->GetWorld() != GetWorld())
	{
		return;
	}
	
	/**
	 * Actor spawn 중간 상태에서 Component를 읽지 않도록
	 * 실제 검색은 PostActorTick까지 미룹니다.
	 */
	PendingSpawnedActors.AddUnique(SpawnedActor);
}

void UWPPortalLightCollectionSubsystem::HandleWorldPostActorTick(UWorld* TickedWorld, ELevelTick TickType,
	float DeltaSeconds)
{
	check(IsInGameThread());
	(void)TickType;
	(void)DeltaSeconds;
	
	if (bDeinitializing || TickedWorld != GetWorld())
	{
		return;
	}
	
	ProcessPendingSpawnedActors();
	
	const double NowSeconds = FPlatformTime::Seconds();
	
	if (NowSeconds >= NextFullLightReconcileRealSeconds)
	{
		/**
		 * Actor가 이미 존재한 상태에서 AddComponent로 추가된 Light를 보정합니다.
		 * TrackLight는 idempotent하므로 기존 항목은 중복되지 않습니다.
		 */
		DiscoverAllLights();
		
		NextFullLightReconcileRealSeconds = NowSeconds + WPPortalLightCollectionPrivate::FullLightReconcileIntervalSeconds;
	}
	
	CompactTrackedLights();
	CompactInvalidPortals();
	
	/**
	 * Light 및 Portal movement가 모두 끝난 시점의 transform으로
	 * 현재 프레임 AffectingLights를 다시 계산합니다.
	 */
	RefreshAllPortalLights();
}

void UWPPortalLightCollectionSubsystem::HandlePortalRegistered(AWormholePortalActor* Portal)
{
	check(IsInGameThread());
	RegisterPortal(Portal);
}

void UWPPortalLightCollectionSubsystem::HandlePortalUnregistered(AWormholePortalActor* Portal)
{
	check(IsInGameThread());
	UnregisterPortal(Portal);
}
