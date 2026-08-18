// Copyright 2026 Team Beaver. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/WPPortalAudioMath.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalAudioCenterPathTest,
	"WormholePortal.PortalAudio.Math.CenterPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalAudioCenterPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	WPPortalAudioMath::FSphericalPortalPath Path;
	const bool bResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
		FVector(-300.0, 0.0, 0.0),
		FVector(300.0, 0.0, 0.0),
		FVector::ZeroVector,
		100.0,
		Path);

	TestTrue(TEXT("Center path resolves"), bResolved);
	TestEqual(TEXT("Source to entry distance"), Path.SourceToEntryDistance, 200.0);
	TestEqual(TEXT("Exit to listener distance"), Path.ExitToListenerDistance, 200.0);
	TestEqual(TEXT("Portal interior is excluded from logical distance"), Path.LogicalDistance, 400.0);
	TestTrue(TEXT("Incoming direction points toward the entry sphere"),
		Path.SourceDirection.Equals(FVector::XAxisVector, UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Outgoing direction points from the exit sphere to the listener"),
		Path.ExitDirection.Equals(FVector::XAxisVector, UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Virtual entry point is on the source-facing surface"),
		Path.VirtualEntrySurfacePoint.Equals(FVector(-100.0, 0.0, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Exit point is on the listener-facing surface"),
		Path.ExitSurfacePoint.Equals(FVector(100.0, 0.0, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Effective proxy preserves direction and logical distance"),
		Path.EffectiveProxyLocation.Equals(FVector(-100.0, 0.0, 0.0), UE_DOUBLE_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalAudioOmnidirectionalPathTest,
	"WormholePortal.PortalAudio.Math.OmnidirectionalPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalAudioOmnidirectionalPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	WPPortalAudioMath::FSphericalPortalPath Path;

	// 이 선분은 구를 빗나가지만 Exit 구면의 재방사는 listener 방향으로 계속 유효해야 합니다.
	const bool bSideResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
		FVector(-300.0, 0.0, 0.0),
		FVector(300.0, 400.0, 0.0),
		FVector::ZeroVector,
		100.0,
		Path);

	TestTrue(TEXT("A listener outside the former aperture cone resolves"), bSideResolved);
	TestEqual(TEXT("Side listener source leg"), Path.SourceToEntryDistance, 200.0);
	TestEqual(TEXT("Side listener exit leg"), Path.ExitToListenerDistance, 400.0);
	TestEqual(TEXT("Side listener logical distance"), Path.LogicalDistance, 600.0);
	TestTrue(TEXT("Side listener exit direction is radial"),
		Path.ExitDirection.Equals(FVector(0.6, 0.8, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Side listener exit point follows the listener"),
		Path.ExitSurfacePoint.Equals(FVector(60.0, 80.0, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Side listener proxy keeps the full logical distance"),
		Path.EffectiveProxyLocation.Equals(FVector(-60.0, -80.0, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Proxy-to-listener distance equals the two portal legs"),
		FMath::IsNearlyEqual(
			FVector::Distance(Path.EffectiveProxyLocation, FVector(300.0, 400.0, 0.0)),
			Path.LogicalDistance,
			UE_DOUBLE_SMALL_NUMBER));

	// Source와 listener가 같은 쪽에 있어도 구면 전체가 재방사하므로 경로가 유지됩니다.
	const bool bSameSideResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
		FVector(-300.0, 0.0, 0.0),
		FVector(-300.0, 0.0, 0.0),
		FVector::ZeroVector,
		100.0,
		Path);

	TestTrue(TEXT("A listener on the same side of the sphere resolves"), bSameSideResolved);
	TestTrue(TEXT("Same-side exit point is listener-facing"),
		Path.ExitSurfacePoint.Equals(FVector(-100.0, 0.0, 0.0), UE_DOUBLE_SMALL_NUMBER));
	TestTrue(TEXT("Same-side proxy still points back toward the portal"),
		Path.EffectiveProxyLocation.Equals(FVector(100.0, 0.0, 0.0), UE_DOUBLE_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalAudioAngularSweepTest,
	"WormholePortal.PortalAudio.Math.AngularSweep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalAudioAngularSweepTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FVector VirtualSource(-300.0, 0.0, 0.0);
	constexpr double ListenerDistance = 300.0;
	constexpr double PortalRadius = 100.0;

	for (int32 Step = 0; Step < 8; ++Step)
	{
		const double Angle = UE_TWO_PI * static_cast<double>(Step) / 8.0;
		const FVector Listener(
			FMath::Cos(Angle) * ListenerDistance,
			FMath::Sin(Angle) * ListenerDistance,
			0.0);

		WPPortalAudioMath::FSphericalPortalPath Path;
		const bool bResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
			VirtualSource,
			Listener,
			FVector::ZeroVector,
			PortalRadius,
			Path);

		TestTrue(*FString::Printf(TEXT("Angular step %d resolves"), Step), bResolved);
		if (!bResolved)
		{
			continue;
		}

		TestTrue(*FString::Printf(TEXT("Angular step %d exit point stays on the sphere"), Step),
			FMath::IsNearlyEqual(Path.ExitSurfacePoint.Size(), PortalRadius, 1.0e-9));
		TestTrue(*FString::Printf(TEXT("Angular step %d preserves logical distance"), Step),
			FMath::IsNearlyEqual(
				FVector::Distance(Path.EffectiveProxyLocation, Listener),
				Path.LogicalDistance,
				1.0e-9));
		TestTrue(*FString::Printf(TEXT("Angular step %d sounds from the portal direction"), Step),
			(Listener - Path.EffectiveProxyLocation).GetSafeNormal().Equals(
				Path.ExitDirection,
				1.0e-9));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalAudioLargeWorldPathTest,
	"WormholePortal.PortalAudio.Math.LargeWorldPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalAudioLargeWorldPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FVector PortalCenter(1000000000.0, -2000000000.0, 3000000000.0);
	const FVector VirtualSource = PortalCenter + FVector(-1000000.0, 0.0, 0.0);
	const FVector Listener = PortalCenter + FVector(0.0, 1000000.0, 0.0);

	WPPortalAudioMath::FSphericalPortalPath Path;
	const bool bResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
		VirtualSource,
		Listener,
		PortalCenter,
		50.0,
		Path);

	TestTrue(TEXT("A sideways path remains stable at large world coordinates"), bResolved);
	TestTrue(TEXT("Large-world source leg is radial"),
		FMath::IsNearlyEqual(Path.SourceToEntryDistance, 999950.0, 1.0e-6));
	TestTrue(TEXT("Large-world exit leg is radial"),
		FMath::IsNearlyEqual(Path.ExitToListenerDistance, 999950.0, 1.0e-6));
	TestTrue(TEXT("Large-world logical distance combines both legs"),
		FMath::IsNearlyEqual(Path.LogicalDistance, 1999900.0, 1.0e-6));
	TestTrue(TEXT("Large-world virtual entry point is stable"),
		Path.VirtualEntrySurfacePoint.Equals(
			PortalCenter + FVector(-50.0, 0.0, 0.0),
			1.0e-6));
	TestTrue(TEXT("Large-world exit point is stable"),
		Path.ExitSurfacePoint.Equals(
			PortalCenter + FVector(0.0, 50.0, 0.0),
			1.0e-6));
	TestTrue(TEXT("Large-world proxy preserves the combined distance"),
		Path.EffectiveProxyLocation.Equals(
			PortalCenter + FVector(0.0, -999900.0, 0.0),
			1.0e-6));
	TestTrue(TEXT("Large-world proxy-to-listener distance remains stable"),
		FMath::IsNearlyEqual(
			FVector::Distance(Path.EffectiveProxyLocation, Listener),
			Path.LogicalDistance,
			1.0e-6));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPortalAudioBoundaryPolicyTest,
	"WormholePortal.PortalAudio.Math.BoundaryPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPortalAudioBoundaryPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	WPPortalAudioMath::FSphericalPortalPath Path;

	TestFalse(TEXT("A listener inside the seam is rejected"),
		WPPortalAudioMath::ResolveSphericalPortalPath(
			FVector(-300.0, 0.0, 0.0),
			FVector::ZeroVector,
			FVector::ZeroVector,
			100.0,
			Path));

	TestFalse(TEXT("A source inside the seam is rejected"),
		WPPortalAudioMath::ResolveSphericalPortalPath(
			FVector::ZeroVector,
			FVector(300.0, 0.0, 0.0),
			FVector::ZeroVector,
			100.0,
			Path));

	TestFalse(TEXT("The one-centimeter seam tolerance is rejected"),
		WPPortalAudioMath::ResolveSphericalPortalPath(
			FVector(-101.0, 0.0, 0.0),
			FVector(300.0, 0.0, 0.0),
			FVector::ZeroVector,
			100.0,
			Path));

	TestTrue(TEXT("A point clearly outside the seam tolerance resolves"),
		WPPortalAudioMath::ResolveSphericalPortalPath(
			FVector(-102.0, 0.0, 0.0),
			FVector(300.0, 0.0, 0.0),
			FVector::ZeroVector,
			100.0,
			Path));

	TestFalse(TEXT("A zero portal radius is rejected"),
		WPPortalAudioMath::ResolveSphericalPortalPath(
			FVector(-300.0, 0.0, 0.0),
			FVector(300.0, 0.0, 0.0),
			FVector::ZeroVector,
			0.0,
			Path));

	return true;
}

#endif
