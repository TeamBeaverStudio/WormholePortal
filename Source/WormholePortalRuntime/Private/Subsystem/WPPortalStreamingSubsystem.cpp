// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPPortalStreamingSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Subsystem/WPRegistrySubsystem.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "Transit/WPTransitComponent.h"

namespace
{
	constexpr float WPPortalStreamingMinimumQueryIntervalSeconds = 0.01f;
#if !UE_BUILD_SHIPPING
	// Log-only: Controls only the reporting interval for periodic performance summaries.
	constexpr float WPPortalStreamingSummaryIntervalSeconds = 5.0f;
#endif
}

void UWPPortalStreamingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
#if !UE_BUILD_SHIPPING
	// Log-only: Measures subsystem initialization CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	Super::Initialize(Collection);
	bDeinitializing = false;
	RegistrySubsystem = Collection.InitializeDependency<UWPRegistrySubsystem>();

#if !UE_BUILD_SHIPPING
	int32 BootstrapPortalCount = 0;
#endif
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		PortalRegisteredHandle = Registry->OnPortalRegistered().AddUObject(
			this, &UWPPortalStreamingSubsystem::HandlePortalRegistered);
		PortalUnregisteredHandle = Registry->OnPortalUnregistered().AddUObject(
			this, &UWPPortalStreamingSubsystem::HandlePortalUnregistered);
		PortalChangedHandle = Registry->OnPortalChanged().AddUObject(
			this, &UWPPortalStreamingSubsystem::HandlePortalChanged);

		TArray<AWormholePortalActor*> Portals;
		Registry->GetRegisteredPortals(Portals);
		for (AWormholePortalActor* Portal : Portals)
		{
			TrackPortal(Portal, TEXT("RegistryBootstrap"));
		}
#if !UE_BUILD_SHIPPING
		BootstrapPortalCount = Portals.Num();
#endif
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Initialized. World=%s WorldType=%d NetMode=%d RegistryValid=%d DelegatesBound=%d BootstrapPortals=%d TrackedPortals=%d DedicatedServerTick=1 DestinationRequestRefCounting=1 CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), GetWorld() ? static_cast<int32>(GetWorld()->WorldType) : -1,
		GetWorld() ? static_cast<int32>(GetWorld()->GetNetMode()) : -1,
		RegistrySubsystem.IsValid() ? 1 : 0,
		PortalRegisteredHandle.IsValid() && PortalUnregisteredHandle.IsValid()
			&& PortalChangedHandle.IsValid() ? 1 : 0,
		BootstrapPortalCount, PortalStates.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalStreamingSubsystem::Deinitialize()
{
#if !UE_BUILD_SHIPPING
	// Log-only: Measures subsystem cleanup CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	bDeinitializing = true;
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		Registry->OnPortalRegistered().Remove(PortalRegisteredHandle);
		Registry->OnPortalUnregistered().Remove(PortalUnregisteredHandle);
		Registry->OnPortalChanged().Remove(PortalChangedHandle);
	}
	PortalRegisteredHandle.Reset();
	PortalUnregisteredHandle.Reset();
	PortalChangedHandle.Reset();

#if !UE_BUILD_SHIPPING
	// Log-only: Preserves pre-cleanup tracked and request counts for the shutdown log.
	const int32 TrackedBefore = PortalStates.Num();
	const int32 RequestsBefore = GetActiveRequestCount();
#endif
	for (TPair<TWeakObjectPtr<AWormholePortalActor>, FPortalStreamingState>& Pair : PortalStates)
	{
		ReleaseDestinationRequest(Pair.Value, TEXT("SubsystemDeinitialize"));
	}
	PortalStates.Reset();
	DestinationDemandCounts.Reset();
	RegistrySubsystem.Reset();

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Deinitialized. World=%s TrackedPortalsBefore=%d ActiveRequestsBefore=%d TrackedPortalsAfter=0 ActiveRequestsAfter=0 ActiveDestinationsAfter=0 Acquires=%llu Releases=%llu Balance=%lld CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), TrackedBefore, RequestsBefore,
		static_cast<unsigned long long>(SummaryRequestAcquireCount),
		static_cast<unsigned long long>(SummaryRequestReleaseCount),
		static_cast<long long>(SummaryRequestAcquireCount)
			- static_cast<long long>(SummaryRequestReleaseCount),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif

	Super::Deinitialize();
}

