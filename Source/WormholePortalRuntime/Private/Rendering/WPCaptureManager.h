// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "Transit/WPTransitTypes.h"
#include "UObject/Object.h"
#include "WPCaptureManager.generated.h"

class AActor;
class AWormholePortalActor;
class FRenderCommandFence; class USceneCaptureComponentCube;
class UTextureRenderTargetCube;
class UWorld;

/** Snapshot of Endpoint resources that the Runtime and Renderer can use without reading Actor internals. */
struct WORMHOLEPORTALRUNTIME_API FWPCaptureEndpointSnapshot
{
	TWeakObjectPtr<USceneCaptureComponentCube> CaptureComponent;
	/** Current physical Endpoint Cube used by SceneCapture; it preserves the previous AA result. */
	TWeakObjectPtr<UTextureRenderTargetCube> CaptureTarget;
	/** Published Texture Reference owner whose UObject address remains stable. */
	TWeakObjectPtr<UTextureRenderTargetCube> RenderTarget;
	FWPCubeContract CubeContract;
	uint32 CaptureGeneration = 0;
	uint64 ResourceEpoch = 0;
	bool bCubeAADirectPublish = true;
	/** Cubemap slices initialized in this resource generation. Publication must not expose an incomplete first fill. */
	uint8 InitialValidFaceMask = 0;

	bool IsReadyForSubmission(const UWorld* ExpectedWorld) const;
};

enum class EWPCaptureTransitPhase : uint8
{
	Started,
	Committed,
	Cancelled
};

/** Endpoint scope that the Manager submits in a single Callback. */
enum class EWPManagedCaptureSubmissionMode : uint8
{
	AtomicPair,
	EndpointA,
	EndpointB
};

/** Result of one Manager-owned submission. CaptureGeneration advances by exactly one for each selected Endpoint that succeeds. */
struct WORMHOLEPORTALRUNTIME_API FWPManagedPairCaptureResult
{
	EWPManagedCaptureSubmissionMode SubmissionMode =
		EWPManagedCaptureSubmissionMode::AtomicPair;
	uint32 CaptureGenerationABefore = 0;
	uint32 CaptureGenerationAAfter = 0;
	uint32 CaptureGenerationBBefore = 0;
	uint32 CaptureGenerationBAfter = 0;
	uint64 PairCaptureEpochBefore = 0;
	uint64 PairCaptureEpochAfter = 0;
	bool bSubmittedA = false;
	bool bSubmittedB = false;
	uint8 SubmittedFaceMaskA = 0;
	uint8 SubmittedFaceMaskB = 0;
	bool bPairEpochCoherent = false;
	bool bPairCycleCompleted = false;
	bool bUsedTransitSynchronizedFirstPerson = false;
	bool bCameraAvailable = false;
	uint8 StaggeredCompletionMaskBefore = 0;
	uint8 StaggeredCompletionMaskAfter = 0;
	uint64 PreviousSubmissionFrameA = MAX_uint64;
	uint64 PreviousSubmissionFrameB = MAX_uint64;
	uint64 SubmissionFrameA = MAX_uint64;
	uint64 SubmissionFrameB = MAX_uint64;
	// Logging only: records per-stage Game Thread CPU cost for the Pair Capture.
	double TransformCpuMs = 0.0;
	double SubmitCpuMs = 0.0;
	double TotalCpuMs = 0.0;

	bool WasSuccessful() const;
	int32 GetSubmittedEndpointCount() const;
	int32 GetSubmittedFaceCount() const;
};

/**
 * World-scoped owner of Cube Capture resources.
 *
 * Portal Actors do not retain Capture Components, Targets, or execution state. This
 * Manager exclusively
 * manages creation, strong references, and explicit release ordering for the production
 * SceneCapture
 * Component, two physical Cubes per Endpoint, and the stable-address Published Texture
 * Reference owner.
 * Endpoint resources must survive Pair unlinking, so resources are keyed by Endpoint
 * Actor rather than
 * PairId. Pair Scheduler ownership and UObject resource ownership are independent.
 */
UCLASS(Transient)
class WORMHOLEPORTALRUNTIME_API UWPCaptureManager : public UObject
{
	GENERATED_BODY()

public:
	/** UObject logging/world helpers must resolve to the same World whose endpoint resources we own. */
	virtual UWorld* GetWorld() const override;

	void Initialize(UWorld* InWorld);
	void Shutdown(const TCHAR* Reason);

