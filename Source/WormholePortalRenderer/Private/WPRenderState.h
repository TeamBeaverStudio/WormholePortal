// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "HAL/PlatformProcess.h"
#include "RHIResources.h"
#include "RenderingThread.h"

/** First deterministic reason a pair cannot be used as a mask-raster candidate on the Render Thread. */
enum class EWPEligibilityReason : uint8
{
	Eligible,
	Disabled,
	InvalidPairOrTransform,
	MetricInvalid,
	MetricMismatch,
	UnsupportedScale,
	CaptureNotSubmitted,
	MissingTextureReference,
	UnresolvedReferencedTexture,
	WrongTextureDimension,
	Count
};

inline const TCHAR* GetWPEligibilityReasonName(const EWPEligibilityReason Reason)
{
	switch (Reason)
	{
	case EWPEligibilityReason::Eligible: return TEXT("Eligible");
	case EWPEligibilityReason::Disabled: return TEXT("Disabled");
	case EWPEligibilityReason::InvalidPairOrTransform: return TEXT("InvalidPairOrTransform");
	case EWPEligibilityReason::MetricInvalid: return TEXT("MetricInvalid");
	case EWPEligibilityReason::MetricMismatch: return TEXT("MetricMismatch");
	case EWPEligibilityReason::UnsupportedScale: return TEXT("UnsupportedScale");
	case EWPEligibilityReason::CaptureNotSubmitted: return TEXT("CaptureNotSubmitted");
	case EWPEligibilityReason::MissingTextureReference: return TEXT("MissingTextureReference");
	case EWPEligibilityReason::UnresolvedReferencedTexture: return TEXT("UnresolvedReferencedTexture");
	case EWPEligibilityReason::WrongTextureDimension: return TEXT("WrongTextureDimension");
	default: return TEXT("Unknown");
	}
}

/**
 * RT-to-GT ownership-feedback mailbox owned by a single registration handle.
 * The GT service entry and RT packet jointly retain its lifetime through a thread-safe
 * shared pointer.
 * Its atomic-only payload allows SceneViewExtension callbacks to record feedback
 * without a lock.
 */
struct FWPPairOwnershipFeedbackState
{
	FORCEINLINE void RecordWarmupPass_RenderThread(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const bool bAcknowledge)
	{
		check(IsInRenderingThread());
		RecordWarmupPassInternal(Epoch, PacketSequence, bAcknowledge);
	}

	FORCEINLINE void ReportProductionFailure_RenderThread(
		const uint64 Epoch,
		const uint64 PacketSequence)
	{
		check(IsInRenderingThread());
		ReportProductionFailureInternal(Epoch, PacketSequence);
	}

	FORCEINLINE void RecordVisibilitySample_RenderThread(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const uint32 RenderFrameNumber,
		const uint8 InVisibleEndpointMask)
	{
		check(IsInRenderingThread());
		AccumulateVisibilitySampleForRenderFrameInternal(
			Epoch, PacketSequence, RenderFrameNumber, InVisibleEndpointMask);
	}

	FORCEINLINE FWPPairOwnershipFeedback ReadGameThread(
		const bool bIncludeVisibility = true) const
	{
		return ReadGameThreadInternal(
			[](const uint32) {},
			bIncludeVisibility);
	}

	/** Deterministically reproduces a torn read in automation by interleaving the producer between field reads. */
#if WITH_DEV_AUTOMATION_TESTS
	FORCEINLINE void RecordWarmupPassForTest(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const bool bAcknowledge)
	{
		RecordWarmupPassInternal(Epoch, PacketSequence, bAcknowledge);
	}

	FORCEINLINE void ReportProductionFailureForTest(
		const uint64 Epoch,
		const uint64 PacketSequence)
	{
		ReportProductionFailureInternal(Epoch, PacketSequence);
	}

	FORCEINLINE void RecordVisibilitySampleForTest(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const uint8 InVisibleEndpointMask)
	{
		RecordVisibilitySampleInternal(
			Epoch, PacketSequence, InVisibleEndpointMask);
	}

