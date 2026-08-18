// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** CPU reference implementation for validating the analytic-proxy rules used by the production composite. */
namespace WPPortalMaskMath
{
	inline constexpr double MinimumHitDistanceCm = 1.0e-4;
	inline constexpr double AnalyticProxySafetyShellCm = 1.0;
	inline constexpr double CoverageCarrierRadiusScale = 1.05;

	inline double GetCoverageCarrierRadiusCm(const double ProxyRadiusCm)
	{
		return ProxyRadiusCm * CoverageCarrierRadiusScale;
	}

	struct FIntersection
	{
		double NearDistanceCm = 0.0;
		double FarDistanceCm = 0.0;
		double SelectedDistanceCm = 0.0;
		double CoverageDistanceCm = 0.0;
		bool bCameraInside = false;
		bool bCameraInsideCarrier = false;
	};

	inline bool IsFiniteVector(const FVector3d& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	inline bool IntersectProxySphere(
		const FVector3d& CameraTranslated,
		const FVector3d& RayDirection,
		const FVector3d& CenterTranslated,
		const double RadiusCm,
		FIntersection& OutIntersection)
	{
		OutIntersection = FIntersection();
		if (!IsFiniteVector(CameraTranslated)
			|| !IsFiniteVector(RayDirection)
			|| !IsFiniteVector(CenterTranslated)
			|| !FMath::IsFinite(RadiusCm)
			|| RadiusCm <= 0.0)
		{
			return false;
		}

		const double DirectionLengthSquared = RayDirection.SquaredLength();
		if (!FMath::IsFinite(DirectionLengthSquared)
			|| DirectionLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		const FVector3d Direction = RayDirection / FMath::Sqrt(DirectionLengthSquared);
		const FVector3d CameraOffset = CameraTranslated - CenterTranslated;
		const double CameraDistanceSquared = CameraOffset.SquaredLength();
		const double RadiusSquared = RadiusCm * RadiusCm;
		const double ProjectedCenter = FVector3d::DotProduct(CameraOffset, Direction);
		const double Discriminant = ProjectedCenter * ProjectedCenter
			- (CameraDistanceSquared - RadiusSquared);
		if (!FMath::IsFinite(Discriminant) || Discriminant < 0.0)
		{
			return false;
		}

		const double Root = FMath::Sqrt(FMath::Max(Discriminant, 0.0));
		OutIntersection.NearDistanceCm = -ProjectedCenter - Root;
		OutIntersection.FarDistanceCm = -ProjectedCenter + Root;
		OutIntersection.bCameraInside = CameraDistanceSquared < RadiusSquared;
		OutIntersection.SelectedDistanceCm = OutIntersection.bCameraInside
			? OutIntersection.FarDistanceCm
			: OutIntersection.NearDistanceCm;
		const double CarrierRadiusCm = GetCoverageCarrierRadiusCm(RadiusCm);
		OutIntersection.bCameraInsideCarrier = CameraDistanceSquared
			< CarrierRadiusCm * CarrierRadiusCm;
		OutIntersection.CoverageDistanceCm = OutIntersection.bCameraInsideCarrier
			? OutIntersection.FarDistanceCm
			: OutIntersection.NearDistanceCm;
		return FMath::IsFinite(OutIntersection.SelectedDistanceCm)
			&& FMath::IsFinite(OutIntersection.CoverageDistanceCm)
			&& OutIntersection.SelectedDistanceCm > MinimumHitDistanceCm
			&& OutIntersection.CoverageDistanceCm > MinimumHitDistanceCm;
	}

	/** UE reversed-Z near-plane coverage test used by the production composite shader. */
	inline bool IsCoverageRejectedByNearPlaneClip(
		const double CoverageClipZ,
		const double CoverageClipW)
	{
		return !FMath::IsFinite(CoverageClipZ)
			|| !FMath::IsFinite(CoverageClipW)
			|| CoverageClipW <= 0.0
			|| CoverageClipZ > CoverageClipW;
	}

	/** Tests the proxy sphere against the view near plane along the plane normal. */
	inline bool DoesProxySphereIntersectNearPlane(
		const double PortalForwardDistanceCm,
		const double NearClipCm,
		const double ProxyRadiusCm)
	{
		return FMath::IsFinite(PortalForwardDistanceCm)
			&& FMath::IsFinite(NearClipCm)
			&& FMath::IsFinite(ProxyRadiusCm)
			&& NearClipCm >= 0.0
			&& ProxyRadiusCm > 0.0
			&& FMath::Abs(PortalForwardDistanceCm - NearClipCm) <= ProxyRadiusCm;
	}

	/** UE 5.8 post-TAA/TSR convention for reconstructing current-frame geometry. */
	inline FVector2d GetTemporalJitterCompensatedViewportUV(
		const FVector2d& StableViewportUV,
		const FVector2d& TemporalAAJitter)
	{
		return StableViewportUV + FVector2d(
			0.5 * TemporalAAJitter.X,
			-0.5 * TemporalAAJitter.Y);
	}

	/**
	 * Resolves the eye slice for a Texture2DArray SceneDepth. Prefer the actual
	 * SceneColor input slice, then the stereo index; never silently bind eye 0.
	 */
	inline int32 ResolveSceneDepthArraySlice(
		const bool bSceneColorUsesArray,
		const int32 SceneColorArraySlice,
		const int32 StereoViewIndex,
		const int32 SceneDepthArraySize)
	{
		if (SceneDepthArraySize <= 0)
		{
			return INDEX_NONE;
		}
		if (bSceneColorUsesArray
			&& SceneColorArraySlice >= 0
			&& SceneColorArraySlice < SceneDepthArraySize)
		{
			return SceneColorArraySlice;
		}
		if (StereoViewIndex >= 0 && StereoViewIndex < SceneDepthArraySize)
		{
			return StereoViewIndex;
		}
		return SceneDepthArraySize == 1 ? 0 : INDEX_NONE;
	}

	inline bool IsOccludedByOpaqueDepth(
		const bool bCameraInsideProxy,
		const double OpaqueSceneDepthCm,
		const double ProxySceneDepthCm,
		const double DepthBiasCm)
	{
		return !bCameraInsideProxy
			&& FMath::IsFinite(OpaqueSceneDepthCm)
			&& FMath::IsFinite(ProxySceneDepthCm)
			&& FMath::IsFinite(DepthBiasCm)
			&& OpaqueSceneDepthCm + FMath::Max(DepthBiasCm, 0.0) < ProxySceneDepthCm;
	}
}