	/**
	 * Advances at most one persistent Cubemap allocation stage for this Endpoint.
	 * Returns true only after both AA ping-pong Cubes and the manual Capture Component are
	 * fully ready at DesiredResolution. It never flushes the Render Thread.
	 */
	bool EnsureEndpointResources(
		AWormholePortalActor* Portal,
		uint32 DesiredResolution);

	/**
	 * Asynchronously prepares a replacement resolution generation while preserving the
	 * existing Endpoint record as the frozen Renderer publication. Capture submission uses
	 * the replacement record, but BuildRenderPacket continues to receive the old texture until
	 * ActivatePairResolutionPublication(). Each call creates at most one persistent Cube and
	 * never waits for the Game or Render Thread.
	 */
	bool EnsureEndpointTransitionResources(
		AWormholePortalActor* Portal,
		uint32 DesiredResolution);

	/** Detaches the Manager Component's Target and registration before removing GC-traced strong references. */
	bool ReleaseEndpointResources(AWormholePortalActor* Portal, const TCHAR* Reason);

	/** Releases only Endpoint render resources while preserving Pair authority/Transit state. */
	bool ReleaseEndpointResourcesForResolutionChange(
		AWormholePortalActor* Portal,
		const TCHAR* Reason);

	bool HasEndpointResources(const AWormholePortalActor* Portal) const;
	bool HasEndpointResources(
		const AWormholePortalActor* Portal,
		uint32 ExpectedResolution) const;
	uint32 GetEndpointCaptureResolution(const AWormholePortalActor* Portal) const;
	double GetEndpointEstimatedColorMemoryMiB(
		const AWormholePortalActor* Portal) const;
	double GetEstimatedPairColorMemoryMiB(uint32 Resolution) const;
	bool GetEndpointSnapshot(
		const AWormholePortalActor* Portal,
		FWPCaptureEndpointSnapshot& OutSnapshot) const;
	/** Renderer-packet snapshot; returns the frozen old generation during a seamless transition. */
	bool GetPublishedEndpointSnapshot(
		const AWormholePortalActor* Portal,
		FWPCaptureEndpointSnapshot& OutSnapshot) const;
	UTextureRenderTargetCube* GetEndpointRenderTarget(const AWormholePortalActor* Portal) const;
	uint32 GetEndpointCaptureGeneration(const AWormholePortalActor* Portal) const;

	/** Exposes replacement A/B to the Renderer at one publication boundary without releasing old A/B. */
	bool ActivatePairResolutionPublication(
		AWormholePortalActor* PortalA,
		AWormholePortalActor* PortalB,
		const TCHAR* Reason);
	/** Releases frozen old A/B only after the Render fence following the replacement packet completes. */
	bool ReleaseRetiredPairResolutionResources(
		AWormholePortalActor* PortalA,
		AWormholePortalActor* PortalB,
		const TCHAR* Reason);
	/** Discards replacement generations and restores frozen old records after allocation or capture failure. */
	bool CancelPairResolutionTransition(
		AWormholePortalActor* PortalA,
		AWormholePortalActor* PortalB,
		const TCHAR* Reason);
	/** Resets the steady-state stagger mask before the replacement A-then-B warmup sequence. */
	bool ResetPairCaptureCycleForResolutionTransition(
		const FGuid& PairId,
		const TCHAR* Reason);

	/** Enables or disables Capture execution authority for a Registry PairId lifetime. */
	bool SetPairCaptureAuthority(
		const FGuid& PairId,
		AWormholePortalActor* PortalA,
		AWormholePortalActor* PortalB,
		uint64 OwnershipEpoch,
		bool bEnabled,
		const TCHAR* Reason);

	/** Receives each Pair's first-person parallax snapshot from the Runtime's single Transit Delegate. */
	bool ApplyPairTransitEvent(
		const FGuid& PairId,
		const FWPTransitEvent& Event,
		EWPCaptureTransitPhase Phase);

	/** Returns whether the Pair must wait until the next Engine Frame to avoid using a stale PlayerCameraManager from the Commit Event Frame. */
	bool IsPairCommitWaitingForFreshCamera(const FGuid& PairId) const;

	/**
	 * Computes both Endpoint capture locations from the current PlayerCameraManager and the
	 * stored Transit
	 * snapshot. AtomicPair submits both Endpoints consecutively in the same Game Thread
	 * Callback, while
	 * EndpointA or EndpointB submits only one side of the steady-state stagger.
	 * RuntimeSubsystem continues to own cadence and low-FPS atomic fallback policy.
	 * The Transit lifecycle affects position calculation only; it does not force an
	 * additional submission or
	 * an atomic capture.
	 */
	bool SubmitPairCapture(
		const FGuid& PairId,
		AWormholePortalActor* PortalA,
		AWormholePortalActor* PortalB,
		uint64 ExpectedOwnershipEpoch,
		EWPManagedCaptureSubmissionMode SubmissionMode,
		FWPManagedPairCaptureResult& OutResult,
		uint8 SelectedFaceMask = 0x3f);

