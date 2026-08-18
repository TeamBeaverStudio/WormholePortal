// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "Subsystem/WPRuntimeCaptureScheduler.h"
#include "Subsystem/WPRuntimePairState.h"
#include "Subsystem/WPRuntimeRenderPublication.h"
#include "Subsystem/WPRuntimeTelemetry.h"
#include "Subsystems/WorldSubsystem.h"
#include "WPRuntimeSubsystem.generated.h"

class AActor;
class AWormholePortalActor;
class UWPCaptureManager;
class UWPLUTEndpointManager;
class UWPRegistrySubsystem;
class UWPTransitSubsystem;
enum class EWPPortalChangeType : uint8;
struct FWPPortalPairSnapshot;
struct FWPTransitEvent;
struct FWPCaptureEndpointSnapshot;
struct FWPLUTEndpointSnapshot;

/**
 * World-scoped coordinator for the Wormhole Runtime.
 *
 * Mirrors Pair lifetimes published by RegistrySubsystem into the FWPPortalPairState Map
 * and creates and
 * destroys the Managers and two non-UObject worker objects with the World. Each Tick,
 * it reads one Reference
 * View, then coordinates CaptureScheduler visibility/occlusion updates followed by
 * RenderPublication Packet
 * publication.
 *
 * Ownership:
 * - UWPCaptureManager owns Cubemap Components/Targets and Pair capture state.
 * - UWPLUTEndpointManager owns Endpoint LUT requests and Texture contracts.
 * - FWPRuntimeCaptureScheduler owns visibility, occlusion, cadence, and
 *   capture-submission state.
 * - FWPRuntimeRenderPublication owns RenderHandles, the ownership handshake, and Packet
 *   build/publish work.
 * - UWPRegistrySubsystem is the sole authority for Pair identity and lifetime.
 *
 * Renderer Service and SceneViewExtension perform the actual SceneColor compositing.
 * This class never passes
 * Actor or UObject pointers directly to the Render Thread; it publishes only value-type
 * FWPRenderPackets.
 *
 * World initialization order:
 * 1. Verify that this is a render-capable Game World. Create no Render resources if the
 *    gate fails.
 * 2. Create CaptureManager and LUTEndpointManager and initialize them with the World
 *    context.
 * 3. Create CaptureScheduler and RenderPublication and connect non-owning Manager and
 *    PairStates references.
 * 4. Bind Registry Pair Delegates before bootstrapping from the full Snapshot. Binding
 *    first prevents Pair
 *    add/remove Events occurring during the Snapshot read from being lost.
 * 5. Register Capture/LUT resources for existing Portal Endpoints with the Managers.
 * 6. Bind Transit and WorldPostActorTick Delegates and apply the initial effective
 *    Runtime CVar state.
 *
 * Per-Frame order:
 * 1. Process RuntimeEnabled changes and Registry-dirty state in Tick.
 * 2. Select one Local Player Reference View so every Pair uses the same Camera
 *    Snapshot.
 * 3. Update CaptureScheduler visibility/occlusion state.
 * 4. Ensure each Pair's RenderHandle and compute its Reference Region, Packet, and
 *    ownership.
 * 5. Publish only changed Pairs and commit publication baselines only after success.
 * 6. In a separate PostActorTick Callback, submit Cubemap capture for Pairs whose
 *    cadence is due. This is
 *    separate from regular Tick so the Capture Camera is computed after Actor
 *    Transforms finish updating.
 *
 * Shutdown order:
 * 1. Unbind World, Registry, and Transit Delegates first to prevent new Callbacks.
 * 2. Remove every Pair's Renderer Handle and Manager Pair state before Endpoint
 *    resources.
 * 3. Release Endpoint UObjects and Render Targets through the LUT/Capture Managers.
 * 4. Reset non-UObject workers and weak Subsystem references, then log
 *    allocation/release balance.
 *
 * Threading:
 * This class, PairStates, and Manager APIs are Game Thread-only. The only data shared
 * with the Render Thread
 * is FWPRenderPacket, which the Renderer copies and owns, and Renderer-internal
 * mailboxes. RuntimeSubsystem
 * does not call synchronization operations such as FlushRenderingCommands directly on
 * the normal Frame path.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPRuntimeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Configures Managers, worker objects, and Registry/Transit/World Delegates in a
	 * render-capable Game World.
	 * On a dedicated server, non-Game World, or when CanEverRender is false, emits only a
	 * RuntimeGate log and
	 * performs no allocation.
	 * @param Collection World Subsystem collection in which RegistrySubsystem and
	 *                   TransitSubsystem initialization
	 * dependencies are declared.
	 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Unbinds Delegates first, then tears down Pairs, Endpoint Managers, and worker objects in order, and records the final balance. */
	virtual void Deinitialize() override;

	/**
	 * Processes Reference View selection, Pair Packet construction, and publication once
	 * each. Actual capture
	 * submission runs in PostActorTick.
	 * @param DeltaTime Frame Delta for this World Tick. Publication uses a separate
	 *                  monotonic time source.
	 */
	virtual void Tick(float DeltaTime) override;

	/** Returns the Stat ID used to identify RuntimeSubsystem Tick cost in the Unreal stat system. */
	virtual TStatId GetStatId() const override;

	/** Allows Tick only after initialization in a render-capable, non-dedicated Game World. */
	virtual bool IsTickable() const override;

	/** Returns the current number of valid bidirectional Pairs matching the Registry Snapshot; used by diagnostics and automation tests. */
	int32 GetPairCount() const { return PairStates.Num(); }

	/**
	 * Returns the latest ownership snapshot for the Pair containing Portal. A false result
	 * means Portal is not
	 * part of a registered bidirectional Pair. The value is for read-only Game Thread
	 * diagnostics and production
	 * state inspection; it exposes no Renderer UObject.
	 * @param Portal Endpoint Actor to query. Fails if null or unregistered.
	 * @param OutSnapshot On success, receives requested/effective mode, Epoch, readiness,
	 *                    and handle identity.
	 * @return true if an exactly matching active Pair was found.
	 */
	bool GetPairOwnershipSnapshot(
		const AWormholePortalActor* Portal,
		FWPPairOwnershipSnapshot& OutSnapshot) const;

	/**
	 * Returns the latest read-only Snapshot for an Endpoint owned by the LUT Manager.
	 * Texture ownership is not transferred; returns false if Portal is not registered.
	 * @param Portal Endpoint Actor to query.
	 * @param OutSnapshot On success, receives registration/readiness, Texture contract, and
	 *                    Generation.
	 * @return true if the Manager contains a valid Endpoint record.
	 */
	bool GetLUTEndpointSnapshot(
		const AWormholePortalActor* Portal,
		FWPLUTEndpointSnapshot& OutSnapshot) const;
	/**
	 * Returns the latest read-only Snapshot for a Cubemap Endpoint owned by CaptureManager.
	 * Component/Target ownership remains with the Manager; callers observe only non-owning
	 * references and
	 * Generation/contract values.
	 * @param Portal Endpoint Actor to query.
	 * @param OutSnapshot On success, receives non-owning Component/Target references, the
	 *                    Cube contract, and
	 * resource/capture Generations.
	 * @return true if the Manager contains a valid Endpoint record.
	 */
	bool GetCaptureEndpointSnapshot(
		const AWormholePortalActor* Portal,
		FWPCaptureEndpointSnapshot& OutSnapshot) const;

