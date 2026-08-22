// Copyright 2026 Team Beaver Studio. All Rights Reserved.

#include "WormholePortalEditor.h"

#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "ISettingsModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Settings/ProjectPackagingSettings.h"
#include "SSettingsEditorCheckoutNotice.h"
#include "Subsystem/WPLUTCacheSubsystem.h"
#include "TransitManager/SWPTransitManagerWidget.h"
#include "UObject/UObjectBase.h"
#include "UObject/UnrealType.h"
#include "WPLog.h"
#include "WPSettings.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ToolMenus.h"
#include "UnrealEdGlobals.h"
#include "Debug/WPPortalDebugComponent.h"
#include "Debug/WPPortalDebugComponentVisualizer.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "PropertyEditorModule.h"
#include "Transit/WPTransitComponent.h"
#include "WPTransitComponentDetails.h"
#include "WPSettingsDetails.h"
#include "WormholePortalActor.h"
#include "WormholePortalActorDetails.h"
#include "LUT/WPLUTBaker.h"

#define LOCTEXT_NAMESPACE "FWormholePortalEditorModule"

namespace
{
	constexpr TCHAR DefaultGeneratedLUTCookPath[] = TEXT("/WormholePortal/WormholePortal/Generated/LUT");

	FString NormalizePackageDirectory(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}
		return Path;
	}
	
	// **** Channel Helper Functions Start****
	FText DescribeChannelStatus(const TCHAR* ChannelName, FWPPortalChannelConfig::EStatus Status)
	{
		using EStatus = FWPPortalChannelConfig::EStatus;
		switch (Status)
		{
		case EStatus::Valid:			return FText::Format(LOCTEXT("ChannelValid", "{0}: Valid"), FText::FromString(ChannelName));
		case EStatus::Missing:			return FText::Format(LOCTEXT("ChannelMissing", "{0}: Missing"), FText::FromString(ChannelName));
		case EStatus::WrongType:		return FText::Format(LOCTEXT("ChannelWrongType", "{0}: Wrong Channel Type"), FText::FromString(ChannelName));
		case EStatus::ChannelMismatch:	return FText::Format(LOCTEXT("ChannelMismatch", "{0}: Settings Point To A Different Channel"), FText::FromString(ChannelName));
		case EStatus::NoFreeChannel:	return FText::Format(LOCTEXT("ChannelNoFreeChannel", "{0}: No Free Game Channel"), FText::FromString(ChannelName));
		default:						return FText::GetEmpty();
		}
	}
	
	bool AppendAddOrAlignResultMessage(const TCHAR* ChannelName, FWPPortalChannelConfig::EAddOrAlignResult Result, TArray<FText>& OutMessages)
	{
		using EAddOrAlignResult = FWPPortalChannelConfig::EAddOrAlignResult;
		switch (Result)
		{
		case EAddOrAlignResult::Added:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelAdded", "Added {0} to Config/DefaultEngine.ini. Restart the editor before relying on it."),
				FText::FromString(ChannelName)));
			return false;

		case EAddOrAlignResult::SettingsAligned:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelSettingsAligned", "Wormhole Portal settings now use the existing {0} channel. Restart the editor before relying on it."),
				FText::FromString(ChannelName)));
			return false;

		case EAddOrAlignResult::WrongType:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelWrongTypeFixFailed", "{0} already exists, but it is not the expected channel type. Open Collision Settings and convert or rename it manually."),
				FText::FromString(ChannelName)));
			return true;

		case EAddOrAlignResult::NoFreeChannel:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelNoFreeChannelFixFailed", "Could not add {0} because no free game channel was found."),
				FText::FromString(ChannelName)));
			return true;

		case EAddOrAlignResult::DefaultEngineIniReadOnly:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelDefaultEngineIniReadOnly", "Config/DefaultEngine.ini is read-only. Check it out from source control, then add {0} again."),
				FText::FromString(ChannelName)));
			return true;

		case EAddOrAlignResult::AddFailed:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelAddFailed", "Failed to add {0} to Config/DefaultEngine.ini."),
				FText::FromString(ChannelName)));
			return true;

		case EAddOrAlignResult::ExistingSettingsSaveFailed:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelSettingsAlignFailed", "Found the existing {0} channel, but failed to save Wormhole Portal settings."),
				FText::FromString(ChannelName)));
			return true;

		case EAddOrAlignResult::AddedSettingsSaveFailed:
			OutMessages.Add(FText::Format(
				LOCTEXT("ChannelSettingsSaveFailed", "Added {0}, but failed to save Wormhole Portal settings. Check Config/DefaultEngine.ini before continuing."),
				FText::FromString(ChannelName)));
			return true;

		default:
			return true;
		}
	}
	// **** Channel Helper Functions End ****

	bool IsSamePackageDirectory(const FString& Left, const FString& Right)
	{
		return NormalizePackageDirectory(Left).Equals(
			NormalizePackageDirectory(Right),
			ESearchCase::IgnoreCase);
	}
}

