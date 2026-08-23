// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPPortalChannelConfig.h"

#include "WPSettings.h"
#include "Engine/CollisionProfile.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "Collision.h"

constexpr TCHAR CollisionProfileSection[] = TEXT("/Script/Engine.CollisionProfile");
constexpr TCHAR DefaultChannelResponsesKey[] = TEXT("DefaultChannelResponses");

namespace
{
	//Trace Channel
	const FWPPortalChannelConfig::FRequiredChannel TraceSpec{
		TEXT("WPPortalTrace"),
		true,
		ECR_Ignore
	};
	
	// Collision Channel
	const FWPPortalChannelConfig::FRequiredChannel CollisionSpec{
		TEXT("WPPortalCollision"),
		false,
		ECR_Overlap
	};
}

FWPPortalChannelConfig::EStatus FWPPortalChannelConfig::GetTraceChannelStatus()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	return GetStatus(TraceSpec, Settings->PortalTraceChannel.GetValue());
}

FWPPortalChannelConfig::EStatus FWPPortalChannelConfig::GetCollisionChannelStatus()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	return GetStatus(CollisionSpec, Settings->PortalCollisionChannel.GetValue());
}

FWPPortalChannelConfig::EAddOrAlignResult FWPPortalChannelConfig::AddOrAlignTraceChannel()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	
	return AddOrAlignProjectConfig(TraceSpec, Settings->PortalTraceChannel.GetValue(),
		[](ECollisionChannel Channel)
		{
			UWPSettings* MutableSettings = GetMutableDefault<UWPSettings>();
			MutableSettings->PortalTraceChannel = Channel;
			MutableSettings->PortalTraceChannelName = FName(*TraceSpec.Name);
			
			const FString SpecificFileLocation;
			constexpr bool bWarnIfFail = false;
			return MutableSettings->TryUpdateDefaultConfigFile(SpecificFileLocation, bWarnIfFail);
		});
}

FWPPortalChannelConfig::EAddOrAlignResult FWPPortalChannelConfig::AddOrAlignCollisionChannel()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	
	return AddOrAlignProjectConfig(CollisionSpec, Settings->PortalCollisionChannel.GetValue(),
		[](ECollisionChannel Channel)
		{
			UWPSettings* MutableSettings = GetMutableDefault<UWPSettings>();
			MutableSettings->PortalCollisionChannel = Channel;
			MutableSettings->PortalCollisionChannelName = FName(*CollisionSpec.Name);
			
			const FString SpecificFileLocation;
			constexpr bool bWarnIfFail = false;
			return MutableSettings->TryUpdateDefaultConfigFile(SpecificFileLocation, bWarnIfFail);
		});
}

FString FWPPortalChannelConfig::GetProjectDefaultEngineIniFilename()
{
	return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
}

FString FWPPortalChannelConfig::GetCollisionChannelName(ECollisionChannel Channel)
{
	if (const UEnum* CollisionChannelEnum = StaticEnum<ECollisionChannel>())
	{
		return CollisionChannelEnum->GetNameStringByValue(static_cast<int64>(Channel));
	}

	return FString();
}

bool FWPPortalChannelConfig::DoesEntryReferenceChannel(const FString& Entry, ECollisionChannel Channel)
{
	FString EntryChannelName;
	const FString ChannelName = GetCollisionChannelName(Channel);
	return !ChannelName.IsEmpty() &&
		FParse::Value(*Entry, TEXT("Channel="), EntryChannelName) &&
		EntryChannelName == ChannelName;
}

bool FWPPortalChannelConfig::DoesEntryHaveName(const FString& Entry, const FString& DesiredName)
{
	return Entry.Contains(FString::Printf(TEXT("Name=\"%s\""), *DesiredName)) ||
		Entry.Contains(FString::Printf(TEXT("Name=%s"), *DesiredName));
}

bool FWPPortalChannelConfig::IsEntryTraceType(const FString& Entry)
{
	return Entry.Contains(TEXT("bTraceType=True")) ||
		Entry.Contains(TEXT("bTraceType=true"));
}

bool FWPPortalChannelConfig::TryFindGameChannelInEntry(const FString& Entry, ECollisionChannel& OutChannel)
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

