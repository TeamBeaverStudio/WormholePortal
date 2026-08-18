// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Debug/WPPortalDebugComponent.h"

#include "Engine/World.h"

#if WITH_EDITOR
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"
#endif

namespace
{
	constexpr int32 PortalDebugCircleSegments = 32;
	constexpr float SeamLineThickness = 2.5f;
	constexpr float MouthLineThickness = 2.0f;
	constexpr float TransitionLineThickness = 1.5f;
	constexpr float PortalDebugHitProxyThickness = 12.0f;
	const FColor SeamColor(255, 128, 0);
	const FColor MouthColor(255, 220, 0);
	const FColor TransitionColor(0, 220, 255);

	bool IsPortalDebugWorld(const UWorld* World)
	{
		return World && (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE);
	}
}

UWPPortalDebugComponent::UWPPortalDebugComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bSelectable = true;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCollisionResponseToAllChannels(ECR_Ignore);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	SetCastShadow(false);
	SetReceivesDecals(false);
	SetRenderCustomDepth(false);
	SetVisibleInRayTracing(false);
	SetHiddenInGame(false);
	SetVisibility(true);
}

void UWPPortalDebugComponent::SetPortalDebugData(const bool bEnabled, const float InSeamRadius,
	const float InMouthRadius, const float InTransitionRadius, const FTransform& InSeamRelativeTransform)
{
	const float NewSeamRadius = FMath::Max(0.0f, InSeamRadius);
	const float NewMouthRadius = FMath::Max(0.0f, InMouthRadius);
	const float NewTransitionRadius = FMath::Max(0.0f, InTransitionRadius);
	const bool bChanged = bDebugDrawEnabled != bEnabled
		|| !FMath::IsNearlyEqual(SeamRadius, NewSeamRadius)
		|| !FMath::IsNearlyEqual(MouthRadius, NewMouthRadius)
		|| !FMath::IsNearlyEqual(TransitionRadius, NewTransitionRadius)
		|| !SeamRelativeTransform.Equals(InSeamRelativeTransform);

	bDebugDrawEnabled = bEnabled;
	SeamRadius = NewSeamRadius;
	MouthRadius = NewMouthRadius;
	TransitionRadius = NewTransitionRadius;
	SeamRelativeTransform = InSeamRelativeTransform;

	if (bChanged)
	{
		UpdateBounds();
		MarkRenderStateDirty();
	}
}

FBoxSphereBounds UWPPortalDebugComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// 포탈 Actor는 unit scale로 가정하며 metric 반지름을 Scale 보정 없이 월드 Bounds에 사용한다.
	const FTransform UnitPortalTransform(
		LocalToWorld.GetRotation(), LocalToWorld.GetLocation(), FVector::OneVector);
	FBoxSphereBounds Result(UnitPortalTransform.GetLocation(), FVector(TransitionRadius), TransitionRadius);
	const FTransform UnitSeamRelative(
		SeamRelativeTransform.GetRotation(), SeamRelativeTransform.GetLocation(), FVector::OneVector);
	const FTransform SeamToWorld = UnitSeamRelative * UnitPortalTransform;
	const FBoxSphereBounds SeamBounds(
		SeamToWorld.GetLocation(), FVector(SeamRadius), SeamRadius);

	return Result + SeamBounds;
}

