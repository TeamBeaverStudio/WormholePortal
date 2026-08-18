// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FSceneViewExtensionBase;
class FSceneView;
class FWPRendererService;

TSharedRef<FSceneViewExtensionBase, ESPMode::ThreadSafe>
	CreateWPWormholeLayerRenderPassViewExtension(FWPRendererService& RendererService);

void ShutdownWPWormholeLayerRenderPassViewExtension_GameThread();

bool TryClaimWPWormholeLayerOwnership_RenderThread(const FSceneView& View);
