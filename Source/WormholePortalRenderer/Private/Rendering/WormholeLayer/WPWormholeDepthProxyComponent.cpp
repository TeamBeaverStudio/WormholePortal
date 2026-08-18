// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/WormholeLayer/WPWormholeDepthProxyComponent.h"

#include "Rendering/WormholeLayer/WPWormholeDepthProxyRenderData.h"

#include "DynamicMeshBuilder.h"
#include "Engine/CollisionProfile.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRelevance.h"
#include "MeshElementCollector.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveSceneProxyDesc.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "StaticMeshSceneProxy.h"
#include "StaticMeshSceneProxyDesc.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WPWormholeDepthProxyComponent)

namespace
{
	constexpr int32 WPWormholeDepthProxyCapSides = 64;
	constexpr double WPWormholeDepthProxyCapMinimumOffsetCm = 0.1;

	void AddWPWormholeNearClipDepthCap(const FSceneView& View, const FVector& SphereCenter, const double SphereRadiusCm, const FMaterialRenderProxy* MaterialRenderProxy, const int32 ViewIndex, FMeshElementCollector& Collector)
	{
		if (SphereRadiusCm <= UE_DOUBLE_SMALL_NUMBER || View.NearClippingDistance <= 0.0f) return;
		const FVector CameraOrigin = View.ViewMatrices.GetViewOrigin();
		const FVector CameraToCenter = SphereCenter - CameraOrigin;
		const double CameraDistanceSquared = CameraToCenter.SizeSquared();
		const double SphereRadiusSquared = FMath::Square(SphereRadiusCm);
		if (CameraDistanceSquared <= SphereRadiusSquared) return;

		const FVector ViewDirection = View.GetViewDirection().GetSafeNormal();
		const FVector ViewRight = View.GetViewRight().GetSafeNormal();
		const FVector ViewUp = View.GetViewUp().GetSafeNormal();
		if (ViewDirection.IsNearlyZero() || ViewRight.IsNearlyZero() || ViewUp.IsNearlyZero()) return;

		const double NearDistanceCm = static_cast<double>(View.NearClippingDistance) + FMath::Max(WPWormholeDepthProxyCapMinimumOffsetCm, static_cast<double>(View.NearClippingDistance) * 0.01);
		const FVector CapPlanePoint = CameraOrigin + ViewDirection * NearDistanceCm;
		const double CenterToPlaneSignedDistanceCm = FVector::DotProduct(SphereCenter - CapPlanePoint, ViewDirection);
		if (FMath::Abs(CenterToPlaneSignedDistanceCm) >= SphereRadiusCm) return;

		const FVector CapCenter = SphereCenter - ViewDirection * CenterToPlaneSignedDistanceCm;
		const double CapRadiusCm = FMath::Sqrt(FMath::Max(0.0, SphereRadiusSquared - FMath::Square(CenterToPlaneSignedDistanceCm)));
		if (CapRadiusCm <= UE_DOUBLE_SMALL_NUMBER) return;

		FDynamicMeshBuilder MeshBuilder(Collector.GetFeatureLevel());
		MeshBuilder.ReserveVertices(WPWormholeDepthProxyCapSides + 1);
		MeshBuilder.ReserveTriangles(WPWormholeDepthProxyCapSides);
		const FVector3f TangentX(ViewRight);
		const FVector3f TangentY(ViewUp);
		const FVector3f Normal(-ViewDirection);
		const int32 CenterVertexIndex = MeshBuilder.AddVertex(FVector3f(CapCenter), FVector2f(0.5f, 0.5f), TangentX, TangentY, Normal, FColor::White);
		for (int32 SideIndex = 0; SideIndex < WPWormholeDepthProxyCapSides; ++SideIndex)
		{
			const double Angle = UE_TWO_PI * static_cast<double>(SideIndex) / static_cast<double>(WPWormholeDepthProxyCapSides);
			const FVector RingDirection = ViewRight * FMath::Cos(Angle) + ViewUp * FMath::Sin(Angle);
			MeshBuilder.AddVertex(FVector3f(CapCenter + RingDirection * CapRadiusCm), FVector2f(static_cast<float>(0.5 + FMath::Cos(Angle) * 0.5), static_cast<float>(0.5 + FMath::Sin(Angle) * 0.5)), TangentX, TangentY, Normal, FColor::White);
		}
		for (int32 SideIndex = 0; SideIndex < WPWormholeDepthProxyCapSides; ++SideIndex)
		{
			MeshBuilder.AddTriangle(CenterVertexIndex, SideIndex + 1, (SideIndex + 1) % WPWormholeDepthProxyCapSides + 1);
		}
		MeshBuilder.GetMesh(FMatrix::Identity, MaterialRenderProxy, SDPG_World, true, false, ViewIndex, Collector);
	}

