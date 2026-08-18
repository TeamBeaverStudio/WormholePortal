// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FWormholePortalRuntimeModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
