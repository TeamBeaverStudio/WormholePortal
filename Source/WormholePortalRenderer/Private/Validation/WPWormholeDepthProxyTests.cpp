// Copyright 2026 Team Beaver. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Rendering/WormholeLayer/WPWormholeDepthProxyComponent.h"
#include "Rendering/WormholeLayer/WPWormholeDepthProxyRenderData.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWPWormholeDepthProxyPassMarkerContractTest, "WormholePortal.Renderer.WormholeDepthProxy.PassMarkerContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPWormholeDepthProxyPassMarkerContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FPrimitiveComponentId SelectedId;
	SelectedId.PrimIDValue = 101;
	FPrimitiveComponentId OtherId;
	OtherId.PrimIDValue = 102;
	TArray<FPrimitiveComponentId> SelectedIds;
	SelectedIds.Add(SelectedId);
	FWPWormholeDepthProxyPassUserData UserData(MoveTemp(SelectedIds));
	TestTrue(TEXT("Selected primitive is accepted"), UserData.Contains(SelectedId));
	TestFalse(TEXT("Unselected primitive is rejected"), UserData.Contains(OtherId));
	TestFalse(TEXT("Invalid primitive ID is rejected"), UserData.Contains(FPrimitiveComponentId{}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWPWormholeDepthProxyComponentPolicyContractTest, "WormholePortal.Renderer.WormholeDepthProxy.ComponentPolicyContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPWormholeDepthProxyComponentPolicyContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWPWormholeDepthProxyComponent* Component = NewObject<UWPWormholeDepthProxyComponent>(GetTransientPackage());
	TestNotNull(TEXT("Production component can be constructed"), Component);
	if (!Component) return false;

	TestEqual(TEXT("Collision is disabled"), Component->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestFalse(TEXT("Main/base pass is disabled"), Component->bRenderInMainPass);
	TestTrue(TEXT("Depth pass is enabled"), Component->bRenderInDepthPass);
	TestTrue(TEXT("Occluder hint is enabled"), Component->bUseAsOccluder);
	TestFalse(TEXT("Custom depth is disabled"), Component->bRenderCustomDepth);
	TestFalse(TEXT("Shadows are disabled"), Component->CastShadow);
	TestFalse(TEXT("Ray tracing is disabled"), Component->bVisibleInRayTracing);
	TestFalse(TEXT("Ticking is disabled"), Component->PrimaryComponentTick.bCanEverTick);

	constexpr double RadiusCm = 1500.0;
	Component->SetProxyRadiusCm(RadiusCm);
	const UStaticMesh* SphereMesh = Component->GetStaticMesh();
	TestNotNull(TEXT("Cached engine sphere mesh is assigned"), SphereMesh);
	if (!SphereMesh) return false;
	const double ScaledRadiusCm = static_cast<double>(SphereMesh->GetBounds().SphereRadius) * Component->GetComponentScale().GetAbsMax();
	TestTrue(TEXT("Cached sphere scale follows the analytic proxy radius"), FMath::IsNearlyEqual(ScaledRadiusCm, RadiusCm, RadiusCm * 0.01));
	return true;
}

#endif