const FName FWormholePortalEditor::TransitManagerTabName(TEXT("WormholePortalTransitManager"));

void FWormholePortalEditor::StartupModule()
{
	RegisterLUTSettingsSync();
	ValidatePortalChannels();
	RegisterTransitDetails();
	RegisterPortalDebugVisualizer();
	
	// Editor에 Transit Manager Tab 생성 방법 등록 
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TransitManagerTabName,
		FOnSpawnTab::CreateRaw(this, &FWormholePortalEditor::SpawnTransitManagerTab))
		.SetDisplayName(LOCTEXT("TransitManagerTabTitle", "Transit Manager"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
	
	// ToolsMenu가 준비되면 이 함수를 호출하라고 예약
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWormholePortalEditor::RegisterTransitManagerMenus));
}

void FWormholePortalEditor::ShutdownModule()
{
	UnregisterLUTSettingsSync();
	UnregisterTransitDetails();
	UnregisterPortalDebugVisualizer();
	StopPortalChannelValidationTicker();
	PortalChannelNotification.Reset();
	
	// Transit Manager 관련 콜백 및 등록 해제
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TransitManagerTabName);
}

void FWormholePortalEditor::RegisterLUTSettingsSync()
{
	if (!GIsEditor || IsRunningCommandlet() || LUTSettingsChangedHandle.IsValid())
	{
		return;
	}

	if (UWPSettings* Settings = GetMutableDefault<UWPSettings>())
	{
		LUTSettingsChangedHandle = Settings->OnSettingChanged().AddRaw(
			this,
			&FWormholePortalEditor::HandleLUTSettingsChanged);
		SyncLUTCookPath(false);
	}
}

void FWormholePortalEditor::UnregisterLUTSettingsSync()
{
	if (LUTSettingsChangedHandle.IsValid())
	{
		if (UObjectInitialized())
		{
			if (UWPSettings* Settings = GetMutableDefault<UWPSettings>())
			{
				Settings->OnSettingChanged().Remove(LUTSettingsChangedHandle);
			}
		}
		LUTSettingsChangedHandle.Reset();
	}

	LastSynchronizedLUTCookPath.Reset();
}

void FWormholePortalEditor::HandleLUTSettingsChanged(
	UObject* SettingsObject,
	FPropertyChangedEvent& PropertyChangedEvent)
{
	const FProperty* ChangedProperty = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty
		: PropertyChangedEvent.Property;
	if (!SettingsObject || !SettingsObject->IsA<UWPSettings>() || !ChangedProperty
		|| ChangedProperty->GetFName() != GET_MEMBER_NAME_CHECKED(UWPSettings, GeneratedLUTAssetPath)
		|| (PropertyChangedEvent.ChangeType & EPropertyChangeType::Interactive) != 0)
	{
		return;
	}

	ResetLUTCacheForPathChange();
	SyncLUTCookPath(true);
}

