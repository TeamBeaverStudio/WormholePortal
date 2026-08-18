// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "WPRegistrySubsystem.generated.h"

class AWormholePortalActor;

/**
 * Runtime snapshot of a valid, mutually linked Portal Pair in the current World.
 *
 * PairId is the shared identifier for the current connection lifetime.
 * PortalA and PortalB are stored as weak pointers so the Registry does not own Actor lifetimes.
 * An endpoint may already be destroyed when a PairRemoved event fires, so only PairId is always reliable.
 * Weak-endpoint access and helper calls must occur on the Game Thread.
 */
struct WORMHOLEPORTALRUNTIME_API FWPPortalPairSnapshot
{
	/** Identifier for the current connection lifetime. A new value is assigned after unlinking and relinking. */
	FGuid PairId;

	/** Canonical endpoints ordered by Actor UniqueID. They are not inherently directional. */
	TWeakObjectPtr<AWormholePortalActor> PortalA;
	TWeakObjectPtr<AWormholePortalActor> PortalB;

	/**
	 * Tests only whether the Snapshot's own fields are structurally valid.
	 * Current active-Pair status must be determined from Registry membership, not from this
	 * function.
	 */
	bool IsStructurallyValid() const
	{
		check(IsInGameThread());
		return PairId.IsValid()
			&& PortalA.IsValid()
			&& PortalB.IsValid()
			&& PortalA.Get() != PortalB.Get();
	}

	bool Contains(const AWormholePortalActor* Portal) const
	{
		check(IsInGameThread());
		return Portal != nullptr && (PortalA.Get() == Portal || PortalB.Get() == Portal);
	}

	AWormholePortalActor* GetOther(const AWormholePortalActor* Portal) const
	{
		check(IsInGameThread());

		if (!Portal)
		{
			return nullptr;
		}

		if (PortalA.Get() == Portal)
		{
			return PortalB.Get();
		}

		if (PortalB.Get() == Portal)
		{
			return PortalA.Get();
		}

		return nullptr;
	}
};

/** Identifies a link or domain-data change that preserves Portal registration. */
UENUM()
enum class EWPPortalChangeType : uint8
{
	Link,
	Metric,
	/** Render-only appearance changed without changing Metric, bounds, capture, or LUT resources. */
	Visual,
	RenderResources
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPPortalRegistered, AWormholePortalActor*);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPPortalUnregistered, AWormholePortalActor*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnWPPortalChanged, AWormholePortalActor*, EWPPortalChangeType);

DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPPortalPairAdded, const FWPPortalPairSnapshot&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPPortalPairRemoved, const FWPPortalPairSnapshot&);

