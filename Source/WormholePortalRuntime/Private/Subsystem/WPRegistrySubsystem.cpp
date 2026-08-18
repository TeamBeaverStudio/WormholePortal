// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPRegistrySubsystem.h"
#include "WormholePortalActor.h"
#include "WPLog.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

namespace
{
	constexpr int32 MaxRegistryEventsPerDrain = 128;

#if !UE_BUILD_SHIPPING
	const TCHAR* GetPortalChangeTypeName(const EWPPortalChangeType ChangeType)
	{
		switch (ChangeType)
		{
		case EWPPortalChangeType::Link: return TEXT("Link");
		case EWPPortalChangeType::Metric: return TEXT("Metric");
		case EWPPortalChangeType::Visual: return TEXT("Visual");
		case EWPPortalChangeType::RenderResources: return TEXT("RenderResources");
		default: return TEXT("Unknown");
		}
	}
#endif
}

void UWPRegistrySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	// 로그 전용: Registry 초기화 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	Super::Initialize(Collection);

	bIsDeinitializing = false;
	PublishedRegisteredPortals.Reset();
	PendingPortalChangeEvents.Reset();
	bIsDrainingPortalEvents = false;
	bPortalPairReconcileRequested = false;
	bIsReconcilingPortalPairs = false;
	bIsDrainingPortalPairEvents = false;
	bRegistryEventDrainScheduled = false;
	PortalPairEventDeferralDepth = 0;

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Registry][Initialize] World=%s WorldType=%d RegisteredCount=%d CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), GetWorld() ? static_cast<int32>(GetWorld()->WorldType) : INDEX_NONE,
		RegisteredPortals.Num(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRegistrySubsystem::Deinitialize()
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	// 로그 전용: 종료 전 등록 수와 cleanup CPU 시간을 보존합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 ReleasedPortalCount = RegisteredPortals.Num();
#endif
	bIsDeinitializing = true;

	RegisteredPortalPairs.Reset();
	PublishedPortalPairs.Reset();
	PublishedRegisteredPortals.Reset();

	PortalPairAddedDelegate.Clear();
	PortalPairRemovedDelegate.Clear();

	RegisteredPortals.Reset();
	PortalRegisteredDelegate.Clear();
	PortalUnregisteredDelegate.Clear();
	PortalChangedDelegate.Clear();

	PendingPortalChangeEvents.Reset();
	bIsDrainingPortalEvents = false;
	bPortalPairReconcileRequested = false;
	bIsReconcilingPortalPairs = false;
	bIsDrainingPortalPairEvents = false;
	bRegistryEventDrainScheduled = false;
	PortalPairEventDeferralDepth = 0;

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Registry][Deinitialize] World=%s ReleasedPortalCount=%d CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), ReleasedPortalCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif

	Super::Deinitialize();
}

void UWPRegistrySubsystem::RegisterPortal(AWormholePortalActor* Portal)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	// 로그 전용: register 처리 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (bIsDeinitializing)
	{
		return;
	}

	const int32 RemovedInvalidCount = CompactInvalidPortals();
	if (!IsValid(Portal) || Portal->GetWorld() != GetWorld())
	{
		if (RemovedInvalidCount > 0)
		{
			RequestPortalPairReconcile();
			DrainPortalEvents();
		}

		if (IsValid(Portal) && Portal->GetWorld() != GetWorld())
		{
			WP_LOG(this, Warning,
				TEXT("[Registry][RegisterRejected] Portal=%s PortalWorld=%s RegistryWorld=%s Reason=DifferentWorld"),
				*GetNameSafe(Portal), *GetNameSafe(Portal->GetWorld()), *GetNameSafe(GetWorld()));
		}
		return;
	}

	const bool bAlreadyRegistered = RegisteredPortals.ContainsByPredicate([Portal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
	{
		return RegisteredPortal.Get() == Portal;
	});

	if (bAlreadyRegistered)
	{
		if (RemovedInvalidCount > 0)
		{
			RequestPortalPairReconcile();
			DrainPortalEvents();
		}
		return;
	}

#if !UE_BUILD_SHIPPING
	// 로그 전용: registration 완료 로그에 사용할 이름 복사본입니다.
	const FString PortalName = GetNameSafe(Portal);
#endif
	RegisteredPortals.Add(Portal);

	{
		TGuardValue<int32> PairEventDeferral(
			PortalPairEventDeferralDepth,
			PortalPairEventDeferralDepth + 1);

		// Generic callback에서도 새 Registry topology를 조회할 수 있게 map을 먼저 갱신하되,
		// PairAdded는 generic callback이 모두 끝난 뒤 최종 상태만 발행합니다.
		RequestPortalPairReconcile();
		DrainPortalEvents();
	}

	if (bIsDeinitializing)
	{
		return;
	}

	DrainPortalPairEvents();

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Registry][Registered] Portal=%s RegisteredCount=%d CpuMs=%.3f"),
		*PortalName, RegisteredPortals.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRegistrySubsystem::UnregisterPortal(AWormholePortalActor* Portal)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	// 로그 전용: unregister 처리 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (bIsDeinitializing)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	// 로그 전용: unregister 처리 중 Portal 수명이 바뀌어도 완료 로그에 사용할 이름 복사본입니다.
	const FString PortalName = GetNameSafe(Portal);
