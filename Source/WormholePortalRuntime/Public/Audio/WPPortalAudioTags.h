// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WPPortalAudioTags
{
	/**
	 * Excludes this Audio Component, or the Actor that owns it, from Portal Audio
	 * discovery.
	 */
	inline const FName Disabled(TEXT("WP.PortalAudio.Disabled"));

	/**
	 * Identifies proxy audio created on the opposite side by the Portal Audio Subsystem.
	 */
	inline const FName Generated(TEXT("WP.PortalAudio.Generated"));
}