	/**
	 * Reproduces multiple accepted view families recorded in the same render frame for
	 * automation tests.
	 * The first observation only creates pending state. The first observation of the next
	 * frame publishes
	 * the previous frame's endpoint-mask union to the mailbox exactly once.
	 */
	FORCEINLINE void AccumulateVisibilitySampleForRenderFrameForTest(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const uint32 RenderFrameNumber,
		const uint8 InVisibleEndpointMask)
	{
		AccumulateVisibilitySampleForRenderFrameInternal(
			Epoch, PacketSequence, RenderFrameNumber, InVisibleEndpointMask);
	}

	FORCEINLINE void SetFeedbackSnapshotVersionForTest(const uint64 Version)
	{
		FeedbackSnapshotVersion.Store(Version);
	}

	/** Reproduces the odd-version partial-write window after the real failure writer clears the ACK and before it publishes the failure tuple. */
	FORCEINLINE void BeginProductionFailurePartialWriteForTest()
	{
		BeginFeedbackWrite_RenderThread();
		++ProductionFailureCount;
		WarmupSucceededEpoch.Store(0);
		WarmupSucceededPacketSequence.Store(0);
	}

	FORCEINLINE void CompleteProductionFailurePartialWriteForTest(
		const uint64 Epoch,
		const uint64 PacketSequence)
	{
		if (Epoch != 0)
		{
			ProductionFailedPacketSequence.Store(PacketSequence);
			ProductionFailedEpoch.Store(Epoch);
		}
		EndFeedbackWrite_RenderThread();
	}

	FORCEINLINE static uint32 GetMaxFeedbackSnapshotReadRetriesForTest()
	{
		return MaxFeedbackSnapshotReadRetries;
	}

	FORCEINLINE FWPPairOwnershipFeedback ReadGameThreadWithInterleaveForTest(
		const TFunctionRef<void(uint32)> InterleaveHook,
		const bool bIncludeVisibility = true) const
	{
		return ReadGameThreadInternal(InterleaveHook, bIncludeVisibility);
	}
#endif

	FORCEINLINE static bool IsStableFeedbackSnapshotVersion(
		const uint64 StartVersion,
		const uint64 EndVersion)
	{
		return StartVersion == EndVersion && (EndVersion & 1ull) == 0;
	}

private:
	static constexpr uint32 FeedbackSnapshotYieldInterval = 32;
	static constexpr uint32 MaxFeedbackSnapshotReadRetries = 128;

	FORCEINLINE void RecordWarmupPassInternal(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const bool bAcknowledge)
	{
		BeginFeedbackWrite_RenderThread();
		++WarmupPassCount;
		if (bAcknowledge && Epoch != 0)
		{
			// Warmup failures use the same mailbox for diagnostics, but a later
			// same-epoch ACK proves recovery. The enclosing seqlock makes all four
			// event fields one coherent tuple for the GT reader.
			ProductionFailedEpoch.Store(0);
			ProductionFailedPacketSequence.Store(0);
			WarmupSucceededPacketSequence.Store(PacketSequence);
			WarmupSucceededEpoch.Store(Epoch);
		}
		EndFeedbackWrite_RenderThread();
	}

	FORCEINLINE void ReportProductionFailureInternal(
		const uint64 Epoch,
		const uint64 PacketSequence)
	{
		BeginFeedbackWrite_RenderThread();
		++ProductionFailureCount;
		if (Epoch != 0)
		{
			// A later failure invalidates any same-epoch warmup ACK. The enclosing
			// odd/even version prevents the GT from accepting a tuple sampled while
			// these success/failure fields are transitioning.
			WarmupSucceededEpoch.Store(0);
			WarmupSucceededPacketSequence.Store(0);
			ProductionFailedPacketSequence.Store(PacketSequence);
			ProductionFailedEpoch.Store(Epoch);
		}
		EndFeedbackWrite_RenderThread();
	}