bool FWPPortalChannelConfig::TryFindFreeGameChannel(ECollisionChannel& OutChannel)
{
	OutChannel = ECC_MAX;
	TArray<FString> ChannelResponse;
	GConfig->GetArray(CollisionProfileSection, DefaultChannelResponsesKey, ChannelResponse, GEngineIni);
	for (int32 ChannelValue = ECC_GameTraceChannel1; ChannelValue <= ECC_GameTraceChannel18; ++ChannelValue)
	{
		const ECollisionChannel CandidateChannel = static_cast<ECollisionChannel>(ChannelValue);
		bool bChannelInUse = false;
		for (const FString& Entry : ChannelResponse)
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

bool FWPPortalChannelConfig::TryFindNamedChannel(const FRequiredChannel& Spec, ECollisionChannel& OutChannel,
	bool& bOutIsTraceType)
{
	OutChannel = ECC_MAX;
	TArray<FString> ChannelResponse;
	GConfig->GetArray(CollisionProfileSection, DefaultChannelResponsesKey, ChannelResponse, GEngineIni);
	
	for (const FString& Entry : ChannelResponse)
	{
		if (!DoesEntryHaveName(Entry, Spec.Name))
		{
			continue;
		}
		
		bOutIsTraceType = IsEntryTraceType(Entry);
		return TryFindGameChannelInEntry(Entry, OutChannel);
	}
	return false;
}

bool FWPPortalChannelConfig::AddChannelToCollisionProfile(const FRequiredChannel& Spec, ECollisionChannel Channel)
{
	UCollisionProfile* CollisionProfile = UCollisionProfile::Get();
	FArrayProperty* DefaultChannelResponseProperty = FindFProperty<FArrayProperty>(UCollisionProfile::StaticClass(), 
		TEXT("DefaultChannelResponses"));
	
	FStructProperty* ChannelSetupProperty = DefaultChannelResponseProperty ? CastField<FStructProperty>(DefaultChannelResponseProperty->Inner) : nullptr;
	
	if (!CollisionProfile || !DefaultChannelResponseProperty || !ChannelSetupProperty || 
		ChannelSetupProperty->Struct != FCustomChannelSetup::StaticStruct())
	{
		return false;
	}
	
	void* DefaultChannelResponsePtr = DefaultChannelResponseProperty->ContainerPtrToValuePtr<void>(CollisionProfile);
	FScriptArrayHelper DefaultChannelResponse(DefaultChannelResponseProperty, DefaultChannelResponsePtr);
	const FName ChannelName(*Spec.Name);
	
	for (int32 Index=  0; Index < DefaultChannelResponse.Num(); ++Index)
	{
		const FCustomChannelSetup* ExistingChannelSetup = reinterpret_cast<const FCustomChannelSetup*>(DefaultChannelResponse.GetRawPtr(Index));
		if (!ExistingChannelSetup)
		{
			continue;
		}
		
		if (ExistingChannelSetup->Name == ChannelName || ExistingChannelSetup->Channel == Channel)
		{
			return ExistingChannelSetup->Name  == ChannelName && 
					ExistingChannelSetup->Channel == Channel && 
					ExistingChannelSetup->bTraceType == Spec.bTraceType;
		}
	}
	FCustomChannelSetup NewChannelSetup;
	NewChannelSetup.Channel = Channel;
	NewChannelSetup.DefaultResponse = Spec.DefaultResponse;
	NewChannelSetup.bTraceType = Spec.bTraceType;
	NewChannelSetup.bStaticObject = false;
	NewChannelSetup.Name = ChannelName;
	
	CollisionProfile->Modify();
	const int32 NewIndex = DefaultChannelResponse.AddValue();
	ChannelSetupProperty->CopyCompleteValue(DefaultChannelResponse.GetRawPtr(NewIndex), &NewChannelSetup);
	
	CollisionProfile->LoadProfileConfig(true);
	const FString SpecificFileLocation;
	constexpr bool bWarnIfFailed = false;
	return CollisionProfile->TryUpdateDefaultConfigFile(SpecificFileLocation, bWarnIfFailed);
}

FWPPortalChannelConfig::EStatus FWPPortalChannelConfig::GetStatus(const FRequiredChannel& Spec, ECollisionChannel SettingsChannel)
{
	ECollisionChannel ExistingChannel = ECC_MAX;
	bool bExistingChannelIsTraceType = false;

	if (TryFindNamedChannel(Spec, ExistingChannel, bExistingChannelIsTraceType))
	{
		if (bExistingChannelIsTraceType != Spec.bTraceType)
		{
			return EStatus::WrongType;
		}

		return ExistingChannel == SettingsChannel ? EStatus::Valid : EStatus::ChannelMismatch;
	}

	ECollisionChannel FreeChannel = ECC_MAX;
	return TryFindFreeGameChannel(FreeChannel) ? EStatus::Missing : EStatus::NoFreeChannel;
}

FWPPortalChannelConfig::EAddOrAlignResult FWPPortalChannelConfig::AddOrAlignProjectConfig(const FRequiredChannel& Spec,
	ECollisionChannel SettingsChannel, TFunctionRef<bool(ECollisionChannel)> SaveSettings)
{
	const FString DefaultEngineIni = GetProjectDefaultEngineIniFilename();

	ECollisionChannel ExistingChannel = ECC_MAX;
	bool bExistingChannelIsTraceType = false;
	if (TryFindNamedChannel(Spec, ExistingChannel, bExistingChannelIsTraceType))
	{
		if (bExistingChannelIsTraceType != Spec.bTraceType)
		{
			return EAddOrAlignResult::WrongType;
		}

		return SaveSettings(ExistingChannel) ? EAddOrAlignResult::SettingsAligned : EAddOrAlignResult::ExistingSettingsSaveFailed;
	}

	ECollisionChannel FreeChannel = ECC_MAX;
	if (!TryFindFreeGameChannel(FreeChannel))
	{
		return EAddOrAlignResult::NoFreeChannel;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*DefaultEngineIni) && PlatformFile.IsReadOnly(*DefaultEngineIni))
	{
		return EAddOrAlignResult::DefaultEngineIniReadOnly;
	}

	if (!AddChannelToCollisionProfile(Spec, FreeChannel))
	{
		return EAddOrAlignResult::AddFailed;
	}

	return SaveSettings(FreeChannel) ? EAddOrAlignResult::Added : EAddOrAlignResult::AddedSettingsSaveFailed;
}
