// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystem/WPRuntimePairState.h"

class AActor;
class UWPCaptureManager;
class UWPRuntimeSubsystem;
enum class EWPManagedCaptureSubmissionMode : uint8;

/**
 * Owns Portal Pair capture scheduling for one World.
 *
 * This is a Game Thread-only, non-UObject type. UWPRuntimeSubsystem creates and
 * destroys it, and it holds
 * non-owning references to the supplied Manager and Pair Map. The Scheduler must
 * therefore have a shorter
 * lifetime than RuntimeSubsystem and CaptureManager.
 *
 * Responsibilities:
 * - 0/1/2-Endpoint visibility policy driven by Renderer visibility feedback
 * - CPU line traces to eight SafeProxy ring points per Endpoint and camera-inside
 *   exclusive Pair selection
 * - Endpoint target-Hz and A/B staggered-cadence calculation
 * - Capture-authority warmup/recovery and CaptureManager submission
 *
 * Non-responsibilities:
 * - Pair creation and removal decided by the Registry
 * - RenderHandle ownership handshake and RenderPacket publication
 * - Actual ownership of Cubemap/LUT UObject resources
 *
 * Terms:
 * - Visibility: whether a Portal Endpoint was a screen-compositing candidate in the
 *   final Render Frame's
 *   View Frustum. The Game Thread reads Renderer feedback in the next Callback, so this
 *   is not an immediate
 *   result for the current Frame. It is the latest completed-Frame result whose
 *   Ownership Epoch and Packet
 *   Sequence have been validated.
 * - Occlusion: an additional conservative CPU line-trace test for a Portal that is
 *   inside the Frustum but
 *   completely hidden by walls or opaque geometry. Any open ring point keeps the
 *   Endpoint visible, avoiding
 *   false suspension.
 * - Cadence: accumulated time that determines the interval between Cubemap Endpoint
 *   captures. At a target
 *   rate of 30 Hz, each Endpoint interval is 33.333 ms and the alternating A/B Callback
 *   interval is half that.
 * - Submission: requesting actual capture render work from UWPCaptureManager. The
 *   Scheduler selects timing
 *   and A/B scope; the Manager creates and destroys Render Targets and Components.
 * - Authority: the state and Epoch contract under which the Runtime Manager, rather
 *   than an Actor, may
 *   execute capture for the Pair. Authority transitions to RuntimeOnly after warmup
 *   succeeds; repeated
 *   failures trigger Manager-state recovery.
 *
 * Typical single-Frame sequence:
 * 1. UWPRuntimeSubsystem::Tick selects one Reference Camera Snapshot.
 * 2. UpdateVisibilityAndOcclusion updates the camera-inside exclusive Pair and
 *    CPU-occlusion state.
 * 3. After Actor Movement, RunCaptureScheduler evaluates the same Pair Map from
 *    OnWorldPostActorTick.
 * 4. Visibility 2 keeps A/B compositing and capture active. Visibility 1 composites
 *    only the visible side
 *    while continuing to update both Cubemaps. Visibility 0 or complete occlusion of
 *    both sides stops A/B
 *    capture after the hold period.
 * 5. While stopped, the VRAM-first resolution policy may release the current large
 *    Cubemaps and retain only
 *    the 64 tier. Replacement allocation is serialized and release-first; the Wormhole
 *    may disappear while
 *    RenderPublication drops old references and the asynchronous resources become ready.
 *
 * If the Camera is inside a SafeProxy, only that Pair is updated and all other Pairs
 * stop. When SafeProxies
 * overlap, the Pair with the smallest deterministic PairId-based Sort Key is selected
 * so results do not vary
 * between runs. An ambiguous Reference View or Feedback state fails open to favor
 * visual correctness over
 * performance.
 */
class FWPRuntimeCaptureScheduler final
{
public:
	/**
	 * Connects the execution context owned by RuntimeSubsystem.
	 * Every referenced argument must outlive the Scheduler; no null substitute objects are
	 * created.
	 * @param InRuntime Owner that provides the World, logging context, and Reference View
	 *                  forwarding.
	 * @param InCaptureManager Manager reference that owns Endpoint resources and actual
	 *                         capture submission.
	 * @param InPairStates Shared Map storing capture, visibility, and cadence state by
	 *                     Registry PairId.
	 * @param bInRenderPacketPipelineActive Packet-pipeline state evaluated together with
	 *                                      capture-mode policy.
	 * @param bInPairsDirty Shared flag used to request Registry reconciliation after an
	 *                      invalid Endpoint is found.
	 */
	FWPRuntimeCaptureScheduler(
		UWPRuntimeSubsystem& InRuntime,
		TObjectPtr<UWPCaptureManager>& InCaptureManager,
		TMap<FGuid, FWPPortalPairState>& InPairStates,
		bool& bInRenderPacketPipelineActive,
		bool& bInPairsDirty);