	FORCEINLINE void RecordVisibilitySampleInternal(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const uint8 InVisibleEndpointMask)
	{
		BeginFeedbackWrite_RenderThread();
		VisibilityOwnershipEpoch.Store(Epoch);
		VisibilityPacketSequence.Store(PacketSequence);
		VisibleEndpointMask.Store(static_cast<uint32>(InVisibleEndpointMask & 0x3u));
		++VisibilitySampleSequence;
		EndFeedbackWrite_RenderThread();
	}

	FORCEINLINE void AccumulateVisibilitySampleForRenderFrameInternal(
		const uint64 Epoch,
		const uint64 PacketSequence,
		const uint32 RenderFrameNumber,
		const uint8 InVisibleEndpointMask)
	{
		if (Epoch == 0 || PacketSequence == 0 || RenderFrameNumber == MAX_uint32)
		{
			return;
		}

		const uint8 ClampedVisibleEndpointMask = static_cast<uint8>(
			InVisibleEndpointMask & 0x3u);
		const auto BeginPendingFrame =
			[this, Epoch, PacketSequence, RenderFrameNumber,
				ClampedVisibleEndpointMask]()
		{
			bVisibilityRenderFramePending = true;
			VisibilityPendingOwnershipEpoch = Epoch;
			VisibilityPendingPacketSequence = PacketSequence;
			VisibilityPendingRenderFrameNumber = RenderFrameNumber;
			VisibilityPendingVisibleEndpointMask = ClampedVisibleEndpointMask;
			VisibilityPendingAcceptedFamilyCount = 1;
		};

		if (!bVisibilityRenderFramePending)
		{
			BeginPendingFrame();
			return;
		}

		if (VisibilityPendingOwnershipEpoch != Epoch)
		{
			// An ownership epoch defines a new mailbox domain. The old pending
			// frame is deliberately discarded so GT can only accept current-epoch
			// visibility.
			BeginPendingFrame();
			return;
		}

		if (VisibilityPendingRenderFrameNumber == RenderFrameNumber)
		{
			++VisibilityPendingAcceptedFamilyCount;
			VisibilityPendingVisibleEndpointMask |= ClampedVisibleEndpointMask;
			if (VisibilityPendingPacketSequence != PacketSequence)
			{
				// Multiple packet revisions in one render frame cannot be tied to
				// one exact visibility contract. Publish a conservative visible
				// result so the GT keeps A+B capture active.
				VisibilityPendingPacketSequence = FMath::Max(
					VisibilityPendingPacketSequence, PacketSequence);
				VisibilityPendingVisibleEndpointMask = 0x3;
			}
			return;
		}

		// A completed render frame is published exactly once. Delaying publication
		// until the next frame prevents the first accepted family's partial zero
		// from being observed before another accepted family contributes visibility.
		RecordVisibilitySampleInternal(
			VisibilityPendingOwnershipEpoch,
			VisibilityPendingPacketSequence,
			VisibilityPendingVisibleEndpointMask);
		BeginPendingFrame();
	}

	FORCEINLINE void BeginFeedbackWrite_RenderThread()
	{
		const uint64 WriteVersion = ++FeedbackSnapshotVersion;
		check((WriteVersion & 1ull) != 0);
	}

	FORCEINLINE void EndFeedbackWrite_RenderThread()
	{
		const uint64 PublishedVersion = ++FeedbackSnapshotVersion;
		check((PublishedVersion & 1ull) == 0);
	}

	FORCEINLINE static bool RetryFeedbackSnapshotReadGameThread(uint32& InOutRetryCount)
	{
		++InOutRetryCount;
		if (InOutRetryCount % FeedbackSnapshotYieldInterval == 0)
		{
			FPlatformProcess::YieldThread();
		}
		return InOutRetryCount < MaxFeedbackSnapshotReadRetries;
	}

	FORCEINLINE static FWPPairOwnershipFeedback MakeIncoherentFeedbackSnapshot(
		const uint64 LastObservedVersion,
		const uint32 RetryCount)
	{
		FWPPairOwnershipFeedback Result;
		Result.SnapshotVersion = LastObservedVersion;
		Result.SnapshotReadRetryCount = RetryCount;
		Result.bSnapshotCoherent = false;
		return Result;
	}

