// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"

// Exposes the DLL boundary so separate modules, such as the Renderer, can use the same log category.
WORMHOLEPORTALRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogWormhole, Log, All);

namespace WormholeLog
{
	/**
	 * Returns a readable network mode name for the given world context object.
	 *
	 * @param WorldContextObject Any UObject associated with a valid UWorld.
	 * @return A constant string describing the current network mode.
	 */
	inline const TCHAR* GetNetModeName(const UObject* WorldContextObject)
	{
		const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
		
		if (!World) return TEXT("NoWorld");
		
		switch (World->GetNetMode())
		{
			case NM_Standalone:			return TEXT("STANDALONE");
			case NM_ListenServer:		return TEXT("LISTEN");
			case NM_DedicatedServer:	return TEXT("DEDICATED");
			case NM_Client:				return TEXT("CLIENT");
			default:					return TEXT("UNKNOWN");
		}
	}
}


/**
 * Writes a Wormhole log message with network mode and calling function information.
 * 
 * example: WP_LOG(this, Log, TEXT("Begin Transit started!")); (assume StandAlone && WormholePortal::StartupModule)
 * OUTPUT:  [StandAlone][WormholePortal::StartupModule] Begin Transit started!
 *
 * @param WorldContextObject Any UObject associated with the target UWorld.
 * @param Verbosity Unreal log verbosity level.
 * @param Format UE_LOG-compatible TCHAR format string.
 */
#define WP_LOG(WorldContextObject, Verbosity, Format, ...) \
			UE_LOG(LogWormhole, Verbosity, TEXT("[%s][%s] ") Format, \
			WormholeLog::GetNetModeName(WorldContextObject), \
			ANSI_TO_TCHAR(__FUNCTION__), ##__VA_ARGS__)
