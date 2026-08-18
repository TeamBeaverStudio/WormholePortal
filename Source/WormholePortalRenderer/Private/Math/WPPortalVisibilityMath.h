// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WPPortalVisibilityMath
{
	inline constexpr double AnalyticProxySafetyShellCm = 1.0;
	
	inline double GetSafeProxyRadiusCm(const double PortalRadiusCm, const double OuterRadiusCm)
	{
		return FMath::Max(OuterRadiusCm, PortalRadiusCm + AnalyticProxySafetyShellCm);
	}
}
