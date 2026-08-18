// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"

/**
 * Normalized rho=1 metric profile used by both LUT baking and runtime
 * fallback. The profile always uses the current finite C2 smooth tail that
 * joins flat space at the finite transition boundary.
 */
class WORMHOLEPORTALRUNTIME_API FNormalizedMetricProfile
{
public:
	FNormalizedMetricProfile(const FWPLUTDescriptor& InDescriptor, double InTransitionRatio);

	double GetTransitionRatio() const;
	double GetOuterRadius() const;
	double Radius(double TransitionEll) const;

private:
	double StrictRadius(double TransitionEll) const;
	double StrictSlope(double TransitionEll) const;
	double StrictCurvature(double TransitionEll) const;
	double ComputeOuterRadius() const;

	FWPLUTDescriptor Descriptor;
	double TransitionRatio = 1.0;
	double M = 1.0;
	double FlattenStart = 0.5;
	double OuterRadius = 1.0;
};

/**
 * Pure CPU normalized LUT generator shared by editor baking and runtime
 * fallback. It never creates or touches UObjects and is safe to call off the
 * game thread. BuildVolumeData parallelizes independent V/Z rows internally.
 */
class WORMHOLEPORTALRUNTIME_API FWPLUTGenerator
{
public:
	using FShouldCancel = TFunction<bool()>;

	static bool BuildVolumeData(
		const FWPLUTDescriptor& Descriptor,
		FWPLUTVolumeData& OutVolumeData,
		FWPLUTBuildStats& OutStats,
		FShouldCancel ShouldCancel = FShouldCancel());

	/** r/rho at transition-local V for rho=1 and T/rho=TransitionRatio. */
	static double EvaluateNormalizedRadius(
		const FWPLUTDescriptor& Descriptor,
		double TransitionRatio,
		double TransitionCoordinate01);

	static double ComputeNormalizedOuterRadius(
		const FWPLUTDescriptor& Descriptor,
		double TransitionRatio);
};