	/** Immediately clears Camera hysteresis and the Transit snapshot on Pair unlink, relink, or World teardown. */
	bool RemovePairCaptureState(const FGuid& PairId, const TCHAR* Reason);

	int32 GetEndpointCount() const { return EndpointRecords.Num(); }
	int32 GetPairStateCount() const { return PairCaptureStates.Num(); }
	int32 GetStrongCaptureCount() const { return ManagedCaptureComponents.Num(); }
	int32 GetStrongRenderTargetCount() const { return ManagedRenderTargets.Num(); }
	/** Returns allocation and release telemetry used only for logging. */
	uint64 GetAllocationCount() const { return AllocationCount; }
	uint64 GetReleaseCount() const { return ReleaseCount; }
	double GetEstimatedResidentColorMemoryMiB() const;

private:
	struct FEndpointRecord
	{
		TWeakObjectPtr<AWormholePortalActor> Portal;
		TWeakObjectPtr<USceneCaptureComponentCube> CaptureComponent;
		/** Physical Cube used by the next SceneCapture; it preserves the previous AA result. */
		TWeakObjectPtr<UTextureRenderTargetCube> CaptureTarget;
		/** The UObject address remains stable for its lifetime; only its RHI Texture Reference is retargeted to the latest AA physical Cube. */
		TWeakObjectPtr<UTextureRenderTargetCube> RenderTarget;
		/** Physical Cube used as the next AA output in the direct-publish path, then swapped with CaptureTarget after submission. */
		TWeakObjectPtr<UTextureRenderTargetCube> AlternateRenderTarget;
		uint64 ResourceEpoch = 0;
		uint32 ResourceGeneration = 0;
		uint32 CaptureGeneration = 0;
		uint32 CaptureResolution = 0;
		uint8 InitialValidFaceMask = 0;
		bool bCubeAADirectPublish = true;
		/** Logging only: CPU cost of this Endpoint allocation. */
		double AllocationCpuMs = 0.0;
	};

	enum class EEndpointAllocationStage : uint8
	{
		CreateCurrent,
		WaitCurrent,
		CreateAlternate,
		WaitAlternate,
		Commit
	};

	/**
	 * Partial Endpoint allocation retained across frames. Only one Cube is created per
	 * call so large RHI allocations cannot be submitted as one Game Thread burst.
	 */
	struct FPendingEndpointAllocation
	{
		TWeakObjectPtr<AWormholePortalActor> Portal;
		TWeakObjectPtr<UTextureRenderTargetCube> CurrentTarget;
		TWeakObjectPtr<UTextureRenderTargetCube> AlternateTarget;
		uint64 ResourceEpoch = 0;
		uint32 ResourceGeneration = 0;
		uint32 CaptureResolution = 0;
		bool bCubeAADirectPublish = true;
		/** Moves the existing active record into frozen publication ownership when allocation commits. */
		bool bPreserveExistingForResolutionTransition = false;
		EEndpointAllocationStage Stage = EEndpointAllocationStage::CreateCurrent;
		/**
		 * Observes whether the most recently submitted Cube RHI allocation finished on the
		 * Render Thread. The Game Thread never waits or flushes and only polls IsFenceComplete
		 * on a later Tick.
		 */
		TSharedPtr<FRenderCommandFence> AllocationFence;
		double StartSeconds = 0.0;
	};

	enum class EFirstPersonTransitParallaxPhase : uint8
	{
		Idle,
		Crossing,
		CommitPending,
		CancelPending
	};

	struct FPairCaptureState
	{
		FGuid PairId;
		TWeakObjectPtr<AWormholePortalActor> PortalA;
		TWeakObjectPtr<AWormholePortalActor> PortalB;
		uint64 OwnershipEpoch = 0;
		uint64 PairCaptureEpoch = 0;
		uint64 LastSubmissionFrame = MAX_uint64;
		uint64 LastSubmissionFrameA = MAX_uint64;
		uint64 LastSubmissionFrameB = MAX_uint64;
		uint8 StaggeredCompletionMask = 0;
		bool bAuthorityEnabled = false;

