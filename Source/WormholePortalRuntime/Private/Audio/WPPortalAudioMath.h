// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WPPortalAudioMath
{
	/**
	 * Represents a path in which the entry source is mapped into exit space and reradiated
	 * by a spherical Portal.
	 * The logical distance is the sum of the two radial legs from the Virtual Source and
	 * Listener to the Portal
	 * sphere; distance through the Portal interior is excluded.
	 */
	struct FSphericalPortalPath
	{
		FVector SourceDirection = FVector::ZeroVector;
		FVector ExitDirection = FVector::ZeroVector;
		FVector VirtualEntrySurfacePoint = FVector::ZeroVector;
		FVector ExitSurfacePoint = FVector::ZeroVector;
		FVector EffectiveProxyLocation = FVector::ZeroVector;

		double SourceToEntryDistance = 0.0;
		double ExitToListenerDistance = 0.0;
		double LogicalDistance = 0.0;
	};

	/**
	 * Treats the spherical Portal as an acoustic boundary that reradiates in every
	 * direction and computes the
	 * two radial path legs. VirtualSource must be the corresponding virtual origin produced
	 * by
	 * FWPTransform::MapRayOrigin from the same local coordinates.
	 */
	inline bool ResolveSphericalPortalPath(
		const FVector& VirtualSource,
		const FVector& Listener,
		const FVector& PortalCenter,
		const double PortalRadius,
		FSphericalPortalPath& OutPath)
	{
		OutPath = FSphericalPortalPath{};

		if (VirtualSource.ContainsNaN() || Listener.ContainsNaN() || PortalCenter.ContainsNaN()
			|| !FMath::IsFinite(PortalRadius) || PortalRadius <= KINDA_SMALL_NUMBER)
		{
			return false;
		}

		const FVector SourceOffset = VirtualSource - PortalCenter;
		const FVector ListenerOffset = Listener - PortalCenter;
		const double SourceCenterDistance = SourceOffset.Size();
		const double ListenerCenterDistance = ListenerOffset.Size();
		if (!FMath::IsFinite(SourceCenterDistance)
			|| !FMath::IsFinite(ListenerCenterDistance))
		{
			return false;
		}

		const double OutsideTolerance = FMath::Max(1.0, PortalRadius * 1.0e-3);
		const double OutsideRadius = PortalRadius + OutsideTolerance;

		// VirtualSource and Listener locations at or inside the Portal sphere plus its tolerance require a separate Transit policy and are excluded from this path.
		if (SourceCenterDistance <= OutsideRadius
			|| ListenerCenterDistance <= OutsideRadius)
		{
			return false;
		}

		const FVector SourceDirection = -SourceOffset / SourceCenterDistance;
		const FVector ExitDirection = ListenerOffset / ListenerCenterDistance;
		if (SourceDirection.IsNearlyZero() || ExitDirection.IsNearlyZero())
		{
			return false;
		}

		const double SourceToEntryDistance = SourceCenterDistance - PortalRadius;
		const double ExitToListenerDistance = ListenerCenterDistance - PortalRadius;
		const double LogicalDistance = SourceToEntryDistance + ExitToListenerDistance;
		if (!FMath::IsFinite(LogicalDistance) || LogicalDistance <= KINDA_SMALL_NUMBER)
		{
			return false;
		}

		OutPath.SourceDirection = SourceDirection;
		OutPath.ExitDirection = ExitDirection;
		OutPath.VirtualEntrySurfacePoint = VirtualSource
			+ SourceDirection * SourceToEntryDistance;
		OutPath.ExitSurfacePoint = PortalCenter + ExitDirection * PortalRadius;
		OutPath.EffectiveProxyLocation = Listener - ExitDirection * LogicalDistance;
		OutPath.SourceToEntryDistance = SourceToEntryDistance;
		OutPath.ExitToListenerDistance = ExitToListenerDistance;
		OutPath.LogicalDistance = LogicalDistance;
		return !OutPath.VirtualEntrySurfacePoint.ContainsNaN()
			&& !OutPath.ExitSurfacePoint.ContainsNaN()
			&& !OutPath.EffectiveProxyLocation.ContainsNaN();
	}
}
