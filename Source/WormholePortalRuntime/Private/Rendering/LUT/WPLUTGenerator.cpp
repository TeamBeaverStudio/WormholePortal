// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/LUT/WPLUTGenerator.h"

#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "WPLog.h"

#include <limits>

namespace
{
	constexpr double Pi = UE_DOUBLE_PI;
	constexpr double MinimumPositiveRadicand = 1.0e-12;
	constexpr double CriticalEqualityUlps = 32.0;
	constexpr int32 TurningSearchIterations = 40; 
	constexpr int32 TailIntegrationCells = 64;
	constexpr double GaussOffset = 0.77459666924148337704;
	constexpr int64 MaxLUTTexels = 8ll * 1024ll * 1024ll;
	constexpr int64 MaxQuadratureSamples = 32ll * 1000ll * 1000ll * 1000ll;
	constexpr int64 MaxQuadratureLegsPerTexel = 6;

	bool TryMultiplyPositive(const int64 A, const int64 B, int64& OutProduct)
	{
		if (A <= 0 || B <= 0 || A > TNumericLimits<int64>::Max() / B)
		{
			OutProduct = -1;
			return false;
		}
		OutProduct = A * B;
		return true;
	}

	double SmoothStepQuintic(const double X)
	{
		const double T = FMath::Clamp(X, 0.0, 1.0);
		return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
	}

	double ImpactFractionFromCoordinate(const double U)
	{
		const double SafeU = FMath::Clamp(U, 0.0, 1.0);
		return 0.5 - 0.5 * FMath::Cos(Pi * SafeU);
	}

	double InwardImpactFractionFromCoordinate(const double U, const double CriticalImpact)
	{
		const double SafeU = FMath::Clamp(U, 0.0, 1.0);
		const double SafeCritical = FMath::Clamp(CriticalImpact, 0.0, 1.0);
		if (SafeU < 0.5)
		{
			return SafeCritical * ImpactFractionFromCoordinate(2.0 * SafeU);
		}
		return SafeCritical
			+ (1.0 - SafeCritical) * ImpactFractionFromCoordinate(2.0 * SafeU - 1.0);
	}

	double IntegrateRegular(
		const FNormalizedMetricProfile& Profile,
		const double Impact,
		const double StartEll,
		const double EndEll,
		const int32 Steps)
	{
		if (EndEll <= StartEll + UE_DOUBLE_SMALL_NUMBER || Impact <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}

		const double Span = EndEll - StartEll;
		const bool bStartsAtMouth = StartEll <= UE_DOUBLE_SMALL_NUMBER;
		const double LinearStep = Span / static_cast<double>(Steps);
		const double DS = 1.0 / static_cast<double>(Steps);
		double Angle = 0.0;
		for (int32 StepIndex = 0; StepIndex < Steps; ++StepIndex)
		{
			const double S = (static_cast<double>(StepIndex) + 0.5) * DS;
			const double Ell = bStartsAtMouth
				? StartEll + Span * S * S
				: StartEll + (static_cast<double>(StepIndex) + 0.5) * LinearStep;
			const double Weight = bStartsAtMouth ? DS * 2.0 * Span * S : LinearStep;
			const double Radius = FMath::Max(1.0, Profile.Radius(Ell));
			const double Q = FMath::Clamp(Impact / Radius, 0.0, 1.0 - 1.0e-12);
			const double Denominator = Radius * Radius
				* FMath::Sqrt(FMath::Max(MinimumPositiveRadicand, 1.0 - Q * Q));
			Angle += Impact / Denominator * Weight;
		}
		return Angle;
	}

	double FindTurningEll(
		const FNormalizedMetricProfile& Profile,
		const double Impact,
		const double StartEll)
	{
		if (Impact <= 1.0 || StartEll <= 0.0)
		{
			return 0.0;
		}
		double Low = 0.0;
		double High = StartEll;
		for (int32 Iteration = 0; Iteration < TurningSearchIterations; ++Iteration)
		{
			const double Mid = 0.5 * (Low + High);
			if (Profile.Radius(Mid) < Impact)
			{
				Low = Mid;
			}
			else
			{
				High = Mid;
			}
		}
		return High;
	}

