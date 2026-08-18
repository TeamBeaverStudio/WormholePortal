// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "SceneViewExtension.h"
#include "WPRenderState.h"

struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/** UE 5.8 production SceneViewExtension that consumes per-world packets. */
class FWPSceneViewExtension final : public FWorldSceneViewExtension
{
public:
	FWPSceneViewExtension(
		const FAutoRegister& AutoRegister,
		UWorld* World,
		TSharedRef<FWPRenderState, ESPMode::ThreadSafe> InRenderState);

	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

	/**
	 * Observes the primary Player View before post processing and publishes only the
	 * A/B SafeProxy frustum result. This hook remains active without Cubemap/LUT capture
	 * resources and never submits a render pass or modifies SceneColor.
	 */
	virtual void PreRenderView_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	struct FEligibilityFingerprint
	{
		EWPEligibilityReason Reason = EWPEligibilityReason::Count;

		bool Equals(const FEligibilityFingerprint& Other) const
		{
			return Reason == Other.Reason;
		}
	};

	bool ShouldAcceptView_RenderThread(const FSceneView& View, const TCHAR*& OutSkipReason) const;
	bool ShouldAcceptOwnershipView_RenderThread(
		const FSceneView& View,
		const TCHAR*& OutSkipReason) const;
	EWPEligibilityReason EvaluateEligibility_RenderThread(
		const FWPRenderThreadPacket& Packet) const;
	void RecordEligibilityTransition_RenderThread(
		uint64 HandleValue,
		const FWPRenderThreadPacket& Packet,
		EWPEligibilityReason Reason,
		double CpuMs);
	FScreenPassTexture PostProcessPassAfterMotionBlur_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	bool TryPostProcessOwnershipPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs,
		FScreenPassTexture& OutOutput);
	bool TryPostProcessMultiPairOwnershipPass_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs,
		FScreenPassTexture& OutOutput);
	void RecordVisibilityObservations_RenderThread(const FSceneView& View);
	void MaybeLogVisibilityObservationSummary_RenderThread();
	void MaybeLogOwnershipSummary_RenderThread();

private:
	TSharedRef<FWPRenderState, ESPMode::ThreadSafe> RenderState;
	struct FOwnershipWarmupTracker
	{
		uint64 Epoch = 0;
		uint64 PacketSequence = 0;
		uint32 LastFrameNumber = MAX_uint32;
		uint32 ConsecutiveSuccessCount = 0;
		bool bAcknowledged = false;
		bool bFailureLatched = false;
	};
	TMap<uint64, FOwnershipWarmupTracker> OwnershipWarmupByHandleRenderThread;
	// Logging only: emits a detailed record when a Pair's observed A/B visibility mask changes.
	TMap<uint64, uint64> LastVisibilityObservationFingerprintByHandleRenderThread;
	// Logging only: interval telemetry for the resource-independent frustum observer.
	double VisibilityObservationSummaryStartSeconds = 0.0;
	double VisibilityObservationAccumulatedCpuMs = 0.0;
	double VisibilityObservationMaxCpuMs = 0.0;
	uint64 VisibilityObservationViewInvocationCount = 0;
	uint64 VisibilityObservationPairEvaluationCount = 0;
	uint64 VisibilityObservationSampleWriteCount = 0;
	uint64 VisibilityObservationRejectedViewCount = 0;
	// Logging only: fingerprint used to emit detailed logs only when a view's ownership result changes.
	TMap<uint64, uint64> LastOwnershipResultFingerprintByViewRenderThread;
	// Logging only: accumulates the interval, counts, and CPU costs reported by the production-ownership summary.
	double OwnershipSummaryStartSeconds = 0.0;
	double OwnershipAccumulatedCallbackCpuMs = 0.0;
	double OwnershipMaxCallbackCpuMs = 0.0;
	double OwnershipAccumulatedSetupCpuMs = 0.0;
	double OwnershipMaxSetupCpuMs = 0.0;
	uint64 OwnershipRDGSetupAttemptCount = 0;
	double OwnershipAccumulatedPreflightCpuMs = 0.0;
	double OwnershipMaxPreflightCpuMs = 0.0;
	double OwnershipAccumulatedEligibilityCpuMs = 0.0;
	double OwnershipMaxEligibilityCpuMs = 0.0;
	uint64 OwnershipEligibilityEvaluationCount = 0;
	uint64 OwnershipWarmupCallbackCount = 0;
	uint64 OwnershipWarmupEndpointPassCount = 0;
	uint64 OwnershipWarmupAckCount = 0;
	uint64 OwnershipWarmupFailureCount = 0;
	uint64 OwnershipWarmupLatchedSkipCount = 0;
	uint64 OwnershipWarmupProgressPreservedCount = 0;
	uint64 OwnershipProductionViewCount = 0;
	uint64 OwnershipProductionEndpointPassCount = 0;
	uint64 OwnershipProductionFailureCount = 0;
	uint64 OwnershipUnsupportedViewCount = 0;
	uint64 OwnershipMissingReadyPacketViewCount = 0;
	uint64 OwnershipInvisibleViewCount = 0;
	uint64 OwnershipCallbackInvocationCount = 0;
	uint64 OwnershipPairAttemptCount = 0;
	uint64 OwnershipPreflightAttemptCount = 0;
	uint64 OwnershipPreflightFailureCount = 0;
	uint64 OwnershipPreflightPairFailureCount = 0;
	uint64 OwnershipUnexpectedSubmissionFailureCount = 0;
	// Test only: callback state used to inject forced production preflight or pass failures.
	int32 LastObservedOwnershipForceFailureCallbacks = INDEX_NONE;
	int32 RemainingOwnershipForceFailureCallbacks = 0;
	// Logging only: cache used to emit detailed logs only when a production-eligibility result changes.
	TMap<uint64, FEligibilityFingerprint> LastEligibilityByHandle;
};
