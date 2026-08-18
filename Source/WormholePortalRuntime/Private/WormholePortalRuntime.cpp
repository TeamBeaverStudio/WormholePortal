// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WormholePortalRuntime.h"

void FWormholePortalRuntimeModule::StartupModule()
{
	// Renderer-specific initialization, including shader source mapping, is owned by
	// WormholePortalRenderer so this module remains safe for non-rendering targets.
}

void FWormholePortalRuntimeModule::ShutdownModule()
{
}
	
IMPLEMENT_MODULE(FWormholePortalRuntimeModule, WormholePortalRuntime)