	double IntegrateFromTurningPoint(
		const FNormalizedMetricProfile& Profile,
		const double Impact,
		const double TurningEll,
		const double EndEll,
		const int32 Steps)
	{
		const double Length = FMath::Max(0.0, EndEll - TurningEll);
		if (Length <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}
		const double Step = 1.0 / static_cast<double>(Steps);
		double Angle = 0.0;
		for (int32 StepIndex = 0; StepIndex < Steps; ++StepIndex)
		{
			const double U = (static_cast<double>(StepIndex) + 0.5) * Step;
			const double Ell = TurningEll + Length * U * U;
			const double DEllDU = 2.0 * Length * U;
			const double Radius = FMath::Max(1.0, Profile.Radius(Ell));
			const double Q = FMath::Clamp(Impact / Radius, 0.0, 1.0 - 1.0e-12);
			const double Denominator = Radius * Radius
				* FMath::Sqrt(FMath::Max(MinimumPositiveRadicand, 1.0 - Q * Q));
			Angle += Impact / Denominator * DEllDU * Step;
		}
		return Angle;
	}

	double IntegrateTurningToOuter(
		const FNormalizedMetricProfile& Profile,
		const double Impact,
		const double StartEll,
		const int32 Steps)
	{
		const double StartRadius = Profile.Radius(StartEll);
		if (Impact <= 1.0 || Impact > StartRadius || StartEll <= 0.0)
		{
			return 0.0;
		}
		// impact == r(start) is a finite tangent ray whenever r(start) > rho.
		// Its turning point is exactly the ray start, so the inbound leg has zero
		// length and the outbound leg is integrated with the same square-root
		// singularity-removing substitution used by Test4.
		const double TurningEll = FindTurningEll(Profile, Impact, StartEll);
		return IntegrateFromTurningPoint(Profile, Impact, TurningEll, StartEll, Steps)
			+ IntegrateFromTurningPoint(Profile, Impact, TurningEll, Profile.GetTransitionRatio(), Steps);
	}

	FLinearColor BuildTexel(
		const FNormalizedMetricProfile& Profile,
		const int32 Steps,
		const double ImpactCoordinate,
		const double TransitionCoordinate)
	{
		const double StartEll = TransitionCoordinate * Profile.GetTransitionRatio();
		const double StartRadius = Profile.Radius(StartEll);
		const double OuterRadius = FMath::Max(1.0, Profile.GetOuterRadius());
		const double CriticalImpact = FMath::Clamp(1.0 / FMath::Max(1.0, StartRadius), 0.0, 1.0);
		const double InwardFraction = InwardImpactFractionFromCoordinate(ImpactCoordinate, CriticalImpact);
		const double OutwardFraction = ImpactFractionFromCoordinate(ImpactCoordinate);
		const double InwardImpact = InwardFraction * StartRadius;
		const double OutwardImpact = OutwardFraction * StartRadius;
		const double EqualityTolerance = CriticalEqualityUlps
			* std::numeric_limits<double>::epsilon()
			* FMath::Max(1.0, FMath::Abs(InwardImpact));

		double InwardResidual = 0.0;
		double InwardValidity = 0.0;
		if (FMath::Abs(InwardImpact - 1.0) > EqualityTolerance)
		{
			if (InwardImpact < 1.0)
			{
				// The a-dependent constant-radius throat is deliberately absent here.
				// It is reconstructed analytically in the shader.
				InwardResidual = IntegrateRegular(Profile, InwardImpact, 0.0, StartEll, Steps)
					+ IntegrateRegular(Profile, InwardImpact, 0.0, Profile.GetTransitionRatio(), Steps);
				InwardValidity = 1.0;
			}
			else
			{
				InwardResidual = IntegrateTurningToOuter(Profile, InwardImpact, StartEll, Steps);
				// A tangent ray at the finite outer boundary legitimately has a
				// zero-length numerical leg and receives its finite pi/2 exit angle
				// below. Validity therefore follows the branch, not integral magnitude.
				InwardValidity = 1.0;
			}
		}
		if (InwardValidity > 0.0)
		{
			InwardResidual += FMath::Asin(FMath::Clamp(InwardImpact / OuterRadius, 0.0, 1.0));
		}

		// At p=1 the outward ray also starts at a turning point. Use the
		// endpoint-singularity substitution instead of regular midpoint
		// quadrature, matching Test4's tangent-ray contract.
		double OutwardResidual = (OutwardFraction >= 1.0 - 1.0e-9)
			? IntegrateFromTurningPoint(
				Profile, OutwardImpact, StartEll, Profile.GetTransitionRatio(), Steps)
			: IntegrateRegular(
				Profile, OutwardImpact, StartEll, Profile.GetTransitionRatio(), Steps);
		const double OutwardValidity = (TransitionCoordinate > UE_DOUBLE_SMALL_NUMBER
			|| OutwardFraction < 1.0 - 1.0e-12) ? 2.0 : 0.0;
		if (OutwardValidity > 0.0)
		{
			OutwardResidual += FMath::Asin(FMath::Clamp(OutwardImpact / OuterRadius, 0.0, 1.0));
		}

		return FLinearColor(
			static_cast<float>(InwardValidity > 0.0 ? InwardResidual : 0.0),
			static_cast<float>(OutwardValidity > 0.0 ? OutwardResidual : 0.0),
			static_cast<float>(CriticalImpact),
			static_cast<float>(InwardValidity + OutwardValidity));
	}
}

