// Copyright 2026 Team Beaver. All Rights Reserved.

/**
 * @file WPRuntimeCaptureScheduler.inl
 *
 * Implements FWPRuntimeCaptureScheduler. Policy helpers and CVars remain private to the
 * Runtime
 * translation unit, while an independent Scheduler object owns capture state and
 * behavior.
 */
namespace
{
	struct FWPMetricFacePredictionResult
	{
		uint8 LocalFaceMask = 0;
		uint8 LinkedFaceMask = 0;
		int32 RayCount = 0;
		int32 ScreenRejectedRayCount = 0;
		int32 BlockedRayCount = 0;
		int32 OpenRayCount = 0;
		int32 LUTEvaluationCount = 0;
		bool bFailOpen = false;
		const TCHAR* Reason = TEXT("Predicted");
	};

	struct FWPCPUOpticalRayResult
	{
		FVector BentDirection = FVector::ForwardVector;
		FVector LinkedDirection = FVector::ForwardVector;
		double Impact = 0.0;
		double CriticalImpact = 0.0;
		bool bValid = false;
		bool bConservativeAllFaces = false;
	};

	FLinearColor SampleWPCPUVolumeTrilinear(
		const FWPLUTVolumeData& Volume,
		const FVector3f& LogicalUVW)
	{
		const FIntVector Size = Volume.Dimensions;
		const FVector3f Coordinate(
			FMath::Clamp(LogicalUVW.X, 0.0f, 1.0f) * FMath::Max(Size.X - 1, 0),
			FMath::Clamp(LogicalUVW.Y, 0.0f, 1.0f) * FMath::Max(Size.Y - 1, 0),
			FMath::Clamp(LogicalUVW.Z, 0.0f, 1.0f) * FMath::Max(Size.Z - 1, 0));
		const FIntVector I0(
			FMath::FloorToInt(Coordinate.X),
			FMath::FloorToInt(Coordinate.Y),
			FMath::FloorToInt(Coordinate.Z));
		const FIntVector I1(
			FMath::Min(I0.X + 1, Size.X - 1),
			FMath::Min(I0.Y + 1, Size.Y - 1),
			FMath::Min(I0.Z + 1, Size.Z - 1));
		const FVector3f Alpha(
			Coordinate.X - I0.X, Coordinate.Y - I0.Y, Coordinate.Z - I0.Z);
		const auto Read = [&Volume, &Size](const int32 X, const int32 Y, const int32 Z)
		{
			return Volume.Voxels[(Z * Size.Y + Y) * Size.X + X];
		};
		const FLinearColor C00 = FMath::Lerp(Read(I0.X, I0.Y, I0.Z), Read(I1.X, I0.Y, I0.Z), Alpha.X);
		const FLinearColor C10 = FMath::Lerp(Read(I0.X, I1.Y, I0.Z), Read(I1.X, I1.Y, I0.Z), Alpha.X);
		const FLinearColor C01 = FMath::Lerp(Read(I0.X, I0.Y, I1.Z), Read(I1.X, I0.Y, I1.Z), Alpha.X);
		const FLinearColor C11 = FMath::Lerp(Read(I0.X, I1.Y, I1.Z), Read(I1.X, I1.Y, I1.Z), Alpha.X);
		return FMath::Lerp(
			FMath::Lerp(C00, C10, Alpha.Y),
			FMath::Lerp(C01, C11, Alpha.Y),
			Alpha.Z);
	}

	double WPRaisedCosineCoordinate(const double Impact01)
	{
		return FMath::Acos(FMath::Clamp(1.0 - 2.0 * FMath::Clamp(Impact01, 0.0, 1.0), -1.0, 1.0))
			/ PI;
	}