private:
	/** Grants the Scheduler limited access to Owner bridges and shared private state. */
	friend class FWPRuntimeCaptureScheduler;
	/** Grants the Publication object limited access to World/Metric helpers and Manager references. */
	friend class FWPRuntimeRenderPublication;

	/**
	 * Reconciles the Registry's complete current Pair Snapshot with PairStates.
	 * Preserves state when existing Endpoint identity matches and removes replaced or
	 * deleted lifetimes through
	 * RemovePair. Creates no state for an invalid Snapshot, self-Pair, or invalid Endpoint
	 * and never invents a
	 * Registry PairId.
	 */
	void RebuildPairs();

	/**
	 * Releases one Pair's Renderer Handle and Manager Pair state before its Endpoint
	 * resources.
	 * The caller performs the Map erase; Reason is retained in the lifetime-termination
	 * log.
	 * @param PairStateKey Registry PairId key used in the PairStates Map.
	 * @param PairState Identity, capture, ownership, and publication state to release.
	 * @param Reason Invocation cause, such as Registry removal, Endpoint release, or
	 *               reconciliation replacement.
	 */
	void RemovePair(const FGuid& PairStateKey, FWPPortalPairState& PairState, const TCHAR* Reason);

	/**
	 * Marks publication/validation state dirty for every Pair containing Portal.
	 * Also schedules full Registry reconciliation for a topology change.
	 * @param Portal Changed Endpoint Actor. If null, no state is modified.
	 * @param bTopologyChanged If true, also sets bPairsDirty because Pair composition may
	 *                         have changed.
	 * @return Number of active Pairs marked dirty.
	 */
	int32 MarkPairDirtyForPortal(AWormholePortalActor* Portal, bool bTopologyChanged);

	/**
	 * Thin forwarding entry that bridges UObject Delegate lifetime to the non-UObject
	 * Scheduler.
	 * @param World Callback source; the Scheduler revalidates it against the Owner World.
	 * @param TickType Engine Level Tick type.
	 * @param DeltaSeconds Frame Delta forwarded to cadence processing.
	 */
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);

	/**
	 * Forwarding entry that makes CaptureScheduler use the same Reference View selection
	 * rule as
	 * RenderPublication.
	 * @param OutCameraLocation World-space Camera location on success.
	 * @param OutCameraRotation World-space Camera rotation on success.
	 * @param OutViewActor Local Pawn or View Target on success.
	 * @param OutCameraFOVDegrees Camera FOV on success.
	 * @return true if RenderPublication resolved exactly one Reference View.
	 */
	bool ResolveReferenceView(
		FVector& OutCameraLocation,
		FRotator& OutCameraRotation,
		AActor*& OutViewActor,
		float& OutCameraFOVDegrees) const;

	/**
	 * Registers Capture/LUT resources for a new Endpoint and marks related Pair
	 * publications dirty.
	 * @param Portal Endpoint just registered by the Registry. Ignored if invalid.
	 */
	void HandlePortalRegistered(AWormholePortalActor* Portal);

	/**
	 * Tears down related Pair lifetimes and Renderer Handles before releasing the Endpoint.
	 * @param Portal Endpoint being removed from the Registry. Its Pairs are removed before
	 *               Manager resources.
	 */
	void HandlePortalUnregistered(AWormholePortalActor* Portal);

	/**
	 * Schedules topology reconciliation or Packet republication according to the
	 * Portal-property change type.
	 * @param Portal Endpoint that published the change Event.
	 * @param ChangeType Registry Enum that distinguishes Link/Topology changes from
	 *                   Metric/Resource changes.
	 */
	void HandlePortalChanged(AWormholePortalActor* Portal, EWPPortalChangeType ChangeType);

	/**
	 * Propagates an LUT Manager Snapshot change to related Pairs and updates Renderer
	 * resource notification when
	 * required.
	 * @param Portal Weak Endpoint retained by the Manager Callback.
	 * @param Snapshot Latest LUT Snapshot with finalized registration/readiness/Generation.
	 */
	void HandleLUTEndpointChanged(
		TWeakObjectPtr<AWormholePortalActor> Portal,
		const FWPLUTEndpointSnapshot& Snapshot);
	/**
	 * Receives an add Event for a Registry-confirmed PairId and immediately reconciles
	 * against the current full
	 * Snapshot.
	 * @param PairSnapshot Identity at Event time. The Registry's current full Snapshot is
	 *                     authoritative.
	 */
	void HandlePortalPairAdded(const FWPPortalPairSnapshot& PairSnapshot);

	/**
	 * Immediately removes state by Registry PairId, then reconciles idempotently against
	 * the latest Snapshot.
	 * @param PairSnapshot Provides the PairId to remove. Endpoint pointers may be
	 *                     terminating and are not read.
	 */
	void HandlePortalPairRemoved(const FWPPortalPairSnapshot& PairSnapshot);

	/**
	 * Forwards the Transit Started Event to the common apply function. Does not force a
	 * capture recapture.
	 * @param Event Immutable Event containing the starting Sequence and related
	 *              Actors/Portals.
	 */
	void HandleTransitStarted(const FWPTransitEvent& Event);

	/**
	 * Forwards the Transit Committed Event to the common apply function.
	 * @param Event Contains the committed Transit Sequence and related Actor/Portal
	 *              information.
	 */
	void HandleTransitCommitted(const FWPTransitEvent& Event);

	/**
	 * Forwards the Transit Cancelled Event to the common apply function.
	 * @param Event Contains the cancelled Transit Sequence and related Actor/Portal
	 *              information.
	 */
	void HandleTransitCancelled(const FWPTransitEvent& Event);

	/**
	 * Applies only the relevant Reference Actor's Transit state/Sequence to the Pair and
	 * marks Packet publication
	 * dirty. Phase is a diagnostic-log identifier; Transit itself does not force capture or
	 * atomic submission.
	 * @param Event Immutable Transit Event containing the Actor, Source/Destination
	 *              Portals, and Sequence.
	 * @param Phase Current Callback phase name: Started, Committed, or Cancelled.
	 */
	void ApplyTransitEvent(const FWPTransitEvent& Event, const TCHAR* Phase);

	/**
	 * Returns whether the Event Actor is a Local Player Pawn or View Target in the current World.
	 * @param Actor Gameplay Actor whose relevance will be tested.
	 * @return true if a Local Player is actually observing the Actor.
	 */
	bool IsRelevantReferenceActor(const AActor* Actor) const;

	/**
	 * Stably mixes every FGuid bit into a deterministic Sort Key for a Pair candidate.
	 * @param PairId 128-bit Pair identity issued by the Registry.
	 * @return 64-bit key that is identical for the same PairId regardless of execution
	 *         order.
	 */
	static uint64 MakePairSortKey(const FGuid& PairId);

	/**
	 * Converts Portal settings into a Renderer Metric value object after Scale validation.
	 * @param Portal Endpoint whose Shape/Transition/Metric properties will be read.
	 * @return Renderer Metric Snapshot containing no UObject references.
	 */
	FWPMetricSettings MakeMetricSettings(const AWormholePortalActor& Portal) const;

