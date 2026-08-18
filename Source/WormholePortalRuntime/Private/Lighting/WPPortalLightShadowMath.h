// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Vector.h"
#include "Math/Vector2D.h"

namespace WPPortalLightShadowMath
{
	inline constexpr double MinimumMagnitudeSquared = 1.0e-12;
	inline constexpr double MinimumForwardCosine = 1.0e-6;

	struct FRaySphereIntersection
	{
		double NearDistanceCm = 0.0;
		double FarDistanceCm = 0.0;
		bool bOriginInside = false;
	};

	struct FCaptureProjection
	{
		FVector2d UV = FVector2d::ZeroVector;
		double ViewZCm = 0.0;
		double ForwardCosine = 0.0;
		bool bInsideFrustum = false;
	};

	/** One due route considered by the per-frame source-shadow capture scheduler. */
	struct FCaptureCandidate
	{
		bool bCaptureDue = true;
		bool bNeedsInitialCapture = false;
		bool bTransformDirty = false;
		double ScreenDiameterPixels = 0.0;
		double LastCaptureTimeSeconds = 0.0;
	};

	inline bool IsFiniteVector(const FVector3d& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	/**
	 * Intersects a ray with a sphere and preserves both analytic roots.
	 * RayDirection does not need to be normalized. An inside origin is a valid
	 * intersection, but is reported so portal-shadow capture can fail open.
	 */
	inline bool IntersectRaySphere(
		const FVector3d& RayOrigin,
		const FVector3d& RayDirection,
		const FVector3d& SphereCenter,
		const double SphereRadiusCm,
		FRaySphereIntersection& OutIntersection)
	{
		OutIntersection = FRaySphereIntersection{};

		if (!IsFiniteVector(RayOrigin)
			|| !IsFiniteVector(RayDirection)
			|| !IsFiniteVector(SphereCenter)
			|| !FMath::IsFinite(SphereRadiusCm)
			|| SphereRadiusCm <= 0.0)
		{
			return false;
		}

		const double DirectionMagnitudeSquared = RayDirection.SquaredLength();
		if (!FMath::IsFinite(DirectionMagnitudeSquared)
			|| DirectionMagnitudeSquared <= MinimumMagnitudeSquared)
		{
			return false;
		}

		const FVector3d Direction = RayDirection / FMath::Sqrt(DirectionMagnitudeSquared);
		const FVector3d CenterToOrigin = RayOrigin - SphereCenter;
		const double HalfB = FVector3d::DotProduct(CenterToOrigin, Direction);
		const double C = CenterToOrigin.SquaredLength() - FMath::Square(SphereRadiusCm);
		const double Discriminant = FMath::Square(HalfB) - C;

		if (!FMath::IsFinite(Discriminant) || Discriminant < 0.0)
		{
			return false;
		}

		const double Root = FMath::Sqrt(FMath::Max(Discriminant, 0.0));
		const double NearDistanceCm = -HalfB - Root;
		const double FarDistanceCm = -HalfB + Root;
		if (!FMath::IsFinite(NearDistanceCm)
			|| !FMath::IsFinite(FarDistanceCm)
			|| FarDistanceCm < 0.0)
		{
			return false;
		}

		OutIntersection.NearDistanceCm = NearDistanceCm;
		OutIntersection.FarDistanceCm = FarDistanceCm;
		OutIntersection.bOriginInside = C < 0.0;
		return true;
	}

	/**
	 * Projects a portal ray into a perspective SceneCapture and converts its
	 * ray distance to the linear View-Z stored by SCS_SceneDepth.
	 */
	inline bool ProjectRayToCapture(
		const FVector3d& RayDirection,
		const double RayDistanceCm,
		const FVector3d& CaptureForward,
		const FVector3d& CaptureRight,
		const FVector3d& CaptureUp,
		const double TanHalfFov,
		FCaptureProjection& OutProjection)
	{
		OutProjection = FCaptureProjection{};

		if (!IsFiniteVector(RayDirection)
			|| !IsFiniteVector(CaptureForward)
			|| !IsFiniteVector(CaptureRight)
			|| !IsFiniteVector(CaptureUp)
			|| !FMath::IsFinite(RayDistanceCm)
			|| RayDistanceCm <= 0.0
			|| !FMath::IsFinite(TanHalfFov)
			|| TanHalfFov <= 0.0)
		{
			return false;
		}

		const double DirectionMagnitudeSquared = RayDirection.SquaredLength();
		const double ForwardMagnitudeSquared = CaptureForward.SquaredLength();
		const double RightMagnitudeSquared = CaptureRight.SquaredLength();
		const double UpMagnitudeSquared = CaptureUp.SquaredLength();
		if (DirectionMagnitudeSquared <= MinimumMagnitudeSquared
			|| ForwardMagnitudeSquared <= MinimumMagnitudeSquared
			|| RightMagnitudeSquared <= MinimumMagnitudeSquared
			|| UpMagnitudeSquared <= MinimumMagnitudeSquared)
		{
			return false;
		}

		const FVector3d Direction = RayDirection / FMath::Sqrt(DirectionMagnitudeSquared);
		const FVector3d Forward = CaptureForward / FMath::Sqrt(ForwardMagnitudeSquared);
		const FVector3d Right = CaptureRight / FMath::Sqrt(RightMagnitudeSquared);
		const FVector3d Up = CaptureUp / FMath::Sqrt(UpMagnitudeSquared);
		const double ForwardCosine = FVector3d::DotProduct(Direction, Forward);
		if (!FMath::IsFinite(ForwardCosine) || ForwardCosine <= MinimumForwardCosine)
		{
			return false;
		}

		const double ProjectionDenominator = 2.0 * ForwardCosine * TanHalfFov;
		const FVector2d UV(
			0.5 + FVector3d::DotProduct(Direction, Right) / ProjectionDenominator,
			0.5 - FVector3d::DotProduct(Direction, Up) / ProjectionDenominator);
		const double ViewZCm = RayDistanceCm * ForwardCosine;

		if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y) || !FMath::IsFinite(ViewZCm))
		{
			return false;
		}