	/**
	 * Applies the initial effective mode and schedules mode diagnostics for the first
	 * PostActorTick.
	 * @param bInitialRuntimeActive Manager-owned capture activation state computed from the
	 *                              CVar at Initialize time.
	 */
	void Initialize(bool bInitialRuntimeActive);

	/** Resets the exclusive Pair and mode cache during World shutdown without releasing resources. */
	void Reset();

	/**
	 * Entry point for FWorldDelegates::OnWorldPostActorTick.
	 * Runs the Scheduler once using Camera and Portal Transforms after Actor Movement has
	 * completed.
	 * @param World World that emitted the Callback. Ignored if it differs from the Owner
	 *              World.
	 * @param TickType Level Tick type supplied by the Engine.
	 * @param DeltaSeconds Frame Delta, clamped for hitches before being applied to cadence.
	 */
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);

	/**
	 * Sequentially updates camera-inside Pair selection and occlusion from the same
	 * Reference View Snapshot
	 * supplied by the Runtime Tick. If the Reference View is unavailable or invalid,
	 * remains fail-open to avoid
	 * stopping capture incorrectly.
	 * @param CameraLocation World-space location of the selected Reference Camera.
	 * @param ReferenceViewActor Local Player Pawn or View Target, or null if unavailable.
	 * @param bHasReferenceView Whether Camera location, rotation, FOV, and Actor were
	 *                          acquired coherently.
	 * @param NowSeconds Monotonic platform time used for feedback-freshness and
	 *                   trace-interval comparisons.
	 */
	void UpdateVisibilityAndOcclusion(
		const FVector& CameraLocation,
		AActor* ReferenceViewActor,
		bool bHasReferenceView,
		double NowSeconds);

	/** Returns whether the current CVar and Runtime pipeline combination enables the Manager-owned capture path. */
	bool IsRuntimeActive() const { return bRuntimeActive; }

	/** Pair ID whose SafeProxy contains the Camera and blocks capture for other Pairs. An invalid Guid means no Pair is exclusive. */
	const FGuid& GetExclusiveInsidePairId() const { return ExclusiveInsidePairId; }

	/**
	 * Returns a stable Capture Authority name for logging and diagnostics.
	 * @param Authority Enum value to convert to a string.
	 * @return Static string that must not be freed by the caller.
	 */
	static const TCHAR* GetCaptureAuthorityName(EWPCaptureAuthority Authority);