bool FWormholePortalEditor::SyncLUTCookPath(const bool bNotifyFailure)
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	FString GeneratedRootPath;
	FString CatalogObjectPath;
	FString PathError;
	if (!IsValid(Settings)
		|| !Settings->TryResolveGeneratedLUTPaths(GeneratedRootPath, CatalogObjectPath, PathError))
	{
		if (PathError.IsEmpty())
		{
			PathError = TEXT("Wormhole Portal settings are unavailable.");
		}
		WP_LOG(nullptr, Warning, TEXT("LUT cook path was not synchronized: %s"), *PathError);
		if (bNotifyFailure)
		{
			ShowCompletionNotification(FText::FromString(PathError), SNotificationItem::CS_Fail);
		}
		return false;
	}

	UProjectPackagingSettings* PackagingSettings = GetMutableDefault<UProjectPackagingSettings>();
	if (!IsValid(PackagingSettings))
	{
		const FText Error = LOCTEXT("MissingPackagingSettings", "Project Packaging Settings are unavailable. The LUT cook path was not updated.");
		WP_LOG(nullptr, Warning, TEXT("%s"), *Error.ToString());
		if (bNotifyFailure)
		{
			ShowCompletionNotification(Error, SNotificationItem::CS_Fail);
		}
		return false;
	}

	TArray<FDirectoryPath> UpdatedDirectories = PackagingSettings->DirectoriesToAlwaysCook;
	bool bDirectoriesChanged = false;
	const bool bSameAsLastPath = !LastSynchronizedLUTCookPath.IsEmpty()
		&& IsSamePackageDirectory(LastSynchronizedLUTCookPath, GeneratedRootPath);

	if (!LastSynchronizedLUTCookPath.IsEmpty() && !bSameAsLastPath)
	{
		bDirectoriesChanged |= UpdatedDirectories.RemoveAll([this](const FDirectoryPath& Directory)
		{
			return IsSamePackageDirectory(Directory.Path, LastSynchronizedLUTCookPath);
		}) > 0;
	}
	else if (LastSynchronizedLUTCookPath.IsEmpty()
		&& !IsSamePackageDirectory(GeneratedRootPath, DefaultGeneratedLUTCookPath))
	{
		bDirectoriesChanged |= UpdatedDirectories.RemoveAll([](const FDirectoryPath& Directory)
		{
			return IsSamePackageDirectory(Directory.Path, DefaultGeneratedLUTCookPath);
		}) > 0;
	}

	const bool bPathAlreadyPresent = UpdatedDirectories.ContainsByPredicate(
		[&GeneratedRootPath](const FDirectoryPath& Directory)
		{
			return IsSamePackageDirectory(Directory.Path, GeneratedRootPath);
		});
	if (!bPathAlreadyPresent)
	{
		FDirectoryPath& NewDirectory = UpdatedDirectories.AddDefaulted_GetRef();
		NewDirectory.Path = GeneratedRootPath;
		bDirectoriesChanged = true;
	}

	LastSynchronizedLUTCookPath = GeneratedRootPath;

	if (!bDirectoriesChanged)
	{
		return true;
	}

	PackagingSettings->Modify();
	PackagingSettings->DirectoriesToAlwaysCook = MoveTemp(UpdatedDirectories);
	FConfigCacheIni::ClearOtherPlatformConfigs();

	const FString ConfigFilename = FPaths::ConvertRelativePathToFull(
		PackagingSettings->GetDefaultConfigFilename());
	FText WriteError;
	bool bConfigReady = true;
	if (!SettingsHelpers::IsSourceControlled(ConfigFilename, true)
		|| !SettingsHelpers::CheckOutOrAddFile(ConfigFilename, true, false, &WriteError))
	{
		bConfigReady = SettingsHelpers::MakeWritable(ConfigFilename, false, &WriteError);
	}

	const bool bConfigSaved = bConfigReady
		&& PackagingSettings->TryUpdateDefaultConfigFile(ConfigFilename, true);
	if (!bConfigSaved)
	{
		const FText Error = WriteError.IsEmpty()
			? FText::Format(
				LOCTEXT("LUTCookPathSaveFailed", "LUT assets will use '{0}' in this editor session, but Config/DefaultGame.ini could not be updated. Check it out before packaging."),
				FText::FromString(GeneratedRootPath))
			: WriteError;
		WP_LOG(nullptr, Warning, TEXT("%s"), *Error.ToString());
		if (bNotifyFailure)
		{
			ShowCompletionNotification(Error, SNotificationItem::CS_Fail);
		}
		return false;
	}

	return true;
}

void FWormholePortalEditor::ResetLUTCacheForPathChange() const
{
	if (GEngine)
	{
		if (UWPLUTCacheSubsystem* CacheSubsystem = GEngine->GetEngineSubsystem<UWPLUTCacheSubsystem>())
		{
			CacheSubsystem->ResetCache();
		}
	}
}

