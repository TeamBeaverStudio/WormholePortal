// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "WPPortalDebugComponent.generated.h"

/**
 * A lightweight Primitive Component that displays portal metric boundaries together
 * with Scene Depth
 * in Editor Worlds and PIE. It does not create a Scene Proxy in Game Worlds, Standalone
 * Worlds,
 * or packaged builds.
 */
UCLASS(ClassGroup = (WormholePortal), NotBlueprintable, NotBlueprintType)
class WORMHOLEPORTALRUNTIME_API UWPPortalDebugComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UWPPortalDebugComponent();

	/**
	 * Updates the render state with the Actor's current debug enablement, unit-scale
	 * metric, and TriggerVolume-relative location and rotation.
	 */
	void SetPortalDebugData(bool bEnabled, float InSeamRadius, float InMouthRadius,
		float InTransitionRadius, const FTransform& InSeamRelativeTransform);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	bool bDebugDrawEnabled = true;
	float SeamRadius = 50.0f;
	float MouthRadius = 150.0f;
	float TransitionRadius = 350.0f;
	FTransform SeamRelativeTransform = FTransform::Identity;
};