		TWeakObjectPtr<AWormholePortalActor> ActiveCameraRelativeSourcePortal;
		EFirstPersonTransitParallaxPhase FirstPersonPhase =
			EFirstPersonTransitParallaxPhase::Idle;
		uint64 FirstPersonSequence = 0;
		uint64 FirstPersonTerminalEventFrame = 0;
		TWeakObjectPtr<AActor> FirstPersonActor;
		TWeakObjectPtr<AWormholePortalActor> FirstPersonSourcePortal;
		TWeakObjectPtr<AWormholePortalActor> FirstPersonDestinationPortal;
		FVector FirstPersonEntryPointWorld = FVector::ZeroVector;
		EWPTransitPlane FirstPersonSelectedPlane = EWPTransitPlane::YZ;
		FWPTransform FirstPersonMapping;
		bool bFirstPersonPathMappingValid = false;
	};

	void ReleaseRecord(FEndpointRecord& Record, const TCHAR* Reason);
	bool EnsureEndpointResourcesInternal(
		AWormholePortalActor* Portal,
		uint32 DesiredResolution,
		bool bPreserveExistingForResolutionTransition);
	bool GetSnapshotFromRecord(
		const FEndpointRecord* Record,
		FWPCaptureEndpointSnapshot& OutSnapshot) const;
	void ReleasePendingAllocation(
		FPendingEndpointAllocation& Pending,
		const TCHAR* Reason);
	UTextureRenderTargetCube* CreateEndpointCubeTarget(
		AWormholePortalActor* Portal,
		const TCHAR* Role,
		uint32 CaptureResolution,
		bool bSupportsUAV);
	bool ValidateEndpointForSubmission(
		const AWormholePortalActor* Portal,
		FEndpointRecord*& OutRecord,
		// Logging only: detailed failure reason to include in the Pair preflight log.
		FString& OutFailureReason);
	bool SubmitEndpointCapture(
		FEndpointRecord& Record,
		AWormholePortalActor* Portal,
		AWormholePortalActor* OtherPortal,
		const FVector& CaptureLocation,
		// Logging only: label identifying the calculated Capture-position policy.
		const TCHAR* PositionMode,
		double& OutTransformCpuMs,
		double& OutSubmitCpuMs,
		uint8 SelectedFaceMask);
	void ResetFirstPersonTransitState(
		FPairCaptureState& PairState,
		const TCHAR* Reason,
		bool bResetActiveSource = true);
	uint32 AllocateResourceGeneration();
	uint64 AllocateResourceEpoch();

private:
	TWeakObjectPtr<UWorld> ManagedWorld;
	TMap<TWeakObjectPtr<AWormholePortalActor>, FEndpointRecord> EndpointRecords;
	/**
	 * Old generations sampled by the Renderer during a resolution transition. Capture submission
	 * uses only the replacement EndpointRecords, so these texture contents remain frozen.
	 */
	TMap<TWeakObjectPtr<AWormholePortalActor>, FEndpointRecord>
		RetiredResolutionEndpointRecords;
	/** Endpoints whose RenderPublication snapshot must continue to use the retired record. */
	TSet<TWeakObjectPtr<AWormholePortalActor>> RetiredPublicationOverrides;
	TMap<TWeakObjectPtr<AWormholePortalActor>, FPendingEndpointAllocation>
		PendingEndpointAllocations;
	TMap<FGuid, FPairCaptureState> PairCaptureStates;

	/** PairState is not a UPROPERTY, so the Manager's GC-traced arrays provide the actual strong ownership. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneCaptureComponentCube>> ManagedCaptureComponents;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTargetCube>> ManagedRenderTargets;

	uint64 NextResourceEpoch = 0;
	uint32 NextResourceGeneration = 0;
	// Logging only: accumulates resource-lifetime balance and allocation/release CPU costs for the shutdown log.
	uint64 AllocationCount = 0;
	uint64 ReleaseCount = 0;
	uint64 AllocationFailureCount = 0;
	// Logging only: accumulates Endpoint Cube lifetime statistics and direct-publish AA savings for the shutdown log.
	uint64 CubeTargetAllocationCount = 0;
	uint64 CubeTargetReleaseCount = 0;
	uint64 CubeAADirectPublishCount = 0;
	uint64 CubeAACopyBackLogicalBytesAvoided = 0;
	double TotalCubeTargetAllocationCpuMs = 0.0;
	double TotalCubeTargetReleaseCpuMs = 0.0;
	double TotalAllocationCpuMs = 0.0;
	double TotalReleaseCpuMs = 0.0;
	bool bInitialized = false;
	bool bShuttingDown = false;
};