FNormalizedMetricProfile::FNormalizedMetricProfile(
	const FWPLUTDescriptor& InDescriptor,
	const double InTransitionRatio)
	: Descriptor(InDescriptor.GetSanitized())
	, TransitionRatio(FMath::Max(0.01, InTransitionRatio))
	, M(TransitionRatio / WPLUT::LensingWidthToMassRatio)
	, FlattenStart(TransitionRatio * Descriptor.TailFlattenStartFraction)
	, OuterRadius(ComputeOuterRadius())
{
}

double FNormalizedMetricProfile::GetTransitionRatio() const
{
	return TransitionRatio;
}

double FNormalizedMetricProfile::GetOuterRadius() const
{
	return OuterRadius;
}

double FNormalizedMetricProfile::Radius(const double TransitionEll) const
{
	const double X = FMath::Clamp(TransitionEll, 0.0, TransitionRatio);

	if (X <= FlattenStart)
	{
		return StrictRadius(X);
	}
	if (X >= TransitionRatio)
	{
		return OuterRadius;
	}

	const double TailSpan = FMath::Max(UE_DOUBLE_SMALL_NUMBER, TransitionRatio - FlattenStart);
	const double T = FMath::Clamp((X - FlattenStart) / TailSpan, 0.0, 1.0);
	const double R0 = StrictRadius(FlattenStart);
	const double S0 = StrictSlope(FlattenStart);
	const double K0 = StrictCurvature(FlattenStart);
	const double C0 = R0;
	const double C1 = S0 * TailSpan;
	const double C2 = 0.5 * K0 * TailSpan * TailSpan;
	const double A = OuterRadius - (C0 + C1 + C2);
	const double B = TailSpan - (C1 + 2.0 * C2);
	const double C = -2.0 * C2;
	const double C3 = 10.0 * A - 4.0 * B + 0.5 * C;
	const double C4 = -15.0 * A + 7.0 * B - C;
	const double C5 = 6.0 * A - 3.0 * B + 0.5 * C;
	return C0 + T * (C1 + T * (C2 + T * (C3 + T * (C4 + T * C5))));
}

double FNormalizedMetricProfile::StrictRadius(const double TransitionEll) const
{
	const double U = 2.0 * FMath::Max(0.0, TransitionEll) / (Pi * M);
	return 1.0 + M * (U * FMath::Atan(U) - 0.5 * FMath::Loge(1.0 + U * U));
}

