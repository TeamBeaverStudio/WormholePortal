// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "WPWormholeDepthProxyComponent.generated.h"

/**
 * Depth-only sphere isolated to the wormhole-layer custom pass.
 * It is transient, non-interactive, and excluded from normal main and capture views.
 */
UCLASS(Transient, NotBlueprintable)
class UWPWormholeDepthProxyComponent final : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UWPWormholeDepthProxyComponent();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

	void SetProxyRadiusCm(double RadiusCm);
};