/**
 * @brief World-scoped Wormhole Portal Registry Subsystem.
 *
 * As a UWorldSubsystem, a separate instance is created for each Editor, PIE, and
 * Runtime World.
 * It collects Wormhole Portal Actors that have begun play in the same World and
 * provides the current valid list
 * to code that needs it, such as global queries or Portal-count enforcement.
 *
 * The Registry does not own Portal Actor lifetimes.
 * It stores only TWeakObjectPtrs and removes already-destroyed objects during
 * registration, removal, or change mutations.
 * Read functions do not mutate internal state; they only omit invalid entries from
 * their results.
 * Pointers returned by this Subsystem therefore represent objects that are valid only
 * at call time.
 * Consumers that retain them must perform their own validity checks.
 *
 * All public APIs and delegates are Game Thread only.
 * Consumers on other threads must create the required value snapshot on the Game Thread
 * and transfer that snapshot
 * to their own thread.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPRegistrySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Fires after a Portal enters the public registration set.
	 * Unobserved intermediate membership, such as an unregister/register round trip inside
	 * a callback,
	 * may be coalesced into the final state.
	 * A late consumer must bind first, bootstrap with GetRegisteredPortals, and process
	 * Actors idempotently.
	 */
	FOnWPPortalRegistered& OnPortalRegistered()
	{
		check(IsInGameThread());
		return PortalRegisteredDelegate;
	}

	/**
	 * Fires after a Portal has been removed from the Registry and public registration set.
	 * Delivery is not guaranteed during abnormal cleanup when the Portal is already
	 * destroyed and no raw pointer
	 * can be provided, or during Subsystem shutdown.
	 */
	FOnWPPortalUnregistered& OnPortalUnregistered()
	{
		check(IsInGameThread());
		return PortalUnregisteredDelegate;
	}

	/** Invalidation delegate requesting that consumers reread a registered Portal's current link, metric, or render resources. */
	FOnWPPortalChanged& OnPortalChanged()
	{
		check(IsInGameThread());
		return PortalChangedDelegate;
	}

	/**
	 * @brief Registers a Wormhole Portal Actor that has begun play with the current World's
	 *        Portal Registry.
	 *
	 * An already-registered Portal is not duplicated, and invalid weak pointers are removed
	 * before registration.
	 * A Portal belonging to a different World is not registered.
	 *
	 * @param Portal Wormhole Portal Actor to register. Invalid objects are ignored.
	 * @return No return value.
	 */
	void RegisterPortal(AWormholePortalActor* Portal);

	/**
	 * @brief Removes a Wormhole Portal Actor from the Portal Registry during EndPlay or
	 *        explicit cleanup.
	 *
	 * Removes both the specified Portal and any weak pointers that are already invalid.
	 *
	 * @param Portal Wormhole Portal Actor to unregister. Invalid entries are still cleaned
	 *               up when this is nullptr.
	 * @return No return value.
	 */
	void UnregisterPortal(AWormholePortalActor* Portal);

	/**
	 * @brief Returns the Wormhole Portal Actors that are registered and still valid in the
	 *        current World.
	 *
	 * Invalid weak pointers are omitted from the result, but this read API does not mutate
	 * internal state or fire delegates.
	 *
	 * @param OutPortals Array that receives valid Portal pointers. It is reset and
	 *                   repopulated by the function.
	 * @return No return value. Results are stored in OutPortals.
	 */
	void GetRegisteredPortals(OUT TArray<AWormholePortalActor*>& OutPortals) const;

	/**
	 * Returns the mutually linked Pairs currently contained in the Registry.
	 * This function only reads the list; it neither mutates Registry state nor fires
	 * events.
	 *
	 * Consumers must use this API in the following order:
	 *
	 * 1. During initialization, bind callbacks to OnPortalPairAdded and OnPortalPairRemoved
	 *    first.
	 *    Then call this function to obtain Pairs that already existed before delegate
	 *    binding.
	 * 2. A consumer must not reconstruct Pairs or generate new PairIds by inspecting the
	 *    Portal list and GetLinkedPortal.
	 *    Use the PairId supplied by the Registry directly as the PairStates key and
	 *    Renderer-packet PairId.
	 * 3. On PairAdded or PairRemoved, reread the complete current Pair list with this
	 *    function and reconcile PairStates to it.
	 *    The same PairId may appear in both the initial list and PairAdded, so adding or
	 *    removing the same Pair twice
	 *    must be idempotent.
	 * 4. PairId remains stable while the two Portals remain linked. Unlinking and relinking
	 *    creates a new PairId.
	 *    Before starting the new Pair, clean up the old Renderer handle, packet sequence,
	 *    ownership, and warmup state.
	 * 5. Do not use OnPortalRegistered or OnPortalUnregistered to construct a consumer's
	 *    Pair topology.
	 *    Metric and RenderResources notifications from OnPortalChanged remain valid for
	 *    refreshing the affected Pair.
	 *    Process Portal-link changes from PairAdded, PairRemoved, and the list returned by
	 *    this function.
	 * 6. A Portal Actor may already be destroyed when PairRemoved fires. Remove consumer
	 *    state and the Renderer handle
	 *    by PairId without reading Actor pointers. PairRemoved may not fire during Registry
	 *    shutdown, so consumers must
	 *    also release every remaining Pair during Deinitialize.
	 * 7. Read this API and Portal Actors only on the Game Thread. Do not send Actor
	 *    pointers to the Render Thread;
	 *    create a packet containing only copied values on the Game Thread.
	 */
	void GetRegisteredPortalPairs(OUT TArray<FWPPortalPairSnapshot>& OutPairs) const;

	/** Finds the active Pair containing the specified Portal without mutating Registry state. */
	bool FindRegisteredPortalPair(const AWormholePortalActor* Portal, OUT FWPPortalPairSnapshot& OutPair) const;

	/** Notifies subscribers that a Portal's link, metric, or render resources changed. Subscribers must reread the Portal's current values. */
	void NotifyPortalChanged(AWormholePortalActor* Portal, EWPPortalChangeType ChangeType);

	/**
	 * Fires after a new Pair is added to the Registry's current list.
	 * This event alone does not deliver Pairs that existed before subscription.
	 * Bind the callback first, then call GetRegisteredPortalPairs to read the complete
	 * current list.
	 * The same PairId may appear in both the complete list and this event, so duplicate
	 * addition must be safe.
	 * If changes repeat excessively during event handling, delivery may be deferred to the
	 * next Game Thread tick.
	 * If the event and complete list appear inconsistent, use the current list returned by
	 * GetRegisteredPortalPairs
	 * as the authoritative state.
	 */
	FOnWPPortalPairAdded& OnPortalPairAdded()
	{
		check(IsInGameThread());
		return PortalPairAddedDelegate;
	}

	/**
	 * Fires after a Pair is removed from the Registry's current list.
	 * A Portal Actor may already be destroyed at this point, so related state must be
	 * removed by PairId,
	 * not by reading an Actor pointer.
	 * Removing the same PairId twice must be idempotent.
	 * This event may not fire during World or Registry shutdown, so subscribers must also
	 * release
	 * every retained Pair state during Deinitialize.
	 */
	FOnWPPortalPairRemoved& OnPortalPairRemoved()
	{
		check(IsInGameThread());
		return PortalPairRemovedDelegate;
	}

