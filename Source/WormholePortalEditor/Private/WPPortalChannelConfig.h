// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Templates/Function.h"

/**
 * Wormhole Portal requires a dedicated Trace Channel (WPPortalTrace) and Object Channel (WPPortalCollision). 
 * This class finds them in DefaultEngine.ini, adds them if missing, and saves the actual Slot to UWPSettings. 
 * Both Channels share the same Game Channel Slot pool, so free-slot searches always scan both kinds together.
 */
class FWPPortalChannelConfig final
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

	/**
	 * Trace/Collision Channel Share Response Spec (Name, Type, Default Response)
	 */
	struct FRequiredChannel
	{
		FString Name;
		bool bTraceType = false;
		ECollisionResponse DefaultResponse = ECR_Ignore;
	};

	static EStatus GetTraceChannelStatus();
	static EStatus GetCollisionChannelStatus();
	static EAddOrAlignResult AddOrAlignTraceChannel();
	static EAddOrAlignResult AddOrAlignCollisionChannel();

private:
	static FString GetProjectDefaultEngineIniFilename();
	static FString GetCollisionChannelName(ECollisionChannel Channel);
	
	static bool DoesEntryReferenceChannel(const FString& Entry, ECollisionChannel Channel);
	static bool DoesEntryHaveName(const FString& Entry, const FString& DesiredName);
	static bool IsEntryTraceType(const FString& Entry);
	
	static bool TryFindGameChannelInEntry(const FString& Entry, ECollisionChannel& OutChannel);
	static bool TryFindFreeGameChannel(ECollisionChannel& OutChannel);
	static bool TryFindNamedChannel(const FRequiredChannel& Spec, ECollisionChannel& OutChannel, bool& bOutIsTraceType);
	static bool AddChannelToCollisionProfile(const FRequiredChannel& Spec, ECollisionChannel Channel);
	
	static EStatus GetStatus(const FRequiredChannel& Spec, ECollisionChannel SettingsChannel);
	static EAddOrAlignResult AddOrAlignProjectConfig(const FRequiredChannel& Spec, ECollisionChannel SettingsChannel, TFunctionRef<bool(ECollisionChannel)> SaveSettings);
};