private:
	/**
	 * Selects the one Camera-containing SafeProxy candidate with the smallest deterministic
	 * Sort Key.
	 * @param CameraLocation World-space location tested against each Proxy.
	 * @param bHasReferenceView If false, clears the existing exclusive selection and fails
	 *                          every Pair open.
	 */
	void UpdateCaptureInsidePairSelection(const FVector& CameraLocation, bool bHasReferenceView);

	/**
	 * Traces the eight SafeProxy ring points for each Endpoint and updates the Pair-level
	 * blocked state.
	 * @param CameraLocation Start location for every line trace.
	 * @param ReferenceViewActor Current Camera-owning Actor to ignore in the traces.
	 * @param bHasReferenceView If false, trace results cannot produce a stop decision.
	 * @param NowSeconds Time basis used to update each Pair's trace cadence and result
	 *                   timestamp.
	 */
	void UpdateCaptureOcclusionStates(
		const FVector& CameraLocation,
		AActor* ReferenceViewActor,
		bool bHasReferenceView,
		double NowSeconds);

	/**
	 * Evaluates visibility, cadence, and warmup state for every Pair and selects
	 * submissions for this Callback.
	 * @param DeltaSeconds PostActorTick Delta, clamped to a safe range before accumulation
	 *                     into Pair cadence.
	 */
	void RunCaptureScheduler(float DeltaSeconds);

	/**
	 * Computes conservative SafeProxy screen size, applies VRAM-budget hysteresis, and
	 * advances the release-first asynchronous Pair resolution state. Returns true only
	 * while both Endpoint resources are ready at the committed resolution.
	 */
	bool UpdatePairCaptureResolution(
		FWPPortalPairState& PairState,
		const FVector& CameraLocation,
		float CameraFOVDegrees,
		bool bHasReferenceView,
		double DeltaSeconds,
		double NowSeconds);

	/**
	 * Applies the same authority/epoch transition to the Pair state and CaptureManager.
	 * @param PairState Mutable Runtime state for the Registry Pair to transition.
	 * @param NewAuthority Target state, either RuntimeWarmup or RuntimeOnly.
	 * @param Reason Diagnostic string describing the transition cause.
	 */
	void SetPairCaptureAuthority(
		FWPPortalPairState& PairState,
		EWPCaptureAuthority NewAuthority,
		const TCHAR* Reason);

	/**
	 * Re-establishes Endpoint resources and Manager authority after consecutive submission
	 * failures.
	 * @param PairState Pair state to recover.
	 * @param Reason Failure condition that triggered recovery.
	 */
	void RecoverPairCaptureAuthority(FWPPortalPairState& PairState, const TCHAR* Reason);

	/**
	 * Performs one CaptureManager submission in the selected atomic or staggered mode.
	 * Successful Endpoint Generations and the Pair Epoch are copied directly from the
	 * CaptureManager result.
	 * @param PairState Target Pair and its cadence/epoch state.
	 * @param SchedulerElapsedSeconds Safe Scheduler time accumulated through the current
	 *                                evaluation.
	 * @param SubmissionMode Atomic A/B submission or staggered single-Endpoint scope.
	 * @param OutCaptureCpuMs CPU time in milliseconds spent in the Manager Callback,
	 *                        whether it succeeds or fails.
	 * @return true if the Manager successfully submitted every requested Endpoint.
	 */
	bool ExecuteRuntimePairCapture(
		FWPPortalPairState& PairState,
		float SchedulerElapsedSeconds,
		EWPManagedCaptureSubmissionMode SubmissionMode,
		double& OutCaptureCpuMs,
		uint8 SelectedFaceMask = 0x3f);

	/** Owner bridge that returns the RuntimeSubsystem's World. */
	UWorld* GetWorld() const;

	/** Owner bridge that returns whether the Owner Subsystem is still in a valid execution phase. */
	bool IsInitialized() const;

	/**
	 * Owner bridge used to share the same Reference View selected by RenderPublication.
	 * @param OutCameraLocation World-space Camera location on success.
	 * @param OutCameraRotation World-space Camera rotation on success.
	 * @param OutViewActor Local Pawn or View Target on success.
	 * @param OutCameraFOVDegrees Camera FOV on success.
	 * @return true if the Owner found one coherent Local Player View.
	 */
	bool ResolveReferenceView(
		FVector& OutCameraLocation,
		FRotator& OutCameraRotation,
		AActor*& OutViewActor,
		float& OutCameraFOVDegrees) const;

	/** Owner that provides the logging context, World, and Reference View helper. */
	UWPRuntimeSubsystem& Runtime;
	/** Non-owning reference to the Manager that owns Endpoint Cubemaps and Pair capture state. */
	TObjectPtr<UWPCaptureManager>& CaptureManager;
	/** Non-owning reference to the RuntimeSubsystem Pair Map keyed by Registry PairId. */
	TMap<FGuid, FWPPortalPairState>& PairStates;
	/** Shared state reference used to evaluate Scheduler mode together with RenderPacket pipeline state. */
	bool& bRenderPacketPipelineActive;
	/** Shared dirty flag used by the Scheduler to request Registry reconciliation after finding an invalid Endpoint. */
	bool& bPairsDirty;

	/** ID of the Pair that exclusively owns capture while the Camera is inside its SafeProxy. */
	FGuid ExclusiveInsidePairId;
	/** Only one Pair may allocate or release large Cubemap resources at a time. */
	FGuid ActiveResolutionTransitionPairId;
	/** Whether the current effective Scheduler policy executes Runtime capture. */
	bool bRuntimeActive = false;
	/** Raw-value cache used to log the CVar mode once initially and only when it changes thereafter. */
	int32 LastModeRaw = MIN_int32;
};
