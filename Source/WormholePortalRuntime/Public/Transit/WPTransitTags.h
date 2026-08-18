// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Common tags used to exclude Transit targets and identify Runtime-generated objects.
namespace WPTransitTags
{
	inline const FName Generated(TEXT("WormholeGeneratedTransit"));
	inline const FName Ignore(TEXT("Wormhole.Ignore"));
}