private:
	struct FPendingPortalChangeEvent
	{
		TWeakObjectPtr<AWormholePortalActor> Portal;
		EWPPortalChangeType ChangeType = EWPPortalChangeType::Link;
	};

	/** Removes weak pointers to already-destroyed objects from RegisteredPortals and returns the number removed. */
	int32 CompactInvalidPortals();

	/** Adds a PortalChanged invalidation to a FIFO queue instead of broadcasting it recursively. */
	void EnqueuePortalChangeEvent(
		AWormholePortalActor* Portal,
		EWPPortalChangeType ChangeType);

	/** Processes differences between current and public registration, followed by pending change invalidations. */
	void DrainPortalEvents();

	/** Tests whether any registration difference or change invalidation remains unpublished externally. */
	bool HasPendingPortalEvents() const;

	/**
	 * Immediately recomputes the Pair map from current Actor links and requests the required event drain.
	 * Calling it again from a Pair delegate does not reenter event broadcasting.
	 */
	void RequestPortalPairReconcile();

	/** Broadcasts differences between the current Pair map and externally published Pair set in order, with Removed events first. */
	void DrainPortalPairEvents();

	/** Defers work exceeding one event-drain budget to the next Game Thread tick. */
	void ScheduleRegistryEventDrain();

	/**
	 * Recomputes the Pair map from RegisteredPortals and current Actor-link state.
	 * Preserves PairId for continuing Pairs and assigns a new PairId to a Pair recreated after disconnection.
	 */
	void ReconcilePortalPairs();

	/** Creates a process-local structural lookup key for two endpoints. */
	static uint64 MakePortalPairKey(const AWormholePortalActor* PortalA, const AWormholePortalActor* PortalB);

private:
	/**
	 * @brief Weak-reference list of Wormhole Portal Actors registered with the Registry in the current World.
	 * The array does not own Portal objects and serves only as a lookup index.
	 * When an Actor is destroyed, its weak pointer becomes invalid and is cleaned up lazily by the next Register, Unregister, or Notify path.
	 */
	TArray<TWeakObjectPtr<AWormholePortalActor>> RegisteredPortals;

	/** Authoritative Pair map representing the current Actor topology. */
	TMap<uint64, FWPPortalPairSnapshot> RegisteredPortalPairs;

	/** Externally published state for which PairAdded has been sent but PairRemoved has not. */
	TMap<FGuid, FWPPortalPairSnapshot> PublishedPortalPairs;

	/** Externally published state for which PortalRegistered has been sent but PortalUnregistered has not. */
	TSet<
		TWeakObjectPtr<AWormholePortalActor>,
		TWeakObjectPtrSetKeyFuncs<TWeakObjectPtr<AWormholePortalActor>>> PublishedRegisteredPortals;

	FOnWPPortalRegistered PortalRegisteredDelegate;
	FOnWPPortalUnregistered PortalUnregisteredDelegate;
	FOnWPPortalChanged PortalChangedDelegate;

	FOnWPPortalPairAdded PortalPairAddedDelegate;
	FOnWPPortalPairRemoved PortalPairRemovedDelegate;

	/** Stores PortalChanged invalidations produced during reentrancy in FIFO order. */
	TArray<FPendingPortalChangeEvent> PendingPortalChangeEvents;

	/** Prevents the same delegate queue from being drained recursively inside a general Portal delegate. */
	bool bIsDrainingPortalEvents = false;

	/** Nesting depth used to defer only Pair events while general Portal delegates are broadcasting recursively. */
	int32 PortalPairEventDeferralDepth = 0;

	/** Indicates that Pair reconciliation was requested by the current or a nested callback. */
	bool bPortalPairReconcileRequested = false;

	/** Prevents recursive entry into reconciliation while the Pair map is being computed. */
	bool bIsReconcilingPortalPairs = false;

	/** Prevents a Pair delegate from recursively broadcasting another Pair delegate. */
	bool bIsDrainingPortalPairEvents = false;

	/** Indicates whether a Registry event drain is already scheduled for the next Game Thread tick. */
	bool bRegistryEventDrainScheduled = false;

	/** State used to stop follow-up callback processing during Deinitialize. */
	bool bIsDeinitializing = false;
};
