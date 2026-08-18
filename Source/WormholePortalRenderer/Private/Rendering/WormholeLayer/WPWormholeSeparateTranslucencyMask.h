// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ScreenPass.h"

class FRDGBuilder;
class FSceneView;

FScreenPassTexture AddWPWormholeSeparateTranslucencyMaskPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& SeparateTranslucency,
	const FScreenPassTexture& WormholeCoverageMask,
	const FScreenPassRenderTarget& OverrideOutput);

FScreenPassTexture AddWPCleanSceneColorWithSeparateTranslucencyPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& CleanSceneColor,
	const FScreenPassTexture& SeparateTranslucency);