void FWormholePortalEditor::RegisterPortalDebugVisualizer()
{
	if (!GUnrealEd)
	{
		return;
	}

	const TSharedPtr<FComponentVisualizer> Visualizer = MakeShared<FWPPortalDebugComponentVisualizer>();
	GUnrealEd->RegisterComponentVisualizer(UWPPortalDebugComponent::StaticClass()->GetFName(), Visualizer);
	Visualizer->OnRegister();
}

void FWormholePortalEditor::UnregisterPortalDebugVisualizer()
{
	if (GUnrealEd)
	{
		GUnrealEd->UnregisterComponentVisualizer(UWPPortalDebugComponent::StaticClass()->GetFName());
	}
}

void FWormholePortalEditor::ValidatePortalChannels()
{
	const FWPPortalChannelConfig::EStatus TraceStatus = FWPPortalChannelConfig::GetTraceChannelStatus();
	const FWPPortalChannelConfig::EStatus CollisionStatus = FWPPortalChannelConfig::GetCollisionChannelStatus();
	
	if (TraceStatus != FWPPortalChannelConfig::EStatus::Valid || CollisionStatus != FWPPortalChannelConfig::EStatus::Valid)
	{
		ShowPortalChannelNotification(TraceStatus, CollisionStatus);
	}
}

void FWormholePortalEditor::ShowPortalChannelNotification(FWPPortalChannelConfig::EStatus TraceStatus, FWPPortalChannelConfig::EStatus CollisionStatus)
{
	const FText Message = FText::Format(
		LOCTEXT("PortalChannelSetupRequired", "Wormhole Portal Collision Setup Required\n\n{0}\n{1}"),
		DescribeChannelStatus(TEXT("WPPortalTrace"), TraceStatus),
		DescribeChannelStatus(TEXT("WPPortalCollision"), CollisionStatus));

	FNotificationInfo Info(Message);
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.f;

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("AddPortalChannelsAutomatically", "Add/Repair Automatically"),
		LOCTEXT("AddPortalChannelsAutomaticallyTooltip", "Add missing Wormhole Portal channels to Config/DefaultEngine.ini and update Wormhole Portal settings."),
		FSimpleDelegate::CreateRaw(this, &FWormholePortalEditor::AddPortalChannelsToProjectConfig),
		SNotificationItem::CS_None
	));

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("OpenCollisionSettings", "Open Collision Settings"),
		LOCTEXT("OpenCollisionSettingsTooltip", "Open Project Settings > Engine > Collision."),
		FSimpleDelegate::CreateRaw(this, &FWormholePortalEditor::OpenCollisionProjectSettings),
		SNotificationItem::CS_None
	));

	if (const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
	{
		PortalChannelNotification = Notification;
		StartPortalChannelValidationTicker();
	}
}

void FWormholePortalEditor::ShowCompletionNotification(
	const FText& Message,
	SNotificationItem::ECompletionState CompletionState) const
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 8.f;

	if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Notification->SetCompletionState(CompletionState);
	}
}

void FWormholePortalEditor::AddPortalChannelsToProjectConfig()
{
	using EStatus = FWPPortalChannelConfig::EStatus;
	
	TArray<FText> ResultMessage;
	bool bAnyFailed = false;
	if (FWPPortalChannelConfig::GetTraceChannelStatus() != EStatus::Valid)
	{
		bAnyFailed |= AppendAddOrAlignResultMessage(TEXT("WPPortalTrace"),  FWPPortalChannelConfig::AddOrAlignTraceChannel(), ResultMessage);
	}
	
	if (FWPPortalChannelConfig::GetCollisionChannelStatus() != EStatus::Valid)
	{
		bAnyFailed |= AppendAddOrAlignResultMessage(TEXT("WPPortalCollision"),  FWPPortalChannelConfig::AddOrAlignCollisionChannel(), ResultMessage);
	}
	
	if (ResultMessage.IsEmpty()) return;

	ShowCompletionNotification(FText::Join(FText::FromString(TEXT("\n")), ResultMessage), bAnyFailed ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	
	if (FWPPortalChannelConfig::GetTraceChannelStatus() == EStatus::Valid && FWPPortalChannelConfig::GetCollisionChannelStatus() == EStatus::Valid)
	{
		DismissPortalChannelNotification();
	}
}

void FWormholePortalEditor::OpenCollisionProjectSettings() const
{
	if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->ShowViewer("Project", "Engine", "Collision");
	}
}