void UWPPortalStreamingSubsystem::Tick(const float DeltaTime)
{
#if !UE_BUILD_SHIPPING
	// Log-only: Accumulates tick CPU cost for periodic performance summaries.
	const double TickStartSeconds = FPlatformTime::Seconds();
	++SummaryTickCount;
#endif
	const float SafeDeltaTime = FMath::Max(DeltaTime, 0.0f);
	for (auto It = PortalStates.CreateIterator(); It; ++It)
	{
		FPortalStreamingState& State = It.Value();
		AWormholePortalActor* Portal = State.Portal.Get();
		if (!IsValid(Portal) || Portal->GetWorld() != GetWorld())
		{
			ReleaseDestinationRequest(State, TEXT("InvalidTrackedPortal"));
			It.RemoveCurrent();
			continue;
		}

		State.QueryElapsedSeconds += SafeDeltaTime;
		const float QueryInterval = FMath::Max(
			Portal->StreamingQueryInterval,
			WPPortalStreamingMinimumQueryIntervalSeconds);
		if (State.QueryElapsedSeconds < QueryInterval)
		{
			continue;
		}
		State.QueryElapsedSeconds = FMath::Fmod(State.QueryElapsedSeconds, QueryInterval);
		EvaluatePortal(State);
	}

#if !UE_BUILD_SHIPPING
	SummaryElapsedSeconds += SafeDeltaTime;
	const double TickCpuMs = (FPlatformTime::Seconds() - TickStartSeconds) * 1000.0;
	SummaryCpuMs += TickCpuMs;
	SummaryMaxTickCpuMs = FMath::Max(SummaryMaxTickCpuMs, TickCpuMs);
	if (SummaryElapsedSeconds >= WPPortalStreamingSummaryIntervalSeconds)
	{
		// Log-only: Calculates the average CPU cost for the current summary interval.
		const double AverageTickCpuMs = SummaryTickCount > 0
			? SummaryCpuMs / static_cast<double>(SummaryTickCount) : 0.0;
		const double AverageDemandQueryCpuMs = SummaryDemandQueryCount > 0
			? SummaryDemandQueryCpuMs / static_cast<double>(SummaryDemandQueryCount) : 0.0;
		WP_LOG(this, VeryVerbose,
			TEXT("[Streaming][Subsystem][Perf] Summary. World=%s IntervalSeconds=%.3f Ticks=%llu TrackedPortals=%d Evaluations=%llu DemandQueries=%llu ActiveRequests=%d ActiveDestinations=%d Acquires=%llu Releases=%llu TotalTickCpuMs=%.4f AverageTickCpuMs=%.5f MaxTickCpuMs=%.5f TotalDemandQueryCpuMs=%.4f AverageDemandQueryCpuMs=%.5f MaxDemandQueryCpuMs=%.5f ActorTickDependency=0 DedicatedServerTick=1 DestinationRequestRefCounting=1"),
			*GetNameSafe(GetWorld()), SummaryElapsedSeconds,
			static_cast<unsigned long long>(SummaryTickCount), PortalStates.Num(),
			static_cast<unsigned long long>(SummaryEvaluationCount),
			static_cast<unsigned long long>(SummaryDemandQueryCount),
			GetActiveRequestCount(), DestinationDemandCounts.Num(),
			static_cast<unsigned long long>(SummaryRequestAcquireCount),
			static_cast<unsigned long long>(SummaryRequestReleaseCount),
			SummaryCpuMs, AverageTickCpuMs, SummaryMaxTickCpuMs,
			SummaryDemandQueryCpuMs, AverageDemandQueryCpuMs,
			SummaryMaxDemandQueryCpuMs);
		SummaryElapsedSeconds = FMath::Fmod(
			SummaryElapsedSeconds, WPPortalStreamingSummaryIntervalSeconds);
		SummaryTickCount = 0;
		SummaryEvaluationCount = 0;
		SummaryDemandQueryCount = 0;
		SummaryCpuMs = 0.0;
		SummaryMaxTickCpuMs = 0.0;
		SummaryDemandQueryCpuMs = 0.0;
		SummaryMaxDemandQueryCpuMs = 0.0;
	}
#endif
}

