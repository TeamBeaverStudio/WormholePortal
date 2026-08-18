// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Math/WPPortalMaskMath.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

#include "WPPortalVisibilityMath.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPProductionPortalMaskMathTest,
	"WormholePortal.Production.AnalyticProxyMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPProductionPortalMaskMathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace WPPortalMaskMath;
	TestEqual(TEXT("Authored outer radius remains the proxy radius"),
		WPPortalVisibilityMath::GetSafeProxyRadiusCm(500.0, 800.0), 800.0);
	TestEqual(TEXT("Degenerate authored transition receives the analytic 1 cm safety shell"),
		WPPortalVisibilityMath::GetSafeProxyRadiusCm(500.0, 500.0), 501.0);
	TestEqual(TEXT("Coverage carrier keeps the five-percent near-plane shell"),
		GetCoverageCarrierRadiusCm(800.0), 840.0);

	FIntersection Hit;
	TestTrue(TEXT("Outside camera intersects proxy"), IntersectProxySphere(
		FVector3d(0.0, 0.0, -10.0), FVector3d::UnitZ(), FVector3d::ZeroVector, 2.0, Hit));
	TestFalse(TEXT("Outside camera selects near root"), Hit.bCameraInside);
	TestEqual(TEXT("Outside near root"), Hit.NearDistanceCm, 8.0);
	TestEqual(TEXT("Outside far root"), Hit.FarDistanceCm, 12.0);
	TestEqual(TEXT("Outside selected root"), Hit.SelectedDistanceCm, 8.0);
	TestEqual(TEXT("Outside coverage root"), Hit.CoverageDistanceCm, 8.0);
	TestFalse(TEXT("Far outside camera is outside carrier"), Hit.bCameraInsideCarrier);

	TestTrue(TEXT("Inside camera intersects proxy"), IntersectProxySphere(
		FVector3d::ZeroVector, FVector3d::UnitX(), FVector3d::ZeroVector, 2.0, Hit));
	TestTrue(TEXT("Inside camera selects far root"), Hit.bCameraInside);
	TestEqual(TEXT("Inside selected root"), Hit.SelectedDistanceCm, 2.0);
	TestTrue(TEXT("Inside camera is inside carrier"), Hit.bCameraInsideCarrier);
	TestEqual(TEXT("Inside coverage selects far root"), Hit.CoverageDistanceCm, 2.0);

	TestTrue(TEXT("Carrier shell camera still intersects proxy"), IntersectProxySphere(
		FVector3d(0.0, 0.0, -2.05), FVector3d::UnitZ(), FVector3d::ZeroVector, 2.0, Hit));
	TestFalse(TEXT("Carrier shell camera remains outside analytic proxy"), Hit.bCameraInside);
	TestTrue(TEXT("Carrier shell switches raster coverage to far root"), Hit.bCameraInsideCarrier);
	TestTrue(TEXT("Carrier shell analytic root remains near"),
		FMath::IsNearlyEqual(Hit.SelectedDistanceCm, 0.05, 1.0e-9));
	TestTrue(TEXT("Carrier shell coverage root is far"),
		FMath::IsNearlyEqual(Hit.CoverageDistanceCm, 4.05, 1.0e-9));

	TestTrue(TEXT("Tangent ray is retained"), IntersectProxySphere(
		FVector3d(2.0, 0.0, -10.0), FVector3d::UnitZ(), FVector3d::ZeroVector, 2.0, Hit));
	TestEqual(TEXT("Tangent selected root"), Hit.SelectedDistanceCm, 10.0);
	TestFalse(TEXT("Miss is rejected"), IntersectProxySphere(
		FVector3d(3.0, 0.0, -10.0), FVector3d::UnitZ(), FVector3d::ZeroVector, 2.0, Hit));
	TestFalse(TEXT("Sphere fully behind ray is rejected"), IntersectProxySphere(
		FVector3d::ZeroVector, FVector3d::UnitZ(), FVector3d(0.0, 0.0, -10.0), 2.0, Hit));
	TestFalse(TEXT("Exact outer boundary follows analytic epsilon rejection"), IntersectProxySphere(
		FVector3d(0.0, 0.0, -2.0), FVector3d::UnitZ(), FVector3d::ZeroVector, 2.0, Hit));

	TestFalse(TEXT("Zero radius is rejected"), IntersectProxySphere(
		FVector3d::ZeroVector, FVector3d::UnitZ(), FVector3d::ZeroVector, 0.0, Hit));
	TestFalse(TEXT("Zero direction is rejected"), IntersectProxySphere(
		FVector3d::ZeroVector, FVector3d::ZeroVector, FVector3d::ZeroVector, 2.0, Hit));
	TestFalse(TEXT("NaN center is rejected"), IntersectProxySphere(
		FVector3d::ZeroVector, FVector3d::UnitZ(),
		FVector3d(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0), 2.0, Hit));

	TestTrue(TEXT("Opaque geometry in front rejects proxy"),
		IsOccludedByOpaqueDepth(false, 8.0, 10.0, 1.0));
	TestFalse(TEXT("Depth exactly on bias boundary remains visible"),
		IsOccludedByOpaqueDepth(false, 9.0, 10.0, 1.0));
	TestFalse(TEXT("Opaque geometry behind proxy remains visible"),
		IsOccludedByOpaqueDepth(false, 12.0, 10.0, 1.0));
	TestFalse(TEXT("Inside proxy intentionally bypasses opaque rejection"),
		IsOccludedByOpaqueDepth(true, 1.0, 10.0, 1.0));

	TestTrue(TEXT("Coverage in front of the reversed-Z near plane is rejected"),
		IsCoverageRejectedByNearPlaneClip(1.01, 1.0));
	TestFalse(TEXT("Coverage exactly on the near plane remains valid"),
		IsCoverageRejectedByNearPlaneClip(1.0, 1.0));
	TestFalse(TEXT("Coverage behind the near plane remains valid"),
		IsCoverageRejectedByNearPlaneClip(0.5, 1.0));
	TestTrue(TEXT("Coverage behind the camera is rejected"),
		IsCoverageRejectedByNearPlaneClip(0.0, 0.0));
	TestTrue(TEXT("Proxy intersects near plane at the coverage carrier-near position"),
		DoesProxySphereIntersectNearPlane(805.0, 10.0, 800.0));
	TestFalse(TEXT("Proxy clears near plane in the carrier-only shell"),
		DoesProxySphereIntersectNearPlane(820.0, 10.0, 800.0));
	TestTrue(TEXT("Off-axis proxy uses plane-normal distance instead of radial distance"),
		DoesProxySphereIntersectNearPlane(10.0, 10.0, 800.0));
	TestFalse(TEXT("Proxy fully behind the camera does not reach the near plane"),
		DoesProxySphereIntersectNearPlane(-805.0, 10.0, 800.0));

	const FVector2d StableViewportUV(0.25, 0.75);
	const FVector2d CompensatedViewportUV = GetTemporalJitterCompensatedViewportUV(
		StableViewportUV, FVector2d(0.02, -0.04));
	TestTrue(TEXT("Temporal jitter compensation X follows UE post-TSR convention"),
		FMath::IsNearlyEqual(CompensatedViewportUV.X, 0.26, 1.0e-12));
	TestTrue(TEXT("Temporal jitter compensation Y follows UE post-TSR convention"),
		FMath::IsNearlyEqual(CompensatedViewportUV.Y, 0.77, 1.0e-12));
	TestEqual(TEXT("Zero temporal jitter preserves stable viewport UV"),
		GetTemporalJitterCompensatedViewportUV(StableViewportUV, FVector2d::ZeroVector),
		StableViewportUV);

	TestEqual(TEXT("SceneColor array slice has priority for SceneDepth"),
		ResolveSceneDepthArraySlice(true, 1, 0, 2), 1);
	TestEqual(TEXT("Stereo index resolves SceneDepth when SceneColor is 2D"),
		ResolveSceneDepthArraySlice(false, 0, 1, 2), 1);
	TestEqual(TEXT("Single-slice depth array safely falls back to slice zero"),
		ResolveSceneDepthArraySlice(false, 0, INDEX_NONE, 1), 0);
	TestEqual(TEXT("Ambiguous multi-eye depth array fails closed"),
		ResolveSceneDepthArraySlice(false, 0, INDEX_NONE, 2), INDEX_NONE);
	TestEqual(TEXT("Out-of-range stereo slice fails closed"),
		ResolveSceneDepthArraySlice(false, 0, 2, 2), INDEX_NONE);

	constexpr double LargeWorldOffset = 100000000.0;
	const FVector3d PreViewTranslation(-LargeWorldOffset, -LargeWorldOffset, -LargeWorldOffset);
	const FVector3d LargeCameraWorld(LargeWorldOffset, LargeWorldOffset, LargeWorldOffset - 10.0);
	const FVector3d LargeCenterWorld(LargeWorldOffset, LargeWorldOffset, LargeWorldOffset);
	FIntersection LargeWorldHit;
	TestTrue(TEXT("Translated LWC placement intersects"), IntersectProxySphere(
		LargeCameraWorld + PreViewTranslation, FVector3d::UnitZ(),
		LargeCenterWorld + PreViewTranslation, 2.0, LargeWorldHit));
	TestEqual(TEXT("Translated LWC near root matches origin"),
		LargeWorldHit.SelectedDistanceCm, 8.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