	template <typename InterleaveHookType>
	FORCEINLINE FWPPairOwnershipFeedback ReadGameThreadInternal(
		const InterleaveHookType& InterleaveHook,
		const bool bIncludeVisibility) const
	{
		uint32 RetryCount = 0;
		for (;;)
		{
			const uint64 StartVersion = FeedbackSnapshotVersion.Load();
			if ((StartVersion & 1ull) != 0)
			{
				if (!RetryFeedbackSnapshotReadGameThread(RetryCount))
				{
					return MakeIncoherentFeedbackSnapshot(StartVersion, RetryCount);
				}
				continue;
			}

			FWPPairOwnershipFeedback Result;
			Result.WarmupSucceededEpoch = WarmupSucceededEpoch.Load();
			Result.WarmupSucceededPacketSequence = WarmupSucceededPacketSequence.Load();
			InterleaveHook(RetryCount);
			Result.ProductionFailedEpoch = ProductionFailedEpoch.Load();
			Result.ProductionFailedPacketSequence = ProductionFailedPacketSequence.Load();
			Result.WarmupPassCount = WarmupPassCount.Load();
			Result.ProductionFailureCount = ProductionFailureCount.Load();
			if (bIncludeVisibility)
			{
				Result.VisibilityOwnershipEpoch = VisibilityOwnershipEpoch.Load();
				Result.VisibilityPacketSequence = VisibilityPacketSequence.Load();
				Result.VisibilitySampleSequence = VisibilitySampleSequence.Load();
				Result.VisibleEndpointMask = static_cast<uint8>(
					VisibleEndpointMask.Load() & 0x3u);
			}

			const uint64 EndVersion = FeedbackSnapshotVersion.Load();
			if (IsStableFeedbackSnapshotVersion(StartVersion, EndVersion))
			{
				Result.SnapshotVersion = EndVersion;
				Result.SnapshotReadRetryCount = RetryCount;
				Result.bSnapshotCoherent = true;
				return Result;
			}
			if (!RetryFeedbackSnapshotReadGameThread(RetryCount))
			{
				return MakeIncoherentFeedbackSnapshot(EndVersion, RetryCount);
			}
		}
	}

	// SceneViewExtension callbacks are the single Render Thread writer. The
	// odd/even version publishes the event fields and counters as one snapshot.
	TAtomic<uint64> FeedbackSnapshotVersion{0};
	TAtomic<uint64> WarmupSucceededEpoch{0};
	TAtomic<uint64> WarmupSucceededPacketSequence{0};
	TAtomic<uint64> ProductionFailedEpoch{0};
	TAtomic<uint64> ProductionFailedPacketSequence{0};
	TAtomic<uint64> WarmupPassCount{0};
	TAtomic<uint64> ProductionFailureCount{0};
	TAtomic<uint64> VisibilityOwnershipEpoch{0};
	TAtomic<uint64> VisibilityPacketSequence{0};
	TAtomic<uint64> VisibilitySampleSequence{0};
	TAtomic<uint32> VisibleEndpointMask{0};
	// Render-thread-only pending frame. Multiple accepted view families are
	// reduced with a bitwise endpoint-mask union before one coherent mailbox write.
	bool bVisibilityRenderFramePending = false;
	uint64 VisibilityPendingOwnershipEpoch = 0;
	uint64 VisibilityPendingPacketSequence = 0;
	uint32 VisibilityPendingRenderFrameNumber = MAX_uint32;
	uint8 VisibilityPendingVisibleEndpointMask = 0;
	uint32 VisibilityPendingAcceptedFamilyCount = 0;
};

/**
 * Game-Thread bookkeeping for one published Pair.
 * Ownership readiness controls production rendering, while visibility-observation readiness
 * independently keeps the lightweight Player View frustum observer alive when capture
 * textures are unavailable.
 */
struct FWPGameThreadOwnershipEntry
{
	FWPPairOwnershipSnapshot Ownership;
	uint64 PacketSequence = 0;
	bool bVisibilityObservationReady = false;
};