FPrimitiveSceneProxy* UWPPortalDebugComponent::CreateSceneProxy()
{
#if WITH_EDITOR
	if (!bDebugDrawEnabled || !IsPortalDebugWorld(GetWorld()))
	{
		return nullptr;
	}

	class FWPPortalDebugSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		explicit FWPPortalDebugSceneProxy(const UWPPortalDebugComponent* Component)
			: FPrimitiveSceneProxy(Component)
			, SeamRadius(Component->SeamRadius)
			, MouthRadius(Component->MouthRadius)
			, TransitionRadius(Component->TransitionRadius)
			, SeamRelativeTransform(Component->SeamRelativeTransform)
			, bExpandEditorHitProxy(Component->GetWorld()
				&& Component->GetWorld()->WorldType == EWorldType::Editor)
		{
			bWillEverBeLit = false;
		}

		SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
			const FSceneViewFamily& ViewFamily, const uint32 VisibilityMap,
			FMeshElementCollector& Collector) const override
		{
			const FMatrix& ComponentLocalToWorld = GetLocalToWorld();
			const FTransform UnitPortalTransform(
				ComponentLocalToWorld.ToQuat(), ComponentLocalToWorld.GetOrigin(), FVector::OneVector);
			const FMatrix LocalToWorld = UnitPortalTransform.ToMatrixNoScale();
			const FVector Center = LocalToWorld.GetOrigin();
			const FVector UnitX = LocalToWorld.GetUnitAxis(EAxis::X);
			const FVector UnitY = LocalToWorld.GetUnitAxis(EAxis::Y);
			const FVector UnitZ = LocalToWorld.GetUnitAxis(EAxis::Z);
			const FTransform UnitSeamRelative(
				SeamRelativeTransform.GetRotation(), SeamRelativeTransform.GetLocation(), FVector::OneVector);
			const FMatrix SeamToWorld = UnitSeamRelative.ToMatrixNoScale() * LocalToWorld;
			const FVector SeamX = SeamToWorld.GetUnitAxis(EAxis::X);
			const FVector SeamY = SeamToWorld.GetUnitAxis(EAxis::Y);
			const FVector SeamZ = SeamToWorld.GetUnitAxis(EAxis::Z);
			const bool bUseExpandedHitProxy = bExpandEditorHitProxy
				&& ViewFamily.EngineShowFlags.HitProxies;
			const float DrawTransitionThickness = bUseExpandedHitProxy
				? PortalDebugHitProxyThickness : TransitionLineThickness;
			const float DrawMouthThickness = bUseExpandedHitProxy
				? PortalDebugHitProxyThickness : MouthLineThickness;
			const float DrawSeamThickness = bUseExpandedHitProxy
				? PortalDebugHitProxyThickness : SeamLineThickness;

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if ((VisibilityMap & (1u << ViewIndex)) == 0)
				{
					continue;
				}

				FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
				DrawBoundary(PDI, Center, UnitX, UnitY, UnitZ,
					TransitionRadius, TransitionColor, DrawTransitionThickness);
				DrawBoundary(PDI, Center, UnitX, UnitY, UnitZ,
					MouthRadius, MouthColor, DrawMouthThickness);
				DrawBoundary(PDI, SeamToWorld.GetOrigin(), SeamX, SeamY, SeamZ,
					SeamRadius, SeamColor, DrawSeamThickness);
			}
		}

		FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View);
			Result.bDynamicRelevance = true;
			Result.bShadowRelevance = false;
			Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
			return Result;
		}

		uint32 GetMemoryFootprint() const override
		{
			return sizeof(*this) + GetAllocatedSize();
		}

		uint32 GetAllocatedSize() const
		{
			return FPrimitiveSceneProxy::GetAllocatedSize();
		}

	private:
		static void DrawBoundary(FPrimitiveDrawInterface* PDI, const FVector& Center,
			const FVector& XAxis, const FVector& YAxis, const FVector& ZAxis,
			const float Radius, const FColor& Color, const float Thickness)
		{
			DrawCircle(PDI, Center, XAxis, YAxis, Color, Radius,
				PortalDebugCircleSegments, SDPG_World, Thickness);
			DrawCircle(PDI, Center, XAxis, ZAxis, Color, Radius,
				PortalDebugCircleSegments, SDPG_World, Thickness);
			DrawCircle(PDI, Center, YAxis, ZAxis, Color, Radius,
				PortalDebugCircleSegments, SDPG_World, Thickness);
		}

		const float SeamRadius;
		const float MouthRadius;
		const float TransitionRadius;
		const FTransform SeamRelativeTransform;
		const bool bExpandEditorHitProxy;
	};

	return new FWPPortalDebugSceneProxy(this);
#else
	return nullptr;
#endif
}
