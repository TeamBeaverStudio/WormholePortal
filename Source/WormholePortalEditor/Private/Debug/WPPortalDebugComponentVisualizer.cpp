// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Debug/WPPortalDebugComponentVisualizer.h"

#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "SceneView.h"
#include "Debug/WPPortalDebugComponent.h"
#include "WormholePortalActor.h"

namespace
{
	constexpr float LabelScreenOffsetX = 12.0f;
	constexpr float LabelScreenOffsetY = 8.0f;
	const FLinearColor SeamLabelColor(FColor(255, 128, 0));
	const FLinearColor MouthLabelColor(FColor(255, 220, 0));
	const FLinearColor TransitionLabelColor(FColor(0, 220, 255));
}

void FWPPortalDebugComponentVisualizer::DrawVisualizationHUD(const UActorComponent* Component,
	const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	(void)Viewport;

	const UWPPortalDebugComponent* DebugComponent = Cast<const UWPPortalDebugComponent>(Component);
	const AWormholePortalActor* Portal = DebugComponent
		? Cast<const AWormholePortalActor>(DebugComponent->GetOwner())
		: nullptr;
	const UWorld* World = DebugComponent ? DebugComponent->GetWorld() : nullptr;
	if (!Portal || !World || World->WorldType != EWorldType::Editor
		|| !Portal->IsSelectedInEditor() || !Portal->IsPortalDebugEnabled()
		|| !View || !Canvas || !GEngine || !GEngine->GetSmallFont())
	{
		return;
	}

	const FVector TransitionTop = DebugComponent->GetComponentTransform().TransformPosition(
		FVector::UpVector * Portal->GetTransitionRadius());
	FVector2D ScreenPosition;
	if (!View->ScreenToPixel(View->WorldToScreen(TransitionTop), ScreenPosition))
	{
		return;
	}

	const UFont* Font = GEngine->GetSmallFont();
	const float LineHeight = Font->GetMaxCharHeight() + 2.0f;
	const float DrawX = ScreenPosition.X + LabelScreenOffsetX;
	float DrawY = ScreenPosition.Y + LabelScreenOffsetY;

	const FString Labels[] =
	{
		FString::Printf(TEXT("SEAM %.1f cm"), Portal->GetPortalRadius()),
		FString::Printf(TEXT("MOUTH %.1f cm"), Portal->GetMouthRadius()),
		FString::Printf(TEXT("TRANSITION %.1f cm"), Portal->GetTransitionRadius())
	};
	const FLinearColor Colors[] =
	{
		SeamLabelColor,
		MouthLabelColor,
		TransitionLabelColor
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Labels); ++Index)
	{
		Canvas->DrawShadowedString(DrawX, DrawY, Labels[Index], Font, Colors[Index], FLinearColor::Black);
		DrawY += LineHeight;
	}
}
