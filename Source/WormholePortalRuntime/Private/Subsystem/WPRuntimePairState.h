// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderCommandFence.h"
#include "Rendering/WPRenderTypes.h"

class AWormholePortalActor;

/** Whether Pair Cubemap submission authority is in RuntimeWarmup or RuntimeOnly state. */
enum class EWPCaptureAuthority : uint8
{
	RuntimeWarmup,
	RuntimeOnly
};

/**
 * Capture-visibility decision state for one Wormhole Pair.
 *
 * Stores Renderer feedback, continuous-invisibility time, the Reference View guard, CPU
 * occlusion,
 * camera-inside exclusive selection, and final pause state on the Game Thread.
 * Owns no UObject or Render Thread resource.
 */
struct FWPCaptureVisibilityState
{
	// Capture may stop only after Renderer feedback reflects a Packet at or above this Sequence floor.
	uint64 RequiredPacketSequence = 0;

	// Accumulated duration for which both Endpoints have remained continuously invisible.
	double InvisibleElapsedSeconds = 0.0;
	// The current production policy is strict stop, so the effective hidden refresh rate is 0 Hz.
	double HiddenRefreshElapsedSeconds = 0.0;

	// Most recently received coherent Renderer visibility sample.
	double LastSampleReceiptSeconds = -1.0e30;
	uint64 LastOwnershipEpoch = 0;
	uint64 LastSampleSequence = 0;
	// Latest coherent Player View frustum bits: bit 0 = A, bit 1 = B. Unknown state fails open to both.
	uint8 LastVisibleEndpointMask = 0x3;

	uint32 GetLastVisibleEndpointCount() const
	{
		return ((LastVisibleEndpointMask & 0x1u) != 0 ? 1u : 0u)
			+ ((LastVisibleEndpointMask & 0x2u) != 0 ? 1u : 0u);
	}

	// Rejection barrier that prevents reuse of a sample discarded after a Camera-guard change or similar event.
	uint64 RejectedOwnershipEpoch = 0;
	uint64 RejectedThroughSampleSequence = 0;

	// Last sample accepted during the current continuous-invisibility interval.
	double LastInvisibleSampleReceiptSeconds = -1.0e30;
	uint64 LastInvisibleOwnershipEpoch = 0;
	uint64 LastInvisibleSampleSequence = 0;

	// Reference View Snapshot stored as the baseline when the invisible hold begins.
	uint32 GuardViewActorId = 0;
	FVector GuardCameraLocation = FVector::ZeroVector;
	FRotator GuardCameraRotation = FRotator::ZeroRotator;
	float GuardCameraFOVDegrees = 0.0f;
	bool bGuardInitialized = false;

	// CPU line-trace occlusion result. Bit 0 represents Endpoint A; bit 1 represents Endpoint B.
	uint8 OcclusionVisibleEndpointMask = 0x3;
	uint8 LastLoggedOcclusionVisibleEndpointMask = 0x3;
	double NextOcclusionTraceSeconds = 0.0;
	bool bOcclusionValid = false;
	bool bLastLoggedOcclusionValid = false;

	// Exclusive Pair selection and blocking state determined by a Camera inside a SafeProxy.
	bool bInsideSelected = false;
	bool bInsideBlocked = false;

	// Final Scheduler state. When bPaused is true, all A/B capture submissions are blocked.
	bool bPaused = false;
	bool bResumePrewarmPending = false;
	bool bAwaitingPostGuardSample = false;
};

/** Registry-defined Pair identity and weak references to the current Endpoints. */
struct FWPPortalPairIdentityState
{
	FGuid PairId;
	TWeakObjectPtr<AWormholePortalActor> PortalA;
	TWeakObjectPtr<AWormholePortalActor> PortalB;
	FName StableSelectorNameA = NAME_None;
	FName StableSelectorNameB = NAME_None;
};

/** Registration-handle, ownership-handshake, and feedback state shared between the Runtime and Renderer. */
struct FWPPairOwnershipState
{
	FWPRenderHandle RenderHandle;
	EWPPairOwnershipMode RequestedOwnership = EWPPairOwnershipMode::Disabled;
	EWPPairOwnershipMode EffectiveOwnership = EWPPairOwnershipMode::Disabled;
	uint64 OwnershipEpoch = 0;
	FWPCubeContract LastOwnershipCubeContractA;
	FWPCubeContract LastOwnershipCubeContractB;
	FWPRayLUTContract LastOwnershipRayLUTContractA;
	FWPRayLUTContract LastOwnershipRayLUTContractB;
	/** Dimensionless metric-shape identities; absolute uniform scale is not an ownership resource change. */
	uint32 LastOwnershipMetricResourceIdentityRevisionA = 0;
	uint32 LastOwnershipMetricResourceIdentityRevisionB = 0;
	FTransform LastOwnershipTransformA = FTransform::Identity;
	FTransform LastOwnershipTransformB = FTransform::Identity;
	FWPRenderHandle LastOwnershipRenderHandle;
	FWPPairOwnershipFeedback LastOwnershipFeedback;
	uint64 LastRejectedWarmupAckEpoch = 0;
	uint64 LastRejectedWarmupAckCurrentEpoch = 0;
	uint64 LastRejectedWarmupAckFailureEpoch = 0;
	bool bOwnershipObservationInitialized = false;
	bool bOwnershipResourceIdentityInitialized = false;
	bool bLastOwnershipInputsReady = false;
	bool bOwnershipEndpointAReady = false;
	bool bOwnershipEndpointBReady = false;
	bool bOwnershipInputsReady = false;
};

