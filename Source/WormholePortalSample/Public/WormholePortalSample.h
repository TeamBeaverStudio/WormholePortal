// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * @brief WormholePortal Sample Module 
 */
class FWormholePortalSampleModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
