// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPPortalTraceChannelConfig.h"

#include "WPSettings.h"
#include "Engine/CollisionProfile.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

constexpr TCHAR CollisionProfileSection[] = TEXT("/Script/Engine.CollisionProfile");
constexpr TCHAR DefaultChannelResponsesKey[] = TEXT("DefaultChannelResponses");
constexpr TCHAR RequiredPortalTraceChannelName[] = TEXT("WPPortalTrace");

FWPPortalTraceChannelConfig::EStatus FWPPortalTraceChannelConfig::GetStatus()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	ECollisionChannel ExistingChannel = ECC_MAX;
	bool bExistingChannelIsTraceType = false;

	if (TryFindPortalTraceChannel(ExistingChannel, bExistingChannelIsTraceType))
	{
		if (!bExistingChannelIsTraceType)
		{
			return EStatus::WrongType;
		}

		return ExistingChannel == Settings->PortalTraceChannel.GetValue()
			? EStatus::Valid
			: EStatus::ChannelMismatch;
	}

	ECollisionChannel FreeChannel = ECC_MAX;
	return TryFindFreeGameTraceChannel(FreeChannel)
		? EStatus::Missing
		: EStatus::NoFreeChannel;
}

FWPPortalTraceChannelConfig::EAddOrAlignResult FWPPortalTraceChannelConfig::AddOrAlignProjectConfig()
{
	const FString DesiredName = RequiredPortalTraceChannelName;
	const FString DefaultEngineIni = GetProjectDefaultEngineIniFilename();

	ECollisionChannel ExistingChannel = ECC_MAX;
	bool bExistingChannelIsTraceType = false;
	if (TryFindPortalTraceChannel(ExistingChannel, bExistingChannelIsTraceType))
	{
		if (!bExistingChannelIsTraceType)
		{
			return EAddOrAlignResult::WrongType;
		}

		return SaveWormholePortalTraceSettings(ExistingChannel, DesiredName)
			? EAddOrAlignResult::SettingsAligned
			: EAddOrAlignResult::ExistingSettingsSaveFailed;
	}

	ECollisionChannel FreeChannel = ECC_MAX;
	if (!TryFindFreeGameTraceChannel(FreeChannel))
	{
		return EAddOrAlignResult::NoFreeChannel;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*DefaultEngineIni) && PlatformFile.IsReadOnly(*DefaultEngineIni))
	{
		return EAddOrAlignResult::DefaultEngineIniReadOnly;
	}

	if (!AddPortalTraceChannelToCollisionProfile(FreeChannel, DesiredName))
	{
		return EAddOrAlignResult::AddFailed;
	}

	return SaveWormholePortalTraceSettings(FreeChannel, DesiredName)
		? EAddOrAlignResult::Added
		: EAddOrAlignResult::AddedSettingsSaveFailed;
}

FString FWPPortalTraceChannelConfig::GetProjectDefaultEngineIniFilename()
{
	return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
}

FString FWPPortalTraceChannelConfig::GetCollisionChannelName(ECollisionChannel Channel)
{
	if (const UEnum* CollisionChannelEnum = StaticEnum<ECollisionChannel>())
	{
		return CollisionChannelEnum->GetNameStringByValue(static_cast<int64>(Channel));
	}

	return FString();
}

bool FWPPortalTraceChannelConfig::DoesEntryReferenceChannel(const FString& Entry, ECollisionChannel Channel)
{
	FString EntryChannelName;
	const FString ChannelName = GetCollisionChannelName(Channel);
	return !ChannelName.IsEmpty() &&
		FParse::Value(*Entry, TEXT("Channel="), EntryChannelName) &&
		EntryChannelName == ChannelName;
}

bool FWPPortalTraceChannelConfig::DoesEntryHaveName(const FString& Entry, const FString& DesiredName)
{
	return Entry.Contains(FString::Printf(TEXT("Name=\"%s\""), *DesiredName)) ||
		Entry.Contains(FString::Printf(TEXT("Name=%s"), *DesiredName));
}

bool FWPPortalTraceChannelConfig::IsEntryTraceType(const FString& Entry)
{
	return Entry.Contains(TEXT("bTraceType=True")) ||
		Entry.Contains(TEXT("bTraceType=true"));
}

bool FWPPortalTraceChannelConfig::TryFindGameTraceChannelInEntry(const FString& Entry, ECollisionChannel& OutChannel)
{
	OutChannel = ECC_MAX;

	for (int32 ChannelValue = ECC_GameTraceChannel1; ChannelValue <= ECC_GameTraceChannel18; ++ChannelValue)
	{
		const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelValue);
		if (DoesEntryReferenceChannel(Entry, Channel))
		{
			OutChannel = Channel;
			return true;
		}
	}

	return false;
}

