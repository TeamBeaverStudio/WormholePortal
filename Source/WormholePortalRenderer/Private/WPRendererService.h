// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/IWPRenderer.h"

class FWPSceneViewExtension;
struct FWPPairOwnershipFeedbackState;
struct FWPRenderState;
struct FWPRenderThreadPacket;

/** Manages GT pair registration and immutable snapshot delivery from the GT to the RT. */
class FWPRendererService final : public IWPRenderer
{
public:
	FWPRendererService();
	virtual ~FWPRendererService() override;

	virtual uint64 GetServiceId() const override { return ServiceId; }
	virtual FWPRenderHandle RegisterPair(UWorld& World, const FGuid& PairId) override;
	virtual bool UpdatePair(const FWPRenderHandle& Handle, const FWPRenderPacket& Packet) override;
	virtual bool QueryPairOwnershipFeedback(
		const FWPRenderHandle& Handle,
		FWPPairOwnershipFeedback& OutFeedback) const override;
	virtual bool QueryPairOwnershipFeedback(
		const FWPRenderHandle& Handle,
		FWPPairOwnershipFeedback& OutFeedback,
		bool bIncludeVisibility) const override;
	virtual bool CanEnqueueCubeAAPass(
		UTextureRenderTargetCube& InputCubeTarget,
		UTextureRenderTargetCube& OutputCubeTarget,
		UTextureRenderTargetCube& PublishedReferenceOwner,
		bool bDirectPublish) const override;
	virtual bool EnqueueCubeAAPass(
		UTextureRenderTargetCube& InputCubeTarget,
		UTextureRenderTargetCube& OutputCubeTarget,
		UTextureRenderTargetCube& PublishedReferenceOwner,
		bool bDirectPublish,
		uint8 FaceMask) override;
	virtual bool AddSelectedCubeCaptureRenderer(
		USceneCaptureComponentCube& CaptureComponent, uint8 SelectedFaceMask, ISceneRenderBuilder& SceneRenderBuilder) override;
	virtual void UnregisterPair(const FWPRenderHandle& Handle) override;

	/** Returns the existing per-world render state without creating or changing ownership. */
	TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> FindRenderStateForWorld(UWorld* World) const;

	/** Drains all SceneViewExtension callbacks and queued render commands before the module unloads. */
	void Shutdown();

private:
	struct FWorldEntry
	{
		TWeakObjectPtr<UWorld> World;
		TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState;
		TSharedPtr<FWPSceneViewExtension, ESPMode::ThreadSafe> ViewExtension;
		TSet<uint64> PairHandles;
	};

	struct FPairEntry
	{
		FGuid PairId;
		uint32 WorldId = 0;
		uint64 LastSubmittedSequence = 0;
		TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState;
		TSharedPtr<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe> OwnershipFeedbackState;
		// Logging only: tracks the count, fingerprint, and rate limit for incoherent-feedback warnings.
		mutable uint64 IncoherentFeedbackReadCount = 0;
		mutable uint64 LastIncoherentFeedbackLoggedVersion = 0;
		mutable double LastIncoherentFeedbackLogSeconds = 0.0;
		mutable bool bHasLoggedIncoherentFeedback = false;
	};

	static FWPRenderThreadPacket MakeRenderThreadPacket(
		const FWPRenderPacket& Packet,
		const TSharedRef<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe>& OwnershipFeedbackState);
	bool IsHandleOwned(const FWPRenderHandle& Handle) const;

private:
	TMap<uint32, FWorldEntry> Worlds;
	TMap<uint64, FPairEntry> Pairs;
	uint64 ServiceId = 0;
	uint64 NextHandleValue = 1;
	bool bShutdown = false;
};