/** Render-Thread-only pair snapshot containing no UObjects. */
struct FWPRenderThreadPacket
{
	FGuid PairId;
	FName StableSelectorNameA = NAME_None;
	FName StableSelectorNameB = NAME_None;
	EWPPairOwnershipMode RequestedOwnership = EWPPairOwnershipMode::Disabled;
	EWPPairOwnershipMode EffectiveOwnership = EWPPairOwnershipMode::Disabled;
	uint64 OwnershipEpoch = 0;
	TSharedPtr<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe> OwnershipFeedbackState;
	FMatrix44d PortalAToWorld = FMatrix44d::Identity;
	FMatrix44d WorldToPortalA = FMatrix44d::Identity;
	FMatrix44d PortalBToWorld = FMatrix44d::Identity;
	FMatrix44d WorldToPortalB = FMatrix44d::Identity;
	FVector3d PortalACenterWorld = FVector3d::ZeroVector;
	FVector3d PortalBCenterWorld = FVector3d::ZeroVector;
	FWPMetricSettings MetricA;
	FWPMetricSettings MetricB;
	FWPPortalVisualSettings VisualA;
	FWPPortalVisualSettings VisualB;
	// Transit-diagnostics baseline for the first local player; not used for actual view-derived state.
	EWPSide CurrentSide = EWPSide::None;
	EWPSide EntrySide = EWPSide::None;
	EWPRegion Region = EWPRegion::Flat;
	float SignedEllCm = 0.0f;
	float TransitionAlpha = 1.0f;
	uint32 ReferenceViewActorId = 0;
	uint32 TransitActorId = 0;
	uint64 TransitEventSequence = 0;
	FTextureReferenceRHIRef CubeA;
	FTextureReferenceRHIRef CubeB;
	FWPCubeContract CubeContractA;
	FWPCubeContract CubeContractB;
	FTextureReferenceRHIRef RayLUTA;
	FTextureReferenceRHIRef RayLUTB;
	FWPRayLUTContract RayLUTContractA;
	FWPRayLUTContract RayLUTContractB;
	bool bAnalyticNoTransitionA = false;
	bool bAnalyticNoTransitionB = false;
	float RayLUTZA = 0.0f;
	float RayLUTZB = 0.0f;
	uint32 RayLUTRevisionA = 0;
	uint32 RayLUTRevisionB = 0;
	uint64 PacketSequence = 0;
	uint32 CaptureGenerationA = 0;
	uint32 CaptureGenerationB = 0;
	// Logging only: measures GT-to-RT command-queue latency for summary reporting.
	double QueueSubmitSeconds = 0.0;
	double QueueLatencyMs = 0.0;
	bool bEnabled = false;
	bool bHasReferenceView = false;
	bool bTransitActive = false;
	bool bMetricCompatible = false;
	// Hint produced by Game Thread UObject checks; final eligibility is determined on the RT from the actual texture references.
	bool bResourcesReady = false;
	bool bCaptureReady = false;
	bool bScaleSupported = false;
	bool bOwnershipEndpointAReady = false;
	bool bOwnershipEndpointBReady = false;
	bool bOwnershipInputsReady = false;
	/** GT CPU-occlusion mask used only to suppress eligible Production endpoints. */
	uint8 CaptureOcclusionVisibleEndpointMask = 0x3;
	bool bCaptureOcclusionValid = false;
	bool bCaptureVisibilityFeedbackEnabled = false;

	FORCEINLINE bool IsOwnershipContractReady() const
	{
		return PairId.IsValid()
			&& RequestedOwnership == EWPPairOwnershipMode::Production
			&& (EffectiveOwnership == EWPPairOwnershipMode::Warmup
				|| EffectiveOwnership == EWPPairOwnershipMode::Production)
			&& OwnershipEpoch != 0
			&& PacketSequence != 0
			&& bOwnershipEndpointAReady
			&& bOwnershipEndpointBReady
			&& bOwnershipInputsReady;
	}