#endif
	const bool bWasRegistered = IsValid(Portal) && RegisteredPortals.ContainsByPredicate(
		[Portal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
		{
			return RegisteredPortal.Get() == Portal;
		});

	const int32 RemovedInvalidCount = CompactInvalidPortals();
	const int32 RemovedPortalCount = RegisteredPortals.RemoveAllSwap(
		[Portal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
		{
			return RegisteredPortal.Get() == Portal;
		});
	const int32 RemovedCount = RemovedInvalidCount + RemovedPortalCount;

	if (bWasRegistered)
	{
		TGuardValue<int32> PairEventDeferral(
			PortalPairEventDeferralDepth,
			PortalPairEventDeferralDepth + 1);

		RequestPortalPairReconcile();
		DrainPortalEvents();
	}
	else if (RemovedCount > 0)
	{
		// 이미 파괴된 weak endpoint만 정리된 경우에도 PairRemoved가 빠지지 않게 합니다.
		RequestPortalPairReconcile();
		DrainPortalEvents();
	}

	if (bIsDeinitializing)
	{
		return;
	}

	DrainPortalPairEvents();

	if (bWasRegistered)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Registry][Unregistered] Portal=%s RegisteredCount=%d CpuMs=%.3f"),
			*PortalName, RegisteredPortals.Num(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}
}

void UWPRegistrySubsystem::GetRegisteredPortals(TArray<AWormholePortalActor*>& OutPortals) const
{
	check(IsInGameThread());

	OutPortals.Reset();
	if (bIsDeinitializing)
	{
		return;
	}

	OutPortals.Reserve(RegisteredPortals.Num());

	for (const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal : RegisteredPortals)
	{
		if (AWormholePortalActor* Portal = RegisteredPortal.Get())
		{
			OutPortals.Add(Portal);
		}
	}
}

void UWPRegistrySubsystem::GetRegisteredPortalPairs(TArray<FWPPortalPairSnapshot>& OutPairs) const
{
	check(IsInGameThread());

	if (bIsDeinitializing)
	{
		OutPairs.Reset();
		return;
	}

	OutPairs.Reset(RegisteredPortalPairs.Num());
	TArray<uint64> PairKeys;
	RegisteredPortalPairs.GetKeys(PairKeys);
	PairKeys.Sort();

	for (const uint64 PairKey : PairKeys)
	{
		const FWPPortalPairSnapshot* Pair = RegisteredPortalPairs.Find(PairKey);
		if (Pair && Pair->IsStructurallyValid())
		{
			OutPairs.Add(*Pair);
		}
	}
}

bool UWPRegistrySubsystem::FindRegisteredPortalPair(const AWormholePortalActor* Portal,
	FWPPortalPairSnapshot& OutPair) const
{
	check(IsInGameThread());

	OutPair = FWPPortalPairSnapshot{};

	if (bIsDeinitializing)
	{
		return false;
	}

	if (!IsValid(Portal))
	{
		return false;
	}

	for (const TPair<uint64, FWPPortalPairSnapshot>& Entry : RegisteredPortalPairs)
	{
		if (Entry.Value.IsStructurallyValid() && Entry.Value.Contains(Portal))
		{
			OutPair = Entry.Value;
			return true;
		}
	}

	return false;
}