	FStaticMeshSceneProxyDesc MakeWPWormholeDepthProxyDesc(const UWPWormholeDepthProxyComponent* Component)
	{
		FStaticMeshSceneProxyDesc Desc(Component);
		// Static render mobility prevents DDM_AllOpaqueNoVelocity from deferring this depth-only primitive to a velocity pass.
		Desc.Mobility = EComponentMobility::Static;
		Desc.bUseAsOccluder = true;
		Desc.bSupportsWorldPositionOffsetVelocity = false;
		Desc.bRenderInDepthPass = true;
		Desc.bRenderInMainPass = false;
		return Desc;
	}

	bool IsSelectedForWPWormholeLayer(const FSceneView* View, const FPrimitiveComponentId PrimitiveId)
	{
		if (!View || !View->CustomRenderPass) return false;

		const FWPWormholeDepthProxyPassUserData* PassUserData = View->CustomRenderPass->GetUserDataTyped<FWPWormholeDepthProxyPassUserData>();
		return PassUserData && PassUserData->Contains(PrimitiveId);
	}

	class FWPWormholeDepthProxySceneProxy final : public FStaticMeshSceneProxy
	{
	public:
		explicit FWPWormholeDepthProxySceneProxy(const UWPWormholeDepthProxyComponent* Component)
			: FStaticMeshSceneProxy(MakeWPWormholeDepthProxyDesc(Component), false)
		{
			const UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
			check(DefaultMaterial);
			MaterialRenderProxy = DefaultMaterial->GetRenderProxy();
			MaterialRelevance = DefaultMaterial->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
		}

		virtual bool GetMeshElement(int32 LODIndex, int32 BatchIndex, int32 SectionIndex, uint8 InDepthPriorityGroup, bool bUseSelectionOutline, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const override
		{
			if (!FStaticMeshSceneProxy::GetMeshElement(LODIndex, BatchIndex, SectionIndex, InDepthPriorityGroup, bUseSelectionOutline, bAllowPreCulledIndices, OutMeshBatch)) return false;
			OutMeshBatch.bDisableBackfaceCulling = true;
			OutMeshBatch.bUseForDepthPass = true;
			return true;
		}

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, const uint32 VisibilityMap, FMeshElementCollector& Collector) const override
		{
			(void)ViewFamily;
			const FVector SphereCenter = GetLocalToWorld().GetOrigin();
			const double SphereRadiusCm = GetBounds().SphereRadius;
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if ((VisibilityMap & (1u << ViewIndex)) == 0 || !IsSelectedForWPWormholeLayer(Views[ViewIndex], GetPrimitiveComponentId())) continue;
				AddWPWormholeNearClipDepthCap(*Views[ViewIndex], SphereCenter, SphereRadiusCm, MaterialRenderProxy, ViewIndex, Collector);
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			if (!IsSelectedForWPWormholeLayer(View, GetPrimitiveComponentId()))
			{
				Result.bRenderInDepthPass = false;
				Result.bRenderInMainPass = false;
				return Result;
			}

			Result = FStaticMeshSceneProxy::GetViewRelevance(View);
			Result.bDrawRelevance = IsShown(View);
			Result.bStaticRelevance = true;
			Result.bDynamicRelevance = true;
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
			Result.bRenderInDepthPass = true;
			Result.bRenderInMainPass = false;
			Result.bShadowRelevance = false;
			Result.bVelocityRelevance = false;
			Result.bRenderCustomDepth = false;
			return Result;
		}

		virtual bool CanBeOccluded() const override { return true; }
		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	private:
		const FMaterialRenderProxy* MaterialRenderProxy = nullptr;
		FMaterialRelevance MaterialRelevance;
	};
}

UWPWormholeDepthProxyComponent::UWPWormholeDepthProxyComponent()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded()) SetStaticMesh(SphereMesh.Object);
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	SetCastShadow(false);
	SetReceivesDecals(false);
	SetRenderCustomDepth(false);
	SetVisibleInRayTracing(false);
	SetAffectDistanceFieldLighting(false);
	SetAffectDynamicIndirectLighting(false);
	SetRenderInMainPass(false);
	SetRenderInDepthPass(true);
	SetVisibleInSceneCaptureOnly(false);
	SetHiddenInSceneCapture(false);
	bVisibleInReflectionCaptures = false;
	bVisibleInRealTimeSkyCaptures = false;
	bUseAsOccluder = true;
	SetMobility(EComponentMobility::Movable);
}

FPrimitiveSceneProxy* UWPWormholeDepthProxyComponent::CreateSceneProxy()
{
	if (!GetStaticMesh() || !GetStaticMesh()->GetRenderData()) return nullptr;
	return new FWPWormholeDepthProxySceneProxy(this);
}

void UWPWormholeDepthProxyComponent::SetProxyRadiusCm(const double RadiusCm)
{
	const UStaticMesh* SphereMesh = GetStaticMesh();
	const double MeshRadiusCm = SphereMesh ? static_cast<double>(SphereMesh->GetBounds().SphereRadius) : 0.0;
	if (!FMath::IsFinite(RadiusCm) || RadiusCm <= UE_KINDA_SMALL_NUMBER || MeshRadiusCm <= UE_KINDA_SMALL_NUMBER) return;
	SetWorldScale3D(FVector(RadiusCm / MeshRadiusCm));
}