		OutProjection.UV = UV;
		OutProjection.ViewZCm = ViewZCm;
		OutProjection.ForwardCosine = ForwardCosine;
		OutProjection.bInsideFrustum = UV.X >= 0.0 && UV.X <= 1.0
			&& UV.Y >= 0.0 && UV.Y <= 1.0;
		return true;
	}

	/** Invalid depth data fails open so a stale/uninitialized map cannot black out portal light. */
	inline bool IsSourceVisibleFromViewZ(
		const double CapturedViewZCm,
		const double PortalBoundaryViewZCm,
		const double DepthBiasCm)
	{
		if (!FMath::IsFinite(CapturedViewZCm)
			|| !FMath::IsFinite(PortalBoundaryViewZCm)
			|| !FMath::IsFinite(DepthBiasCm)
			|| CapturedViewZCm <= 0.0
			|| PortalBoundaryViewZCm <= 0.0)
		{
			return true;
		}

		return CapturedViewZCm + FMath::Max(DepthBiasCm, 0.0) >= PortalBoundaryViewZCm;
	}

	/** Selects the 128/256/512 tier from the exit portal's maximum on-screen diameter. */
	inline int32 SelectAdaptiveResolution(const double ScreenDiameterPixels)
	{
		if (!FMath::IsFinite(ScreenDiameterPixels) || ScreenDiameterPixels < 128.0)
		{
			return 128;
		}

		return ScreenDiameterPixels <= 512.0 ? 256 : 512;
	}

	/**
	 * Returns indices into Candidates, ordered by initial capture, transform
	 * dirtiness, screen size, oldest capture, then input order.
	 */
	inline void SelectCaptureCandidateIndices(
		const TConstArrayView<FCaptureCandidate> Candidates,
		const int32 MaxCaptures,
		TArray<int32>& OutCandidateIndices)
	{
		OutCandidateIndices.Reset();
		if (MaxCaptures <= 0 || Candidates.IsEmpty())
		{
			return;
		}

		TArray<int32> DueCandidateIndices;
		DueCandidateIndices.Reserve(Candidates.Num());
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			if (Candidates[CandidateIndex].bCaptureDue)
			{
				DueCandidateIndices.Add(CandidateIndex);
			}
		}

		DueCandidateIndices.Sort([Candidates](const int32 AIndex, const int32 BIndex)
		{
			const FCaptureCandidate& A = Candidates[AIndex];
			const FCaptureCandidate& B = Candidates[BIndex];
			if (A.bNeedsInitialCapture != B.bNeedsInitialCapture)
			{
				return A.bNeedsInitialCapture;
			}
			if (A.bTransformDirty != B.bTransformDirty)
			{
				return A.bTransformDirty;
			}

			const double AScreenDiameter = FMath::IsFinite(A.ScreenDiameterPixels)
				? FMath::Max(A.ScreenDiameterPixels, 0.0) : 0.0;
			const double BScreenDiameter = FMath::IsFinite(B.ScreenDiameterPixels)
				? FMath::Max(B.ScreenDiameterPixels, 0.0) : 0.0;
			if (!FMath::IsNearlyEqual(AScreenDiameter, BScreenDiameter))
			{
				return AScreenDiameter > BScreenDiameter;
			}

			const double ALastCapture = FMath::IsFinite(A.LastCaptureTimeSeconds)
				? A.LastCaptureTimeSeconds : 0.0;
			const double BLastCapture = FMath::IsFinite(B.LastCaptureTimeSeconds)
				? B.LastCaptureTimeSeconds : 0.0;
			if (!FMath::IsNearlyEqual(ALastCapture, BLastCapture))
			{
				return ALastCapture < BLastCapture;
			}

			return AIndex < BIndex;
		});

		const int32 SelectionCount = FMath::Min(MaxCaptures, DueCandidateIndices.Num());
		OutCandidateIndices.Reserve(SelectionCount);
		for (int32 SelectionIndex = 0; SelectionIndex < SelectionCount; ++SelectionIndex)
		{
			OutCandidateIndices.Add(DueCandidateIndices[SelectionIndex]);
		}
	}
}
