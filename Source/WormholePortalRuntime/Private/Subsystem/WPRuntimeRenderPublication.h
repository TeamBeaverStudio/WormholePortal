// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "Subsystem/WPRuntimePairState.h"
#include "Subsystem/WPRuntimeTelemetry.h"

class AActor;
class AWormholePortalActor;
class IWPRenderer;
class UWPCaptureManager;
class UWPLUTEndpointManager;
class UWPRuntimeSubsystem;

/**
 * Owns Renderer registration, the ownership handshake, RenderPacket construction, and
 * publication.
 *
 * Called only on the Game Thread. It never passes Actors or UObjects to the Render
 * Thread. Instead, it
 * freezes Manager snapshots and Portal Transforms into the value-type FWPRenderPacket
 * and publishes it
 * through IWPRenderer.
 *
 * Responsibilities:
 * - Registering RenderHandles per Renderer Service and replacing stale handles
 * - Ownership-Epoch handshake using warmup ACK and failure feedback
 * - Serializing capture, LUT, metric, and Reference View snapshots into a Packet
 * - Comparing change reasons, selecting heartbeats, and committing successful
 *   publication state
 *
 * It does not manage capture cadence, visibility pause, occlusion traces, or Pair
 * lifetime.
 *
 * Terms:
 * - RenderHandle: `(ServiceId, Value)` identifier used to locate one Portal Pair in a
 *   Renderer Service.
 *   After a Service restart, an old handle is stale even if its Value remains because
 *   its ServiceId differs.
 * - Ownership: Disabled/Warmup/Production state indicating whether the Pair may be used
 *   for production
 *   SceneColor compositing. The Runtime writes requested state and Epoch to the Packet;
 *   the Renderer returns
 *   the actual Render Pass result through a feedback mailbox.
 * - Handshake: the Runtime publishes a Warmup Packet for a new Ownership Epoch and
 *   transitions to Production
 *   only after the Renderer returns a successful ACK for that exact Epoch and Packet
 *   Sequence. A late ACK
 *   from an earlier Frame is rejected if its Epoch does not match, preventing
 *   publication of the wrong Texture.
 * - RenderPacket: values that the Render Thread can copy and consume safely instead of
 *   Actors or UObjects.
 *   A single Sequence freezes Pair ID, Transforms, Metrics, Cubemap/LUT RHI references
 *   and Generations,
 *   Reference View diagnostics, Transit state, and the ownership request.
 * - Publication: publishing a completed Packet through Renderer::UpdatePair. Resource,
 *   Metric, Transform,
 *   Ownership, and Transit changes publish immediately. Camera diagnostics or
 *   CaptureGeneration-only changes
 *   are coalesced to avoid unnecessary Packet replacement or delivered by a
 *   rate-limited heartbeat.
 *
 * Single-publication flow:
 * 1. EnsureRendererRegistration obtains a handle for the current Renderer Service.
 * 2. BuildRenderPacket assembles Manager snapshots and Portal values into one Sequence
 *    candidate.
 * 3. UpdatePairOwnership reads Renderer feedback and computes requested/effective
 *    ownership and Epoch.
 * 4. MakePublishDecision compares against the last successful Packet and decides
 *    whether to publish.
 * 5. CommitPublishedState advances comparison baselines and the visibility Packet floor
 *    only after
 *    Renderer::UpdatePair succeeds. On failure, the previous successful state remains
 *    so Renderer and
 *    Runtime observations cannot diverge.
 */
class FWPRuntimeRenderPublication final
{
public:
	/**
	 * Connects the Owner and Manager references required to construct Packets.
	 * Every argument must outlive this object; resource ownership is not transferred.
	 * @param InRuntime Owner that provides the World, logging context, and Metric helper.
	 * @param InCaptureManager Manager reference that provides Cubemap resource/generation
	 *                         snapshots.
	 * @param InLUTEndpointManager Manager reference that provides LUT texture/contract
	 *                             snapshots.
	 * @param InTelemetry Minimal Telemetry that records successful publications and stale
	 *                    ACKs.
	 */
	FWPRuntimeRenderPublication(
		UWPRuntimeSubsystem& InRuntime,
		TObjectPtr<UWPCaptureManager>& InCaptureManager,
		TObjectPtr<UWPLUTEndpointManager>& InLUTEndpointManager,
		FWPRuntimeTelemetry& InTelemetry);

	/**
	 * Ensures that the Pair has a valid handle for the current IWPRenderer Service.
	 * Discards a stale handle whose Service ID changed and also resets the visibility
	 * mailbox and Packet floor.
	 * @param PairState Mutable Pair state that stores the handle and mailbox baselines.
	 */
	void EnsureRendererRegistration(FWPPortalPairState& PairState);

	/**
	 * Updates requested/effective ownership and Epoch from the latest Renderer feedback and
	 * Packet readiness.
	 * A warmup ACK commits Production only when both its Epoch and Packet match.
	 * @param PairState Pair that stores ownership state and the latest Renderer feedback.
	 * @param Packet Current publication candidate; computed ownership fields are also
	 *               written to this Packet.
	 * @param Renderer Current Service. If null, remains in a safe Warmup/Disabled state
	 *                 without feedback.
	 */
	void UpdatePairOwnership(
		FWPPortalPairState& PairState,
		FWPRenderPacket& Packet,
		IWPRenderer* Renderer);