void UWPRegistrySubsystem::NotifyPortalChanged(AWormholePortalActor* Portal, const EWPPortalChangeType ChangeType)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	// 로그 전용: change notification 처리 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (bIsDeinitializing)
	{
		return;
	}

	const int32 RemovedInvalidCount = CompactInvalidPortals();
	if (!IsValid(Portal))
	{
		if (RemovedInvalidCount > 0)
		{
			RequestPortalPairReconcile();
			DrainPortalEvents();
		}
		return;
	}

	const bool bIsRegistered = RegisteredPortals.ContainsByPredicate(
		[Portal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
		{
			return RegisteredPortal.Get() == Portal;
		});
	if (!bIsRegistered)
	{
		if (RemovedInvalidCount > 0)
		{
			RequestPortalPairReconcile();
			DrainPortalEvents();
		}
		return;
	}

#if !UE_BUILD_SHIPPING
	// 로그 전용: change 로그에 출력할 Portal 이름과 capture generation입니다.
	const FString PortalName = GetNameSafe(Portal);
	const uint32 CaptureGeneration = Portal->GetPortalCaptureGeneration();
#endif

	{
		TGuardValue<int32> PairEventDeferral(
			PortalPairEventDeferralDepth,
			PortalPairEventDeferralDepth + 1);

		if (ChangeType == EWPPortalChangeType::Link || RemovedInvalidCount > 0)
		{
			// Link callback이 Registry를 조회할 때는 이미 최신 topology가 보이게 하되,
			// callback 안의 추가 변경까지 끝난 뒤 pair event를 발행합니다.
			RequestPortalPairReconcile();
		}

		EnqueuePortalChangeEvent(Portal, ChangeType);
	}

	if (bIsDeinitializing)
	{
		return;
	}

	// Metric/RenderResources callback 안에서 topology가 바뀐 경우의 지연 event도 여기서 마무리합니다.
	DrainPortalPairEvents();

#if !UE_BUILD_SHIPPING
	if (ChangeType != EWPPortalChangeType::RenderResources
		&& ChangeType != EWPPortalChangeType::Visual)
	{
		WP_LOG(this, Verbose,
			TEXT("[Registry][Changed] Portal=%s ChangeType=%s CaptureGeneration=%u CpuMs=%.3f"),
			*PortalName, GetPortalChangeTypeName(ChangeType), CaptureGeneration,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
#endif
}

int32 UWPRegistrySubsystem::CompactInvalidPortals()
{
	check(IsInGameThread());

	return RegisteredPortals.RemoveAllSwap(
		[](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
		{
			return !RegisteredPortal.IsValid();
		});
}

void UWPRegistrySubsystem::EnqueuePortalChangeEvent(
	AWormholePortalActor* Portal,
	const EWPPortalChangeType ChangeType)
{
	check(IsInGameThread());

	if (bIsDeinitializing)
	{
		return;
	}

	FPendingPortalChangeEvent Event;
	Event.Portal = Portal;
	Event.ChangeType = ChangeType;
	PendingPortalChangeEvents.Add(MoveTemp(Event));

	DrainPortalEvents();
}

bool UWPRegistrySubsystem::HasPendingPortalEvents() const
{
	check(IsInGameThread());

	if (PendingPortalChangeEvents.Num() > 0)
	{
		return true;
	}

	for (const TWeakObjectPtr<AWormholePortalActor>& PublishedPortal : PublishedRegisteredPortals)
	{
		const bool bStillRegistered = RegisteredPortals.ContainsByPredicate(
			[&PublishedPortal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
			{
				return RegisteredPortal.IsValid() && RegisteredPortal == PublishedPortal;
			});

		if (!bStillRegistered)
		{
			return true;
		}
	}

	for (const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal : RegisteredPortals)
	{
		if (RegisteredPortal.IsValid() && !PublishedRegisteredPortals.Contains(RegisteredPortal))
		{
			return true;
		}
	}

	return false;
}

void UWPRegistrySubsystem::DrainPortalEvents()
{
	check(IsInGameThread());

	if (bIsDeinitializing || bIsDrainingPortalEvents)
	{
		return;
	}

	{
		TGuardValue<bool> PortalEventDrainGuard(bIsDrainingPortalEvents, true);
		TGuardValue<int32> PairEventDeferral(
			PortalPairEventDeferralDepth,
			PortalPairEventDeferralDepth + 1);

		int32 DrainedEventCount = 0;
		while (!bIsDeinitializing)
		{
			bool bProcessedEvent = false;

			// 현재 Registry에서 먼저 사라진 공개 registration을 처리합니다.
			TWeakObjectPtr<AWormholePortalActor> RemovedPortal;
			for (const TWeakObjectPtr<AWormholePortalActor>& PublishedPortal : PublishedRegisteredPortals)
			{
				const bool bStillRegistered = RegisteredPortals.ContainsByPredicate(
					[&PublishedPortal](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
					{
						return RegisteredPortal.IsValid() && RegisteredPortal == PublishedPortal;
					});

				if (!bStillRegistered)
				{
					RemovedPortal = PublishedPortal;
					break;
				}
			}

			if (!RemovedPortal.IsExplicitlyNull())
			{
				PublishedRegisteredPortals.Remove(RemovedPortal);
				CompactInvalidPortals();
				RequestPortalPairReconcile();
				if (AWormholePortalActor* Portal = RemovedPortal.Get())
				{
					PortalUnregisteredDelegate.Broadcast(Portal);
				}
				bProcessedEvent = true;
			}
			else
			{
				// 제거할 registration이 없을 때만 새 registration을 공개합니다.
				TWeakObjectPtr<AWormholePortalActor> AddedPortal;
				for (const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal : RegisteredPortals)
				{
					if (RegisteredPortal.IsValid() && !PublishedRegisteredPortals.Contains(RegisteredPortal))
					{
						AddedPortal = RegisteredPortal;
						break;
					}
				}

				if (!AddedPortal.IsExplicitlyNull())
				{
					PublishedRegisteredPortals.Add(AddedPortal);
					if (AWormholePortalActor* Portal = AddedPortal.Get())
					{
						PortalRegisteredDelegate.Broadcast(Portal);
					}
					bProcessedEvent = true;
				}
			}

			// Registration 공개 상태가 현재 상태와 맞을 때만 change invalidation을 하나 전달합니다.
			if (!bProcessedEvent && PendingPortalChangeEvents.Num() > 0)
			{
				const FPendingPortalChangeEvent Event = PendingPortalChangeEvents[0];
				PendingPortalChangeEvents.RemoveAt(0, 1, EAllowShrinking::No);

				const bool bCurrentlyRegistered = RegisteredPortals.ContainsByPredicate(
					[&Event](const TWeakObjectPtr<AWormholePortalActor>& RegisteredPortal)
					{
						return RegisteredPortal.IsValid() && RegisteredPortal == Event.Portal;
					});

				if (bCurrentlyRegistered && PublishedRegisteredPortals.Contains(Event.Portal))
				{
					if (AWormholePortalActor* Portal = Event.Portal.Get())
					{
						PortalChangedDelegate.Broadcast(Portal, Event.ChangeType);
					}
				}

				bProcessedEvent = true;
			}

			if (!bProcessedEvent)
			{
				break;
			}

			if (bIsDeinitializing)
			{
				return;
			}

			++DrainedEventCount;
			if (DrainedEventCount >= MaxRegistryEventsPerDrain && HasPendingPortalEvents())
			{
#if !UE_BUILD_SHIPPING
				WP_LOG(this, VeryVerbose,
					TEXT("[Registry][PortalEventDrainDeferred] Processed=%d PendingChanges=%d Registered=%d Published=%d"),
					DrainedEventCount, PendingPortalChangeEvents.Num(),
					RegisteredPortals.Num(), PublishedRegisteredPortals.Num());
#endif
				ScheduleRegistryEventDrain();
				return;
			}
		}
	}

	if (!bIsDeinitializing && !HasPendingPortalEvents())
	{
		DrainPortalPairEvents();
	}
}

void UWPRegistrySubsystem::RequestPortalPairReconcile()
{
	check(IsInGameThread());

	if (bIsDeinitializing)
	{
		return;
	}

	bPortalPairReconcileRequested = true;
	if (bIsReconcilingPortalPairs)
	{
		return;
	}

	{
		TGuardValue<bool> ReconcileGuard(bIsReconcilingPortalPairs, true);

		while (bPortalPairReconcileRequested && !bIsDeinitializing)
		{
			bPortalPairReconcileRequested = false;
			CompactInvalidPortals();
			ReconcilePortalPairs();
		}
	}

	if (!bIsDeinitializing)
	{
		DrainPortalPairEvents();
	}
}

void UWPRegistrySubsystem::DrainPortalPairEvents()
{
	check(IsInGameThread());

	if (bIsDeinitializing
		|| bIsDrainingPortalEvents
		|| bIsReconcilingPortalPairs
		|| bIsDrainingPortalPairEvents
		|| HasPendingPortalEvents()
		|| PortalPairEventDeferralDepth > 0)
	{
		return;
	}

	TGuardValue<bool> DrainGuard(bIsDrainingPortalPairEvents, true);
	int32 DrainedEventCount = 0;

	while (!bIsDeinitializing)
	{
		// 현재 topology에서 먼저 사라진 공개 pair를 한 개 찾습니다.
		FWPPortalPairSnapshot RemovedPair;
		bool bHasRemovedPair = false;

		for (const TPair<FGuid, FWPPortalPairSnapshot>& PublishedEntry : PublishedPortalPairs)
		{
			bool bStillRegistered = false;
			for (const TPair<uint64, FWPPortalPairSnapshot>& RegisteredEntry : RegisteredPortalPairs)
			{
				if (RegisteredEntry.Value.PairId == PublishedEntry.Key)
				{
					bStillRegistered = true;
					break;
				}
			}

			if (!bStillRegistered)
			{
				RemovedPair = PublishedEntry.Value;
				bHasRemovedPair = true;
				break;
			}
		}

		if (bHasRemovedPair)
		{
			// callback이 다시 Registry를 변경해도 같은 PairId의 Removed가 중복되지 않게 먼저 공개 상태에서 뺍니다.
			PublishedPortalPairs.Remove(RemovedPair.PairId);
			PortalPairRemovedDelegate.Broadcast(RemovedPair);

			if (bIsDeinitializing)
			{
				return;
			}

#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose, TEXT("[Registry][PairRemoved] PairId=%s PortalA=%s PortalB=%s"),
				*RemovedPair.PairId.ToString(),
				*GetNameSafe(RemovedPair.PortalA.Get()),
				*GetNameSafe(RemovedPair.PortalB.Get()));
#endif

			++DrainedEventCount;
			if (HasPendingPortalEvents())
			{
				return;
			}

			if (DrainedEventCount >= MaxRegistryEventsPerDrain)
			{
#if !UE_BUILD_SHIPPING
				WP_LOG(this, VeryVerbose,
					TEXT("[Registry][PairEventDrainDeferred] Processed=%d Registered=%d Published=%d"),
					DrainedEventCount, RegisteredPortalPairs.Num(), PublishedPortalPairs.Num());
#endif
				ScheduleRegistryEventDrain();
				return;
			}
			continue;
		}

		// 제거할 공개 pair가 없을 때만 새 pair를 발행하여 relink의 Removed -> Added 순서를 보장합니다.
		FWPPortalPairSnapshot AddedPair;
		bool bHasAddedPair = false;

		for (const TPair<uint64, FWPPortalPairSnapshot>& RegisteredEntry : RegisteredPortalPairs)
		{
			const FWPPortalPairSnapshot& Candidate = RegisteredEntry.Value;
			if (Candidate.IsStructurallyValid() && !PublishedPortalPairs.Contains(Candidate.PairId))
			{
				AddedPair = Candidate;
				bHasAddedPair = true;
				break;
			}
		}

		if (!bHasAddedPair)
		{
			return;
		}

		// PairAdded callback 안에서 즉시 unlink되어도 Added -> Removed lifecycle이 유지되게 먼저 공개 상태에 넣습니다.
		PublishedPortalPairs.Add(AddedPair.PairId, AddedPair);
		PortalPairAddedDelegate.Broadcast(AddedPair);

		if (bIsDeinitializing)
		{
			return;
		}

#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Registry][PairAdded] PairId=%s PortalA=%s PortalB=%s"),
			*AddedPair.PairId.ToString(),
			*GetNameSafe(AddedPair.PortalA.Get()),
			*GetNameSafe(AddedPair.PortalB.Get()));
#endif

		++DrainedEventCount;
		if (HasPendingPortalEvents())
		{
			return;
		}

		if (DrainedEventCount >= MaxRegistryEventsPerDrain)
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(this, VeryVerbose,
				TEXT("[Registry][PairEventDrainDeferred] Processed=%d Registered=%d Published=%d"),
				DrainedEventCount, RegisteredPortalPairs.Num(), PublishedPortalPairs.Num());
#endif
			ScheduleRegistryEventDrain();
			return;
		}
	}
}

void UWPRegistrySubsystem::ScheduleRegistryEventDrain()
{
	check(IsInGameThread());

	if (bIsDeinitializing || bRegistryEventDrainScheduled)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	bRegistryEventDrainScheduled = true;
	const TWeakObjectPtr<UWPRegistrySubsystem> WeakRegistry(this);

	World->GetTimerManager().SetTimerForNextTick([WeakRegistry]()
	{
		if (UWPRegistrySubsystem* Registry = WeakRegistry.Get())
		{
			Registry->bRegistryEventDrainScheduled = false;
			Registry->DrainPortalEvents();
		}
	});
}

void UWPRegistrySubsystem::ReconcilePortalPairs()
{
	check(IsInGameThread());

	// 1. 현재 유효한 Portal 집합을 만듭니다.
	TSet<AWormholePortalActor*> RegisteredSet;
	RegisteredSet.Reserve(RegisteredPortals.Num());

	for (const TWeakObjectPtr<AWormholePortalActor>& WeakPortal : RegisteredPortals)
	{
		if (AWormholePortalActor* Portal = WeakPortal.Get())
		{
			RegisteredSet.Add(Portal);
		}
	}

	// PairId가 없는 desired topology입니다.
	TMap<uint64, FWPPortalPairSnapshot> DesiredPairs;

	// 2. 유효한 양방향 link만 찾습니다.
	for (AWormholePortalActor* Portal : RegisteredSet)
	{
		if (!IsValid(Portal))
		{
			continue;
		}

		AWormholePortalActor* LinkedPortal = Portal->GetLinkedPortal();

		if (!IsValid(LinkedPortal)
			|| LinkedPortal == Portal
			|| Portal->GetWorld() != GetWorld()
			|| LinkedPortal->GetWorld() != GetWorld()
			|| !RegisteredSet.Contains(LinkedPortal)
			|| LinkedPortal->GetLinkedPortal() != Portal)
		{
			continue;
		}

		AWormholePortalActor* PortalA = Portal->GetUniqueID() < LinkedPortal->GetUniqueID() ? Portal : LinkedPortal;
		AWormholePortalActor* PortalB = PortalA == Portal ? LinkedPortal : Portal;

		const uint64 PairKey = MakePortalPairKey(PortalA, PortalB);

		if (PairKey == 0 || DesiredPairs.Contains(PairKey))
		{
			continue;
		}

		FWPPortalPairSnapshot DesiredPair;
		DesiredPair.PortalA = PortalA;
		DesiredPair.PortalB = PortalB;

		DesiredPairs.Add(PairKey, DesiredPair);
	}

	// 3. 유지되는 pair는 기존 PairId를 보존하고, 새 Pair만 새 PairId를 발급합니다.
	TMap<uint64, FWPPortalPairSnapshot> NewPairs;
	NewPairs.Reserve(DesiredPairs.Num());

	for (const TPair<uint64, FWPPortalPairSnapshot>& DesiredEntry : DesiredPairs)
	{
		const FWPPortalPairSnapshot* Existing = RegisteredPortalPairs.Find(DesiredEntry.Key);
		const bool bSameEndpoints = Existing
									&& Existing->IsStructurallyValid()
									&& Existing->PortalA == DesiredEntry.Value.PortalA
									&& Existing->PortalB == DesiredEntry.Value.PortalB;

		if (bSameEndpoints)
		{
			NewPairs.Add(DesiredEntry.Key, *Existing);
			continue;
		}

		FWPPortalPairSnapshot NewPair = DesiredEntry.Value;
		NewPair.PairId = FGuid::NewGuid();

		NewPairs.Add(DesiredEntry.Key, NewPair);
	}

	RegisteredPortalPairs = MoveTemp(NewPairs);
}

uint64 UWPRegistrySubsystem::MakePortalPairKey(const AWormholePortalActor* PortalA, const AWormholePortalActor* PortalB)
{
	if (!PortalA || !PortalB || PortalA == PortalB)
	{
		return 0;
	}

	const uint32 IdA = PortalA->GetUniqueID();
	const uint32 IdB = PortalB->GetUniqueID();

	const uint32 LowerId = FMath::Min(IdA, IdB);
	const uint32 UpperId = FMath::Max(IdA, IdB);

	return (static_cast<uint64>(LowerId) << 32) | static_cast<uint64>(UpperId);
}