/** Transit state describing which region of the current Pair contains the Reference Camera or Player. */
struct FWPPairTransitState
{
	EWPSide CurrentSide = EWPSide::None;
	EWPSide EntrySide = EWPSide::None;
	EWPRegion Region = EWPRegion::Flat;
	float SignedEllCm = 0.0f;
	float TransitionAlpha = 1.0f;
	uint32 TransitActorId = 0;
	uint64 LastTransitEventSequence = 0;
	bool bTransitActive = false;
};

/** Game Thread state used as the last-successful-Packet baseline for duplicate-publication comparisons. */
struct FWPPairPublicationState
{
	uint64 PacketSequence = 0;
	uint32 LastPublishedCaptureGenerationA = 0;
	uint32 LastPublishedCaptureGenerationB = 0;
	uint32 LastObservedCaptureGenerationA = 0;
	uint32 LastObservedCaptureGenerationB = 0;
	uint32 LastPublishedMetricRevisionA = 0;
	uint32 LastPublishedMetricRevisionB = 0;
	FWPCubeContract LastPublishedCubeContractA;
	FWPCubeContract LastPublishedCubeContractB;
	FWPRayLUTContract LastPublishedRayLUTContractA;
	FWPRayLUTContract LastPublishedRayLUTContractB;
	uint32 LastPublishedRayLUTRevisionA = 0;
	uint32 LastPublishedRayLUTRevisionB = 0;
	uint32 LastPublishedReferenceViewActorId = 0;
	uint64 LastPublishedTransitEventSequence = 0;
	uint64 LastPublishedOwnershipEpoch = 0;
	EWPPairOwnershipMode LastPublishedRequestedOwnership = EWPPairOwnershipMode::Disabled;
	EWPPairOwnershipMode LastPublishedEffectiveOwnership = EWPPairOwnershipMode::Disabled;
	FName LastPublishedStableSelectorNameA = NAME_None;
	FName LastPublishedStableSelectorNameB = NAME_None;
	FTransform LastPublishedTransformA = FTransform::Identity;
	FTransform LastPublishedTransformB = FTransform::Identity;
	FVector LastPublishedCameraLocation = FVector::ZeroVector;
	double LastPublishSeconds = 0.0;
	bool bHasPublished = false;
	bool bCaptureGenerationObservationInitialized = false;
	bool bLastPublishedResourcesReady = false;
	bool bLastPublishedCaptureReady = false;
	bool bLastPublishedMetricCompatible = false;
	bool bLastPublishedScaleSupported = false;
	bool bLastPublishedTransitActive = false;
	bool bLastPublishedOwnershipEndpointAReady = false;
	bool bLastPublishedOwnershipEndpointBReady = false;
	bool bLastPublishedOwnershipInputsReady = false;
	bool bLastPublishedCaptureVisibilityFeedbackEnabled = false;
	bool bDirty = true;
};

/** State that tracks Resource/Metric/Scale contract transitions and suppresses repeated warnings. */
struct FWPPairValidationState
{
	bool bMetricMismatchLogged = false;
	bool bValidationStateInitialized = false;
	bool bLastResourcesReady = false;
	bool bLastMetricCompatible = false;
	bool bLastCaptureReady = false;
	bool bLastScaleSupported = false;
};

/**
 * Dynamic Cubemap resolution transition phases for one Pair.
 *
 * A visible Pair freezes capture updates for the existing Cubemaps and keeps publishing their
 * last textures. Replacement generations are allocated and captured A-then-B, switched at one
 * publication boundary, and the old textures are released only after a Renderer fence. Hidden
 * Pairs may still use the release-first path to reclaim resources that cannot be seen. Visible
 * transitions never downgrade a requested tier or select release-first because of a memory
 * budget. Neither path synchronously waits for the Game or Render Thread.
 */
enum class EWPCaptureResolutionTransitionPhase : uint8
{
	Stable,
	AwaitingUnavailablePublication,
	WaitingForReleaseFence,
	AllocatingEndpointA,
	WaitingForEndpointA,
	AllocatingEndpointB,
	WaitingForEndpointB,
	SeamlessAllocatingEndpointA,
	SeamlessWaitingForEndpointA,
	SeamlessAllocatingEndpointB,
	SeamlessWaitingForEndpointB,
	SeamlessCapturingEndpointA,
	SeamlessCapturingEndpointB,
	AwaitingSeamlessPublication,
	WaitingForRetiredReleaseFence
};