	/**
	 * Returns a Camera Snapshot for exactly one Local Player.
	 * If no View or multiple Views prevent selecting a definitive reference, returns false
	 * so the caller can
	 * fail open.
	 * @param OutCameraLocation World-space Camera location on success.
	 * @param OutCameraRotation World-space Camera rotation on success.
	 * @param OutViewActor Pawn or View Target Actor on success.
	 * @param OutCameraFOVDegrees Valid horizontal FOV in degrees on success.
	 * @return true if all four outputs were acquired coherently from the same Local Player
	 *         View.
	 */
	bool ResolveReferenceView(
		FVector& OutCameraLocation,
		FRotator& OutCameraRotation,
		AActor*& OutViewActor,
		float& OutCameraFOVDegrees) const;

	/**
	 * Stores which side, A or B, is closer to the Reference Camera and the Metric Region in
	 * Pair state.
	 * @param PairState Pair whose Reference Side, Region, and SignedEllCm will be updated.
	 * @param CameraLocation World-space location used for Metric classification.
	 */
	void UpdateReferenceViewState(
		FWPPortalPairState& PairState,
		const FVector& CameraLocation) const;

	/**
	 * Assembles the current Pair state and read-only Manager Endpoint snapshots into one
	 * immutable Packet.
	 * Represents unavailable resources with null/zero contracts and does not create
	 * arbitrary fallback UObjects.
	 * @param PairState Identity, ownership, validation, and publication state to copy into
	 *                  the Packet.
	 * @param CameraLocation Camera location used for Reference diagnostics and Metric
	 *                       classification.
	 * @param ReferenceViewActor Reference Actor whose value-type Actor ID is written to the
	 *                           Packet.
	 * @param bHasReferenceView Whether Reference-related fields are trustworthy.
	 * @return Complete value-type Packet candidate for Renderer::UpdatePair.
	 */
	FWPRenderPacket BuildRenderPacket(
		const FWPPortalPairState& PairState,
		const FVector& CameraLocation,
		const AActor* ReferenceViewActor,
		bool bHasReferenceView) const;

	/**
	 * Compares the current Packet with the last successful publication and selects
	 * immediate publish,
	 * heartbeat, or coalescing. This function does not mutate state and records comparison
	 * CPU time in the
	 * decision.
	 * @param PairState Last-successful and last-observed publication baselines.
	 * @param Packet Current Packet candidate.
	 * @param TransformA Current World Transform of Portal A.
	 * @param TransformB Current World Transform of Portal B.
	 * @param NowSeconds Monotonic time used to compute heartbeat age.
	 * @return Decision value with publication and individual change reasons reported
	 *         separately.
	 */
	FWPPublishDecision MakePublishDecision(
		const FWPPortalPairState& PairState,
		const FWPRenderPacket& Packet,
		const FTransform& TransformA,
		const FTransform& TransformB,
		double NowSeconds) const;

	/**
	 * Called only after Renderer::UpdatePair succeeds.
	 * Commits the published Sequence, comparison baselines, visibility Packet floor, and
	 * lifetime Telemetry as
	 * one state transition.
	 * @param PairState Mutable Pair state receiving the successful result.
	 * @param Packet Exact Packet accepted by the Renderer.
	 * @param TransformA Portal A Transform stored as a comparison baseline.
	 * @param TransformB Portal B Transform stored as a comparison baseline.
	 * @param CameraLocation Last published Reference Camera location.
	 * @param PublishDecision Set of reasons that determined the Packet floor and coalescing
	 *                        baselines.
	 * @param NowSeconds Monotonic time stored as the last successful publication/heartbeat
	 *                   time.
	 */
	void CommitPublishedState(
		FWPPortalPairState& PairState,
		const FWPRenderPacket& Packet,
		const FTransform& TransformA,
		const FTransform& TransformB,
		const FVector& CameraLocation,
		const FWPPublishDecision& PublishDecision,
		double NowSeconds);

private:
	/** Owner bridge that returns the RuntimeSubsystem's World. */
	UWorld* GetWorld() const;

	/**
	 * Owner bridge that creates a Renderer-consumable Metric value object from Portal
	 * properties.
	 * @param Portal Endpoint whose Metric properties and Scale will be read.
	 * @return Metric Snapshot that can be copied by value to the Render Thread.
	 */
	FWPMetricSettings MakeMetricSettings(const AWormholePortalActor& Portal) const;

	/** Owner that provides the World, logging context, and Metric helper. */
	UWPRuntimeSubsystem& Runtime;
	/** Non-owning Manager reference that provides Cubemap resource/generation snapshots. */
	TObjectPtr<UWPCaptureManager>& CaptureManager;
	/** Non-owning Manager reference that provides LUT texture/contract snapshots. */
	TObjectPtr<UWPLUTEndpointManager>& LUTEndpointManager;
	/** Minimal cumulative values required for shutdown and error diagnostics, such as successful publications and rejected ACKs. */
	FWPRuntimeTelemetry& Telemetry;
};