double FNormalizedMetricProfile::StrictSlope(const double TransitionEll) const
{
	const double U = 2.0 * FMath::Max(0.0, TransitionEll) / (Pi * M);
	return (2.0 / Pi) * FMath::Atan(U);
}

double FNormalizedMetricProfile::StrictCurvature(const double TransitionEll) const
{
	const double U = 2.0 * FMath::Max(0.0, TransitionEll) / (Pi * M);
	return 4.0 / (Pi * Pi * M * (1.0 + U * U));
}

double FNormalizedMetricProfile::ComputeOuterRadius() const
{
	const double TailSpan = FMath::Max(UE_DOUBLE_SMALL_NUMBER, TransitionRatio - FlattenStart);
	const double CellWidth = TailSpan / static_cast<double>(TailIntegrationCells);
	double TailIntegral = 0.0;
	for (int32 Cell = 0; Cell < TailIntegrationCells; ++Cell)
	{
		const double CellStart = FlattenStart + static_cast<double>(Cell) * CellWidth;
		const double Mid = CellStart + 0.5 * CellWidth;
		const double Half = 0.5 * CellWidth;
		auto BlendedSlope = [this, TailSpan](const double TransitionX)
		{
			const double Alpha = (TransitionX - FlattenStart) / TailSpan;
			return FMath::Lerp(StrictSlope(TransitionX), 1.0, SmoothStepQuintic(Alpha));
		};
		TailIntegral += Half * (
			(5.0 / 9.0) * BlendedSlope(Mid - GaussOffset * Half)
			+ (8.0 / 9.0) * BlendedSlope(Mid)
			+ (5.0 / 9.0) * BlendedSlope(Mid + GaussOffset * Half));
	}
	return StrictRadius(FlattenStart) + TailIntegral;
}