private:
	UPROPERTY(Transient)
	/** UObject that owns Endpoint Cubemaps and Manager-owned Pair capture state. */
	TObjectPtr<UWPCaptureManager> CaptureManager;

	UPROPERTY(Transient)
	/** UObject that owns Endpoint LUT requests, Texture bindings, and the Generation contract. */
	TObjectPtr<UWPLUTEndpointManager> LUTEndpointManager;

	/** Source of Pair identity and lifetime. Initialized before this Subsystem but stored as a weak reference. */
	TWeakObjectPtr<UWPRegistrySubsystem> RegistrySubsystem;
	/** Source of Transit Events; does not own the Gameplay Subsystem lifetime. */
	TWeakObjectPtr<UWPTransitSubsystem> TransitSubsystem;
	/** Game Thread state Map keyed by Registry PairId, the sole source of Pair-lifetime identity. */
	TMap<FGuid, FWPPortalPairState> PairStates;
	/** Non-UObject Scheduler that owns visibility, occlusion, cadence, and capture submission. */
	TUniquePtr<FWPRuntimeCaptureScheduler> CaptureScheduler;
	/** Worker object that owns Renderer registration, the ownership handshake, and Packet publication. */
	TUniquePtr<FWPRuntimeRenderPublication> RenderPublication;

	/** Handle used to unbind the Registry Endpoint-registration Delegate. */
	FDelegateHandle PortalRegisteredHandle;
	/** Handle used to unbind the Registry Endpoint-unregistration Delegate. */
	FDelegateHandle PortalUnregisteredHandle;
	/** Handle used to unbind the Registry Portal-change Delegate. */
	FDelegateHandle PortalChangedHandle;
	/** Handle used to unbind the Registry Pair-added Delegate. */
	FDelegateHandle PortalPairAddedHandle;
	/** Handle used to unbind the Registry Pair-removed Delegate. */
	FDelegateHandle PortalPairRemovedHandle;
	/** Handle used to unbind the Transit Started Delegate. */
	FDelegateHandle TransitStartedHandle;
	/** Handle used to unbind the Transit Committed Delegate. */
	FDelegateHandle TransitCommittedHandle;
	/** Handle used to unbind the Transit Cancelled Delegate. */
	FDelegateHandle TransitCancelledHandle;
	/** Handle used to unbind the post-Actor-Tick Capture Scheduler Callback. */
	FDelegateHandle WorldPostActorTickHandle;
	/** Handle used to unbind the LUT Endpoint-Snapshot-change Delegate. */
	FDelegateHandle LUTEndpointChangedHandle;

	/** Whether the full Registry Snapshot requires reconciliation. */
	bool bPairsDirty = true;
	/** If false, this World cannot render, so Managers and Render Delegates are not created. */
	bool bRenderRuntimeEnabled = false;
	/** Current effective Packet-pipeline state for wp.RuntimeEnabled, independent of the Capture Scheduler. */
	bool bRenderPacketPipelineActive = false;
	/** Lifetime guard that blocks reentry and late Event handling during shutdown. */
	bool bDeinitializing = false;
	/** Cache used to log the raw CVar value once initially and only when it changes thereafter. */
	int32 LastRuntimeEnabledRaw = MIN_int32;
	/** Timestamp of the last missing-Renderer warning, used to rate-limit subsequent warnings. */
	double LastRendererUnavailableLogSeconds = -1.0e30;
	/** Minimal cumulative lifetime values required for shutdown and error diagnostics; never used as Runtime policy input. */
	FWPRuntimeTelemetry Telemetry;
};