void FWormholePortalEditor::StartPortalChannelValidationTicker()
{
	if (PortalChannelValidationTickerHandle.IsValid())
	{
		return;	
	}
	
	constexpr float ValidationIntervalSeconds = 1.0f;
	PortalChannelValidationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWormholePortalEditor::TickPortalChannelValidation),
		ValidationIntervalSeconds);
}

void FWormholePortalEditor::StopPortalChannelValidationTicker()
{
	if (!PortalChannelValidationTickerHandle.IsValid())
	{
		return;
	}
	
	FTSTicker::RemoveTicker(PortalChannelValidationTickerHandle);
	PortalChannelValidationTickerHandle.Reset();
}

bool FWormholePortalEditor::TickPortalChannelValidation(float DeltaTime)
{
	if (!PortalChannelNotification.IsValid())
	{
		PortalChannelValidationTickerHandle.Reset();
		return false;
	}
	
	if (FWPPortalChannelConfig::GetTraceChannelStatus() == FWPPortalChannelConfig::EStatus::Valid && 
		FWPPortalChannelConfig::GetCollisionChannelStatus() == FWPPortalChannelConfig::EStatus::Valid)
	{
		DismissPortalChannelNotification();
		return false;
	}
	
	return true;
}

void FWormholePortalEditor::DismissPortalChannelNotification()
{
	if (TSharedPtr<SNotificationItem> Notification = PortalChannelNotification.Pin())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Success);
		Notification->ExpireAndFadeout();
	}
	
	PortalChannelNotification.Reset();
	StopPortalChannelValidationTicker();
}

void FWormholePortalEditor::RegisterTransitManagerMenus()
{
	// Level Editor의 Tools Menu에 Transit Manager 등록
	FToolMenuOwnerScoped OwnerScoped(this);
	
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("WormholePortal");
	Section.Label = LOCTEXT("WormholePortalMenuSection", "Wormhole Portal");
	Section.AddMenuEntry(
		"OpenWormholePortalTransitManager",
		LOCTEXT("OpenTransitManager", "Transit Manager"),
		LOCTEXT("OpenTransitManagerTooltip", "Open the Wormhole Portal Transit Manager."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(FWormholePortalEditor::TransitManagerTabName);
		})));

	Section.AddMenuEntry(
		"BakeAllWormholePortalLUTs",
		LOCTEXT("BakeAllLUTs", "Bake All LUTs"),
		LOCTEXT("BakeAllLUTsTooltip", "Pre-bake the shared normalized volume LUT used by every wormhole portal in the current level."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FWPLUTBaker::OpenBakeAllWidget)));
}

void FWormholePortalEditor::RegisterTransitDetails()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	
	PropertyEditor.RegisterCustomClassLayout(UWPTransitComponent::StaticClass()->GetFName()
		,FOnGetDetailCustomizationInstance::CreateStatic(&FWPTransitComponentDetails::MakeInstance));
	PropertyEditor.RegisterCustomClassLayout(AWormholePortalActor::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FWormholePortalActorDetails::MakeInstance));
	PropertyEditor.RegisterCustomClassLayout(UWPSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FWPSettingsDetails::MakeInstance));
	PropertyEditor.RegisterCustomPropertyTypeLayout(
		FWPCaptureResolutionTier::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FWPCaptureResolutionTierCustomization::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FWormholePortalEditor::UnregisterTransitDetails()
{
	if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyEditor->UnregisterCustomClassLayout(UWPTransitComponent::StaticClass()->GetFName());
		PropertyEditor->UnregisterCustomClassLayout(AWormholePortalActor::StaticClass()->GetFName());
		PropertyEditor->UnregisterCustomClassLayout(UWPSettings::StaticClass()->GetFName());
		PropertyEditor->UnregisterCustomPropertyTypeLayout(
			FWPCaptureResolutionTier::StaticStruct()->GetFName());
		PropertyEditor->NotifyCustomizationModuleChanged();
	}
}

TSharedRef<SDockTab> FWormholePortalEditor::SpawnTransitManagerTab(const FSpawnTabArgs& Args)
{
	(void)Args;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SWPTransitManagerWidget)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWormholePortalEditor, WormholePortalEditor)