	bool EvaluateWPCPUOpticalRay(
		const FVector& RayWSInput,
		const FVector& CameraOffsetWS,
		const double PortalRadiusCm,
		const double ThroatLengthCm,
		const double ProxyRadiusCm,
		const FWPLUTEndpointSnapshot& LUT,
		const FVector& SelfX,
		const FVector& SelfY,
		const FVector& SelfZ,
		const FVector& LinkedX,
		const FVector& LinkedY,
		const FVector& LinkedZ,
		const FVector& ViewRightWS,
		const FVector& ViewUpWS,
		FWPCPUOpticalRayResult& OutResult)
	{
		constexpr double Epsilon = 1.0e-6;
		constexpr double CriticalTolerance = 4.0e-7;
		OutResult = FWPCPUOpticalRayResult();
		const FVector RayWS = RayWSInput.GetSafeNormal();
		const double SafeThroatRadius = FMath::Max(PortalRadiusCm, Epsilon);
		const double SafeProxyRadius = FMath::Max(ProxyRadiusCm, SafeThroatRadius + 1.0);
		const double OuterProperEll = FMath::Max(SafeProxyRadius - SafeThroatRadius, Epsilon);
		const double MouthProperEll = FMath::Min(
			FMath::Max(ThroatLengthCm * 0.5, 0.0), OuterProperEll);
		const double TransitionProperLength = FMath::Max(
			OuterProperEll - MouthProperEll, Epsilon);
		const double MouthEll01 = FMath::Clamp(MouthProperEll / OuterProperEll, 0.0, 1.0);
		const double CameraRadius = CameraOffsetWS.Length();
		const bool bInsideProxy = CameraRadius < SafeProxyRadius;
		const double ProjectedCenter = FVector::DotProduct(CameraOffsetWS, RayWS);
		const double SphereDiscriminant = ProjectedCenter * ProjectedCenter
			- (CameraOffsetWS.SquaredLength() - SafeProxyRadius * SafeProxyRadius);
		if (!FMath::IsFinite(SphereDiscriminant) || SphereDiscriminant < 0.0)
		{
			return false;
		}
		const double SphereRoot = FMath::Sqrt(FMath::Max(SphereDiscriminant, 0.0));
		const double AnalyticProxyDistance = bInsideProxy
			? -ProjectedCenter + SphereRoot : -ProjectedCenter - SphereRoot;
		if (!FMath::IsFinite(AnalyticProxyDistance) || AnalyticProxyDistance <= 1.0e-4)
		{
			return false;
		}

		const FVector SurfaceRadialWS =
			(CameraOffsetWS + RayWS * AnalyticProxyDistance).GetSafeNormal(
				UE_SMALL_NUMBER, FVector::UpVector);
		const FVector CameraRadialWS = CameraRadius > 1.0e-4
			? CameraOffsetWS / CameraRadius : SurfaceRadialWS;
		const FVector RadialWS = (bInsideProxy ? CameraRadialWS : SurfaceRadialWS)
			.GetSafeNormal(UE_SMALL_NUMBER, SurfaceRadialWS);
		const double IncidenceCos = FMath::Clamp(
			FVector::DotProduct(RayWS, RadialWS), -1.0, 1.0);
		const FVector Tangent = RayWS - IncidenceCos * RadialWS;
		const double TangentLengthSquared = Tangent.SquaredLength();
		const double Impact = FMath::Sqrt(FMath::Max(0.0, 1.0 - IncidenceCos * IncidenceCos));
		const double CameraProperEll01 = FMath::Clamp(
			(CameraRadius - SafeThroatRadius) / OuterProperEll, 0.0, 1.0);
		const double RayStartEll01 = bInsideProxy ? CameraProperEll01 : 1.0;
		const double StartProperEll = RayStartEll01 * OuterProperEll;
		const double RayStartTransition01 = FMath::Clamp(
			(StartProperEll - MouthProperEll) / TransitionProperLength, 0.0, 1.0);
		const bool bInsideOutward = bInsideProxy && IncidenceCos >= 0.0;

		FLinearColor GlobalSample(0.0f, 0.0f, 1.0f, 3.0f);
		FLinearColor InwardSample(0.0f, 0.0f, 1.0f, 3.0f);
		const bool bUseLUT = !LUT.bAnalyticNoTransition;
		if (bUseLUT)
		{
			if (!LUT.CPUVolumeData.IsValid() || !LUT.CPUVolumeData->IsValid())
			{
				return false;
			}
			GlobalSample = SampleWPCPUVolumeTrilinear(
				*LUT.CPUVolumeData,
				FVector3f(
					static_cast<float>(WPRaisedCosineCoordinate(Impact)),
					static_cast<float>(RayStartTransition01),
					LUT.RatioCoordinate01));
		}

		const double CriticalImpact = FMath::Clamp(static_cast<double>(GlobalSample.B), 0.0, 1.0);
		const bool bInwardTrapped = !bInsideOutward
			&& FMath::Abs(Impact - CriticalImpact) <= CriticalTolerance;
		const bool bOutwardTrapped = bInsideOutward
			&& RayStartEll01 <= MouthEll01 + CriticalTolerance
			&& Impact >= 1.0 - CriticalTolerance;
		const bool bRemoteBranch = Impact < CriticalImpact;
		const double RemoteFraction = FMath::Clamp(
			Impact / FMath::Max(CriticalImpact, 1.0e-8), 0.0, 1.0);
		const double LocalFraction = FMath::Clamp(
			(Impact - CriticalImpact) / FMath::Max(1.0 - CriticalImpact, 1.0e-8), 0.0, 1.0);
		const double BranchFraction = bRemoteBranch ? RemoteFraction : LocalFraction;
		double InwardLogicalU = bRemoteBranch
			? 0.5 * WPRaisedCosineCoordinate(BranchFraction)
			: 0.5 + 0.5 * WPRaisedCosineCoordinate(BranchFraction);
		if (bUseLUT)
		{
			const double LUTWidth = LUT.CPUVolumeData->Dimensions.X;
			const double HalfImpactSamples = FMath::FloorToDouble(0.5 * LUTWidth);
			const double Denominator = FMath::Max(LUTWidth - 1.0, 1.0);
			const double RemoteMaxU = FMath::Max(HalfImpactSamples - 1.0, 0.0) / Denominator;
			const double LocalMinU = HalfImpactSamples / Denominator;
			InwardLogicalU = bRemoteBranch
				? FMath::Min(InwardLogicalU, RemoteMaxU)
				: FMath::Max(InwardLogicalU, LocalMinU);
			InwardSample = SampleWPCPUVolumeTrilinear(
				*LUT.CPUVolumeData,
				FVector3f(
					static_cast<float>(InwardLogicalU),
					static_cast<float>(RayStartTransition01),
					LUT.RatioCoordinate01));
		}

		const int32 GlobalValidity = FMath::FloorToInt(GlobalSample.A + 0.5f);
		const int32 InwardValidity = FMath::FloorToInt(InwardSample.A + 0.5f);
		const bool bSelectedLUTValid = bInsideOutward
			? (GlobalValidity & 2) != 0 : (InwardValidity & 1) != 0;
		const bool bTrappedCritical = bInwardTrapped || bOutwardTrapped || !bSelectedLUTValid;
		const bool bUseInwardThroat = !bInsideOutward && bRemoteBranch && !bTrappedCritical;
		const bool bUseOutwardThroat = bInsideOutward
			&& StartProperEll < MouthProperEll && !bTrappedCritical;
		const double TraversedThroatLength =
			(bUseInwardThroat ? MouthProperEll + FMath::Min(StartProperEll, MouthProperEll) : 0.0)
			+ (bUseOutwardThroat ? FMath::Max(MouthProperEll - StartProperEll, 0.0) : 0.0);
		const double ThroatQ = bUseOutwardThroat ? Impact : RemoteFraction;
		const double FiniteThroatQ = FMath::Min(
			FMath::Clamp(ThroatQ, 0.0, 1.0), 1.0 - CriticalTolerance);
		const double ThroatDenominator = FMath::Sqrt(FMath::Max(
			1.0 - FiniteThroatQ * FiniteThroatQ, 1.0e-12));
		const double AnalyticThroatAngle = TraversedThroatLength * FiniteThroatQ
			/ (SafeThroatRadius * ThroatDenominator);
		const double ResidualBendAngle = bInsideOutward ? GlobalSample.G : InwardSample.R;
		const double BendAngle = ResidualBendAngle + AnalyticThroatAngle;
		const FVector FallbackAxis = FMath::Abs(RadialWS.Z) < 0.999
			? FVector::UpVector : FVector::RightVector;
		const FVector TangentDirection = TangentLengthSquared > 1.0e-8
			? Tangent / FMath::Sqrt(TangentLengthSquared)
			: FVector::CrossProduct(FallbackAxis, RadialWS).GetSafeNormal(
				UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector BentWS = (FMath::Cos(BendAngle) * RadialWS
			+ FMath::Sin(BendAngle) * TangentDirection).GetSafeNormal(
				UE_SMALL_NUMBER, RayWS);

		const FVector SelfXAxis = SelfX.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector SelfYAxis = SelfY.GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
		const FVector SelfZAxis = SelfZ.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector LinkedXAxis = LinkedX.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector LinkedYAxis = LinkedY.GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
		const FVector LinkedZAxis = LinkedZ.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector SelfLocalDirection(
			FVector::DotProduct(BentWS, SelfXAxis),
			FVector::DotProduct(BentWS, SelfYAxis),
			FVector::DotProduct(BentWS, SelfZAxis));
		FVector LinkedWS = -((SelfLocalDirection.X * LinkedXAxis
			+ SelfLocalDirection.Y * LinkedYAxis
			+ SelfLocalDirection.Z * LinkedZAxis).GetSafeNormal(
				UE_SMALL_NUMBER, BentWS));
		const FVector SelfLocalViewRight(
			FVector::DotProduct(ViewRightWS, SelfXAxis),
			FVector::DotProduct(ViewRightWS, SelfYAxis),
			FVector::DotProduct(ViewRightWS, SelfZAxis));
		const FVector LinkedViewRightWS = (SelfLocalViewRight.X * LinkedXAxis
			+ SelfLocalViewRight.Y * LinkedYAxis
			+ SelfLocalViewRight.Z * LinkedZAxis).GetSafeNormal(
				UE_SMALL_NUMBER, LinkedYAxis);
		LinkedWS = (LinkedWS - 2.0 * FVector::DotProduct(LinkedWS, LinkedViewRightWS)
			* LinkedViewRightWS).GetSafeNormal(UE_SMALL_NUMBER, LinkedWS);
		const FVector SelfLocalViewUp(
			FVector::DotProduct(ViewUpWS, SelfXAxis),
			FVector::DotProduct(ViewUpWS, SelfYAxis),
			FVector::DotProduct(ViewUpWS, SelfZAxis));
		const FVector LinkedViewUpWS = (SelfLocalViewUp.X * LinkedXAxis
			+ SelfLocalViewUp.Y * LinkedYAxis
			+ SelfLocalViewUp.Z * LinkedZAxis).GetSafeNormal(
				UE_SMALL_NUMBER, LinkedZAxis);
		LinkedWS = (LinkedWS - 2.0 * FVector::DotProduct(LinkedWS, LinkedViewUpWS)
			* LinkedViewUpWS).GetSafeNormal(UE_SMALL_NUMBER, LinkedWS);
		if (BentWS.ContainsNaN() || LinkedWS.ContainsNaN()
			|| !FMath::IsFinite(CriticalImpact) || !FMath::IsFinite(BendAngle))
		{
			return false;
		}

		const double GradientBand = bUseLUT
			? FMath::Max(CriticalTolerance,
				2.0 / FMath::Max(LUT.CPUVolumeData->Dimensions.X - 1, 1))
			: CriticalTolerance;
		OutResult.BentDirection = BentWS;
		OutResult.LinkedDirection = LinkedWS;
		OutResult.Impact = Impact;
		OutResult.CriticalImpact = CriticalImpact;
		OutResult.bConservativeAllFaces = bTrappedCritical
			|| FMath::Abs(Impact - CriticalImpact) <= GradientBand;
		OutResult.bValid = true;
		return true;
	}

	void AddWPCaptureFacesForDirection(const FVector& Direction, uint8& InOutMask)
	{
		const FVector Safe = Direction.GetSafeNormal(
			UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector Absolute(FMath::Abs(Safe.X), FMath::Abs(Safe.Y), FMath::Abs(Safe.Z));
		const double Dominant = FMath::Max3(Absolute.X, Absolute.Y, Absolute.Z);
		const double AdjacentThreshold = Dominant * 0.9;
		if (Absolute.X >= AdjacentThreshold)
		{
			InOutMask |= Safe.X >= 0.0 ? 1u << 0 : 1u << 1;
		}
		if (Absolute.Y >= AdjacentThreshold)
		{
			InOutMask |= Safe.Y >= 0.0 ? 1u << 2 : 1u << 3;
		}
		if (Absolute.Z >= AdjacentThreshold)
		{
			InOutMask |= Safe.Z >= 0.0 ? 1u << 4 : 1u << 5;
		}
	}

	bool IsWPMetricSampleOnScreen(
		const FVector& Point,
		const FVector& CameraLocation,
		const FRotator& CameraRotation,
		const float HorizontalFOVDegrees,
		const double ViewAspectRatio)
	{
		if (ViewAspectRatio <= 0.0 || HorizontalFOVDegrees <= 1.0f)
		{
			return true;
		}
		const FVector CameraSpace = CameraRotation.UnrotateVector(Point - CameraLocation);
		if (CameraSpace.X <= 1.0)
		{
			return false;
		}
		const double TanHalfHorizontal = FMath::Tan(
			FMath::DegreesToRadians(HorizontalFOVDegrees * 0.5));
		const double TanHalfVertical = TanHalfHorizontal / ViewAspectRatio;
		return FMath::Abs(CameraSpace.Y / CameraSpace.X) <= TanHalfHorizontal
			&& FMath::Abs(CameraSpace.Z / CameraSpace.X) <= TanHalfVertical;
	}

	FWPMetricFacePredictionResult PredictWPMetricCaptureFaces(
		UWorld& World,
		const AWormholePortalActor& Portal,
		const AWormholePortalActor& LinkedPortal,
		const FWPLUTEndpointSnapshot& LUT,
		const FVector& CameraLocation,
		const FRotator& CameraRotation,
		const float CameraFOVDegrees,
		const double ViewAspectRatio,
		AActor* ReferenceViewActor)
	{
		FWPMetricFacePredictionResult Result;
		const FVector Center = Portal.GetActorLocation();
		const FVector CenterToCamera = CameraLocation - Center;
		if (!LUT.IsReady() || !IsFiniteReferenceLocation(Center)
			|| CenterToCamera.IsNearlyZero())
		{
			Result.LocalFaceMask = 0x3f;
			Result.LinkedFaceMask = 0x3f;
			Result.bFailOpen = true;
			Result.Reason = TEXT("InvalidGeometryOrLUTFailOpen");
			return Result;
		}

		FVector AxisRight = FVector::ZeroVector;
		FVector AxisUp = FVector::ZeroVector;
		CenterToCamera.GetSafeNormal().FindBestAxisVectors(AxisRight, AxisUp);
		if (AxisRight.IsNearlyZero() || AxisUp.IsNearlyZero())
		{
			Result.LocalFaceMask = 0x3f;
			Result.LinkedFaceMask = 0x3f;
			Result.bFailOpen = true;
			Result.Reason = TEXT("InvalidViewBasisFailOpen");
			return Result;
		}

		static const FVector2D RingDirections[8] =
		{
			FVector2D(0.0, 1.0), FVector2D(1.0, 1.0).GetSafeNormal(),
			FVector2D(1.0, 0.0), FVector2D(1.0, -1.0).GetSafeNormal(),
			FVector2D(0.0, -1.0), FVector2D(-1.0, -1.0).GetSafeNormal(),
			FVector2D(-1.0, 0.0), FVector2D(-1.0, 1.0).GetSafeNormal()
		};
		const double VisualScale = FMath::Max(
			static_cast<double>(Portal.GetPortalVisualScale()), KINDA_SMALL_NUMBER);
		const double SeamRadius = Portal.GetPortalRadius() * VisualScale;
		const double MouthRadius = Portal.GetMouthRadius() * VisualScale;
		const double TransitionRadius = Portal.GetTransitionRadius() * VisualScale;
		const double RingRadii[3] =
		{
			0.5 * SeamRadius,
			0.5 * (SeamRadius + MouthRadius),
			0.5 * (MouthRadius + TransitionRadius)
		};
		FVector SamplePoints[25];
		SamplePoints[0] = Center;
		int32 SampleIndex = 1;
		for (const double RingRadius : RingRadii)
		{
			for (const FVector2D& RingDirection : RingDirections)
			{
				SamplePoints[SampleIndex++] = Center
					+ AxisRight * RingDirection.X * RingRadius
					+ AxisUp * RingDirection.Y * RingRadius;
			}
		}

		static const FName PredictionTraceTag(TEXT("WPMetricFacePrediction"));
		FCollisionQueryParams QueryParams(PredictionTraceTag, false);
		QueryParams.AddIgnoredActor(&Portal);
		QueryParams.AddIgnoredActor(&LinkedPortal);
		if (IsValid(ReferenceViewActor))
		{
			QueryParams.AddIgnoredActor(ReferenceViewActor);
		}
		const FTransform SelfTransform = Portal.GetActorTransform();
		const FTransform LinkedTransform = LinkedPortal.GetActorTransform();
		const FVector ViewRightWS = FRotationMatrix(CameraRotation).GetUnitAxis(EAxis::Y);
		const FVector ViewUpWS = FRotationMatrix(CameraRotation).GetUnitAxis(EAxis::Z);
		const double ProxyRadiusCm = FMath::Max(
			TransitionRadius, SeamRadius + WPCaptureProxySafetyShellCm);
		for (const FVector& SamplePoint : SamplePoints)
		{
			if (Result.LocalFaceMask == 0x3f
				&& Result.LinkedFaceMask == 0x3f)
			{
				Result.Reason = TEXT("AllFacesReachedEarlyExit");
				break;
			}
			if (!IsWPMetricSampleOnScreen(
				SamplePoint, CameraLocation, CameraRotation,
				CameraFOVDegrees, ViewAspectRatio))
			{
				++Result.ScreenRejectedRayCount;
				continue;
			}
			++Result.RayCount;
			const bool bBlocked = World.LineTraceTestByChannel(
				CameraLocation, SamplePoint, ECC_Visibility, QueryParams);
			if (bBlocked)
			{
				++Result.BlockedRayCount;
				continue;
			}
			++Result.OpenRayCount;
			FWPCPUOpticalRayResult Optical;
			++Result.LUTEvaluationCount;
			if (!EvaluateWPCPUOpticalRay(
				(SamplePoint - CameraLocation).GetSafeNormal(),
				CameraLocation - Center,
				SeamRadius,
				Portal.GetThroatHalfLength() * 2.0 * VisualScale,
				ProxyRadiusCm,
				LUT,
				SelfTransform.GetUnitAxis(EAxis::X),
				SelfTransform.GetUnitAxis(EAxis::Y),
				SelfTransform.GetUnitAxis(EAxis::Z),
				LinkedTransform.GetUnitAxis(EAxis::X),
				LinkedTransform.GetUnitAxis(EAxis::Y),
				LinkedTransform.GetUnitAxis(EAxis::Z),
				ViewRightWS, ViewUpWS, Optical))
			{
				Result.LocalFaceMask = 0x3f;
				Result.LinkedFaceMask = 0x3f;
				Result.bFailOpen = true;
				Result.Reason = TEXT("OpticalEvaluationFailOpen");
				break;
			}
			if (Optical.bConservativeAllFaces)
			{
				Result.LocalFaceMask = 0x3f;
				Result.LinkedFaceMask = 0x3f;
				Result.bFailOpen = true;
				Result.Reason = TEXT("CriticalTrappedOrHighGradientFailOpen");
				break;
			}
			AddWPCaptureFacesForDirection(Optical.BentDirection, Result.LocalFaceMask);
			AddWPCaptureFacesForDirection(Optical.LinkedDirection, Result.LinkedFaceMask);
		}
		if (Result.RayCount == 0)
		{
			Result.LocalFaceMask = 0x3f;
			Result.LinkedFaceMask = 0x3f;
			Result.bFailOpen = true;
			Result.Reason = TEXT("AllMetricSamplesOffScreenFailOpen");
		}
		return Result;
	}
}



FWPRuntimeCaptureScheduler::FWPRuntimeCaptureScheduler(
	UWPRuntimeSubsystem& InRuntime,
	TObjectPtr<UWPCaptureManager>& InCaptureManager,
	TMap<FGuid, FWPPortalPairState>& InPairStates,
	bool& bInRenderPacketPipelineActive,
	bool& bInPairsDirty)
	: Runtime(InRuntime)
	, CaptureManager(InCaptureManager)
	, PairStates(InPairStates)
	, bRenderPacketPipelineActive(bInRenderPacketPipelineActive)
	, bPairsDirty(bInPairsDirty)
{
}

void FWPRuntimeCaptureScheduler::Initialize(const bool bInitialRuntimeActive)
{
	bRuntimeActive = bInitialRuntimeActive;
	// Retains the sentinel value so the effective mode is logged once on the first PostActorTick.
	LastModeRaw = MIN_int32;
}

void FWPRuntimeCaptureScheduler::Reset()
{
	ExclusiveInsidePairId.Invalidate();
	ActiveResolutionTransitionPairId.Invalidate();
	bRuntimeActive = false;
	LastModeRaw = MIN_int32;
}

UWorld* FWPRuntimeCaptureScheduler::GetWorld() const
{
	return Runtime.GetWorld();
}

bool FWPRuntimeCaptureScheduler::IsInitialized() const
{
	return Runtime.IsInitialized();
}

bool FWPRuntimeCaptureScheduler::ResolveReferenceView(
	FVector& OutCameraLocation,
	FRotator& OutCameraRotation,
	AActor*& OutViewActor,
	float& OutCameraFOVDegrees) const
{
	return Runtime.ResolveReferenceView(
		OutCameraLocation,
		OutCameraRotation,
		OutViewActor,
		OutCameraFOVDegrees);
}

void FWPRuntimeCaptureScheduler::UpdateVisibilityAndOcclusion(
	const FVector& CameraLocation,
	AActor* ReferenceViewActor,
	const bool bHasReferenceView,
	const double NowSeconds)
{
	UpdateCaptureInsidePairSelection(CameraLocation, bHasReferenceView);
	UpdateCaptureOcclusionStates(
		CameraLocation, ReferenceViewActor, bHasReferenceView, NowSeconds);
}

const TCHAR* FWPRuntimeCaptureScheduler::GetCaptureAuthorityName(
	const EWPCaptureAuthority Authority)
{
	switch (Authority)
	{
	case EWPCaptureAuthority::RuntimeWarmup: return TEXT("RuntimeWarmup");
	case EWPCaptureAuthority::RuntimeOnly: return TEXT("RuntimeOnly");
	default: return TEXT("Unknown");
	}
}

void FWPRuntimeCaptureScheduler::SetPairCaptureAuthority(
	FWPPortalPairState& PairState,
	const EWPCaptureAuthority NewAuthority,
	const TCHAR* Reason)
{
	// Logging only: measures CPU time spent processing the authority transition.
	const double StartSeconds = FPlatformTime::Seconds();
	const EWPCaptureAuthority PreviousAuthority = PairState.Capture.Authority;
	AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	const bool bAdvanceOwnershipEpoch = PairState.Capture.OwnershipEpoch == 0
		|| (PreviousAuthority == EWPCaptureAuthority::RuntimeOnly
			&& NewAuthority == EWPCaptureAuthority::RuntimeWarmup);
	uint64 NewEpoch = PairState.Capture.OwnershipEpoch;
	if (bAdvanceOwnershipEpoch)
	{
		++NewEpoch;
		if (NewEpoch == 0)
		{
			++NewEpoch;
		}
	}
	PairState.Capture.OwnershipEpoch = NewEpoch;
	PairState.Capture.Authority = NewAuthority;
	PairState.Capture.AuthorityTransitionFrame = GFrameCounter;
	PairState.Capture.ConsecutiveFailureCount = 0;
	if (NewAuthority == EWPCaptureAuthority::RuntimeWarmup
		&& (PreviousAuthority != EWPCaptureAuthority::RuntimeWarmup
			|| bAdvanceOwnershipEpoch))
	{
		PairState.Capture.CadenceElapsedSeconds = 0.0;
		PairState.Capture.FacePrediction = FWPFacePredictionState();
		PairState.Capture.bNextStaggeredEndpointA = true;
	}
	const bool bManagerAuthorityApplied = CaptureManager
		&& CaptureManager->SetPairCaptureAuthority(
			PairState.Identity.PairId, PortalA, PortalB, NewEpoch, true, Reason);

	if (PreviousAuthority != NewAuthority
		&& NewAuthority == EWPCaptureAuthority::RuntimeWarmup)
	{
	}
	else if (PreviousAuthority != NewAuthority
		&& NewAuthority == EWPCaptureAuthority::RuntimeOnly)
	{
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(&Runtime, Verbose,
		TEXT("[CaptureScheduler][Authority] Pair authority updated. World=%s PairId=%s PortalA=%s PortalB=%s PreviousAuthority=%s EffectiveAuthority=%s OwnershipEpochAdvanced=%d OwnershipEpoch=%llu TransitionFrame=%llu EndpointAValid=%d EndpointBValid=%d RuntimeManagerEndpointA=%d RuntimeManagerEndpointB=%d ManagerAuthorityApplied=%d ActorCaptureAuthorityState=0 ActorTickDependency=0 CadenceElapsedMs=%.3f TargetEndpointHz=%.3f EndpointCadenceMs=%.3f StaggeredSubmissionCadenceMs=%.3f TransitForcesCapture=0 NextStaggeredEndpoint=%s Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *PairIdToString(PairState.Identity.PairId),
		*GetNameSafe(PortalA), *GetNameSafe(PortalB),
		GetCaptureAuthorityName(PreviousAuthority), GetCaptureAuthorityName(NewAuthority),
		bAdvanceOwnershipEpoch ? 1 : 0,
		static_cast<unsigned long long>(NewEpoch),
		static_cast<unsigned long long>(GFrameCounter),
		IsValid(PortalA) ? 1 : 0, IsValid(PortalB) ? 1 : 0,
		CaptureManager && CaptureManager->HasEndpointResources(PortalA) ? 1 : 0,
		CaptureManager && CaptureManager->HasEndpointResources(PortalB) ? 1 : 0,
		bManagerAuthorityApplied ? 1 : 0,
		PairState.Capture.CadenceElapsedSeconds * 1000.0,
		GetWPCaptureTargetEndpointHz(),
		GetWPEndpointCaptureCadenceSeconds() * 1000.0,
		GetWPStaggeredSubmissionCadenceSeconds() * 1000.0,
		PairState.Capture.bNextStaggeredEndpointA ? TEXT("A") : TEXT("B"),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void FWPRuntimeCaptureScheduler::RecoverPairCaptureAuthority(
	FWPPortalPairState& PairState,
	const TCHAR* Reason)
{
	// Logging only: measures CPU time spent processing authority recovery.
	const double StartSeconds = FPlatformTime::Seconds();
	AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	const EWPCaptureAuthority PreviousAuthority = PairState.Capture.Authority;
	uint64 RestartEpoch = PairState.Capture.OwnershipEpoch + 1;
	if (RestartEpoch == 0)
	{
		++RestartEpoch;
	}
	PairState.Capture.OwnershipEpoch = RestartEpoch;
	PairState.Capture.Authority = EWPCaptureAuthority::RuntimeWarmup;
	PairState.Capture.AuthorityTransitionFrame = GFrameCounter;
	PairState.Capture.ConsecutiveFailureCount = 0;
	PairState.Capture.CadenceElapsedSeconds = 0.0;
	PairState.Capture.FacePrediction = FWPFacePredictionState();
	PairState.Capture.bNextStaggeredEndpointA = true;
	const uint32 RecoveryResolution = PairState.Capture.Resolution.CurrentResolution != 0
		? PairState.Capture.Resolution.CurrentResolution
		: PairState.Capture.Resolution.DesiredResolution;
	const bool bEndpointAReady = IsValid(PortalA) && CaptureManager
		&& CaptureManager->EnsureEndpointResources(PortalA, RecoveryResolution);
	const bool bEndpointBReady = IsValid(PortalB) && CaptureManager
		&& CaptureManager->EnsureEndpointResources(PortalB, RecoveryResolution);
	const bool bManagerAuthorityRestarted = CaptureManager
		&& CaptureManager->SetPairCaptureAuthority(
			PairState.Identity.PairId, PortalA, PortalB, RestartEpoch, true, Reason);
	INC_DWORD_STAT(STAT_WP_CaptureRuntimeRollbacks);
	WP_LOG(&Runtime, Warning,
		TEXT("[CaptureScheduler][Recovery] Pair restarted in Runtime warmup. World=%s PairId=%s PortalA=%s PortalB=%s PreviousAuthority=%s EffectiveAuthority=RuntimeWarmup OwnershipEpoch=%llu EndpointAReady=%d EndpointBReady=%d ManagerAuthorityRestarted=%d ActorFallback=0 ActorTickDependency=0 CadenceElapsedMs=%.3f TransitForcesCapture=0 Reason=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *PairIdToString(PairState.Identity.PairId),
		*GetNameSafe(PortalA), *GetNameSafe(PortalB),
		GetCaptureAuthorityName(PreviousAuthority),
		static_cast<unsigned long long>(PairState.Capture.OwnershipEpoch),
		bEndpointAReady ? 1 : 0, bEndpointBReady ? 1 : 0,
		bManagerAuthorityRestarted ? 1 : 0,
		PairState.Capture.CadenceElapsedSeconds * 1000.0,
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

bool FWPRuntimeCaptureScheduler::ExecuteRuntimePairCapture(
	FWPPortalPairState& PairState,
	const float,
	const EWPManagedCaptureSubmissionMode SubmissionMode,
	double& OutCaptureCpuMs,
	const uint8 SelectedFaceMask)
{
	// Includes Capture Callback CPU time in the submission result and error logs.
	const double StartSeconds = FPlatformTime::Seconds();
	OutCaptureCpuMs = 0.0;
	AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	if (!IsValid(PortalA) || !IsValid(PortalB) || !CaptureManager
		|| PairState.Capture.OwnershipEpoch == 0)
	{
		OutCaptureCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		WP_LOG(&Runtime, Error,
			TEXT("[CaptureScheduler][Submit] Pair rejected before manager submission. PairId=%s PortalA=%s PortalB=%s Authority=%s OwnershipEpoch=%llu CaptureManagerValid=%d ActorCaptureExecution=0 ActorTickDependency=0 Reason=InvalidEndpointOrManagerAuthority CpuMs=%.4f"),
			*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
			GetCaptureAuthorityName(PairState.Capture.Authority),
			static_cast<unsigned long long>(PairState.Capture.OwnershipEpoch),
			CaptureManager ? 1 : 0,
			OutCaptureCpuMs);
		return false;
	}

	FWPCaptureEndpointSnapshot SnapshotA;
	FWPCaptureEndpointSnapshot SnapshotB;
	const bool bSnapshotAReady = CaptureManager->GetEndpointSnapshot(PortalA, SnapshotA)
		&& SnapshotA.IsReadyForSubmission(GetWorld());
	const bool bSnapshotBReady = CaptureManager->GetEndpointSnapshot(PortalB, SnapshotB)
		&& SnapshotB.IsReadyForSubmission(GetWorld());
	if (!bSnapshotAReady || !bSnapshotBReady)
	{
		OutCaptureCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
#if !UE_BUILD_SHIPPING
		WP_LOG(&Runtime, Verbose,
			TEXT("[CaptureScheduler][Submit] Manager endpoint preflight failed. PairId=%s PortalA=%s PortalB=%s Authority=%s OwnershipEpoch=%llu SnapshotAReady=%d SnapshotBReady=%d CaptureGenerationA=%u CaptureGenerationB=%u ResourceEpochA=%llu ResourceEpochB=%llu ContractAValid=%d ContractBValid=%d ActorCaptureExecution=0 ActorTickDependency=0 CpuMs=%.4f"),
			*PairIdToString(PairState.Identity.PairId),
			*GetNameSafe(PortalA), *GetNameSafe(PortalB),
			GetCaptureAuthorityName(PairState.Capture.Authority),
			static_cast<unsigned long long>(PairState.Capture.OwnershipEpoch),
			bSnapshotAReady ? 1 : 0, bSnapshotBReady ? 1 : 0,
			SnapshotA.CaptureGeneration, SnapshotB.CaptureGeneration,
			static_cast<unsigned long long>(SnapshotA.ResourceEpoch),
			static_cast<unsigned long long>(SnapshotB.ResourceEpoch),
			SnapshotA.CubeContract.IsValid() ? 1 : 0,
			SnapshotB.CubeContract.IsValid() ? 1 : 0,
			OutCaptureCpuMs);
#endif
		return false;
	}

	FWPManagedPairCaptureResult Result;
	bool bManagerSubmitted = false;
	{
		SCOPE_CYCLE_COUNTER(STAT_WP_CaptureSchedulerRuntimeSubmit);
		bManagerSubmitted = CaptureManager->SubmitPairCapture(
			PairState.Identity.PairId, PortalA, PortalB, PairState.Capture.OwnershipEpoch,
			SubmissionMode, Result, SelectedFaceMask);
	}
	const bool bSuccess = bManagerSubmitted && Result.WasSuccessful();
	OutCaptureCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	if (bSuccess)
	{
		const int32 SubmittedEndpointCount = Result.GetSubmittedEndpointCount();
		const int32 SubmittedFaceCount = Result.GetSubmittedFaceCount();
		const uint64 GenerationSkew = FMath::Abs(
			static_cast<int64>(Result.CaptureGenerationAAfter)
			- static_cast<int64>(Result.CaptureGenerationBAfter));
		INC_DWORD_STAT_BY(
			STAT_WP_CaptureRuntimeEndpointsSubmitted,
			SubmittedEndpointCount);
		INC_DWORD_STAT_BY(
			STAT_WP_CaptureRuntimeFacesSubmitted,
			SubmittedFaceCount);
		if (Result.bPairCycleCompleted)
		{
			INC_DWORD_STAT(STAT_WP_CaptureRuntimePairsSubmitted);
		}
		if (SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair)
		{
			INC_DWORD_STAT(STAT_WP_CaptureRuntimeAtomicSubmissions);
		}
		else
		{
			INC_DWORD_STAT(STAT_WP_CaptureRuntimeStaggeredSubmissions);
		}
	}

	return bSuccess;
}

void FWPRuntimeCaptureScheduler::UpdateCaptureInsidePairSelection(
	const FVector& CameraLocation,
	const bool bHasReferenceView)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	const FGuid PreviousSelectedPairId = ExclusiveInsidePairId;

	struct FInsidePairCandidate
	{
		FGuid PairId;
		double NormalizedDistanceSquared = TNumericLimits<double>::Max();
		bool bTransitActive = false;
	};

	TArray<FInsidePairCandidate, TInlineAllocator<8>> Candidates;
	if (bHasReferenceView && IsFiniteReferenceLocation(CameraLocation))
	{
		for (const TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
		{
			const FWPPortalPairState& PairState = PairEntry.Value;
			const AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
			const AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
			if (!IsValid(PortalA) || !IsValid(PortalB))
			{
				continue;
			}

			double BestNormalizedDistanceSquared = TNumericLimits<double>::Max();
			const auto ConsiderEndpoint =
				[&CameraLocation, &BestNormalizedDistanceSquared](
					const AWormholePortalActor& Portal)
			{
				const double PortalRadiusCm =
					static_cast<double>(Portal.GetPortalRadius());
				const double OuterRadiusCm =
					static_cast<double>(Portal.GetTransitionRadius());
				const double SafeProxyRadiusCm = FMath::Max(
					OuterRadiusCm,
					PortalRadiusCm + WPCaptureProxySafetyShellCm);
				if (!FMath::IsFinite(SafeProxyRadiusCm) || SafeProxyRadiusCm <= 0.0)
				{
					return;
				}

				const double RadiusSquared = FMath::Square(SafeProxyRadiusCm);
				const double DistanceSquared = FVector::DistSquared(
					CameraLocation, Portal.GetActorLocation());
				if (FMath::IsFinite(DistanceSquared) && DistanceSquared <= RadiusSquared)
				{
					BestNormalizedDistanceSquared = FMath::Min(
						BestNormalizedDistanceSquared,
						DistanceSquared / RadiusSquared);
				}
			};

			ConsiderEndpoint(*PortalA);
			ConsiderEndpoint(*PortalB);
			if (BestNormalizedDistanceSquared != TNumericLimits<double>::Max())
			{
				FInsidePairCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.PairId = PairState.Identity.PairId;
				Candidate.NormalizedDistanceSquared = BestNormalizedDistanceSquared;
				Candidate.bTransitActive = PairState.Transit.bTransitActive;
			}
		}
	}

	const auto IsBetterCandidate = [](const FInsidePairCandidate& Left,
		const FInsidePairCandidate& Right)
	{
		if (Left.NormalizedDistanceSquared != Right.NormalizedDistanceSquared)
		{
			return Left.NormalizedDistanceSquared < Right.NormalizedDistanceSquared;
		}
		return UWPRuntimeSubsystem::MakePairSortKey(Left.PairId)
			< UWPRuntimeSubsystem::MakePairSortKey(Right.PairId);
	};

	const FInsidePairCandidate* BestCandidate = nullptr;
	const FInsidePairCandidate* BestTransitCandidate = nullptr;
	const FInsidePairCandidate* StickyCandidate = nullptr;
	for (const FInsidePairCandidate& Candidate : Candidates)
	{
		if (!BestCandidate || IsBetterCandidate(Candidate, *BestCandidate))
		{
			BestCandidate = &Candidate;
		}
		if (Candidate.bTransitActive
			&& (!BestTransitCandidate
				|| IsBetterCandidate(Candidate, *BestTransitCandidate)))
		{
			BestTransitCandidate = &Candidate;
		}
		if (Candidate.PairId == PreviousSelectedPairId)
		{
			StickyCandidate = &Candidate;
		}
	}

	// Active Transit affects only Pair-selection priority among overlapping Proxies.
	// A selection change does not reset cadence or schedule capture/prewarm work.
	const FInsidePairCandidate* SelectedCandidate = BestTransitCandidate
		? BestTransitCandidate
		: (StickyCandidate ? StickyCandidate : BestCandidate);
	ExclusiveInsidePairId = SelectedCandidate
		? SelectedCandidate->PairId
		: FGuid();

#if !UE_BUILD_SHIPPING
	int32 ChangedPairStateCount = 0;
	int32 SuppressedResumePrewarmCount = 0;
#endif
	for (TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
	{
		FWPCaptureVisibilityState& Visibility = PairEntry.Value.Capture.Visibility;
		const bool bWasInsideBlocked = Visibility.bInsideBlocked;
		const bool bInsideSelected = ExclusiveInsidePairId.IsValid()
			&& PairEntry.Value.Identity.PairId == ExclusiveInsidePairId;
		const bool bInsideBlocked = ExclusiveInsidePairId.IsValid()
			&& !bInsideSelected;
		const bool bInsideStateChanged =
			Visibility.bInsideSelected != bInsideSelected
			|| Visibility.bInsideBlocked != bInsideBlocked;
#if !UE_BUILD_SHIPPING
		ChangedPairStateCount += bInsideStateChanged ? 1 : 0;
#endif
		Visibility.bInsideSelected = bInsideSelected;
		Visibility.bInsideBlocked = bInsideBlocked;
		if (bInsideStateChanged)
		{
			// Do not reuse occlusion computed for the previous Pair selection in the new exclusive
			// state.
			// Invalidate it fail-open without changing capture cadence or prewarm state.
			Visibility.OcclusionVisibleEndpointMask = WPCaptureBothEndpointsMask;
			Visibility.bOcclusionValid = false;
			Visibility.NextOcclusionTraceSeconds = 0.0;
			PairEntry.Value.Publication.bDirty = true;
			if ((bInsideSelected || (bWasInsideBlocked && !bInsideBlocked))
				&& Visibility.bPaused)
			{
				// Even if an inside-exclusive transition clears an existing visibility pause,
				// do not schedule an atomic resume capture; preserve cadence.
				Visibility.InvisibleElapsedSeconds = 0.0;
				Visibility.HiddenRefreshElapsedSeconds = 0.0;
				Visibility.LastInvisibleSampleReceiptSeconds = -1.0e30;
				Visibility.LastInvisibleOwnershipEpoch = 0;
				Visibility.LastInvisibleSampleSequence = 0;
				Visibility.GuardViewActorId = 0;
				Visibility.GuardCameraLocation = FVector::ZeroVector;
				Visibility.GuardCameraRotation = FRotator::ZeroRotator;
				Visibility.GuardCameraFOVDegrees = 0.0f;
				Visibility.bGuardInitialized = false;
				Visibility.bAwaitingPostGuardSample = false;
				Visibility.bPaused = false;
				Visibility.bResumePrewarmPending = false;
#if !UE_BUILD_SHIPPING
				++SuppressedResumePrewarmCount;
#endif
			}
		}
	}

#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	const FString PreviousSelectedPairText = PreviousSelectedPairId.IsValid()
		? PairIdToString(PreviousSelectedPairId)
		: TEXT("None");
	const FString CurrentSelectedPairText = ExclusiveInsidePairId.IsValid()
		? PairIdToString(ExclusiveInsidePairId)
		: TEXT("None");
	if (PreviousSelectedPairId != ExclusiveInsidePairId)
	{
		WP_LOG(&Runtime, Verbose,
			TEXT("[CaptureVisibility][InsidePairSelection] State changed. World=%s Frame=%llu PreviousPairId=%s CurrentPairId=%s CandidateCount=%d ChangedPairStates=%d SuppressedResumePrewarmCount=%d SelectedTransitActive=%d SelectedNormalizedDistanceSquared=%.6f ReferenceViewAvailable=%d PairSwitchRecaptureScheduled=0 TransitCaptureForced=0 TransitAtomicForced=0 CadenceReset=0 CpuMs=%.4f"),
			*GetNameSafe(GetWorld()),
			static_cast<unsigned long long>(GFrameCounter),
			*PreviousSelectedPairText, *CurrentSelectedPairText,
			Candidates.Num(), ChangedPairStateCount, SuppressedResumePrewarmCount,
			SelectedCandidate && SelectedCandidate->bTransitActive ? 1 : 0,
			SelectedCandidate
				? SelectedCandidate->NormalizedDistanceSquared
				: -1.0,
			bHasReferenceView ? 1 : 0,
			CpuMs);
	}
#endif
}

void FWPRuntimeCaptureScheduler::UpdateCaptureOcclusionStates(
	const FVector& CameraLocation,
	AActor* ReferenceViewActor,
	const bool bHasReferenceView,
	const double NowSeconds)
{
	// CPU occlusion does not replace the Renderer frustum result. It conservatively
	// supplements that result
	// by testing only whether a Pair reported on-screen is completely hidden by geometry.
	const double TraceIntervalSeconds = FMath::Clamp(
		static_cast<double>(
			CVarWPCaptureOcclusionTraceIntervalSeconds.GetValueOnGameThread()),
		WPCaptureOcclusionMinimumIntervalSeconds,
		WPCaptureOcclusionMaximumIntervalSeconds);
	UWorld* World = GetWorld();

	for (TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
	{
		FWPPortalPairState& PairState = PairEntry.Value;
		FWPCaptureVisibilityState& Visibility = PairState.Capture.Visibility;
		if (NowSeconds < Visibility.NextOcclusionTraceSeconds)
		{
			continue;
		}

#if !UE_BUILD_SHIPPING
		const double EvaluationStartSeconds = FPlatformTime::Seconds();
#endif
		Visibility.NextOcclusionTraceSeconds = NowSeconds + TraceIntervalSeconds;
		int32 TraceCount = 0;
		int32 BlockedTraceCount = 0;
		uint8 NewVisibleMask = WPCaptureBothEndpointsMask;
		bool bNewOcclusionValid = false;
		const TCHAR* DecisionReason = TEXT("FailOpen");

		AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
		AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
		const bool bCanEvaluate = bHasReferenceView
			&& IsFiniteReferenceLocation(CameraLocation)
			&& IsValid(World)
			&& IsValid(PortalA)
			&& IsValid(PortalB)
			&& !Visibility.bInsideSelected
			&& !Visibility.bInsideBlocked
			&& Visibility.LastVisibleEndpointMask != 0;

		if (bCanEvaluate)
		{
			static const FName OcclusionTraceTag(TEXT("WPCaptureOcclusion"));
			FCollisionQueryParams QueryParams(OcclusionTraceTag, false);
			QueryParams.AddIgnoredActor(PortalA);
			QueryParams.AddIgnoredActor(PortalB);
			if (IsValid(ReferenceViewActor))
			{
				QueryParams.AddIgnoredActor(ReferenceViewActor);
			}

			const auto EvaluateEndpoint =
				[World, &CameraLocation, &QueryParams, &TraceCount,
					&BlockedTraceCount](
					const AWormholePortalActor& Portal,
					bool& bOutValid)
			{
				bOutValid = false;
				const FVector Center = Portal.GetActorLocation();
				const double PortalRadiusCm =
					static_cast<double>(Portal.GetPortalRadius());
				const double OuterRadiusCm =
					static_cast<double>(Portal.GetTransitionRadius());
				const double SafeProxyRadiusCm = FMath::Max(
					OuterRadiusCm,
					PortalRadiusCm + WPCaptureProxySafetyShellCm);
				const FVector CenterToCamera = CameraLocation - Center;
				if (!IsFiniteReferenceLocation(Center)
					|| !FMath::IsFinite(SafeProxyRadiusCm)
					|| SafeProxyRadiusCm <= 0.0
					|| CenterToCamera.IsNearlyZero())
				{
					return true;
				}

				FVector AxisRight = FVector::ZeroVector;
				FVector AxisUp = FVector::ZeroVector;
				CenterToCamera.GetSafeNormal().FindBestAxisVectors(
					AxisRight, AxisUp);
				if (AxisRight.IsNearlyZero() || AxisUp.IsNearlyZero())
				{
					return true;
				}

				bOutValid = true;
				const FVector RingDirections[8] =
				{
					AxisUp,
					(AxisUp + AxisRight).GetSafeNormal(),
					AxisRight,
					(-AxisUp + AxisRight).GetSafeNormal(),
					-AxisUp,
					(-AxisUp - AxisRight).GetSafeNormal(),
					-AxisRight,
					(AxisUp - AxisRight).GetSafeNormal()
				};

				for (const FVector& RingDirection : RingDirections)
				{
					const FVector TraceEnd = Center
						+ RingDirection * SafeProxyRadiusCm;
					FHitResult Hit;
					const bool bBlocked = World->LineTraceSingleByChannel(
						Hit,
						CameraLocation,
						TraceEnd,
						ECC_Visibility,
						QueryParams);
					++TraceCount;
					BlockedTraceCount += bBlocked ? 1 : 0;
					if (!bBlocked)
					{
						// Any open ring point keeps this Endpoint eligible for updates.
						return true;
					}
				}
				return false;
			};

			bool bEndpointAValid = false;
			bool bEndpointBValid = false;
			const bool bEndpointAVisible =
				EvaluateEndpoint(*PortalA, bEndpointAValid);
			const bool bEndpointBVisible =
				EvaluateEndpoint(*PortalB, bEndpointBValid);
			bNewOcclusionValid = bEndpointAValid && bEndpointBValid;
			if (bNewOcclusionValid)
			{
				NewVisibleMask = (bEndpointAVisible ? WPCaptureEndpointAMask : 0)
					| (bEndpointBVisible ? WPCaptureEndpointBMask : 0);
				DecisionReason = NewVisibleMask == 0
					? TEXT("BothEndpointsFullyOccluded")
					: TEXT("AtLeastOneEndpointTraceOpen");
			}
			else
			{
				NewVisibleMask = WPCaptureBothEndpointsMask;
				DecisionReason = TEXT("InvalidEndpointGeometryFailOpen");
			}
		}
		else if (Visibility.bInsideSelected)
		{
			DecisionReason = TEXT("InsideSelectedBypass");
		}
		else if (Visibility.bInsideBlocked)
		{
			DecisionReason = TEXT("InsideExclusiveBlockedNoTrace");
		}
		else if (Visibility.LastVisibleEndpointMask == 0)
		{
			DecisionReason = TEXT("FrustumAlreadyInvisibleNoTrace");
		}
		else
		{
			DecisionReason = TEXT("ReferenceViewUnavailableFailOpen");
		}

		Visibility.OcclusionVisibleEndpointMask = NewVisibleMask;
		Visibility.bOcclusionValid = bNewOcclusionValid;
		const bool bStateChanged =
			Visibility.bLastLoggedOcclusionValid != bNewOcclusionValid
			|| Visibility.LastLoggedOcclusionVisibleEndpointMask != NewVisibleMask;
		if (bStateChanged)
		{
			// Publish the conservative CPU mask without changing Renderer frustum feedback.
			// Production can then omit fully occluded endpoints while the RT mailbox keeps
			// reporting the primary-view frustum needed for later capture resumption.
			PairState.Publication.bDirty = true;
		}
		if (bStateChanged)
		{
#if !UE_BUILD_SHIPPING
			const double CpuMs =
				(FPlatformTime::Seconds() - EvaluationStartSeconds) * 1000.0;
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureVisibility][OcclusionTrace] State changed. World=%s Frame=%llu PairId=%s PreviousValid=%d CurrentValid=%d PreviousVisibleMask=0x%02x CurrentVisibleMask=0x%02x TraceCount=%d BlockedTraceCount=%d InsideSelected=%d InsideBlocked=%d Reason=%s PairSwitchRecaptureScheduled=0 TransitCaptureForced=0 CpuMs=%.4f"),
				*GetNameSafe(World),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				Visibility.bLastLoggedOcclusionValid ? 1 : 0,
				bNewOcclusionValid ? 1 : 0,
				static_cast<uint32>(
					Visibility.LastLoggedOcclusionVisibleEndpointMask),
				static_cast<uint32>(NewVisibleMask),
				TraceCount, BlockedTraceCount,
				Visibility.bInsideSelected ? 1 : 0,
				Visibility.bInsideBlocked ? 1 : 0,
				DecisionReason,
				CpuMs);
#endif
		}
		Visibility.bLastLoggedOcclusionValid = bNewOcclusionValid;
		Visibility.LastLoggedOcclusionVisibleEndpointMask = NewVisibleMask;
	}
}

void FWPRuntimeCaptureScheduler::HandleWorldPostActorTick(
	UWorld* World,
	const ELevelTick TickType,
	const float DeltaSeconds)
{
	(void)TickType;
	if (World != GetWorld() || !IsInitialized() || !World || !World->IsGameWorld()
		|| World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	RunCaptureScheduler(DeltaSeconds);
}

bool FWPRuntimeCaptureScheduler::UpdatePairCaptureResolution(
	FWPPortalPairState& PairState,
	const FVector& CameraLocation,
	const float CameraFOVDegrees,
	const bool bHasReferenceView,
	const double DeltaSeconds,
	const double NowSeconds)
{
	const double StartSeconds = FPlatformTime::Seconds();
	AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	FWPCaptureResolutionState& State = PairState.Capture.Resolution;
	if (!CaptureManager || !IsValid(PortalA) || !IsValid(PortalB))
	{
		return false;
	}

	const uint32 ActualResolutionA =
		CaptureManager->GetEndpointCaptureResolution(PortalA);
	const uint32 ActualResolutionB =
		CaptureManager->GetEndpointCaptureResolution(PortalB);
	if (State.Phase == EWPCaptureResolutionTransitionPhase::Stable
		&& ActualResolutionA != 0 && ActualResolutionA == ActualResolutionB)
	{
		State.CurrentResolution = ActualResolutionA;
	}

	double ScreenDiameterRatio = 0.0;
	if (bHasReferenceView
		&& IsFiniteReferenceLocation(CameraLocation)
		&& IsValidWPCaptureVisibilityFOV(CameraFOVDegrees))
	{
		const double HalfFOVRadians =
			FMath::DegreesToRadians(static_cast<double>(CameraFOVDegrees) * 0.5);
		const double TanHalfFOV = FMath::Tan(HalfFOVRadians);
		const auto EvaluateEndpointRatio =
			[&CameraLocation, TanHalfFOV](const AWormholePortalActor& Portal)
		{
			const double PortalRadiusCm =
				static_cast<double>(Portal.GetPortalRadius());
			const double OuterRadiusCm =
				static_cast<double>(Portal.GetTransitionRadius());
			const double SafeProxyRadiusCm = FMath::Max(
				OuterRadiusCm,
				PortalRadiusCm + WPCaptureProxySafetyShellCm);
			const double DistanceSquared = FVector::DistSquared(
				CameraLocation, Portal.GetActorLocation());
			if (!FMath::IsFinite(SafeProxyRadiusCm)
				|| SafeProxyRadiusCm <= 0.0
				|| !FMath::IsFinite(DistanceSquared)
				|| TanHalfFOV <= UE_SMALL_NUMBER)
			{
				return 0.0;
			}
			const double RadiusSquared = FMath::Square(SafeProxyRadiusCm);
			if (DistanceSquared <= RadiusSquared)
			{
				return 1.0;
			}
			const double ProjectedRatio = SafeProxyRadiusCm
				/ (FMath::Sqrt(DistanceSquared - RadiusSquared) * TanHalfFOV);
			return FMath::Clamp(ProjectedRatio, 0.0, 1.0);
		};
		ScreenDiameterRatio = FMath::Max(
			EvaluateEndpointRatio(*PortalA),
			EvaluateEndpointRatio(*PortalB));
	}
	State.LastScreenDiameterRatio = ScreenDiameterRatio;
	const FWPDynamicCaptureResolutionPolicy ResolutionPolicy =
		ResolveWPDynamicCaptureResolutionPolicy();

	// VisibleEndpoints == 0은 아직 "숨김 후보"일 뿐입니다. Renderer feedback이 0을
	// 반환한 첫 프레임부터 해상도를 바꾸면, resource/ownership 전환이 아래의 0.5초
	// visibility hold를 초기화할 수 있습니다. 실제 inactive 판정은 hold가 끝나
	// bPaused가 true가 된 뒤에만 성립합니다.
	const bool bStrictlyInactive = PairState.Capture.Visibility.bInsideBlocked
		|| PairState.Capture.Visibility.bPaused;
	const bool bVisibilityHoldInProgress = !bStrictlyInactive
		&& !PairState.Capture.Visibility.bInsideSelected
		&& PairState.Capture.Visibility.bGuardInitialized
		&& State.CurrentResolution != 0;
	uint32 RequestedResolution = bStrictlyInactive || !bHasReferenceView
		? ResolutionPolicy.LowestVisibleResolution
		: ResolveWPDynamicCaptureTier(ScreenDiameterRatio, ResolutionPolicy);
	if (bVisibilityHoldInProgress)
	{
		// 숨김 후보를 확인하는 동안은 현재 리소스를 그대로 유지합니다. 화면 비율이나
		// VisibleEndpoints=0만으로 upgrade/downgrade를 시작하지 않습니다.
		RequestedResolution = State.CurrentResolution;
	}
	if (PairState.Capture.Visibility.bInsideSelected)
	{
		RequestedResolution = ResolveWPInsideCaptureResolution(
			ResolutionPolicy);
	}

	// Memory is diagnostic only. The requested Project Settings tier is no longer
	// downgraded by a plugin-local Cubemap budget.
	const double PredictedPairMiB =
		CaptureManager->GetEstimatedPairColorMemoryMiB(RequestedResolution);
	State.LastPredictedPairColorMiB = PredictedPairMiB;

	if (State.Phase != EWPCaptureResolutionTransitionPhase::Stable)
	{
		const auto RollbackSeamlessTransition =
			[&](const TCHAR* FailureReason)
		{
			CaptureManager->CancelPairResolutionTransition(
				PortalA, PortalB, FailureReason);
			const uint32 RestoredResolutionA =
				CaptureManager->GetEndpointCaptureResolution(PortalA);
			const uint32 RestoredResolutionB =
				CaptureManager->GetEndpointCaptureResolution(PortalB);
			State.CurrentResolution = RestoredResolutionA != 0
				&& RestoredResolutionA == RestoredResolutionB
				? RestoredResolutionA : 0;
			State.DesiredResolution = State.CurrentResolution;
			State.CandidateResolution = State.CurrentResolution;
			State.CandidateElapsedSeconds = 0.0;
			State.LastTransitionCompleteSeconds = NowSeconds;
			State.Phase = EWPCaptureResolutionTransitionPhase::Stable;
			State.bSeamlessTransition = false;
			State.SeamlessResourceGenerationA = 0;
			State.SeamlessResourceGenerationB = 0;
			State.SeamlessCaptureFailureCount = 0;
			State.LastPredictedTransitionPeakMiB =
				CaptureManager->GetEstimatedResidentColorMemoryMiB();
			State.ReleaseFence.Reset();
			PairState.Capture.CadenceElapsedSeconds = 0.0;
			PairState.Publication.bDirty = true;
			ActiveResolutionTransitionPairId.Invalidate();
			WP_LOG(&Runtime, Error,
				TEXT("[CaptureResolution][Seamless] Replacement transition rolled back; frozen old A+B remain in use. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s RestoredResolution=%u RendererBlankInterval=0 OldCaptureRemainedFrozen=1 GameThreadWait=0 FailureReason=%s EstimatedResidentColorMemoryMiB=%.2f WallMs=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				*GetNameSafe(PortalA), *GetNameSafe(PortalB),
				State.CurrentResolution,
				FailureReason ? FailureReason : TEXT("Unspecified"),
				CaptureManager->GetEstimatedResidentColorMemoryMiB(),
				(NowSeconds - State.TransitionStartSeconds) * 1000.0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		};

		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointA)
		{
			CaptureManager->EnsureEndpointTransitionResources(
				PortalA, State.DesiredResolution);
			State.Phase =
				EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointA;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointA)
		{
			const bool bEndpointAReady =
				CaptureManager->HasEndpointResources(
					PortalA, State.DesiredResolution)
				|| CaptureManager->EnsureEndpointTransitionResources(
					PortalA, State.DesiredResolution);
			if (!bEndpointAReady)
			{
				return false;
			}
			State.Phase =
				EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointB;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointB)
		{
			CaptureManager->EnsureEndpointTransitionResources(
				PortalB, State.DesiredResolution);
			State.Phase =
				EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointB;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointB)
		{
			const bool bEndpointBReady =
				CaptureManager->HasEndpointResources(
					PortalB, State.DesiredResolution)
				|| CaptureManager->EnsureEndpointTransitionResources(
					PortalB, State.DesiredResolution);
			if (!bEndpointBReady)
			{
				return false;
			}
			if (!CaptureManager->ResetPairCaptureCycleForResolutionTransition(
				PairState.Identity.PairId,
				TEXT("SeamlessReplacementResourcesReady")))
			{
				RollbackSeamlessTransition(TEXT("PairCaptureCycleResetFailed"));
				return false;
			}
			State.Phase =
				EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointA;
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][Seamless] Replacement A/B resources ready; old published textures remain frozen. World=%s Frame=%llu PairId=%s DesiredResolution=%u NextCapture=EndpointA OldTextureStillDisplayed=1 OldCaptureSubmission=0 NewCaptureSubmissionThisFrame=0 EstimatedResidentColorMemoryMiB=%.2f PredictedTransitionPeakMiB=%.2f WallMs=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				State.DesiredResolution,
				CaptureManager->GetEstimatedResidentColorMemoryMiB(),
				State.LastPredictedTransitionPeakMiB,
				(NowSeconds - State.TransitionStartSeconds) * 1000.0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointA
			|| State.Phase
				== EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointB)
		{
			const EWPManagedCaptureSubmissionMode SubmissionMode =
				State.Phase
					== EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointA
				? EWPManagedCaptureSubmissionMode::EndpointA
				: EWPManagedCaptureSubmissionMode::EndpointB;
			FWPManagedPairCaptureResult CaptureResult;
			const bool bCaptureSucceeded = CaptureManager->SubmitPairCapture(
				PairState.Identity.PairId, PortalA, PortalB,
				PairState.Capture.OwnershipEpoch, SubmissionMode, CaptureResult);
			if (!bCaptureSucceeded)
			{
				++State.SeamlessCaptureFailureCount;
				const uint32 FailureThreshold = static_cast<uint32>(FMath::Max(
					CVarWPCaptureSchedulerFailureRollbackThreshold
						.GetValueOnGameThread(), 3));
				if (State.SeamlessCaptureFailureCount >= FailureThreshold)
				{
					RollbackSeamlessTransition(
						TEXT("ReplacementCaptureFailureThreshold"));
				}
				return false;
			}
			State.SeamlessCaptureFailureCount = 0;
			if (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA)
			{
				State.Phase =
					EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointB;
#if !UE_BUILD_SHIPPING
				WP_LOG(&Runtime, Verbose,
					TEXT("[CaptureResolution][Seamless] Replacement Endpoint A captured. World=%s Frame=%llu PairId=%s Resolution=%u NextCapture=EndpointB OldTextureStillDisplayed=1 OldCaptureSubmission=0 NewCaptureGenerationA=%u NewCaptureGenerationB=%u PairCycleCompleted=%d CaptureCpuMs=%.4f DecisionCpuMs=%.4f"),
					*GetNameSafe(GetWorld()),
					static_cast<unsigned long long>(GFrameCounter),
					*PairIdToString(PairState.Identity.PairId),
					State.DesiredResolution,
					CaptureResult.CaptureGenerationAAfter,
					CaptureResult.CaptureGenerationBAfter,
					CaptureResult.bPairCycleCompleted ? 1 : 0,
					CaptureResult.TotalCpuMs,
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
				return false;
			}

			FWPCaptureEndpointSnapshot NewSnapshotA;
			FWPCaptureEndpointSnapshot NewSnapshotB;
			const bool bNewSnapshotsReady =
				CaptureManager->GetEndpointSnapshot(PortalA, NewSnapshotA)
				&& CaptureManager->GetEndpointSnapshot(PortalB, NewSnapshotB)
				&& NewSnapshotA.CaptureGeneration != 0
				&& NewSnapshotB.CaptureGeneration != 0;
			if (!bNewSnapshotsReady
				|| !CaptureManager->ActivatePairResolutionPublication(
					PortalA, PortalB, TEXT("ReplacementABCaptured")))
			{
				RollbackSeamlessTransition(
					TEXT("ReplacementPublicationActivationFailed"));
				return false;
			}
			State.SeamlessResourceGenerationA =
				NewSnapshotA.CubeContract.ResourceGeneration;
			State.SeamlessResourceGenerationB =
				NewSnapshotB.CubeContract.ResourceGeneration;
			State.CurrentResolution = State.DesiredResolution;
			State.CandidateResolution = State.DesiredResolution;
			State.CandidateElapsedSeconds = 0.0;
			PairState.Capture.CadenceElapsedSeconds = 0.0;
			PairState.Capture.bNextStaggeredEndpointA = true;
			PairState.Publication.bDirty = true;
			State.Phase =
				EWPCaptureResolutionTransitionPhase::AwaitingSeamlessPublication;
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][Seamless] Replacement A+B captured; atomic publication requested. World=%s Frame=%llu PairId=%s Resolution=%u NewResourceGenerationA=%u NewResourceGenerationB=%u NewCaptureGenerationA=%u NewCaptureGenerationB=%u OldTextureDisplayedUntilNextPacket=1 OldCaptureSubmission=0 RetiredResourcesHeld=1 EstimatedResidentColorMemoryMiB=%.2f CaptureCpuMs=%.4f DecisionCpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				State.CurrentResolution,
				State.SeamlessResourceGenerationA,
				State.SeamlessResourceGenerationB,
				NewSnapshotA.CaptureGeneration,
				NewSnapshotB.CaptureGeneration,
				CaptureManager->GetEstimatedResidentColorMemoryMiB(),
				CaptureResult.TotalCpuMs,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::AwaitingSeamlessPublication)
		{
			const bool bReplacementPacketPublished =
				PairState.Publication.bHasPublished
				&& PairState.Publication.bLastPublishedResourcesReady
				&& PairState.Publication.LastPublishedCubeContractA.ResourceGeneration
					== State.SeamlessResourceGenerationA
				&& PairState.Publication.LastPublishedCubeContractB.ResourceGeneration
					== State.SeamlessResourceGenerationB
				&& PairState.Publication.LastPublishedCaptureGenerationA != 0
				&& PairState.Publication.LastPublishedCaptureGenerationB != 0;
			if (!bReplacementPacketPublished)
			{
				return false;
			}
			State.ReleaseFence = MakeUnique<FRenderCommandFence>();
			State.ReleaseFence->BeginFence();
			State.Phase =
				EWPCaptureResolutionTransitionPhase::WaitingForRetiredReleaseFence;
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][Seamless] Renderer publication of replacement A+B observed; retired release fence begun. World=%s Frame=%llu PairId=%s Resolution=%u PublishedResourceGenerationA=%u PublishedResourceGenerationB=%u RendererNowUsesNew=1 RetiredResourcesHeld=1 GameThreadWait=0 WallMs=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				State.CurrentResolution,
				State.SeamlessResourceGenerationA,
				State.SeamlessResourceGenerationB,
				(NowSeconds - State.TransitionStartSeconds) * 1000.0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::WaitingForRetiredReleaseFence)
		{
			if (!State.ReleaseFence || !State.ReleaseFence->IsFenceComplete())
			{
				return false;
			}
			State.ReleaseFence.Reset();
			CaptureManager->ReleaseRetiredPairResolutionResources(
				PortalA, PortalB, TEXT("ReplacementPacketRenderFenceComplete"));
			State.LastTransitionCompleteSeconds = NowSeconds;
			State.Phase = EWPCaptureResolutionTransitionPhase::Stable;
			State.bSeamlessTransition = false;
			State.SeamlessResourceGenerationA = 0;
			State.SeamlessResourceGenerationB = 0;
			State.SeamlessCaptureFailureCount = 0;
			ActiveResolutionTransitionPairId.Invalidate();
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][Seamless] Transition complete; frozen old A+B released after replacement became visible. World=%s Frame=%llu PairId=%s Resolution=%u RendererBlankInterval=0 OldCaptureSubmissionDuringTransition=0 EstimatedResidentColorMemoryMiB=%.2f PredictedTransitionPeakMiB=%.2f SerializedTransitionReleased=1 GameThreadWait=0 WallMs=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				State.CurrentResolution,
				CaptureManager->GetEstimatedResidentColorMemoryMiB(),
				State.LastPredictedTransitionPeakMiB,
				(NowSeconds - State.TransitionStartSeconds) * 1000.0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return true;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::AwaitingUnavailablePublication)
		{
			if (!PairState.Publication.bHasPublished
				|| !PairState.Publication.bLastPublishedResourcesReady)
			{
				State.ReleaseFence = MakeUnique<FRenderCommandFence>();
				State.ReleaseFence->BeginFence();
				State.Phase =
					EWPCaptureResolutionTransitionPhase::WaitingForReleaseFence;
#if !UE_BUILD_SHIPPING
				WP_LOG(&Runtime, Verbose,
					TEXT("[CaptureResolution][Transition] Unavailable packet observed; release fence begun. World=%s Frame=%llu PairId=%s DesiredResolution=%u LastPublishedResourcesReady=%d GameThreadWait=0 Phase=%s WallMs=%.3f CpuMs=%.4f"),
					*GetNameSafe(GetWorld()),
					static_cast<unsigned long long>(GFrameCounter),
					*PairIdToString(PairState.Identity.PairId),
					State.DesiredResolution,
					PairState.Publication.bLastPublishedResourcesReady ? 1 : 0,
					GetWPCaptureResolutionPhaseName(State.Phase),
					(NowSeconds - State.TransitionStartSeconds) * 1000.0,
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			}
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::WaitingForReleaseFence)
		{
			if (!State.ReleaseFence || !State.ReleaseFence->IsFenceComplete())
			{
				return false;
			}
			State.ReleaseFence.Reset();
			State.Phase =
				EWPCaptureResolutionTransitionPhase::AllocatingEndpointA;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::AllocatingEndpointA)
		{
			CaptureManager->EnsureEndpointResources(
				PortalA, State.DesiredResolution);
			State.Phase =
				EWPCaptureResolutionTransitionPhase::WaitingForEndpointA;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::WaitingForEndpointA)
		{
			const bool bEndpointAReady =
				CaptureManager->HasEndpointResources(
					PortalA, State.DesiredResolution)
				|| CaptureManager->EnsureEndpointResources(
					PortalA, State.DesiredResolution);
			if (!bEndpointAReady)
			{
				return false;
			}
			State.Phase =
				EWPCaptureResolutionTransitionPhase::AllocatingEndpointB;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::AllocatingEndpointB)
		{
			CaptureManager->EnsureEndpointResources(
				PortalB, State.DesiredResolution);
			State.Phase =
				EWPCaptureResolutionTransitionPhase::WaitingForEndpointB;
			return false;
		}
		if (State.Phase
			== EWPCaptureResolutionTransitionPhase::WaitingForEndpointB)
		{
			const bool bEndpointBReady =
				CaptureManager->HasEndpointResources(
					PortalB, State.DesiredResolution)
				|| CaptureManager->EnsureEndpointResources(
					PortalB, State.DesiredResolution);
			if (!bEndpointBReady)
			{
				return false;
			}
			State.CurrentResolution = State.DesiredResolution;
			State.CandidateResolution = State.DesiredResolution;
			State.CandidateElapsedSeconds = 0.0;
			State.LastTransitionCompleteSeconds = NowSeconds;
			State.Phase = EWPCaptureResolutionTransitionPhase::Stable;
			State.bSeamlessTransition = false;
			State.SeamlessResourceGenerationA = 0;
			State.SeamlessResourceGenerationB = 0;
			State.SeamlessCaptureFailureCount = 0;
			PairState.Capture.CadenceElapsedSeconds = 0.0;
			PairState.Capture.bNextStaggeredEndpointA = true;
			PairState.Publication.bDirty = true;
			const bool bWarmupDeferredUntilVisible =
				State.bWarmupDeferredUntilVisible
				|| PairState.Capture.Visibility.bPaused
				|| PairState.Capture.Visibility.bInsideBlocked;
			State.bWarmupDeferredUntilVisible = bWarmupDeferredUntilVisible;
			if (!bWarmupDeferredUntilVisible)
			{
				SetPairCaptureAuthority(
					PairState,
					EWPCaptureAuthority::RuntimeWarmup,
					TEXT("DynamicResolutionResourcesReadyVisible"));
			}
			ActiveResolutionTransitionPairId.Invalidate();
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][Transition] Pair resources committed. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s Resolution=%u PredictedPairMiB=%.2f EstimatedResidentColorMemoryMiB=%.2f VisibilityPaused=%d InsideBlocked=%d WarmupDeferredUntilVisible=%d NextCapture=%s ForcedABCaptureNow=0 SerializedTransitionReleased=1 GameThreadWait=0 WallMs=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
				*GetNameSafe(PortalB), State.CurrentResolution,
				State.LastPredictedPairColorMiB,
				CaptureManager->GetEstimatedResidentColorMemoryMiB(),
				PairState.Capture.Visibility.bPaused ? 1 : 0,
				PairState.Capture.Visibility.bInsideBlocked ? 1 : 0,
				bWarmupDeferredUntilVisible ? 1 : 0,
				bWarmupDeferredUntilVisible
					? TEXT("WaitForVisible")
					: TEXT("AtomicWarmup"),
				(NowSeconds - State.TransitionStartSeconds) * 1000.0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return true;
		}
	}

	State.DesiredResolution = RequestedResolution;
	if (State.CandidateResolution != RequestedResolution)
	{
		State.CandidateResolution = RequestedResolution;
		State.CandidateElapsedSeconds = 0.0;
	}
	else
	{
		State.CandidateElapsedSeconds += FMath::Max(DeltaSeconds, 0.0);
	}

	const bool bEndpointsReadyAtCurrent = State.CurrentResolution != 0
		&& CaptureManager->HasEndpointResources(
			PortalA, State.CurrentResolution)
		&& CaptureManager->HasEndpointResources(
			PortalB, State.CurrentResolution);
	if (bEndpointsReadyAtCurrent
		&& State.CurrentResolution == RequestedResolution)
	{
		return true;
	}
	if (ActiveResolutionTransitionPairId.IsValid()
		&& ActiveResolutionTransitionPairId != PairState.Identity.PairId)
	{
		return bEndpointsReadyAtCurrent;
	}

	const bool bDowngrade = State.CurrentResolution == 0
		|| RequestedResolution < State.CurrentResolution;
	const double RequiredHoldSeconds = State.CurrentResolution == 0
		|| bStrictlyInactive
		? 0.0
		: FMath::Max(
			static_cast<double>((bDowngrade
				? CVarWPCaptureResolutionDowngradeHoldSeconds
				: CVarWPCaptureResolutionUpgradeHoldSeconds)
				.GetValueOnGameThread()),
			0.0);
	const double MinimumDwellSeconds = FMath::Max(
		static_cast<double>(
			CVarWPCaptureResolutionMinimumDwellSeconds.GetValueOnGameThread()),
		0.0);
	const bool bUrgentInactiveDowngrade = bDowngrade && bStrictlyInactive;
	const bool bDwellSatisfied = bUrgentInactiveDowngrade
		|| NowSeconds - State.LastTransitionCompleteSeconds >= MinimumDwellSeconds;
	if (State.CandidateElapsedSeconds < RequiredHoldSeconds || !bDwellSatisfied)
	{
		return bEndpointsReadyAtCurrent;
	}

	ActiveResolutionTransitionPairId = PairState.Identity.PairId;
	State.DesiredResolution = RequestedResolution;
	State.TransitionStartSeconds = NowSeconds;
	// 이 transition이 실제 pause/inside-block에 의해 시작되었는지를 기록합니다.
	// 화면에 보이는 상태에서 시작한 일반적인 tier 전환에는 warmup을 지연하지 않습니다.
	State.bWarmupDeferredUntilVisible = bStrictlyInactive;
	const uint32 PreviousResolution = State.CurrentResolution;
	const bool bHadResources = ActualResolutionA != 0 || ActualResolutionB != 0
		|| CaptureManager->GetEndpointEstimatedColorMemoryMiB(PortalA) > 0.0
		|| CaptureManager->GetEndpointEstimatedColorMemoryMiB(PortalB) > 0.0;
	const double ResidentBeforeTransitionMiB =
		CaptureManager->GetEstimatedResidentColorMemoryMiB();
	const double PredictedTransitionPeakMiB =
		ResidentBeforeTransitionMiB + PredictedPairMiB;
	const bool bCanUseSeamlessFrozenOld = bHadResources
		&& bEndpointsReadyAtCurrent
		&& !bStrictlyInactive;
	State.LastPredictedTransitionPeakMiB = PredictedTransitionPeakMiB;
	State.bSeamlessTransition = bCanUseSeamlessFrozenOld;
	State.SeamlessResourceGenerationA = 0;
	State.SeamlessResourceGenerationB = 0;
	State.SeamlessCaptureFailureCount = 0;
	if (bCanUseSeamlessFrozenOld)
	{
		// The currently published Cubemaps are not submitted again. Their last completed
		// textures remain frozen until separately allocated A/B replacements are captured.
		State.Phase =
			EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointA;
	}
	else
	{
		CaptureManager->ReleaseEndpointResourcesForResolutionChange(
			PortalA, TEXT("DynamicResolutionReleaseFirst"));
		CaptureManager->ReleaseEndpointResourcesForResolutionChange(
			PortalB, TEXT("DynamicResolutionReleaseFirst"));
		State.CurrentResolution = 0;
		PairState.Publication.bDirty = true;
		State.Phase = bHadResources
			? EWPCaptureResolutionTransitionPhase::AwaitingUnavailablePublication
			: EWPCaptureResolutionTransitionPhase::AllocatingEndpointA;
	}
	const FString ResolutionTierSummary =
		DescribeWPDynamicCaptureResolutionTiers(ResolutionPolicy);
#if !UE_BUILD_SHIPPING
	WP_LOG(&Runtime, Verbose,
		TEXT("[CaptureResolution][Transition] Dynamic resolution transition started. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s PreviousResolution=%u DesiredResolution=%u TransitionMode=%s OldTextureDisplayedDuringPreparation=%d OldCaptureFrozen=%d ScreenDiameterRatio=%.5f LowestVisibleResolution=%u TierCount=%d Tiers=%s InsideProjectResolution=%u ResolutionSource=ProjectSettingsArray ThresholdOrderAdjusted=%d ResolutionOrderAdjusted=%d StrictlyInactive=%d VisibilityHoldInProgress=%d InsideSelected=%d WarmupDeferredUntilVisible=%d ResidentBeforeTransitionMiB=%.2f PredictedReplacementPairMiB=%.2f PredictedTransitionPeakMiB=%.2f MemoryBudgetFallbackRemoved=1 HadResources=%d UnavailablePacketRequired=%d SerializedTransition=1 GameThreadWait=0 Phase=%s CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter),
		*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
		*GetNameSafe(PortalB), PreviousResolution, State.DesiredResolution,
		bCanUseSeamlessFrozenOld ? TEXT("SeamlessFrozenOld") : TEXT("ReleaseFirst"),
		bCanUseSeamlessFrozenOld ? 1 : 0,
		bCanUseSeamlessFrozenOld ? 1 : 0,
		ScreenDiameterRatio,
		ResolutionPolicy.LowestVisibleResolution,
		ResolutionPolicy.Tiers.Num(),
		*ResolutionTierSummary,
		ResolutionPolicy.InsideResolution,
		ResolutionPolicy.bThresholdOrderAdjusted ? 1 : 0,
		ResolutionPolicy.bResolutionOrderAdjusted ? 1 : 0,
		bStrictlyInactive ? 1 : 0,
		bVisibilityHoldInProgress ? 1 : 0,
		PairState.Capture.Visibility.bInsideSelected ? 1 : 0,
		State.bWarmupDeferredUntilVisible ? 1 : 0,
		ResidentBeforeTransitionMiB, PredictedPairMiB,
		PredictedTransitionPeakMiB,
		bHadResources ? 1 : 0,
		!bCanUseSeamlessFrozenOld && bHadResources ? 1 : 0,
		GetWPCaptureResolutionPhaseName(State.Phase),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return false;
}

void FWPRuntimeCaptureScheduler::RunCaptureScheduler(const float DeltaSeconds)
{
	// Includes Callback CPU time in mode-transition logs and each Pair-decision log.
	const double CallbackStartSeconds = FPlatformTime::Seconds();
	if (ActiveResolutionTransitionPairId.IsValid()
		&& !PairStates.Contains(ActiveResolutionTransitionPairId))
	{
		ActiveResolutionTransitionPairId.Invalidate();
	}
	const int32 ModeRaw = CVarWPCaptureSchedulerMode.GetValueOnGameThread();
	const FWPCaptureSchedulerModePolicyResult ModePolicy =
		EvaluateWPCaptureSchedulerModePolicy(ModeRaw, bRenderPacketPipelineActive);
	const double SafeDeltaSeconds = FMath::IsFinite(DeltaSeconds) ? FMath::Max(static_cast<double>(DeltaSeconds), 0.0) : 0.0;
	const double ConfiguredMaxEndpointHz = FMath::Clamp(GetWPCaptureTargetEndpointHz(), 1.0, 30.0);
	const double ObservedFPS = SafeDeltaSeconds > UE_SMALL_NUMBER ? 1.0 / SafeDeltaSeconds : 60.0;
	const double TargetEndpointHz = FMath::Max(FMath::Min(ConfiguredMaxEndpointHz, ObservedFPS * 0.5), 0.001);
	const double EndpointCadenceSeconds = 1.0 / TargetEndpointHz;
	const double StaggeredSubmissionCadenceSeconds = 0.5 / TargetEndpointHz;
	const bool bModeChanged = LastModeRaw != ModeRaw
		|| bRuntimeActive != ModePolicy.bRuntimeActive;

	bRuntimeActive = ModePolicy.bRuntimeActive;
	if (bModeChanged)
	{
		if (!ModePolicy.bModeValid)
		{
			WP_LOG(&Runtime, Warning,
				TEXT("[CaptureScheduler][Mode] Invalid mode selected Runtime production capture fail-closed path. World=%s ModeRaw=%d PairCount=%d RuntimeActive=1 ActorFallbackAvailable=0 TransitForcesCapture=0 CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), ModeRaw, PairStates.Num(),
				(FPlatformTime::Seconds() - CallbackStartSeconds) * 1000.0);
		}
#if !UE_BUILD_SHIPPING
		WP_LOG(&Runtime, Verbose,
			TEXT("[CaptureScheduler][Mode] Authority mode evaluated. World=%s ModeRaw=%d ModeValid=%d DeprecatedAliasSelected=%d RenderPacketPipelineEnabled=%d RuntimeActive=%d PairCount=%d StaggeredSteadyState=%d ObservedFPS=%.3f TargetEndpointHz=%.3f EndpointCadenceMs=%.3f StaggeredSubmissionCadenceMs=%.3f LegacyAtomicCadenceMs=%.3f WarmupAtomicFallback=1 TransitAtomicFallback=0 CadenceDebtAtomicFallback=0 TransitImmediate=0 TransitForcesCapture=0 InvalidModeUsesRuntimeProduction=1 ActorFallbackAvailable=0 RuntimeTransitionBoundary=PostActorTick Decision=%s CpuMs=%.4f"),
			*GetNameSafe(GetWorld()), ModeRaw, ModePolicy.bModeValid ? 1 : 0,
			ModePolicy.bDeprecatedAlias ? 1 : 0,
			bRenderPacketPipelineActive ? 1 : 0, ModePolicy.bRuntimeActive ? 1 : 0,
			PairStates.Num(), ModePolicy.bStaggeredEndpointSubmission ? 1 : 0,
			ObservedFPS, TargetEndpointHz, EndpointCadenceSeconds * 1000.0,
			StaggeredSubmissionCadenceSeconds * 1000.0,
			WPLegacyAtomicCaptureCadenceSeconds * 1000.0,
			ModePolicy.DecisionReason,
			(FPlatformTime::Seconds() - CallbackStartSeconds) * 1000.0);
#endif
	}
	LastModeRaw = ModeRaw;
	ensureMsgf(ModePolicy.bRuntimeActive,
		TEXT("Runtime production capture must remain active for every scheduler mode."));

	const int32 FailureRollbackThreshold = FMath::Max(
		CVarWPCaptureSchedulerFailureRollbackThreshold.GetValueOnGameThread(), 1);
	const bool bVisibilityPauseEnabled = WPCaptureVisibilityPolicyAlwaysEnabled;
	FVector VisibilityCameraLocation = FVector::ZeroVector;
	FRotator VisibilityCameraRotation = FRotator::ZeroRotator;
	AActor* VisibilityViewActor = nullptr;
	float VisibilityCameraFOVDegrees = 0.0f;
	// Visibility policy is part of the default production contract, so every Scheduler
	// Callback resolves the
	// Reference View and consumes the frustum result from the Renderer feedback mailbox.
	const bool bVisibilityReferenceViewAvailable = bVisibilityPauseEnabled
		&& ResolveReferenceView(
			VisibilityCameraLocation,
			VisibilityCameraRotation,
			VisibilityViewActor,
			VisibilityCameraFOVDegrees);
	const uint32 VisibilityViewActorId = IsValid(VisibilityViewActor)
		? VisibilityViewActor->GetUniqueID()
		: 0;
	double VisibilityViewAspectRatio = 0.0;
	if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		int32 ViewportWidth = 0;
		int32 ViewportHeight = 0;
		PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
		if (ViewportWidth > 0 && ViewportHeight > 0)
		{
			VisibilityViewAspectRatio = static_cast<double>(ViewportWidth) / static_cast<double>(ViewportHeight);
		}
	}
	const double VisibilityInvisibleHoldSeconds = FMath::Max(
		static_cast<double>(
			CVarWPCaptureVisibilityInvisibleHoldSeconds.GetValueOnGameThread()),
		0.0);
	const double VisibilityFeedbackMaxAgeSeconds = FMath::Max(
		static_cast<double>(
			CVarWPCaptureVisibilityFeedbackMaxAgeSeconds.GetValueOnGameThread()),
		0.05);
	const double RequestedVisibilityHiddenRefreshHz = FMath::Max(
		static_cast<double>(
			CVarWPCaptureVisibilityHiddenRefreshHz.GetValueOnGameThread()),
		0.0);
	const double VisibilityHiddenRefreshHz =
		ResolveWPEffectiveHiddenRefreshHz(
			RequestedVisibilityHiddenRefreshHz);
	const double VisibilityHiddenRefreshCadenceSeconds =
		VisibilityHiddenRefreshHz > 0.0
			? 1.0 / VisibilityHiddenRefreshHz
			: TNumericLimits<double>::Max();

	for (TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
	{
		// Logging only: records CPU time for this Pair's Scheduler decision.
		const double DecisionStartSeconds = FPlatformTime::Seconds();
		FWPPortalPairState& PairState = PairEntry.Value;
		AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
		AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
		if (!IsValid(PortalA) || !IsValid(PortalB))
		{
			WP_LOG(&Runtime, Warning,
				TEXT("[CaptureScheduler][Preflight] Pair endpoint became invalid; Registry reconciliation requested. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s PortalAValid=%d PortalBValid=%d CapturePairStateRetainedUntilRegistryRemoval=1 ActorFallbackAvailable=0 CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
				*GetNameSafe(PortalB), IsValid(PortalA) ? 1 : 0,
				IsValid(PortalB) ? 1 : 0,
				(FPlatformTime::Seconds() - DecisionStartSeconds) * 1000.0);
			bPairsDirty = true;
			continue;
		}
		if (PairState.Capture.OwnershipEpoch == 0)
		{
			SetPairCaptureAuthority(
				PairState, EWPCaptureAuthority::RuntimeWarmup,
				TEXT("ManagerOwnedPairColdStart"));
		}
		// 정상적인 steady-state에서는 Renderer의 최신 View Frustum 결과를 먼저
		// 소비한 뒤 해상도를 결정합니다. 그래야 V0 invisible hold가 해상도 전환의
		// resource/ownership warmup에 의해 먼저 끊기지 않습니다. 리소스가 없거나
		// 이미 전환 중인 경우에만 visibility 판정이 불가능하므로 선행 bootstrap을
		// 허용합니다.
		const auto PassResolutionGate = [&]()
		{
			const bool bResolutionResourcesReady = UpdatePairCaptureResolution(
				PairState,
				VisibilityCameraLocation,
				VisibilityCameraFOVDegrees,
				bVisibilityReferenceViewAvailable,
				SafeDeltaSeconds,
				FPlatformTime::Seconds());
			return bResolutionResourcesReady;
		};
		const uint32 ExistingResolution =
			PairState.Capture.Resolution.CurrentResolution;
		const bool bResolutionBootstrapRequired = !CaptureManager
			|| PairState.Capture.Resolution.Phase
				!= EWPCaptureResolutionTransitionPhase::Stable
			|| ExistingResolution == 0
			|| !CaptureManager->HasEndpointResources(PortalA, ExistingResolution)
			|| !CaptureManager->HasEndpointResources(PortalB, ExistingResolution);
		if (bResolutionBootstrapRequired && !PassResolutionGate())
		{
			continue;
		}

		if (PairState.Capture.Visibility.bInsideBlocked)
		{
			// Inside-exclusive 상태 자체가 먼저 확정된 visibility 결정입니다. 해당
			// Pair의 VRAM downgrade는 이 결정 뒤에 실행합니다.
			if (!bResolutionBootstrapRequired && !PassResolutionGate())
			{
				continue;
			}
			// While the Camera is inside another Pair's SafeProxy, freeze both cadence and
			// submission for this Pair.
			// Retain its resources and existing Cubemap contents, and do not let the selection
			// change create cadence
			// debt or schedule a recapture.
			continue;
		}

		PairState.Capture.CadenceElapsedSeconds += SafeDeltaSeconds;

		const uint32 CommittedResolution =
			PairState.Capture.Resolution.CurrentResolution;
		const bool bEndpointAAllocated = CaptureManager
			&& (CaptureManager->HasEndpointResources(
					PortalA, CommittedResolution)
				|| CaptureManager->EnsureEndpointResources(
					PortalA, CommittedResolution));
		const bool bEndpointBAllocated = CaptureManager
			&& (CaptureManager->HasEndpointResources(
					PortalB, CommittedResolution)
				|| CaptureManager->EnsureEndpointResources(
					PortalB, CommittedResolution));
		FWPCaptureEndpointSnapshot SnapshotA;
		FWPCaptureEndpointSnapshot SnapshotB;
		const bool bEndpointAReady = bEndpointAAllocated
			&& CaptureManager->GetEndpointSnapshot(PortalA, SnapshotA)
			&& SnapshotA.IsReadyForSubmission(GetWorld());
		const bool bEndpointBReady = bEndpointBAllocated
			&& CaptureManager->GetEndpointSnapshot(PortalB, SnapshotB)
			&& SnapshotB.IsReadyForSubmission(GetWorld());
		const bool bManagedResourcesReady = bEndpointAReady && bEndpointBReady;
		if (!bManagedResourcesReady)
		{
			PairState.Capture.LastFailedSubmissionFrame = GFrameCounter;
			++PairState.Capture.ConsecutiveFailureCount;
			if (PairState.Capture.ConsecutiveFailureCount
				>= static_cast<uint32>(FailureRollbackThreshold))
			{
				RecoverPairCaptureAuthority(
					PairState, TEXT("RuntimePreflightFailureThreshold"));
			}
			continue;
		}

		const bool bCommitWaitingForFreshCamera = CaptureManager
			&& CaptureManager->IsPairCommitWaitingForFreshCamera(PairState.Identity.PairId);
		const bool bWarmupRequiresAtomicPair =
			PairState.Capture.Authority == EWPCaptureAuthority::RuntimeWarmup
			|| SnapshotA.CaptureGeneration == 0
			|| SnapshotB.CaptureGeneration == 0;
		const FWPPairOwnershipFeedback& VisibilityFeedback =
			PairState.Ownership.LastOwnershipFeedback;
		double VisibilityFeedbackAgeMsForLog = -1.0;
		const TCHAR* VisibilityFreshnessReasonForLog = TEXT("NotEvaluated");
		const TCHAR* VisibilityCameraGuardReasonForLog = TEXT("NotEvaluated");
		const bool bEvaluateCaptureVisibility =
			bVisibilityPauseEnabled
			|| PairState.Capture.Visibility.bInsideSelected
			|| PairState.Capture.Visibility.bPaused
			|| PairState.Capture.Resolution.bWarmupDeferredUntilVisible
			|| PairState.Capture.Visibility.bResumePrewarmPending
			|| PairState.Capture.Visibility.bGuardInitialized
			|| PairState.Capture.Visibility.RequiredPacketSequence != 0;
		if (bEvaluateCaptureVisibility)
		{
			const double VisibilityDecisionStartSeconds = FPlatformTime::Seconds();
			const bool bCachedVisibilitySampleMatches =
				PairState.Capture.Visibility.LastOwnershipEpoch != 0
				&& PairState.Capture.Visibility.LastSampleSequence != 0
				&& VisibilityFeedback.VisibilityOwnershipEpoch
					== PairState.Capture.Visibility.LastOwnershipEpoch
				&& VisibilityFeedback.VisibilitySampleSequence
					== PairState.Capture.Visibility.LastSampleSequence;
			const double VisibilityFeedbackAgeSeconds =
				bCachedVisibilitySampleMatches
				? VisibilityDecisionStartSeconds
					- PairState.Capture.Visibility.LastSampleReceiptSeconds
				: TNumericLimits<double>::Max();
			VisibilityFeedbackAgeMsForLog = bCachedVisibilitySampleMatches
				? VisibilityFeedbackAgeSeconds * 1000.0
				: -1.0;
			const EWPCaptureVisibilityFreshness VisibilityFreshness =
				EvaluateWPCaptureVisibilityFreshness(
					bVisibilityPauseEnabled,
					VisibilityFeedback.bSnapshotCoherent,
					VisibilityFeedback.VisibilityOwnershipEpoch,
					PairState.Ownership.OwnershipEpoch,
					VisibilityFeedback.VisibilityPacketSequence,
					PairState.Capture.Visibility.RequiredPacketSequence,
					PairState.Publication.PacketSequence,
					bCachedVisibilitySampleMatches
						? VisibilityFeedback.VisibilitySampleSequence
						: 0,
					VisibilityFeedbackAgeSeconds,
					VisibilityFeedbackMaxAgeSeconds);
			VisibilityFreshnessReasonForLog =
				GetWPCaptureVisibilityFreshnessName(VisibilityFreshness);
			const bool bVisibilityFeedbackFresh =
				VisibilityFreshness == EWPCaptureVisibilityFreshness::Fresh;
			const bool bVisibilitySampleBeyondRejectBarrier =
				IsWPCaptureVisibilitySampleBeyondRejectBarrier(
					VisibilityFeedback.VisibilityOwnershipEpoch,
					VisibilityFeedback.VisibilitySampleSequence,
					PairState.Capture.Visibility.RejectedOwnershipEpoch,
					PairState.Capture.Visibility.RejectedThroughSampleSequence);
			if (bVisibilityFeedbackFresh
				&& !bVisibilitySampleBeyondRejectBarrier)
			{
				VisibilityFreshnessReasonForLog =
					TEXT("SampleAtOrBeforeRejectBarrier");
			}
			const bool bVisibilitySteadyStateReady =
				PairState.Capture.Authority == EWPCaptureAuthority::RuntimeOnly
				&& !bWarmupRequiresAtomicPair
				&& PairState.Ownership.EffectiveOwnership
					== EWPPairOwnershipMode::Production;
			const bool bAllEndpointsOccluded =
				PairState.Capture.Visibility.bOcclusionValid
				&& PairState.Capture.Visibility.OcclusionVisibleEndpointMask == 0;
			const bool bEffectiveEndpointVisible =
				HasWPEffectiveVisibleEndpoint(
					PairState.Capture.Visibility.GetLastVisibleEndpointCount(),
					PairState.Capture.Visibility.bOcclusionValid,
					PairState.Capture.Visibility.OcclusionVisibleEndpointMask,
					PairState.Capture.Visibility.bInsideSelected);
			const bool bWasVisibilityPaused = PairState.Capture.Visibility.bPaused;
			const TCHAR* VisibilityDecisionReason = TEXT("PolicyEvaluationPending");
			const auto ResetInvisibleSampleChain = [&PairState]()
			{
				PairState.Capture.Visibility.InvisibleElapsedSeconds = 0.0;
				PairState.Capture.Visibility.LastInvisibleSampleReceiptSeconds =
					-1.0e30;
				PairState.Capture.Visibility.LastInvisibleOwnershipEpoch = 0;
				PairState.Capture.Visibility.LastInvisibleSampleSequence = 0;
				PairState.Capture.Visibility.GuardViewActorId = 0;
				PairState.Capture.Visibility.GuardCameraLocation = FVector::ZeroVector;
				PairState.Capture.Visibility.GuardCameraRotation = FRotator::ZeroRotator;
				PairState.Capture.Visibility.GuardCameraFOVDegrees = 0.0f;
				PairState.Capture.Visibility.bGuardInitialized = false;
				PairState.Capture.Visibility.bAwaitingPostGuardSample = false;
			};
			const auto BeginCameraGuardSnapshot =
				[&PairState, VisibilityViewActorId, &VisibilityCameraLocation,
					&VisibilityCameraRotation, VisibilityCameraFOVDegrees]()
			{
				PairState.Capture.Visibility.GuardViewActorId = VisibilityViewActorId;
				PairState.Capture.Visibility.GuardCameraLocation =
					VisibilityCameraLocation;
				PairState.Capture.Visibility.GuardCameraRotation =
					VisibilityCameraRotation;
				PairState.Capture.Visibility.GuardCameraFOVDegrees =
					VisibilityCameraFOVDegrees;
				PairState.Capture.Visibility.bGuardInitialized = true;
			};
			const auto RejectCurrentVisibilitySample =
				[&PairState, &ResetInvisibleSampleChain]()
			{
				if (PairState.Capture.Visibility.LastOwnershipEpoch
						== PairState.Ownership.OwnershipEpoch
					&& PairState.Capture.Visibility.LastSampleSequence != 0)
				{
					PairState.Capture.Visibility.RejectedOwnershipEpoch =
						PairState.Capture.Visibility.LastOwnershipEpoch;
					PairState.Capture.Visibility.RejectedThroughSampleSequence =
						PairState.Capture.Visibility.LastSampleSequence;
				}
				ResetInvisibleSampleChain();
			};
			const auto PrimeInvisibleCameraGuard =
				[&PairState, &VisibilityFeedback, &ResetInvisibleSampleChain,
					&BeginCameraGuardSnapshot]()
			{
				ResetInvisibleSampleChain();
				BeginCameraGuardSnapshot();
				PairState.Capture.Visibility.RejectedOwnershipEpoch =
					VisibilityFeedback.VisibilityOwnershipEpoch;
				PairState.Capture.Visibility.RejectedThroughSampleSequence =
					VisibilityFeedback.VisibilitySampleSequence;
				PairState.Capture.Visibility.bAwaitingPostGuardSample = true;
			};
			const bool bCameraGuardRequired =
				PairState.Capture.Visibility.bGuardInitialized
				|| PairState.Capture.Visibility.LastInvisibleSampleSequence != 0
				|| PairState.Capture.Visibility.bPaused;
			EWPCaptureVisibilityCameraGuard VisibilityCameraGuard =
				EvaluateWPCaptureVisibilityCameraGuard(
					bCameraGuardRequired,
					bVisibilityReferenceViewAvailable,
					VisibilityViewActorId,
					VisibilityCameraLocation,
					VisibilityCameraRotation,
					VisibilityCameraFOVDegrees,
					PairState.Capture.Visibility.bGuardInitialized,
					PairState.Capture.Visibility.GuardViewActorId,
					PairState.Capture.Visibility.GuardCameraLocation,
					PairState.Capture.Visibility.GuardCameraRotation,
					PairState.Capture.Visibility.GuardCameraFOVDegrees);
			if (!bCameraGuardRequired
				&& bVisibilityFeedbackFresh
				&& bVisibilitySampleBeyondRejectBarrier
				&& !bEffectiveEndpointVisible)
			{
				// Validate the candidate snapshot before starting the first hold.
				VisibilityCameraGuard = EvaluateWPCaptureVisibilityCameraGuard(
					true,
					bVisibilityReferenceViewAvailable,
					VisibilityViewActorId,
					VisibilityCameraLocation,
					VisibilityCameraRotation,
					VisibilityCameraFOVDegrees,
					true,
					VisibilityViewActorId,
					VisibilityCameraLocation,
					VisibilityCameraRotation,
					VisibilityCameraFOVDegrees);
			}
			VisibilityCameraGuardReasonForLog =
				GetWPCaptureVisibilityCameraGuardName(VisibilityCameraGuard);
#if !UE_BUILD_SHIPPING
			const double VisibilityGuardLocationDeltaCm =
				PairState.Capture.Visibility.bGuardInitialized
					? FVector::Dist(
						VisibilityCameraLocation,
						PairState.Capture.Visibility.GuardCameraLocation)
					: -1.0;
			const FRotator VisibilityGuardRotationDelta =
				PairState.Capture.Visibility.bGuardInitialized
					? (VisibilityCameraRotation
						- PairState.Capture.Visibility.GuardCameraRotation)
						.GetNormalized()
					: FRotator::ZeroRotator;
			const double VisibilityGuardRotationDeltaDegrees =
				PairState.Capture.Visibility.bGuardInitialized
					? FMath::Max3(
						FMath::Abs(
							static_cast<double>(VisibilityGuardRotationDelta.Pitch)),
						FMath::Abs(
							static_cast<double>(VisibilityGuardRotationDelta.Yaw)),
						FMath::Abs(
							static_cast<double>(VisibilityGuardRotationDelta.Roll)))
					: -1.0;
			const double VisibilityGuardFOVDeltaDegrees =
				PairState.Capture.Visibility.bGuardInitialized
					? FMath::Abs(
						static_cast<double>(VisibilityCameraFOVDegrees)
						- PairState.Capture.Visibility.GuardCameraFOVDegrees)
					: -1.0;
#endif

			// Fresh renderer feedback already reflects the current primary-view frustum.
			// Camera motion changes that frustum but does not invalidate the sample.
			const bool bCameraMotionOnly =
				VisibilityCameraGuard
					== EWPCaptureVisibilityCameraGuard::LocationChanged
				|| VisibilityCameraGuard
					== EWPCaptureVisibilityCameraGuard::RotationChanged
				|| VisibilityCameraGuard
					== EWPCaptureVisibilityCameraGuard::FOVChanged;

			const bool bCameraGuardFailed =
				VisibilityCameraGuard != EWPCaptureVisibilityCameraGuard::NotRequired
				&& VisibilityCameraGuard != EWPCaptureVisibilityCameraGuard::Stable
				&& !bCameraMotionOnly;

			if (PairState.Capture.Visibility.bInsideSelected)
			{
				// The inside-selected Pair maintains normal cadence regardless of frustum or occlusion
				// results.
				// A selection change does not schedule resume prewarm or reset cadence.
				ResetInvisibleSampleChain();
				PairState.Capture.Visibility.bPaused = false;
				PairState.Capture.Visibility.bResumePrewarmPending = false;
				VisibilityDecisionReason = TEXT("InsideSelectedExclusiveActive");
			}
			else if (PairState.Capture.Resolution.bWarmupDeferredUntilVisible
				&& bWasVisibilityPaused)
			{
				// 숨겨진 상태에서 64-tier 리소스 전환이 끝나면 CaptureGeneration은 0이고
				// ownership은 아직 production steady-state가 아닐 수 있습니다. 기존의
				// WarmupOrOwnershipNotReady fail-open 경로를 타면 bPaused가 즉시 풀리고
				// 화면 밖에서도 A+B warmup이 실행되므로, 이 상태에서는 최신 Renderer
				// visibility가 실제로 보임을 확인할 때까지만 fail-closed로 유지합니다.
				const bool bCanResumeDeferredWarmup =
					bVisibilityFeedbackFresh
					&& bVisibilitySampleBeyondRejectBarrier
					&& !bCommitWaitingForFreshCamera
					&& !bCameraGuardFailed
					&& bEffectiveEndpointVisible;
				if (bCanResumeDeferredWarmup)
				{
					ResetInvisibleSampleChain();
					PairState.Capture.Visibility.RejectedOwnershipEpoch = 0;
					PairState.Capture.Visibility.RejectedThroughSampleSequence = 0;
					PairState.Capture.Visibility.bPaused = false;
					VisibilityDecisionReason =
						TEXT("DeferredResolutionWarmupVisibleResume");
				}
				else
				{
					PairState.Capture.Visibility.bPaused = true;
					PairState.Capture.Visibility.bResumePrewarmPending = false;
					VisibilityDecisionReason = !bVisibilityFeedbackFresh
						? TEXT("DeferredResolutionWarmupAwaitingFreshVisibility")
						: (bCameraGuardFailed
							? TEXT("DeferredResolutionWarmupReferenceViewGuard")
							: (bEffectiveEndpointVisible
								? TEXT("DeferredResolutionWarmupCommitWait")
								: TEXT("DeferredResolutionWarmupHidden")));
				}
			}
			else if (!bVisibilitySteadyStateReady)
			{
				RejectCurrentVisibilitySample();
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason = TEXT("WarmupOrOwnershipNotReady");
			}
			else if (bCommitWaitingForFreshCamera)
			{
				RejectCurrentVisibilitySample();
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason = TEXT("CommitFreshCameraFailOpen");
			}
			else if (bCameraGuardFailed)
			{
				RejectCurrentVisibilitySample();
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason = TEXT("ReferenceViewGuardFailOpen");
			}
			else if (!bVisibilityFeedbackFresh)
			{
				if (VisibilityFreshness
						== EWPCaptureVisibilityFreshness::
							MissingRequiredPacketFloor
					|| VisibilityFreshness
						== EWPCaptureVisibilityFreshness::
							PacketBeforeRequiredFloor
					|| VisibilityFreshness
						== EWPCaptureVisibilityFreshness::
							PacketAfterPublishedState)
				{
				}
				RejectCurrentVisibilitySample();
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason = TEXT("VisibilityFreshnessFailOpen");
			}
			else if (!bVisibilitySampleBeyondRejectBarrier)
			{
				if (!PairState.Capture.Visibility.bAwaitingPostGuardSample)
				{
					RejectCurrentVisibilitySample();
				}
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason =
					PairState.Capture.Visibility.bAwaitingPostGuardSample
						? TEXT("AwaitingPostGuardSample")
						: TEXT("VisibilityRejectBarrierFailOpen");
			}
			else if (bEffectiveEndpointVisible)
			{
				ResetInvisibleSampleChain();
				PairState.Capture.Visibility.RejectedOwnershipEpoch = 0;
				PairState.Capture.Visibility.RejectedThroughSampleSequence = 0;
				PairState.Capture.Visibility.bPaused = false;
				VisibilityDecisionReason = TEXT("FrustumOrOcclusionVisibleEndpoint");
			}
			else
			{
				if (!PairState.Capture.Visibility.bGuardInitialized)
				{
					PrimeInvisibleCameraGuard();
					PairState.Capture.Visibility.bPaused = false;
					VisibilityDecisionReason = bAllEndpointsOccluded
						? TEXT("BothEndpointsOccludedGuardPrimed")
						: TEXT("BothEndpointsInvisibleGuardPrimed");
				}
				else
				{
				const bool bNewInvisibleSample =
					VisibilityFeedback.VisibilityOwnershipEpoch
						!= PairState.Capture.Visibility.LastInvisibleOwnershipEpoch
					|| VisibilityFeedback.VisibilitySampleSequence
						!= PairState.Capture.Visibility.LastInvisibleSampleSequence;
				if (bNewInvisibleSample)
				{
					if (PairState.Capture.Visibility.bAwaitingPostGuardSample)
					{
						PairState.Capture.Visibility.bAwaitingPostGuardSample = false;
						PairState.Capture.Visibility.InvisibleElapsedSeconds = 0.0;
						PairState.Capture.Visibility.LastInvisibleOwnershipEpoch =
							VisibilityFeedback.VisibilityOwnershipEpoch;
						PairState.Capture.Visibility.LastInvisibleSampleSequence =
							VisibilityFeedback.VisibilitySampleSequence;
						PairState.Capture.Visibility.LastInvisibleSampleReceiptSeconds =
							PairState.Capture.Visibility.LastSampleReceiptSeconds;
					}
					else
					{
						const double InvisibleSampleGapSeconds =
							PairState.Capture.Visibility.LastInvisibleSampleSequence != 0
								? PairState.Capture.Visibility.LastSampleReceiptSeconds
									- PairState.Capture.Visibility.LastInvisibleSampleReceiptSeconds
								: TNumericLimits<double>::Max();
						const bool bContinuousInvisibleSamples =
							IsWPCaptureVisibilitySampleGapContinuous(
								PairState.Capture.Visibility.LastInvisibleOwnershipEpoch,
								PairState.Capture.Visibility.LastInvisibleSampleSequence,
								VisibilityFeedback.VisibilityOwnershipEpoch,
								InvisibleSampleGapSeconds,
								VisibilityFeedbackMaxAgeSeconds);
						if (!bContinuousInvisibleSamples)
						{
							// A hitch, missing sample, or epoch discontinuity primes
							// a brand-new immutable guard. This sample is rejected
							// from the hold; only a later RT sample may start it.
							PrimeInvisibleCameraGuard();
							PairState.Capture.Visibility.bPaused = false;
							VisibilityDecisionReason =
								TEXT("InvisibleSampleGapGuardReprimed");
						}
						else
						{
							PairState.Capture.Visibility.InvisibleElapsedSeconds +=
								InvisibleSampleGapSeconds;
							PairState.Capture.Visibility.LastInvisibleOwnershipEpoch =
								VisibilityFeedback.VisibilityOwnershipEpoch;
							PairState.Capture.Visibility.LastInvisibleSampleSequence =
								VisibilityFeedback.VisibilitySampleSequence;
							PairState.Capture.Visibility.LastInvisibleSampleReceiptSeconds =
								PairState.Capture.Visibility.LastSampleReceiptSeconds;
						}
					}
				}
				if (!PairState.Capture.Visibility.bAwaitingPostGuardSample)
				{
					PairState.Capture.Visibility.bPaused =
						PairState.Capture.Visibility.InvisibleElapsedSeconds
							>= VisibilityInvisibleHoldSeconds;
					VisibilityDecisionReason = PairState.Capture.Visibility.bPaused
						? (bAllEndpointsOccluded
							? TEXT("BothEndpointsOccludedPaused")
							: TEXT("BothEndpointsInvisiblePaused"))
						: (bAllEndpointsOccluded
							? TEXT("BothEndpointsOccludedHold")
							: TEXT("BothEndpointsInvisibleHold"));
				}
				}
			}

			if (!bWasVisibilityPaused && PairState.Capture.Visibility.bPaused)
			{
				PairState.Capture.CadenceElapsedSeconds = 0.0;
				PairState.Capture.Visibility.HiddenRefreshElapsedSeconds = 0.0;
				PairState.Capture.Visibility.bResumePrewarmPending = false;
#if !UE_BUILD_SHIPPING
				WP_LOG(&Runtime, Verbose,
					TEXT("[CaptureScheduler][Visibility] State changed. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s Previous=Active Current=Paused Reason=%s FreshnessReason=%s CameraGuardReason=%s FrustumState=V0 VisibleEndpoints=%u FeedbackCoherent=%d FeedbackEpoch=%llu CurrentRendererEpoch=%llu FeedbackPacketSequence=%llu RequiredPacketSequence=%llu LastPublishedPacketSequence=%llu FeedbackSampleSequence=%llu RejectedEpoch=%llu RejectedThroughSampleSequence=%llu AwaitingPostGuardSample=%d FeedbackAgeMs=%.3f InvisibleElapsedMs=%.3f HoldMs=%.3f GuardViewActorId=%u CurrentViewActorId=%u GuardLocationDeltaCm=%.4f GuardRotationDeltaDeg=%.4f GuardFOVDeltaDeg=%.4f HiddenRefreshHzRequested=%.3f HiddenRefreshHzEffective=%.3f StrictFrustumHiddenStop=1 VisibilitySource=PrimaryViewCullingFrustumSphere TransitPending=%d TransitActive=%d CommitFreshCameraWait=%d CadenceDebtAfterMs=%.3f ContinuousFreshSampleGapRequired=1 InitialInvisibleSamplePrimesGuardOnly=1 RejectBarrierRequired=1 CompletedRenderFrameVisibilityUnion=1 ResourcesRetained=1 CaptureComponentVisibilityChanged=0 EngineModified=0 CpuMs=%.4f"),
					*GetNameSafe(GetWorld()),
					static_cast<unsigned long long>(GFrameCounter),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
					*GetNameSafe(PortalB), VisibilityDecisionReason,
					VisibilityFreshnessReasonForLog,
					VisibilityCameraGuardReasonForLog,
					PairState.Capture.Visibility.GetLastVisibleEndpointCount(),
					VisibilityFeedback.bSnapshotCoherent ? 1 : 0,
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilityOwnershipEpoch),
					static_cast<unsigned long long>(PairState.Ownership.OwnershipEpoch),
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilityPacketSequence),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RequiredPacketSequence),
					static_cast<unsigned long long>(PairState.Publication.PacketSequence),
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilitySampleSequence),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RejectedOwnershipEpoch),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RejectedThroughSampleSequence),
					PairState.Capture.Visibility.bAwaitingPostGuardSample ? 1 : 0,
					VisibilityFeedbackAgeMsForLog,
					PairState.Capture.Visibility.InvisibleElapsedSeconds * 1000.0,
					VisibilityInvisibleHoldSeconds * 1000.0,
					PairState.Capture.Visibility.GuardViewActorId,
					VisibilityViewActorId,
					VisibilityGuardLocationDeltaCm,
					VisibilityGuardRotationDeltaDegrees,
					VisibilityGuardFOVDeltaDegrees,
					RequestedVisibilityHiddenRefreshHz,
					VisibilityHiddenRefreshHz,
					0,
					PairState.Transit.bTransitActive ? 1 : 0,
					bCommitWaitingForFreshCamera ? 1 : 0,
					PairState.Capture.CadenceElapsedSeconds * 1000.0,
					(FPlatformTime::Seconds() - VisibilityDecisionStartSeconds)
						* 1000.0);
				#endif
			}
			else if (bWasVisibilityPaused
				&& !PairState.Capture.Visibility.bPaused
				&& !PairState.Capture.Visibility.bInsideSelected)
			{
				PairState.Capture.CadenceElapsedSeconds = 0.0;
				PairState.Capture.Visibility.HiddenRefreshElapsedSeconds = 0.0;
				PairState.Capture.Visibility.bResumePrewarmPending = SnapshotA.InitialValidFaceMask != 0x3f
					|| SnapshotB.InitialValidFaceMask != 0x3f;
#if !UE_BUILD_SHIPPING
				WP_LOG(&Runtime, Verbose,
					TEXT("[CaptureScheduler][Visibility] State changed. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s Previous=Paused Current=Active ResumeAtomicPrewarmPending=%d Reason=%s FreshnessReason=%s CameraGuardReason=%s VisibleEndpoints=%u FeedbackCoherent=%d FeedbackEpoch=%llu CurrentRendererEpoch=%llu FeedbackPacketSequence=%llu RequiredPacketSequence=%llu LastPublishedPacketSequence=%llu FeedbackSampleSequence=%llu RejectedEpoch=%llu RejectedThroughSampleSequence=%llu AwaitingPostGuardSample=%d FeedbackAgeMs=%.3f GuardLocationDeltaCm=%.4f GuardRotationDeltaDeg=%.4f GuardFOVDeltaDeg=%.4f TransitPending=%d TransitActive=%d CommitFreshCameraWait=%d CadenceDebtAfterMs=%.3f SameResolutionResumeRetainsCube=1 RejectBarrierRequired=1 CompletedRenderFrameVisibilityUnion=1 ResourcesRetained=1 CaptureComponentVisibilityChanged=0 EngineModified=0 CpuMs=%.4f"),
					*GetNameSafe(GetWorld()),
					static_cast<unsigned long long>(GFrameCounter),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
					*GetNameSafe(PortalB), PairState.Capture.Visibility.bResumePrewarmPending ? 1 : 0, VisibilityDecisionReason,
					VisibilityFreshnessReasonForLog,
					VisibilityCameraGuardReasonForLog,
					PairState.Capture.Visibility.GetLastVisibleEndpointCount(),
					VisibilityFeedback.bSnapshotCoherent ? 1 : 0,
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilityOwnershipEpoch),
					static_cast<unsigned long long>(PairState.Ownership.OwnershipEpoch),
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilityPacketSequence),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RequiredPacketSequence),
					static_cast<unsigned long long>(PairState.Publication.PacketSequence),
					static_cast<unsigned long long>(
						VisibilityFeedback.VisibilitySampleSequence),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RejectedOwnershipEpoch),
					static_cast<unsigned long long>(
						PairState.Capture.Visibility.RejectedThroughSampleSequence),
					PairState.Capture.Visibility.bAwaitingPostGuardSample ? 1 : 0,
					VisibilityFeedbackAgeMsForLog,
					VisibilityGuardLocationDeltaCm,
					VisibilityGuardRotationDeltaDegrees,
					VisibilityGuardFOVDeltaDegrees,
					0,
					PairState.Transit.bTransitActive ? 1 : 0,
					bCommitWaitingForFreshCamera ? 1 : 0,
					PairState.Capture.CadenceElapsedSeconds * 1000.0,
					(FPlatformTime::Seconds() - VisibilityDecisionStartSeconds)
						* 1000.0);
				#endif
			}
			else if (bWasVisibilityPaused
				&& !PairState.Capture.Visibility.bPaused
				&& PairState.Capture.Visibility.bInsideSelected)
			{
				PairState.Capture.Visibility.HiddenRefreshElapsedSeconds = 0.0;
				PairState.Capture.Visibility.bResumePrewarmPending = false;
			}

			if (PairState.Capture.Visibility.bPaused)
			{
				if (bWasVisibilityPaused)
				{
					PairState.Capture.Visibility.HiddenRefreshElapsedSeconds +=
						SafeDeltaSeconds;
				}
				PairState.Capture.CadenceElapsedSeconds = 0.0;
			}
			else
			{
				PairState.Capture.Visibility.HiddenRefreshElapsedSeconds = 0.0;
			}

		}

		// Steady-state 해상도 변경은 반드시 이 프레임의 visibility 결과를 반영한
		// 뒤 시작합니다. V0 hold가 완료되면 bPaused가 먼저 확정되므로 이후의
		// 64-tier 전환이 frustum 판정을 선점하거나 초기화할 수 없습니다.
		if (!bResolutionBootstrapRequired && !PassResolutionGate())
		{
			continue;
		}

		if (!PairState.Capture.Visibility.bPaused
			&& !PairState.Capture.Visibility.bInsideBlocked
			&& PairState.Capture.Resolution.bWarmupDeferredUntilVisible)
		{
			// Visibility가 실제로 돌아온 프레임에만 새 64-tier Cubemap의 첫 내용을
			// 채웁니다. 해상도 upgrade가 필요하면 위 ResolutionGate가 먼저 전환을
			// 완료시키므로, 이 블록은 리소스가 준비된 뒤에만 RuntimeWarmup을 엽니다.
			PairState.Capture.Resolution.bWarmupDeferredUntilVisible = false;
			PairState.Capture.Visibility.bResumePrewarmPending = true;
			PairState.Capture.CadenceElapsedSeconds = 0.0;
			PairState.Publication.bDirty = true;
			SetPairCaptureAuthority(
				PairState,
				EWPCaptureAuthority::RuntimeWarmup,
				TEXT("DeferredResolutionWarmupBecameVisible"));
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureResolution][WarmupResume] Deferred hidden warmup released by fresh visibility. World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s Resolution=%u VisibleEndpoints=%u OcclusionValid=%d OcclusionVisibleMask=0x%02x WarmupDeferredUntilVisible=0 ResumeAtomicPrewarmPending=1 CaptureA=DeferredToSubmissionGate CaptureB=DeferredToSubmissionGate CpuMs=%.4f"),
				*GetNameSafe(GetWorld()),
				static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				*GetNameSafe(PortalA), *GetNameSafe(PortalB),
				PairState.Capture.Resolution.CurrentResolution,
				PairState.Capture.Visibility.GetLastVisibleEndpointCount(),
				PairState.Capture.Visibility.bOcclusionValid ? 1 : 0,
				static_cast<uint32>(
					PairState.Capture.Visibility.OcclusionVisibleEndpointMask),
				(FPlatformTime::Seconds() - DecisionStartSeconds) * 1000.0);
#endif
		}

		if (ShouldBlockWPFrustumCapture(
			PairState.Capture.Visibility.bPaused))
		{
			// A fresh V0 sample, meaning zero visible Endpoints, or valid occlusion of both sides
			// is a strict
			// production-stop condition. This hard gate intentionally precedes every cadence and
			// forced-submission
			// path so a process-global legacy heartbeat value cannot refresh A or B.
			continue;
		}

		// Reuse the exact per-endpoint Renderer frustum bits and the existing SafeProxy
		// occlusion result. Detailed Metric rays run only for endpoints that survive both
		// gates. A/B detailed work is deliberately staggered across callbacks.
		FWPFacePredictionState& FacePrediction = PairState.Capture.FacePrediction;
		const uint8 PreviousRequiredFaceMaskA = FacePrediction.RequiredFaceMaskA;
		const uint8 PreviousRequiredFaceMaskB = FacePrediction.RequiredFaceMaskB;
		uint8 EffectiveVisibleEndpointMask =
			PairState.Capture.Visibility.LastVisibleEndpointMask & WPCaptureBothEndpointsMask;
		if (PairState.Capture.Visibility.bOcclusionValid)
		{
			EffectiveVisibleEndpointMask &=
				PairState.Capture.Visibility.OcclusionVisibleEndpointMask;
		}

		const bool bInitialFaceFillComplete =
			SnapshotA.InitialValidFaceMask == 0x3f
			&& SnapshotB.InitialValidFaceMask == 0x3f;
		const double FacePredictionNowSeconds = FPlatformTime::Seconds();
		const double FacePredictionCadenceSeconds = 1.0 / TargetEndpointHz;
		bool bPredictionEvaluated = false;
		bool bEvaluatedEndpointA = false;
		FWPMetricFacePredictionResult PredictionResult;
		const TCHAR* FacePredictionDecision = TEXT("RetainPrevious");
		if (PairState.Capture.Visibility.bInsideSelected)
		{
			FacePrediction.RequiredFaceMaskA = 0x3f;
			FacePrediction.RequiredFaceMaskB = 0x3f;
			FacePredictionDecision = TEXT("InsideSafeProxyAllFaces");
		}
		else if (!bVisibilityReferenceViewAvailable)
		{
			FacePrediction.RequiredFaceMaskA = 0x3f;
			FacePrediction.RequiredFaceMaskB = 0x3f;
			FacePredictionDecision = TEXT("ReferenceViewUnavailableFailOpen");
		}
		else if (!bInitialFaceFillComplete)
		{
			FacePrediction.RequiredFaceMaskA = 0x3f;
			FacePrediction.RequiredFaceMaskB = 0x3f;
			FacePredictionDecision = TEXT("InitialFullCubemapFill");
		}
		else if (EffectiveVisibleEndpointMask != 0)
		{
			const auto AgeHiddenView = [FacePredictionNowSeconds,
				FacePredictionCadenceSeconds](FWPFacePredictionViewState& View)
			{
				if (FacePredictionNowSeconds >= View.NextDueSeconds)
				{
					View.PreviousLocalMask = View.CurrentLocalMask;
					View.PreviousLinkedMask = View.CurrentLinkedMask;
					View.CurrentLocalMask = 0;
					View.CurrentLinkedMask = 0;
					View.NextDueSeconds = FacePredictionNowSeconds
						+ FacePredictionCadenceSeconds;
				}
			};
			if ((EffectiveVisibleEndpointMask & WPCaptureEndpointAMask) == 0)
			{
				AgeHiddenView(FacePrediction.ViewA);
			}
			if ((EffectiveVisibleEndpointMask & WPCaptureEndpointBMask) == 0)
			{
				AgeHiddenView(FacePrediction.ViewB);
			}

			const bool bEndpointADue =
				(EffectiveVisibleEndpointMask & WPCaptureEndpointAMask) != 0
				&& FacePredictionNowSeconds >= FacePrediction.ViewA.NextDueSeconds;
			const bool bEndpointBDue =
				(EffectiveVisibleEndpointMask & WPCaptureEndpointBMask) != 0
				&& FacePredictionNowSeconds >= FacePrediction.ViewB.NextDueSeconds;
			if (bEndpointADue || bEndpointBDue)
			{
				bEvaluatedEndpointA = bEndpointADue && bEndpointBDue
					? FacePrediction.bNextEndpointA : bEndpointADue;
				AWormholePortalActor* PredictionPortal = bEvaluatedEndpointA ? PortalA : PortalB;
				AWormholePortalActor* PredictionLinkedPortal = bEvaluatedEndpointA ? PortalB : PortalA;
				FWPLUTEndpointSnapshot LUTSnapshot;
				const bool bHasLUTSnapshot = Runtime.GetLUTEndpointSnapshot(
					PredictionPortal, LUTSnapshot);
				const double PredictionStartSeconds = FPlatformTime::Seconds();
				PredictionResult = bHasLUTSnapshot
					? PredictWPMetricCaptureFaces(
						*GetWorld(), *PredictionPortal, *PredictionLinkedPortal,
						LUTSnapshot, VisibilityCameraLocation, VisibilityCameraRotation,
						VisibilityCameraFOVDegrees, VisibilityViewAspectRatio,
						VisibilityViewActor)
					: FWPMetricFacePredictionResult();
				if (!bHasLUTSnapshot)
				{
					PredictionResult.LocalFaceMask = 0x3f;
					PredictionResult.LinkedFaceMask = 0x3f;
					PredictionResult.bFailOpen = true;
					PredictionResult.Reason = TEXT("LUTSnapshotUnavailableFailOpen");
				}
				FWPFacePredictionViewState& EvaluatedView = bEvaluatedEndpointA
					? FacePrediction.ViewA : FacePrediction.ViewB;
				EvaluatedView.PreviousLocalMask = EvaluatedView.CurrentLocalMask;
				EvaluatedView.PreviousLinkedMask = EvaluatedView.CurrentLinkedMask;
				EvaluatedView.CurrentLocalMask = PredictionResult.LocalFaceMask;
				EvaluatedView.CurrentLinkedMask = PredictionResult.LinkedFaceMask;
				EvaluatedView.NextDueSeconds = FacePredictionNowSeconds
					+ FacePredictionCadenceSeconds;
				if ((EffectiveVisibleEndpointMask & WPCaptureBothEndpointsMask)
					== WPCaptureBothEndpointsMask)
				{
					FacePrediction.bNextEndpointA = !bEvaluatedEndpointA;
				}
				bPredictionEvaluated = true;
				FacePredictionDecision = PredictionResult.Reason;
#if !UE_BUILD_SHIPPING
				WP_LOG(&Runtime, VeryVerbose,
					TEXT("[CaptureScheduler][MetricFacePrediction] Evaluated. World=%s Frame=%llu PairId=%s Endpoint=%s ObservedFPS=%.3f TargetPredictionHz=%.3f CadenceMs=%.3f FrustumVisibleMask=0x%02x OcclusionValid=%d OcclusionVisibleMask=0x%02x EffectiveVisibleMask=0x%02x Rays=%d ScreenRejected=%d Blocked=%d Open=%d LUTEvaluations=%d LocalMask=0x%02x LinkedMask=0x%02x FailOpen=%d Reason=%s EndpointGate=FrustumAndOcclusion ABStaggered=1 DuplicateRaysRemoved=0 TraceAPI=LineTraceTestByChannel CpuMs=%.4f"),
					*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter),
					*PairIdToString(PairState.Identity.PairId),
					bEvaluatedEndpointA ? TEXT("A") : TEXT("B"),
					ObservedFPS, TargetEndpointHz, FacePredictionCadenceSeconds * 1000.0,
					static_cast<uint32>(PairState.Capture.Visibility.LastVisibleEndpointMask),
					PairState.Capture.Visibility.bOcclusionValid ? 1 : 0,
					static_cast<uint32>(PairState.Capture.Visibility.OcclusionVisibleEndpointMask),
					static_cast<uint32>(EffectiveVisibleEndpointMask),
					PredictionResult.RayCount, PredictionResult.ScreenRejectedRayCount,
					PredictionResult.BlockedRayCount, PredictionResult.OpenRayCount,
					PredictionResult.LUTEvaluationCount,
					static_cast<uint32>(PredictionResult.LocalFaceMask),
					static_cast<uint32>(PredictionResult.LinkedFaceMask),
					PredictionResult.bFailOpen ? 1 : 0, PredictionResult.Reason,
					(FPlatformTime::Seconds() - PredictionStartSeconds) * 1000.0);
#endif
			}

			FacePrediction.RequiredFaceMaskA =
				FacePrediction.ViewA.CurrentLocalMask
				| FacePrediction.ViewA.PreviousLocalMask
				| FacePrediction.ViewB.CurrentLinkedMask
				| FacePrediction.ViewB.PreviousLinkedMask;
			FacePrediction.RequiredFaceMaskB =
				FacePrediction.ViewA.CurrentLinkedMask
				| FacePrediction.ViewA.PreviousLinkedMask
				| FacePrediction.ViewB.CurrentLocalMask
				| FacePrediction.ViewB.PreviousLocalMask;
		}
		else
		{
			// During the 0.5-second invisible hold, preserve the previous face set. The
			// existing strict pause gate stops all capture only after the hold completes.
			FacePredictionDecision = TEXT("NoVisibleEndpointRetainUntilPause");
		}
		if (PreviousRequiredFaceMaskA != FacePrediction.RequiredFaceMaskA
			|| PreviousRequiredFaceMaskB != FacePrediction.RequiredFaceMaskB)
		{
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureScheduler][MetricFacePrediction] Required masks changed. World=%s Frame=%llu PairId=%s PreviousA=0x%02x CurrentA=0x%02x PreviousB=0x%02x CurrentB=0x%02x FrustumVisibleMask=0x%02x OcclusionValid=%d OcclusionVisibleMask=0x%02x EffectiveVisibleMask=0x%02x PredictionEvaluated=%d EvaluatedEndpoint=%s Reason=%s EndpointGate=FrustumAndOcclusion CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId),
				static_cast<uint32>(PreviousRequiredFaceMaskA),
				static_cast<uint32>(FacePrediction.RequiredFaceMaskA),
				static_cast<uint32>(PreviousRequiredFaceMaskB),
				static_cast<uint32>(FacePrediction.RequiredFaceMaskB),
				static_cast<uint32>(PairState.Capture.Visibility.LastVisibleEndpointMask),
				PairState.Capture.Visibility.bOcclusionValid ? 1 : 0,
				static_cast<uint32>(PairState.Capture.Visibility.OcclusionVisibleEndpointMask),
				static_cast<uint32>(EffectiveVisibleEndpointMask),
				bPredictionEvaluated ? 1 : 0,
				bPredictionEvaluated ? (bEvaluatedEndpointA ? TEXT("A") : TEXT("B")) : TEXT("None"),
				FacePredictionDecision,
				(FPlatformTime::Seconds() - DecisionStartSeconds) * 1000.0);
		}

		const bool bVisibilityResumePrewarmForced =
			PairState.Capture.Visibility.bResumePrewarmPending
			&& !bCommitWaitingForFreshCamera;
		const bool bVisibilityHiddenHeartbeatForced =
			PairState.Capture.Visibility.bPaused
			&& VisibilityHiddenRefreshHz > 0.0
			&& PairState.Capture.Visibility.HiddenRefreshElapsedSeconds
				>= VisibilityHiddenRefreshCadenceSeconds;
		const bool bForcedAtomicSubmission =
			bVisibilityResumePrewarmForced
			|| bVisibilityHiddenHeartbeatForced;
		// Steady-state debt never merges A+B into one burst. Only cold/warmup resource initialization may submit AtomicPair.
		const bool bCadenceDebtAtomicFallback = false;
		const bool bUseStaggeredEndpointSubmission =
			ModePolicy.bStaggeredEndpointSubmission
			&& !bWarmupRequiresAtomicPair
			&& !bCadenceDebtAtomicFallback
			&& !bForcedAtomicSubmission;
		const EWPManagedCaptureSubmissionMode SubmissionMode =
			bUseStaggeredEndpointSubmission
			? (PairState.Capture.bNextStaggeredEndpointA
				? EWPManagedCaptureSubmissionMode::EndpointA
				: EWPManagedCaptureSubmissionMode::EndpointB)
			: EWPManagedCaptureSubmissionMode::AtomicPair;
		const uint8 SelectedFaceMask = SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA
			? FacePrediction.RequiredFaceMaskA
			: (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB ? FacePrediction.RequiredFaceMaskB : 0x3f);
		const double ActiveCadenceSeconds =
			ModePolicy.bStaggeredEndpointSubmission
			? (bUseStaggeredEndpointSubmission
				? StaggeredSubmissionCadenceSeconds
				: EndpointCadenceSeconds)
			: WPLegacyAtomicCaptureCadenceSeconds;
		const FWPFixedCaptureCadenceResult Decision =
			EvaluateWPFixedCaptureCadence(
				PairState.Capture.CadenceElapsedSeconds,
				ActiveCadenceSeconds,
				bCommitWaitingForFreshCamera);
		const bool bWouldSubmit = bForcedAtomicSubmission || Decision.bWouldSubmit;
		if (!bWouldSubmit)
		{
			continue;
		}
		if (SelectedFaceMask == 0)
		{
			PairState.Capture.CadenceElapsedSeconds = FMath::Fmod(PairState.Capture.CadenceElapsedSeconds, ActiveCadenceSeconds);
			PairState.Capture.bNextStaggeredEndpointA = SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB;
			WP_LOG(&Runtime, VeryVerbose,
				TEXT("[CaptureScheduler][HybridCapture] Endpoint skipped because no Cubemap face is required. World=%s Frame=%llu PairId=%s Endpoint=%s RequiredMaskA=0x%02x RequiredMaskB=0x%02x ObservedFPS=%.3f TargetEndpointHz=%.3f CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter), *PairIdToString(PairState.Identity.PairId),
				SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA ? TEXT("A") : TEXT("B"),
				static_cast<uint32>(FacePrediction.RequiredFaceMaskA), static_cast<uint32>(FacePrediction.RequiredFaceMaskB),
				ObservedFPS, TargetEndpointHz, (FPlatformTime::Seconds() - DecisionStartSeconds) * 1000.0);
			continue;
		}

		if (bVisibilityHiddenHeartbeatForced)
		{
			// Consume cadence on attempt, not only success. A transient capture
			// failure must not turn the configured 2 Hz heartbeat into an
			// every-frame retry loop.
			PairState.Capture.Visibility.HiddenRefreshElapsedSeconds =
				FMath::Fmod(
					PairState.Capture.Visibility.HiddenRefreshElapsedSeconds,
					VisibilityHiddenRefreshCadenceSeconds);
		}
#if !UE_BUILD_SHIPPING
		const double CaptureCadenceBeforeSubmissionSeconds =
			PairState.Capture.CadenceElapsedSeconds;
#endif
		double CaptureCpuMs = 0.0;
		const bool bSubmissionSucceeded = ExecuteRuntimePairCapture(
			PairState,
			0.0f,
			SubmissionMode,
			CaptureCpuMs,
			SelectedFaceMask);

#if !UE_BUILD_SHIPPING
		bool bRolledBack = false;
#endif
		if (bSubmissionSucceeded)
		{
			PairState.Capture.ConsecutiveFailureCount = 0;
			PairState.Capture.LastSuccessfulSubmissionFrame = GFrameCounter;
			PairState.Capture.CadenceElapsedSeconds = FMath::Fmod(
				PairState.Capture.CadenceElapsedSeconds,
				ActiveCadenceSeconds);
			if (PairState.Capture.Authority == EWPCaptureAuthority::RuntimeWarmup)
			{
				ensureMsgf(
					SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair,
					TEXT("Runtime warmup must complete with one atomic A+B submission."));
				SetPairCaptureAuthority(
					PairState, EWPCaptureAuthority::RuntimeOnly,
					TEXT("FirstExactAtomicRuntimePairSubmission"));
			}
			if (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA
				|| SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB)
			{
				PairState.Capture.bNextStaggeredEndpointA =
					SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB;
			}
			else if (!bCadenceDebtAtomicFallback)
			{
				PairState.Capture.bNextStaggeredEndpointA = true;
			}

			if (PairState.Capture.Visibility.bResumePrewarmPending
				&& SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair)
			{
				PairState.Capture.Visibility.bResumePrewarmPending = false;
				PairState.Capture.CadenceElapsedSeconds = 0.0;
				if (bVisibilityResumePrewarmForced)
				{
				}
			}
			if (bVisibilityHiddenHeartbeatForced)
			{
				PairState.Capture.CadenceElapsedSeconds = 0.0;
			}
		}
		else
		{
			++PairState.Capture.ConsecutiveFailureCount;
			PairState.Capture.LastFailedSubmissionFrame = GFrameCounter;
			if (PairState.Capture.ConsecutiveFailureCount
				>= static_cast<uint32>(FailureRollbackThreshold))
			{
				RecoverPairCaptureAuthority(
					PairState, TEXT("RuntimeCaptureSubmissionFailureThreshold"));
#if !UE_BUILD_SHIPPING
				bRolledBack = true;
#endif
			}
		}

#if !UE_BUILD_SHIPPING
		if (bForcedAtomicSubmission)
		{
			const TCHAR* ForcedSubmissionKind = bVisibilityResumePrewarmForced
				? TEXT("VisibilityResumePrewarm")
				: TEXT("VisibilityHiddenHeartbeat");
			WP_LOG(&Runtime, Verbose,
				TEXT("[CaptureScheduler][VisibilityForcedAtomic] World=%s Frame=%llu PairId=%s PortalA=%s PortalB=%s Kind=%s SubmissionMode=%s Success=%d RolledBack=%d VisibilityPolicyAlwaysEnabled=%d ReferenceViewAvailable=%d FreshnessReason=%s CameraGuardReason=%s VisibleEndpoints=%u FeedbackCoherent=%d FeedbackEpoch=%llu CurrentRendererEpoch=%llu FeedbackPacketSequence=%llu RequiredPacketSequence=%llu LastPublishedPacketSequence=%llu FeedbackSampleSequence=%llu FeedbackAgeMs=%.3f HiddenRefreshHz=%.3f CadenceDebtBeforeMs=%.3f CadenceDebtAfterMs=%.3f GenerationA=%u->%u GenerationB=%u->%u TransitForcesCapture=%d TransitForcedSequence=%llu CaptureCpuMs=%.4f TotalDecisionCpuMs=%.4f ResourcesRetained=1 ExistingAtomicPairPath=1 ExistingCubeAAPath=1 EngineModified=0"),
				*GetNameSafe(GetWorld()), static_cast<unsigned long long>(GFrameCounter),
				*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA),
				*GetNameSafe(PortalB), ForcedSubmissionKind,
				GetWPManagedSubmissionModeName(SubmissionMode),
				bSubmissionSucceeded ? 1 : 0,
				bRolledBack ? 1 : 0,
				bVisibilityPauseEnabled ? 1 : 0,
				bVisibilityReferenceViewAvailable ? 1 : 0,
				VisibilityFreshnessReasonForLog,
				VisibilityCameraGuardReasonForLog,
				PairState.Capture.Visibility.GetLastVisibleEndpointCount(),
				VisibilityFeedback.bSnapshotCoherent ? 1 : 0,
				static_cast<unsigned long long>(
					VisibilityFeedback.VisibilityOwnershipEpoch),
				static_cast<unsigned long long>(PairState.Ownership.OwnershipEpoch),
				static_cast<unsigned long long>(
					VisibilityFeedback.VisibilityPacketSequence),
				static_cast<unsigned long long>(
					PairState.Capture.Visibility.RequiredPacketSequence),
				static_cast<unsigned long long>(PairState.Publication.PacketSequence),
				static_cast<unsigned long long>(
					VisibilityFeedback.VisibilitySampleSequence),
				VisibilityFeedbackAgeMsForLog,
				VisibilityHiddenRefreshHz,
				CaptureCadenceBeforeSubmissionSeconds * 1000.0,
				PairState.Capture.CadenceElapsedSeconds * 1000.0,
				SnapshotA.CaptureGeneration,
				CaptureManager
					? CaptureManager->GetEndpointCaptureGeneration(PortalA)
					: SnapshotA.CaptureGeneration,
				SnapshotB.CaptureGeneration,
				CaptureManager
					? CaptureManager->GetEndpointCaptureGeneration(PortalB)
					: SnapshotB.CaptureGeneration,
				0,
				0ull,
				CaptureCpuMs,
				(FPlatformTime::Seconds() - DecisionStartSeconds) * 1000.0);
		}
#endif

	}
}
