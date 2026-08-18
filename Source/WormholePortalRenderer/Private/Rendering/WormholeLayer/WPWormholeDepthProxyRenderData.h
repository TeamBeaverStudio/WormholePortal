// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveComponentId.h"
#include "Rendering/CustomRenderPass.h"

class FWPWormholeDepthProxyPassUserData final : public ICustomRenderPassUserData
{
public:
	explicit FWPWormholeDepthProxyPassUserData(TArray<FPrimitiveComponentId>&& InPrimitiveIds)
		: PrimitiveIds(MoveTemp(InPrimitiveIds))
	{
	}

	IMPLEMENT_CUSTOM_RENDER_PASS_USER_DATA(FWPWormholeDepthProxyPassUserData);

	bool Contains(const FPrimitiveComponentId PrimitiveId) const
	{
		return PrimitiveId.IsValid() && PrimitiveIds.Contains(PrimitiveId);
	}

	TArray<FPrimitiveComponentId> PrimitiveIds;
};
