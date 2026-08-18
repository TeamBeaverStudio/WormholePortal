// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Lighting/WPPortalLightShadowMath.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalLightSourceShadowMathTest,
	"WormholePortal.PortalLight.SourceShadowMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalLightSourceShadowMathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace WPPortalLightShadowMath;

	FRaySphereIntersection Intersection;
	TestTrue(TEXT("Outside source ray intersects portal sphere"), IntersectRaySphere(
		FVector3d(0.0, 0.0, -10.0), FVector3d::UnitZ(), FVector3d::ZeroVector, 1.0, Intersection));
	TestFalse(TEXT("Outside source is not inside portal sphere"), Intersection.bOriginInside);
	TestEqual(TEXT("Portal sphere near root"), Intersection.NearDistanceCm, 9.0);
	TestEqual(TEXT("Portal sphere far root"), Intersection.FarDistanceCm, 11.0);

	TestTrue(TEXT("Inside source still produces analytic roots"), IntersectRaySphere(
		FVector3d::ZeroVector, FVector3d::UnitX(), FVector3d::ZeroVector, 1.0, Intersection));
	TestTrue(TEXT("Inside source is marked unsupported for 2D capture"), Intersection.bOriginInside);
	TestEqual(TEXT("Inside source near root remains behind the ray origin"), Intersection.NearDistanceCm, -1.0);
	TestEqual(TEXT("Inside source far root remains in front"), Intersection.FarDistanceCm, 1.0);
	TestFalse(TEXT("Zero-radius portal sphere is invalid"), IntersectRaySphere(
		FVector3d::ZeroVector, FVector3d::UnitX(), FVector3d::ZeroVector, 0.0, Intersection));
	TestFalse(TEXT("Zero ray direction is invalid"), IntersectRaySphere(
		FVector3d::ZeroVector, FVector3d::ZeroVector, FVector3d::ZeroVector, 1.0, Intersection));

	FCaptureProjection Projection;
	TestTrue(TEXT("Forward portal ray projects into capture"), ProjectRayToCapture(
		FVector3d::UnitX(), 9.0,
		FVector3d::UnitX(), FVector3d::UnitY(), FVector3d::UnitZ(),
		1.0, Projection));
	TestEqual(TEXT("Capture center UV"), Projection.UV, FVector2d(0.5, 0.5));
	TestEqual(TEXT("Forward ray preserves distance as View-Z"), Projection.ViewZCm, 9.0);
	TestTrue(TEXT("Capture center lies inside frustum"), Projection.bInsideFrustum);

	const double ExpectedAngledForwardCosine = FMath::Sqrt(0.5);
	TestTrue(TEXT("Angled portal ray projects into capture"), ProjectRayToCapture(
		FVector3d(1.0, 1.0, 0.0), 10.0,
		FVector3d::UnitX(), FVector3d::UnitY(), FVector3d::UnitZ(),
		1.0, Projection));
	TestTrue(TEXT("Angled ray reports its forward cosine"),
		FMath::IsNearlyEqual(Projection.ForwardCosine, ExpectedAngledForwardCosine));
	TestTrue(TEXT("Angled ray distance converts to linear View-Z"),
		FMath::IsNearlyEqual(Projection.ViewZCm, 10.0 * ExpectedAngledForwardCosine));
	TestFalse(TEXT("Ray behind capture fails projection"), ProjectRayToCapture(
		-FVector3d::UnitX(), 9.0,
		FVector3d::UnitX(), FVector3d::UnitY(), FVector3d::UnitZ(),
		1.0, Projection));

	TestFalse(TEXT("Closer captured View-Z blocks source light"),
		IsSourceVisibleFromViewZ(8.0, 9.0, 0.5));
	TestTrue(TEXT("Captured View-Z on the bias boundary remains visible"),
		IsSourceVisibleFromViewZ(8.0, 9.0, 1.0));
	TestTrue(TEXT("Captured View-Z behind the portal remains visible"),
		IsSourceVisibleFromViewZ(10.0, 9.0, 0.5));
	TestTrue(TEXT("Invalid captured View-Z fails open"),
		IsSourceVisibleFromViewZ(0.0, 9.0, 0.5));

	TestEqual(TEXT("Small portal selects 128 tier"), SelectAdaptiveResolution(127.0), 128);
	TestEqual(TEXT("128-pixel boundary selects 256 tier"), SelectAdaptiveResolution(128.0), 256);
	TestEqual(TEXT("512-pixel boundary remains in 256 tier"), SelectAdaptiveResolution(512.0), 256);
	TestEqual(TEXT("Large portal selects 512 tier"), SelectAdaptiveResolution(513.0), 512);
	TestEqual(TEXT("Off-screen portal selects minimum tier"), SelectAdaptiveResolution(-1.0), 128);

	TArray<FCaptureCandidate> Candidates;
	FCaptureCandidate& LargeVisibleCandidate = Candidates.AddDefaulted_GetRef();
	LargeVisibleCandidate.ScreenDiameterPixels = 500.0;
	LargeVisibleCandidate.LastCaptureTimeSeconds = 1.0;

	FCaptureCandidate& InitialCandidate = Candidates.AddDefaulted_GetRef();
	InitialCandidate.bNeedsInitialCapture = true;
	InitialCandidate.ScreenDiameterPixels = 10.0;
	InitialCandidate.LastCaptureTimeSeconds = 100.0;

	FCaptureCandidate& TransformDirtyCandidate = Candidates.AddDefaulted_GetRef();
	TransformDirtyCandidate.bTransformDirty = true;
	TransformDirtyCandidate.ScreenDiameterPixels = 20.0;
	TransformDirtyCandidate.LastCaptureTimeSeconds = 50.0;

	FCaptureCandidate& NotDueCandidate = Candidates.AddDefaulted_GetRef();
	NotDueCandidate.bCaptureDue = false;
	NotDueCandidate.bNeedsInitialCapture = true;
	NotDueCandidate.bTransformDirty = true;
	NotDueCandidate.ScreenDiameterPixels = 1000.0;

	TArray<int32> SelectedCandidateIndices;
	SelectCaptureCandidateIndices(Candidates, 2, SelectedCandidateIndices);
	TestEqual(TEXT("Capture budget limits selected routes"), SelectedCandidateIndices.Num(), 2);
	if (SelectedCandidateIndices.Num() == 2)
	{
		TestEqual(TEXT("Initial capture receives first priority"), SelectedCandidateIndices[0], 1);
		TestEqual(TEXT("Dirty transform receives second priority"), SelectedCandidateIndices[1], 2);
	}

	SelectCaptureCandidateIndices(Candidates, 0, SelectedCandidateIndices);
	TestTrue(TEXT("Zero capture budget selects no routes"), SelectedCandidateIndices.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