/** Screen-size decision, hysteresis, and non-blocking seamless/release-first state for one Pair. */
struct FWPCaptureResolutionState
{
	/** Resolution currently committed for both Endpoint records; zero means unavailable. */
	uint32 CurrentResolution = 0;
	/** Resolution selected by the latest screen-size evaluation. */
	uint32 DesiredResolution = 64;
	/** Candidate that must remain stable for the upgrade/downgrade hold time. */
	uint32 CandidateResolution = 64;
	/** Continuous time for which CandidateResolution has remained unchanged. */
	double CandidateElapsedSeconds = 0.0;
	/** Monotonic time when the previous transition completed. */
	double LastTransitionCompleteSeconds = -1.0e30;
	/** Monotonic time when the active transition started, used only for diagnostics. */
	double TransitionStartSeconds = 0.0;
	/** Last conservative SafeProxy diameter divided by the vertical view size. */
	double LastScreenDiameterRatio = 0.0;
	/** Predicted steady-state Pair color allocation for diagnostics only. */
	double LastPredictedPairColorMiB = 0.0;
	/** Predicted peak color VRAM while seamless transition holds old and replacement generations. */
	double LastPredictedTransitionPeakMiB = 0.0;
	/** True while the seamless path keeps old textures as the frozen publication. */
	bool bSeamlessTransition = false;
	/** Replacement resource generations used to confirm that the new A/B packet was published. */
	uint32 SeamlessResourceGenerationA = 0;
	uint32 SeamlessResourceGenerationB = 0;
	/** Consecutive replacement warmup failures used for diagnostics and rollback protection. */
	uint32 SeamlessCaptureFailureCount = 0;
	EWPCaptureResolutionTransitionPhase Phase =
		EWPCaptureResolutionTransitionPhase::Stable;
	/**
	 * True when low-resolution resources for a hidden Pair are ready but their first Cubemap
	 * captures have not started. Allocating the hidden tier must not trigger RuntimeWarmup or
	 * a forced A+B capture while the Pair remains off-screen. The flag therefore remains set
	 * until current Renderer visibility feedback reports an Endpoint visible, at which point
	 * warmup may fill the replacement A/B Cubemaps.
	 */
	bool bWarmupDeferredUntilVisible = false;
	/** Fence begun only after an unavailable RenderPacket has been submitted. */
	TUniquePtr<FRenderCommandFence> ReleaseFence;
};

/** One reference view's current and previous LUT-guided Cubemap face prediction. */
struct FWPFacePredictionViewState
{
	uint8 CurrentLocalMask = 0x3f;
	uint8 CurrentLinkedMask = 0x3f;
	uint8 PreviousLocalMask = 0;
	uint8 PreviousLinkedMask = 0;
	double NextDueSeconds = 0.0;
};

/**
 * Game Thread state for 25-Ray selective Cubemap capture.
 * View A and View B are evaluated on different frames. Required masks union current and previous predictions so a
 * moving camera cannot evict a recently required face before the replacement prediction is available.
 */
struct FWPFacePredictionState
{
	FWPFacePredictionViewState ViewA;
	FWPFacePredictionViewState ViewB;
	uint8 RequiredFaceMaskA = 0x3f;
	uint8 RequiredFaceMaskB = 0x3f;
	bool bNextEndpointA = true;
};

/** Runtime capture-authority, cadence, visibility, resolution, and selective-face state for the Pair Cubemap. */
struct FWPPairCaptureState
{
	EWPCaptureAuthority Authority = EWPCaptureAuthority::RuntimeWarmup;
	uint64 OwnershipEpoch = 0;
	uint64 AuthorityTransitionFrame = 0;
	uint64 LastSuccessfulSubmissionFrame = 0;
	uint64 LastFailedSubmissionFrame = 0;
	uint32 ConsecutiveFailureCount = 0;
	double CadenceElapsedSeconds = 0.0;
	FWPCaptureVisibilityState Visibility;
	FWPCaptureResolutionState Resolution;
	FWPFacePredictionState FacePrediction;
	bool bNextStaggeredEndpointA = true;
};

/** Aggregate of role-specific substates that represents one Pair's complete Runtime lifecycle. */
struct FWPPortalPairState
{
	FWPPortalPairIdentityState Identity;
	FWPPairOwnershipState Ownership;
	FWPPairTransitState Transit;
	FWPPairPublicationState Publication;
	FWPPairValidationState Validation;
	FWPPairCaptureState Capture;
};

/** Decision and diagnostic information for whether to publish one Packet to the Renderer. */
struct FWPPublishDecision
{
	bool bShouldPublish = false;
	bool bDirty = false;
	bool bInitial = false;
	bool bResourceReadinessChanged = false;
	bool bCaptureReadinessChanged = false;
	bool bCaptureReadinessChangedA = false;
	bool bCaptureReadinessChangedB = false;
	bool bMetricChanged = false;
	bool bCubeContractChanged = false;
	bool bLUTContractChanged = false;
	bool bTransformChanged = false;
	bool bTransitChanged = false;
	bool bOwnershipChanged = false;
	bool bHeartbeat = false;
	bool bCameraDiagnosticCoalesced = false;
	bool bCaptureDiagnosticCoalesced = false;
	uint64 CoalescedCaptureGenerationAdvanceCount = 0;
	double CpuMs = 0.0;
};
