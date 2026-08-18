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
	ValidatePortalTraceChannel();
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
	StopPortalTraceValidationTicker();
	PortalTraceChannelNotification.Reset();
	
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

void FWormholePortalEditor::ValidatePortalTraceChannel()
{
	const FWPPortalTraceChannelConfig::EStatus Status = FWPPortalTraceChannelConfig::GetStatus();
	if (Status != FWPPortalTraceChannelConfig::EStatus::Valid)
	{
		ShowPortalTraceChannelNotification(Status);
	}
}

void FWormholePortalEditor::ShowPortalTraceChannelNotification(
	FWPPortalTraceChannelConfig::EStatus Status)
{
	FText Message = LOCTEXT(
		"MissingPortalTraceChannel",
		"Wormhole Portal requires a dedicated Trace Channel named WPPortalTrace. Portal tracing will not work until this is configured."
	);

	if (Status == FWPPortalTraceChannelConfig::EStatus::WrongType)
	{
		Message = LOCTEXT(
			"PortalTraceWrongType",
			"Wormhole Portal found a collision channel named WPPortalTrace, but it is not a Trace Channel. Portal tracing will not work until this is fixed."
		);
	}
	else if (Status == FWPPortalTraceChannelConfig::EStatus::ChannelMismatch)
	{
		Message = LOCTEXT(
			"PortalTraceChannelMismatch",
			"Wormhole Portal found WPPortalTrace, but plugin settings point to a different collision channel. Portal tracing will use the wrong channel until this is fixed."
		);
	}
	else if (Status == FWPPortalTraceChannelConfig::EStatus::NoFreeChannel)
	{
		Message = LOCTEXT(
			"PortalTraceNoFreeChannel",
			"Wormhole Portal requires a dedicated Trace Channel named WPPortalTrace, but all game trace channels appear to be in use."
		);
	}

	FNotificationInfo Info(Message);
	Info.bFireAndForget = false;
	Info.ExpireDuration = 0.f;

	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("AddPortalTraceChannelAutomatically", "Add Automatically"),
		LOCTEXT("AddPortalTraceChannelAutomaticallyTooltip", "Add WPPortalTrace to Config/DefaultEngine.ini and update Wormhole Portal settings."),
		FSimpleDelegate::CreateRaw(this, &FWormholePortalEditor::AddPortalTraceChannelToProjectConfig),
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
		PortalTraceChannelNotification = Notification;
		StartPortalTraceValidationTicker();
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

void FWormholePortalEditor::AddPortalTraceChannelToProjectConfig()
{
	using EAddOrAlignResult = FWPPortalTraceChannelConfig::EAddOrAlignResult;

	switch (FWPPortalTraceChannelConfig::AddOrAlignProjectConfig())
	{
	case EAddOrAlignResult::Added:
		DismissPortalTraceChannelNotification();
		ShowCompletionNotification(
			LOCTEXT("PortalTraceAdded", "Added WPPortalTrace to Config/DefaultEngine.ini. Restart the editor before relying on portal traces."),
			SNotificationItem::CS_Success);
		return;

	case EAddOrAlignResult::SettingsAligned:
		DismissPortalTraceChannelNotification();
		ShowCompletionNotification(
			LOCTEXT("PortalTraceSettingsAligned", "Wormhole Portal settings now use the existing WPPortalTrace channel. Restart the editor before relying on portal traces."),
			SNotificationItem::CS_Success);
		return;

	case EAddOrAlignResult::WrongType:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceWrongTypeFixFailed", "WPPortalTrace already exists, but it is not a Trace Channel. Open Collision Settings and convert or rename it manually."),
			SNotificationItem::CS_Fail);
		return;

	case EAddOrAlignResult::NoFreeChannel:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceNoFreeChannelFixFailed", "Could not add WPPortalTrace because no free game trace channel was found."),
			SNotificationItem::CS_Fail);
		return;

	case EAddOrAlignResult::DefaultEngineIniReadOnly:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceDefaultEngineIniReadOnly", "Config/DefaultEngine.ini is read-only. Check it out from source control, then add WPPortalTrace again."),
			SNotificationItem::CS_Fail);
		return;

	case EAddOrAlignResult::AddFailed:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceAddFailed", "Failed to add WPPortalTrace to Config/DefaultEngine.ini."),
			SNotificationItem::CS_Fail);
		return;

	case EAddOrAlignResult::ExistingSettingsSaveFailed:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceSettingsAlignFailed", "Found the existing WPPortalTrace channel, but failed to save Wormhole Portal settings."),
			SNotificationItem::CS_Fail);
		return;

	case EAddOrAlignResult::AddedSettingsSaveFailed:
		ShowCompletionNotification(
			LOCTEXT("PortalTraceSettingsSaveFailed", "Added WPPortalTrace, but failed to save Wormhole Portal settings. Check Config/DefaultEngine.ini before continuing."),
			SNotificationItem::CS_Fail);
		return;
	}
}

void FWormholePortalEditor::OpenCollisionProjectSettings() const
{
	if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->ShowViewer("Project", "Engine", "Collision");
	}
}

void FWormholePortalEditor::StartPortalTraceValidationTicker()
{
	if (PortalTraceValidationTickerHandle.IsValid())
	{
		return;	
	}
	
	constexpr float ValidationIntervalSeconds = 1.0f;
	PortalTraceValidationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWormholePortalEditor::TickPortalTraceValidation),
		ValidationIntervalSeconds);
}

void FWormholePortalEditor::StopPortalTraceValidationTicker()
{
	if (!PortalTraceValidationTickerHandle.IsValid())
	{
		return;
	}
	
	FTSTicker::RemoveTicker(PortalTraceValidationTickerHandle);
	PortalTraceValidationTickerHandle.Reset();
}

bool FWormholePortalEditor::TickPortalTraceValidation(float DeltaTime)
{
	if (!PortalTraceChannelNotification.IsValid())
	{
		PortalTraceValidationTickerHandle.Reset();
		return false;
	}
	
	if (FWPPortalTraceChannelConfig::GetStatus() == FWPPortalTraceChannelConfig::EStatus::Valid)
	{
		DismissPortalTraceChannelNotification();
		return false;
	}
	
	return true;
}

void FWormholePortalEditor::DismissPortalTraceChannelNotification()
{
	if (TSharedPtr<SNotificationItem> Notification = PortalTraceChannelNotification.Pin())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Success);
		Notification->ExpireAndFadeout();
	}
	
	PortalTraceChannelNotification.Reset();
	StopPortalTraceValidationTicker();
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

