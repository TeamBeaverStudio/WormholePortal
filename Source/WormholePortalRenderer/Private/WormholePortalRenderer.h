// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FWPRendererService;
class FSceneViewExtensionBase;

class FWormholePortalRendererModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void HandlePostEngineInit();
	void RegisterWormholeLayerRenderPassViewExtension();

	FDelegateHandle PostEngineInitHandle;
	TUniquePtr<FWPRendererService> RendererService;
	TSharedPtr<FSceneViewExtensionBase, ESPMode::ThreadSafe> WormholeLayerRenderPassViewExtension;
};