bool FWPLUTGenerator::BuildVolumeData(
	const FWPLUTDescriptor& InDescriptor,
	FWPLUTVolumeData& OutVolumeData,
	FWPLUTBuildStats& OutStats,
	FShouldCancel ShouldCancel)
{
	const double TotalStart = FPlatformTime::Seconds();
	OutVolumeData = FWPLUTVolumeData();
	OutStats = FWPLUTBuildStats();

	const double PreflightStart = FPlatformTime::Seconds();
	const FWPLUTDescriptor Descriptor = InDescriptor.GetSanitized();
	if (!Descriptor.IsValid(&OutStats.Error))
	{
		OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
		return false;
	}

	const FIntVector Dimensions = Descriptor.GetDimensions();
	int64 PlaneTexels = -1;
	int64 RayStepSamples = -1;
	const bool bProductsValid =
		TryMultiplyPositive(Dimensions.X, Dimensions.Y, PlaneTexels)
		&& TryMultiplyPositive(PlaneTexels, Dimensions.Z, OutStats.TexelCount)
		&& TryMultiplyPositive(OutStats.TexelCount, Descriptor.IntegrationSteps, RayStepSamples)
		&& TryMultiplyPositive(RayStepSamples, MaxQuadratureLegsPerTexel, OutStats.EstimatedQuadratureSamples);
	
	OutStats.TextureBytes = bProductsValid ? OutStats.TexelCount * sizeof(FLinearColor) : -1;
	OutStats.PreflightMilliseconds = (FPlatformTime::Seconds() - PreflightStart) * 1000.0;
	
	if (!bProductsValid || OutStats.TexelCount > MaxLUTTexels
		|| OutStats.EstimatedQuadratureSamples > MaxQuadratureSamples)
	{
		OutStats.Error = TEXT("LUT build exceeds the validated texel or quadrature budget.");
		OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
		WP_LOG(nullptr, Error,
			TEXT("Build rejected. Size=%dx%dx%d Steps=%d Texels=%lld EstimatedSamples=%lld"),
			Dimensions.X, Dimensions.Y, Dimensions.Z, Descriptor.IntegrationSteps,
			OutStats.TexelCount, OutStats.EstimatedQuadratureSamples);
		return false;
	}
	if (ShouldCancel && ShouldCancel())
	{
		OutStats.bCancelled = true;
		OutStats.Error = TEXT("LUT build was cancelled before profile generation.");
		OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
		return false;
	}

	const double ProfileStart = FPlatformTime::Seconds();
	TArray<FNormalizedMetricProfile> Profiles;
	Profiles.Reserve(Dimensions.Z);
	for (int32 Z = 0; Z < Dimensions.Z; ++Z)
	{
		Profiles.Emplace(Descriptor, Descriptor.RatioFromSlice(Z));
	}
	OutStats.ProfileMilliseconds = (FPlatformTime::Seconds() - ProfileStart) * 1000.0;

	OutVolumeData.Descriptor = Descriptor;
	OutVolumeData.Dimensions = Dimensions;
	OutVolumeData.BuildHash = Descriptor.MakeBuildHash();
	OutVolumeData.Voxels.SetNumUninitialized(OutStats.TexelCount);

	const double IntegrationStart = FPlatformTime::Seconds();
	TAtomic<bool> bCancelled(false);
	const int32 RowCount = Dimensions.Y * Dimensions.Z;
	ParallelFor(RowCount, [&](const int32 RowIndex)
	{
		if (bCancelled.Load())
		{
			return;
		}
		if (ShouldCancel && ShouldCancel())
		{
			bCancelled.Store(true);
			return;
		}

		const int32 Z = RowIndex / Dimensions.Y;
		const int32 Y = RowIndex - Z * Dimensions.Y;
		const double TransitionCoordinate = Dimensions.Y > 1
			? static_cast<double>(Y) / static_cast<double>(Dimensions.Y - 1)
			: 0.0;
		const FNormalizedMetricProfile& Profile = Profiles[Z];
		const int64 RowOffset = static_cast<int64>(RowIndex) * Dimensions.X;
		for (int32 X = 0; X < Dimensions.X; ++X)
		{
			if ((X & 31) == 0 && ShouldCancel && ShouldCancel())
			{
				bCancelled.Store(true);
				return;
			}
			const double ImpactCoordinate = Dimensions.X > 1
				? static_cast<double>(X) / static_cast<double>(Dimensions.X - 1)
				: 0.0;
			OutVolumeData.Voxels[RowOffset + X] = BuildTexel(
				Profile, Descriptor.IntegrationSteps, ImpactCoordinate, TransitionCoordinate);
		}
	});
	OutStats.IntegrationMilliseconds = (FPlatformTime::Seconds() - IntegrationStart) * 1000.0;

	if (bCancelled.Load())
	{
		OutVolumeData = FWPLUTVolumeData();
		OutStats.bCancelled = true;
		OutStats.Error = TEXT("LUT build was cancelled.");
		OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
		return false;
	}

	if (!OutVolumeData.IsValid(&OutStats.Error))
	{
		OutVolumeData = FWPLUTVolumeData();
		OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
		return false;
	}

	OutStats.TotalMilliseconds = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
#if !UE_BUILD_SHIPPING
	WP_LOG(nullptr, Verbose,
		TEXT("Build complete. Hash=%s Size=%dx%dx%d Steps=%d Bytes=%lld CpuMs=%.3f"),
		*OutVolumeData.BuildHash,
		Dimensions.X, Dimensions.Y, Dimensions.Z,
		Descriptor.IntegrationSteps,
		OutStats.TextureBytes,
		OutStats.TotalMilliseconds);
#endif
	return true;
}

double FWPLUTGenerator::EvaluateNormalizedRadius(
	const FWPLUTDescriptor& Descriptor,
	const double TransitionRatio,
	const double TransitionCoordinate01)
{
	const FNormalizedMetricProfile Profile(Descriptor, TransitionRatio);
	return Profile.Radius(
		FMath::Clamp(TransitionCoordinate01, 0.0, 1.0) * Profile.GetTransitionRatio());
}

double FWPLUTGenerator::ComputeNormalizedOuterRadius(
	const FWPLUTDescriptor& Descriptor,
	const double TransitionRatio)
{
	return FNormalizedMetricProfile(Descriptor, TransitionRatio).GetOuterRadius();
}