bool FWPPortalTraceChannelConfig::TryFindFreeGameTraceChannel(ECollisionChannel& OutChannel)
{
	OutChannel = ECC_MAX;

	TArray<FString> ChannelResponses;
	GConfig->GetArray(
		CollisionProfileSection,
		DefaultChannelResponsesKey,
		ChannelResponses,
		GEngineIni);

	for (int32 ChannelValue = ECC_GameTraceChannel1; ChannelValue <= ECC_GameTraceChannel18; ++ChannelValue)
	{
		const ECollisionChannel CandidateChannel = static_cast<ECollisionChannel>(ChannelValue);
		bool bChannelInUse = false;

		for (const FString& Entry : ChannelResponses)
		{
			if (DoesEntryReferenceChannel(Entry, CandidateChannel))
			{
				bChannelInUse = true;
				break;
			}
		}

		if (!bChannelInUse)
		{
			OutChannel = CandidateChannel;
			return true;
		}
	}

	return false;
}

bool FWPPortalTraceChannelConfig::TryFindPortalTraceChannel(ECollisionChannel& OutChannel, bool& bOutIsTraceType)
{
	OutChannel = ECC_MAX;
	bOutIsTraceType = false;

	const FString DesiredName = RequiredPortalTraceChannelName;

	TArray<FString> ChannelResponses;
	GConfig->GetArray(
		CollisionProfileSection,
		DefaultChannelResponsesKey,
		ChannelResponses,
		GEngineIni);

	for (const FString& Entry : ChannelResponses)
	{
		if (!DoesEntryHaveName(Entry, DesiredName))
		{
			continue;
		}

		bOutIsTraceType = IsEntryTraceType(Entry);
		return TryFindGameTraceChannelInEntry(Entry, OutChannel);
	}

	return false;
}

bool FWPPortalTraceChannelConfig::AddPortalTraceChannelToCollisionProfile(
	ECollisionChannel Channel,
	const FString& ChannelDisplayName)
{
	UCollisionProfile* CollisionProfile = UCollisionProfile::Get();
	FArrayProperty* DefaultChannelResponsesProperty = FindFProperty<FArrayProperty>(
		UCollisionProfile::StaticClass(),
		TEXT("DefaultChannelResponses"));
	FStructProperty* ChannelSetupProperty = DefaultChannelResponsesProperty
		? CastField<FStructProperty>(DefaultChannelResponsesProperty->Inner)
		: nullptr;

	if (!CollisionProfile || !DefaultChannelResponsesProperty || !ChannelSetupProperty ||
		ChannelSetupProperty->Struct != FCustomChannelSetup::StaticStruct())
	{
		return false;
	}

	void* DefaultChannelResponsesPtr = DefaultChannelResponsesProperty->ContainerPtrToValuePtr<void>(CollisionProfile);
	FScriptArrayHelper DefaultChannelResponses(DefaultChannelResponsesProperty, DefaultChannelResponsesPtr);
	const FName ChannelName(*ChannelDisplayName);

	for (int32 Index = 0; Index < DefaultChannelResponses.Num(); ++Index)
	{
		const FCustomChannelSetup* ExistingChannelSetup =
			reinterpret_cast<const FCustomChannelSetup*>(DefaultChannelResponses.GetRawPtr(Index));
		if (!ExistingChannelSetup)
		{
			continue;
		}

		if (ExistingChannelSetup->Name == ChannelName || ExistingChannelSetup->Channel == Channel)
		{
			return ExistingChannelSetup->Name == ChannelName &&
				ExistingChannelSetup->Channel == Channel &&
				ExistingChannelSetup->bTraceType;
		}
	}

	FCustomChannelSetup NewChannelSetup;
	NewChannelSetup.Channel = Channel;
	NewChannelSetup.DefaultResponse = ECR_Ignore;
	NewChannelSetup.bTraceType = true;
	NewChannelSetup.bStaticObject = false;
	NewChannelSetup.Name = ChannelName;

	CollisionProfile->Modify();
	const int32 NewIndex = DefaultChannelResponses.AddValue();
	ChannelSetupProperty->CopyCompleteValue(DefaultChannelResponses.GetRawPtr(NewIndex), &NewChannelSetup);

	CollisionProfile->LoadProfileConfig(true);

	const FString SpecificFileLocation;
	constexpr bool bWarnIfFail = false;
	return CollisionProfile->TryUpdateDefaultConfigFile(SpecificFileLocation, bWarnIfFail);
}

bool FWPPortalTraceChannelConfig::SaveWormholePortalTraceSettings(
	ECollisionChannel Channel,
	const FString& ChannelDisplayName)
{
	UWPSettings* MutableSettings = GetMutableDefault<UWPSettings>();
	MutableSettings->PortalTraceChannel = Channel;
	MutableSettings->PortalTraceChannelName = FName(*ChannelDisplayName);

	const FString SpecificFileLocation;
	constexpr bool bWarnIfFail = false;
	return MutableSettings->TryUpdateDefaultConfigFile(SpecificFileLocation, bWarnIfFail);
}