TStatId UWPPortalStreamingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWPPortalStreamingSubsystem, STATGROUP_Tickables);
}

bool UWPPortalStreamingSubsystem::IsTickable() const
{
	const UWorld* World = GetWorld();
	return !HasAnyFlags(RF_ClassDefaultObject) && IsInitialized() && !bDeinitializing
		&& World && World->IsGameWorld();
}

int32 UWPPortalStreamingSubsystem::GetActiveRequestCount() const
{
	int32 Count = 0;
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FPortalStreamingState>& Pair : PortalStates)
	{
		Count += Pair.Value.bHasDestinationRequest ? 1 : 0;
	}
	return Count;
}

void UWPPortalStreamingSubsystem::TrackPortal(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	if (bDeinitializing || !IsValid(Portal) || Portal->GetWorld() != GetWorld())
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	// Log-only: Measures portal tracking CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	FPortalStreamingState& State = PortalStates.FindOrAdd(Portal);
	State.Portal = Portal;
	const TObjectKey<AWormholePortalActor> DestinationKey(Portal);
	const int32 ExistingDemandCount = DestinationDemandCounts.FindRef(DestinationKey);
	if (Portal->PortalAreaStreamingSource)
	{
		if (ExistingDemandCount > 0)
		{
			Portal->PortalAreaStreamingSource->EnableStreamingSource();
		}
		else
		{
			Portal->PortalAreaStreamingSource->DisableStreamingSource();
		}
	}
	// Evaluate on the next subsystem Tick instead of waiting an authored interval.
	State.QueryElapsedSeconds = FMath::Max(
		Portal->StreamingQueryInterval,
		WPPortalStreamingMinimumQueryIntervalSeconds);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Portal tracked and source normalized. World=%s Portal=%s ExistingDestinationDemand=%d SourceEnabled=%d TrackedPortals=%d ActiveRequests=%d SerializedSourceStateAuthoritative=0 Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(Portal), ExistingDemandCount,
		Portal->PortalAreaStreamingSource
			&& Portal->PortalAreaStreamingSource->IsStreamingSourceEnabled() ? 1 : 0,
		PortalStates.Num(), GetActiveRequestCount(), Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalStreamingSubsystem::UntrackPortal(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	// Log-only: Measures portal untracking and request cleanup CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
	ReleaseRequestsTargeting(Portal, Reason);
	const TWeakObjectPtr<AWormholePortalActor> Key(Portal);
	const TObjectKey<AWormholePortalActor> DestinationKey(Portal);
	if (FPortalStreamingState* State = PortalStates.Find(Key))
	{
		ReleaseDestinationRequest(*State, Reason);
		PortalStates.Remove(Key);
	}
	const int32 OrphanedDemandCount = DestinationDemandCounts.FindRef(DestinationKey);
	DestinationDemandCounts.Remove(DestinationKey);
	if (OrphanedDemandCount != 0)
	{
		WP_LOG(this, Error,
			TEXT("[Streaming][Subsystem][Invariant] Destination refcount survived source-state release during untrack. World=%s Portal=%s OrphanedDemandCount=%d ActiveRequests=%d Reason=%s CpuMs=%.4f"),
			*GetNameSafe(GetWorld()), *GetNameSafe(Portal), OrphanedDemandCount,
			GetActiveRequestCount(), Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
	if (Portal && Portal->PortalAreaStreamingSource)
	{
		Portal->PortalAreaStreamingSource->DisableStreamingSource();
	}
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Portal untracked. World=%s Portal=%s TrackedPortals=%d ActiveRequests=%d ActiveDestinations=%d OrphanedDemandCount=%d SourceDisabled=%d Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(Portal), PortalStates.Num(),
		GetActiveRequestCount(), DestinationDemandCounts.Num(), OrphanedDemandCount,
		Portal && Portal->PortalAreaStreamingSource
			&& !Portal->PortalAreaStreamingSource->IsStreamingSourceEnabled() ? 1 : 0,
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalStreamingSubsystem::EvaluatePortal(FPortalStreamingState& State)
{
#if !UE_BUILD_SHIPPING
	++SummaryEvaluationCount;
#endif
	AWormholePortalActor* Portal = State.Portal.Get();
	UWorld* World = GetWorld();
	if (!IsValid(Portal) || !World || !World->IsPartitionedWorld())
	{
		ReleaseDestinationRequest(State, TEXT("NonPartitionedOrInvalidWorld"));
		return;
	}

	FWPPortalPairSnapshot PairSnapshot;
	UWPRegistrySubsystem* Registry = RegistrySubsystem.Get();
	AWormholePortalActor* Destination = Registry
		&& Registry->FindRegisteredPortalPair(Portal, PairSnapshot)
		? PairSnapshot.GetOther(Portal)
		: nullptr;
	if (!IsValid(Destination) || Destination->GetWorld() != World
		|| !Destination->PortalAreaStreamingSource)
	{
		ReleaseDestinationRequest(State, TEXT("RegistryPairDestinationUnavailable"));
		return;
	}
	const bool bRequestAlreadyActive = State.bHasDestinationRequest
		&& State.RequestedDestinationKey == TObjectKey<AWormholePortalActor>(Destination);
	const float DemandDistance = bRequestAlreadyActive
		? Portal->StreamingReleaseDistance
		: Portal->StreamingPreloadDistance;
	const bool bHasDemand = HasNearbyDemand(*Portal, DemandDistance);
	SetDestinationRequest(
		State, Destination, bHasDemand,
		bHasDemand ? TEXT("DemandPresent") : TEXT("DemandAbsent"));
}

void UWPPortalStreamingSubsystem::SetDestinationRequest(
	FPortalStreamingState& State,
	AWormholePortalActor* Destination,
	const bool bRequested,
	const TCHAR* Reason)
{
	if (!bRequested || !IsValid(Destination))
	{
		ReleaseDestinationRequest(State, Reason);
		return;
	}
	const TObjectKey<AWormholePortalActor> DestinationKey(Destination);
	if (State.bHasDestinationRequest && State.RequestedDestinationKey == DestinationKey)
	{
		const int32 EffectiveDemandCount = DestinationDemandCounts.FindRef(DestinationKey);
		if (EffectiveDemandCount > 0)
		{
			if (Destination->PortalAreaStreamingSource
				&& !Destination->PortalAreaStreamingSource->IsStreamingSourceEnabled())
			{
				// Log-only: Measures CPU time spent in the actual EnableStreamingSource repair call.
				const double RepairStartSeconds = FPlatformTime::Seconds();
				Destination->PortalAreaStreamingSource->EnableStreamingSource();
				WP_LOG(this, Warning,
					TEXT("[Streaming][Subsystem][Invariant] Destination source was externally disabled while demand remained; source re-enabled. World=%s SourcePortal=%s DestinationPortal=%s DemandCount=%d Reason=%s CpuMs=%.4f"),
					*GetNameSafe(GetWorld()), *GetNameSafe(State.Portal.Get()),
					*GetNameSafe(Destination), EffectiveDemandCount,
					Reason ? Reason : TEXT("Unspecified"),
					(FPlatformTime::Seconds() - RepairStartSeconds) * 1000.0);
			}
			return;
		}
		ReleaseDestinationRequest(State, TEXT("MissingRefcountEntryRepair"));
	}
	if (State.bHasDestinationRequest)
	{
		ReleaseDestinationRequest(State, TEXT("DestinationChanged"));
	}

#if !UE_BUILD_SHIPPING
	// Log-only: Measures request acquisition CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	int32& DemandCount = DestinationDemandCounts.FindOrAdd(DestinationKey);
	const int32 PreviousDemandCount = DemandCount;
	++DemandCount;
	State.RequestedDestination = Destination;
	State.RequestedDestinationKey = DestinationKey;
	State.bHasDestinationRequest = true;
#if !UE_BUILD_SHIPPING
	++SummaryRequestAcquireCount;
#endif
	if (PreviousDemandCount == 0 && Destination->PortalAreaStreamingSource)
	{
		Destination->PortalAreaStreamingSource->EnableStreamingSource();
	}
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Destination request acquired. World=%s SourcePortal=%s DestinationPortal=%s PreviousDemandCount=%d DemandCount=%d SourceEnabled=%d ActiveRequests=%d ActiveDestinations=%d PreloadDistanceCm=%.1f ReleaseDistanceCm=%.1f QueryIntervalMs=%.1f Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(State.Portal.Get()),
		*GetNameSafe(Destination), PreviousDemandCount, DemandCount,
		Destination->PortalAreaStreamingSource
			&& Destination->PortalAreaStreamingSource->IsStreamingSourceEnabled() ? 1 : 0,
		GetActiveRequestCount(), DestinationDemandCounts.Num(),
		State.Portal.IsValid() ? State.Portal->StreamingPreloadDistance : 0.0f,
		State.Portal.IsValid() ? State.Portal->StreamingReleaseDistance : 0.0f,
		State.Portal.IsValid() ? State.Portal->StreamingQueryInterval * 1000.0f : 0.0f,
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalStreamingSubsystem::ReleaseDestinationRequest(
	FPortalStreamingState& State,
	const TCHAR* Reason)
{
	if (!State.bHasDestinationRequest)
	{
		State.RequestedDestination.Reset();
		return;
	}
	// Log-only: Measures request release CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
	AWormholePortalActor* Destination = State.RequestedDestination.Get();
	const TObjectKey<AWormholePortalActor> DestinationKey = State.RequestedDestinationKey;
	int32 PreviousDemandCount = 0;
	int32 DemandCount = 0;
	if (int32* ExistingDemandCount = DestinationDemandCounts.Find(DestinationKey))
	{
		PreviousDemandCount = *ExistingDemandCount;
		DemandCount = FMath::Max(PreviousDemandCount - 1, 0);
		if (DemandCount == 0)
		{
			DestinationDemandCounts.Remove(DestinationKey);
		}
		else
		{
			*ExistingDemandCount = DemandCount;
		}
	}
	if (PreviousDemandCount <= 0)
	{
		WP_LOG(this, Warning,
			TEXT("[Streaming][Subsystem][Invariant] Active source request had no destination refcount entry. World=%s SourcePortal=%s DestinationPortal=%s DestinationKeyHash=%u Reason=%s CpuMs=%.4f"),
			*GetNameSafe(GetWorld()), *GetNameSafe(State.Portal.Get()),
			*GetNameSafe(Destination), GetTypeHash(DestinationKey),
			Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
	State.RequestedDestination.Reset();
	State.RequestedDestinationKey = TObjectKey<AWormholePortalActor>();
	State.bHasDestinationRequest = false;
#if !UE_BUILD_SHIPPING
	++SummaryRequestReleaseCount;
#endif
	if (DemandCount == 0 && IsValid(Destination) && Destination->PortalAreaStreamingSource)
	{
		Destination->PortalAreaStreamingSource->DisableStreamingSource();
	}
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Destination request released. World=%s SourcePortal=%s DestinationPortal=%s PreviousDemandCount=%d DemandCount=%d SourceEnabled=%d ActiveRequests=%d ActiveDestinations=%d Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(State.Portal.Get()),
		*GetNameSafe(Destination), PreviousDemandCount, DemandCount,
		IsValid(Destination) && Destination->PortalAreaStreamingSource
			&& Destination->PortalAreaStreamingSource->IsStreamingSourceEnabled() ? 1 : 0,
		GetActiveRequestCount(), DestinationDemandCounts.Num(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalStreamingSubsystem::ReleaseRequestsTargeting(
	AWormholePortalActor* Destination,
	const TCHAR* Reason)
{
	if (!Destination)
	{
		return;
	}
	const TObjectKey<AWormholePortalActor> DestinationKey(Destination);
	for (TPair<TWeakObjectPtr<AWormholePortalActor>, FPortalStreamingState>& Pair : PortalStates)
	{
		if (Pair.Value.bHasDestinationRequest
			&& Pair.Value.RequestedDestinationKey == DestinationKey)
		{
			ReleaseDestinationRequest(Pair.Value, Reason);
			Pair.Value.QueryElapsedSeconds = FMath::Max(
				Pair.Value.Portal.IsValid()
					? Pair.Value.Portal->StreamingQueryInterval : 0.0f,
				WPPortalStreamingMinimumQueryIntervalSeconds);
		}
	}
}

bool UWPPortalStreamingSubsystem::HasNearbyDemand(
	const AWormholePortalActor& Portal,
	const float MaximumDistance)
{
#if !UE_BUILD_SHIPPING
	// Log-only: Records demand-query CPU cost in the periodic summary.
	const double StartSeconds = FPlatformTime::Seconds();
	++SummaryDemandQueryCount;
#endif
	UWorld* World = GetWorld();
	bool bHasDemand = false;
	if (World && MaximumDistance >= 0.0f)
	{
		const FVector PortalLocation = Portal.GetActorLocation();
		const float MaximumDistanceSquared = FMath::Square(MaximumDistance);
		if (World->GetNetMode() == NM_Standalone)
		{
			if (const APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				if (const APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
				{
					bHasDemand = FVector::DistSquared(
						CameraManager->GetCameraLocation(), PortalLocation)
						<= MaximumDistanceSquared;
				}
			}
		}

		if (!bHasDemand)
		{
			FCollisionObjectQueryParams ObjectQueryParams;
			ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);
			ObjectQueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
			ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(&Portal);
			TArray<FOverlapResult> Overlaps;
			if (World->OverlapMultiByObjectType(
				Overlaps, PortalLocation, FQuat::Identity, ObjectQueryParams,
				FCollisionShape::MakeSphere(MaximumDistance), QueryParams))
			{
				for (const FOverlapResult& Overlap : Overlaps)
				{
					AActor* OtherActor = Overlap.GetActor();
					const UWPTransitComponent* TransitComponent = IsValid(OtherActor)
						? OtherActor->FindComponentByClass<UWPTransitComponent>() : nullptr;
					if (IsValid(TransitComponent) && TransitComponent->GetTransitEnabled())
					{
						bHasDemand = true;
						break;
					}
				}
			}
		}
	}

#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	SummaryDemandQueryCpuMs += CpuMs;
	SummaryMaxDemandQueryCpuMs = FMath::Max(SummaryMaxDemandQueryCpuMs, CpuMs);
#endif
	return bHasDemand;
}

void UWPPortalStreamingSubsystem::HandlePortalRegistered(AWormholePortalActor* Portal)
{
	TrackPortal(Portal, TEXT("RegistryPortalRegistered"));
}

void UWPPortalStreamingSubsystem::HandlePortalUnregistered(AWormholePortalActor* Portal)
{
	UntrackPortal(Portal, TEXT("RegistryPortalUnregistered"));
}

void UWPPortalStreamingSubsystem::HandlePortalChanged(
	AWormholePortalActor* Portal,
	const EWPPortalChangeType ChangeType)
{
#if !UE_BUILD_SHIPPING
	// Log-only: Measures link invalidation CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (ChangeType != EWPPortalChangeType::Link || !IsValid(Portal))
	{
		return;
	}
	const TWeakObjectPtr<AWormholePortalActor> Key(Portal);
	FPortalStreamingState& State = PortalStates.FindOrAdd(Key);
	State.Portal = Portal;
	ReleaseDestinationRequest(State, TEXT("PortalLinkChanged"));
	State.QueryElapsedSeconds = FMath::Max(
		Portal->StreamingQueryInterval,
		WPPortalStreamingMinimumQueryIntervalSeconds);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Streaming][Subsystem] Portal link invalidation observed. World=%s Portal=%s LinkedPortal=%s EvaluateNextTick=1 ActiveRequests=%d CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(Portal),
		*GetNameSafe(Portal->GetLinkedPortal()), GetActiveRequestCount(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}
