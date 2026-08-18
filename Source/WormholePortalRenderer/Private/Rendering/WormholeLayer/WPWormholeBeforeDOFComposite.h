// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FRDGBuilder;
class FSceneView;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

FScreenPassTexture AddWPWormholeBeforeDOFCompositePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& CleanMainSceneColor,
	const FScreenPassTexture& PreparedWormholeLayer,
	const FScreenPassTexture& WormholeCoverageMask,
	float PreparedToMainPreExposureScale,
	const FScreenPassRenderTarget& OverrideOutput);

bool AddWPWormholeVelocityPatchPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& WormholeVelocityOverride,
	const FScreenPassTexture& WormholeCoverageMask,
	FRDGTextureRef SceneVelocityTexture,
	const FIntRect& SceneVelocityViewRect);
