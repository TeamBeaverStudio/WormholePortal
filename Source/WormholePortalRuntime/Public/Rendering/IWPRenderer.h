// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"

class ISceneRenderBuilder;
class USceneCaptureComponentCube;
class UTextureRenderTargetCube;
class UWorld;

/**
 * One-way boundary between the Runtime state layer and the Renderer implementation
 * module.
 * Runtime Subsystems do not reference SceneViewExtension or Global Shader
 * implementations directly.
 */
class WORMHOLEPORTALRUNTIME_API IWPRenderer : public IModularFeature
{
public:
	virtual ~IWPRenderer() = default;

	static FName GetModularFeatureName()
	{
		static const FName FeatureName(TEXT("WPWormholeRenderer"));
		return FeatureName;
	}

	static IWPRenderer* Find()
	{
		IModularFeatures& ModularFeatures = IModularFeatures::Get();
		if (!ModularFeatures.IsModularFeatureAvailable(GetModularFeatureName()))
		{
			return nullptr;
		}

		TArray<IWPRenderer*> Implementations =
			ModularFeatures.GetModularFeatureImplementations<IWPRenderer>(GetModularFeatureName());
		return Implementations.IsEmpty() ? nullptr : Implementations[0];
	}

	/**
	 * Returns the Service instance ID used to reject stale handles after a hot reload or
	 * module restart.
	 */
	virtual uint64 GetServiceId() const = 0;

	/** Registers a Pair with World-scoped SceneViewExtension state. Game Thread only. */
	virtual FWPRenderHandle RegisterPair(UWorld& World, const FGuid& PairId) = 0;

	/**
	 * Enqueues the latest Packet as an immutable Render Thread snapshot. Game Thread only.
	 */
	virtual bool UpdatePair(const FWPRenderHandle& Handle, const FWPRenderPacket& Packet) = 0;

	/**
	 * Queries the latest epoch snapshot of warmup and production results completed by the
	 * Render Thread.
	 * Game Thread only. False means that feedback is not yet available or the handle is
	 * stale.
	 * This is the base contract that preserves source compatibility with existing Renderer
	 * implementations.
	 */
	virtual bool QueryPairOwnershipFeedback(
		const FWPRenderHandle& Handle,
		FWPPairOwnershipFeedback& OutFeedback) const = 0;

	/**
	 * Queries ownership feedback with optional inclusion of the visibility mailbox.
	 * Existing external Renderer implementations remain build-compatible without
	 * implementing this overload.
	 * The default implementation delegates to the existing two-argument contract and fails
	 * open.
	 */
	virtual bool QueryPairOwnershipFeedback(
		const FWPRenderHandle& Handle,
		FWPPairOwnershipFeedback& OutFeedback,
		bool bIncludeVisibility) const
	{
		static_cast<void>(bIncludeVisibility);
		return QueryPairOwnershipFeedback(Handle, OutFeedback);
	}

	/**
	 * Validates the AA input, output, and RHI shader contract immediately before Capture.
	 * After this function returns true, EnqueueCubeAAPass must succeed if no resource
	 * changes
	 * occur within the same Game Thread submission.
	 */
	virtual bool CanEnqueueCubeAAPass(
		UTextureRenderTargetCube& InputCubeTarget,
		UTextureRenderTargetCube& OutputCubeTarget,
		UTextureRenderTargetCube& PublishedReferenceOwner,
		bool bDirectPublish) const = 0;

	/**
	 * Reads a SceneColorHDRNoAlpha cube and writes a linear-HDR spatial-AA result.
	 * In direct mode, only the fixed PublishedReferenceOwner texture reference is
	 * redirected to the latest physical output.
	 * Direct-mode input and output must differ, and the call is Game Thread only.
	 * When bDirectPublish is false, the pass acts as a quality-validation fallback that
	 * transiently filters or copies
	 * through the same input and output.
	 */
	virtual bool EnqueueCubeAAPass(
		UTextureRenderTargetCube& InputCubeTarget,
		UTextureRenderTargetCube& OutputCubeTarget,
		UTextureRenderTargetCube& PublishedReferenceOwner,
		bool bDirectPublish,
		uint8 FaceMask = 0x3f) = 0;

	/** Stages selected Cubemap faces as one ViewFamily and one SceneRenderer. Runtime never exposes renderer-private types. */
	virtual bool AddSelectedCubeCaptureRenderer(
		USceneCaptureComponentCube& CaptureComponent, uint8 SelectedFaceMask, ISceneRenderBuilder& SceneRenderBuilder)
	{
		static_cast<void>(CaptureComponent);
		static_cast<void>(SelectedFaceMask);
		static_cast<void>(SceneRenderBuilder);
		return false;
	}

	/**
	 * Safely releases the Pair snapshot and World-extension reference. Game Thread only.
	 */
	virtual void UnregisterPair(const FWPRenderHandle& Handle) = 0;
};
