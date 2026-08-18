// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "ComponentVisualizer.h"

/** Displays metric values for the selected Editor World portal in a screen-space HUD. */
class FWPPortalDebugComponentVisualizer final : public FComponentVisualizer
{
public:
	virtual void DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport,
		const FSceneView* View, FCanvas* Canvas) override;
};