	/**
	 * Returns true when this packet contains everything required to observe the two
	 * SafeProxy spheres from the primary Player View. This deliberately excludes Cubemap,
	 * LUT, CaptureGeneration, and ownership-resource readiness so visibility feedback can
	 * break the paused-resolution resume cycle without enabling production rendering.
	 */
	FORCEINLINE bool IsVisibilityObservationContractReady() const
	{
		return bEnabled
			&& bCaptureVisibilityFeedbackEnabled
			&& bHasReferenceView
			&& PairId.IsValid()
			&& OwnershipEpoch != 0
			&& PacketSequence != 0
			&& ReferenceViewActorId != 0
			&& OwnershipFeedbackState.IsValid()
			&& !PortalACenterWorld.ContainsNaN()
			&& !PortalBCenterWorld.ContainsNaN()
			&& MetricA.IsFiniteAndValid()
			&& MetricB.IsFiniteAndValid();
	}

	/** Records a warmup pass and publishes success for the exact packet epoch only when bAcknowledge is true. */
	FORCEINLINE void RecordWarmupPass_RenderThread(const bool bAcknowledge) const
	{
		if (OwnershipFeedbackState.IsValid())
		{
			OwnershipFeedbackState->RecordWarmupPass_RenderThread(
				OwnershipEpoch, PacketSequence, bAcknowledge);
		}
	}

	/** Publishes a production failure for the exact packet epoch. */
	FORCEINLINE void ReportProductionFailure_RenderThread() const
	{
		if (OwnershipFeedbackState.IsValid())
		{
			OwnershipFeedbackState->ReportProductionFailure_RenderThread(
				OwnershipEpoch, PacketSequence);
		}
	}

	/** Publishes exact pair-endpoint visibility computed from an accepted primary view to the GT mailbox. */
	FORCEINLINE void RecordVisibilitySample_RenderThread(
		const uint32 RenderFrameNumber,
		const uint8 InVisibleEndpointMask) const
	{
		if (OwnershipFeedbackState.IsValid())
		{
			OwnershipFeedbackState->RecordVisibilitySample_RenderThread(
				OwnershipEpoch,
				PacketSequence,
				RenderFrameNumber,
				static_cast<uint8>(InVisibleEndpointMask & 0x3u));
		}
	}
};

/** Per-world RT state jointly owned by the SceneViewExtension and queued render commands. */
struct FWPRenderState : public TSharedFromThis<FWPRenderState, ESPMode::ThreadSafe>
{
	explicit FWPRenderState(
		FString InWorldName,
		FString InWorldType,
		const bool bInPIEWorld)
		: WorldName(MoveTemp(InWorldName))
		, WorldType(MoveTemp(InWorldType))
		, bPIEWorld(bInPIEWorld)
	{
	}

	// PairsRenderThread is read and written only on the Render Thread.
	TMap<uint64, FWPRenderThreadPacket> PairsRenderThread;
	// Logging only: telemetry for reporting RT packet apply/drop counts and queue latency in the summary.
	uint64 AppliedUpdateCountRenderThread = 0;
	uint64 DroppedUpdateCountRenderThread = 0;
	double AccumulatedQueueLatencyMsRenderThread = 0.0;
	double MaxQueueLatencyMsRenderThread = 0.0;
	uint64 QueueLatencySampleCountRenderThread = 0;

	// OwnershipSnapshotsGameThread is accessed only by RendererService on the Game Thread.
	// It must not be accessed from the Render Thread.
	TMap<uint64, FWPGameThreadOwnershipEntry> OwnershipSnapshotsGameThread;

	// Activation is also evaluated on the Game Thread, so only atomic values are shared across threads.
	TAtomic<int32> RegisteredPairCount{0};
	// Counts only Warmup or Production pairs whose rendering contract is ready.
	TAtomic<int32> ActiveOwnershipPairCount{0};
	// Counts packets that can publish Player View frustum feedback without render resources.
	TAtomic<int32> ActiveVisibilityObservationPairCount{0};
	TAtomic<bool> bShuttingDown{false};

	// Logging only: immutable strings that provide World context in RT logs without UObjects.
	const FString WorldName;
	const FString WorldType;
	/** Immutable PIE-World classification used to identify SIE without string comparisons and fail closed otherwise. */
	const bool bPIEWorld;
};
