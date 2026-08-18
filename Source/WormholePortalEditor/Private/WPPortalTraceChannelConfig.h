// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class FWPPortalTraceChannelConfig final
{
public:
	enum class EStatus : uint8
	{
		Valid,
		Missing,
		WrongType,
		ChannelMismatch,
		NoFreeChannel
	};

	enum class EAddOrAlignResult : uint8
	{
		Added,
		SettingsAligned,
		WrongType,
		NoFreeChannel,
		DefaultEngineIniReadOnly,
		AddFailed,
		ExistingSettingsSaveFailed,
		AddedSettingsSaveFailed
	};

	static EStatus GetStatus();
	static EAddOrAlignResult AddOrAlignProjectConfig();

private:
	static FString GetProjectDefaultEngineIniFilename();
	static FString GetCollisionChannelName(ECollisionChannel Channel);
	static bool DoesEntryReferenceChannel(const FString& Entry, ECollisionChannel Channel);
	static bool DoesEntryHaveName(const FString& Entry, const FString& DesiredName);
	static bool IsEntryTraceType(const FString& Entry);
	static bool TryFindGameTraceChannelInEntry(const FString& Entry, ECollisionChannel& OutChannel);
	static bool TryFindFreeGameTraceChannel(ECollisionChannel& OutChannel);
	static bool TryFindPortalTraceChannel(ECollisionChannel& OutChannel, bool& bOutIsTraceType);
	static bool AddPortalTraceChannelToCollisionProfile(ECollisionChannel Channel, const FString& ChannelDisplayName);
	static bool SaveWormholePortalTraceSettings(ECollisionChannel Channel, const FString& ChannelDisplayName);
};
