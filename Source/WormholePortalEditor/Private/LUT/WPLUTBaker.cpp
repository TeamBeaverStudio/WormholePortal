// Copyright 2026 Team Beaver. All Rights Reserved.

#include "LUT/WPLUTBaker.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "LUT/SWPLUTBakeWidget.h"
#include "Rendering/LUT/WPLUTAsset.h"
#include "Rendering/LUT/WPLUTCatalog.h"
#include "Rendering/LUT/WPLUTGenerator.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/VolumeTexture.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Subsystem/WPLUTCacheSubsystem.h"
#include "UObject/Package.h"
#include "WPSettings.h"
#include "WormholePortalActor.h"
#include "Widgets/SWindow.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "WPLUTBaker"

void FWPLUTBaker::FinishVolumeTextureBuild(UVolumeTexture* VolumeTexture)
{
	if (!VolumeTexture)
	{
		return;
	}
	VolumeTexture->BlockOnAnyAsyncBuild();
}

void FWPLUTBaker::ShowBakeNotification(
	const FText& Message,
	const SNotificationItem::ECompletionState State)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = 10.0f;
	Info.bFireAndForget = true;
	if (const TSharedPtr<SNotificationItem> Notification =
		FSlateNotificationManager::Get().AddNotification(Info))
	{
		Notification->SetCompletionState(State);
	}
}

template <typename TAsset>
TAsset* FWPLUTBaker::LoadOrCreateAsset(
	const FString& PackageName,
	const FString& AssetName,
	bool& bOutCreated,
	FString& OutError)
{
	bOutCreated = false;
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath))
	{
		if (TAsset* ExistingAsset = Cast<TAsset>(ExistingObject))
		{
			return ExistingAsset;
		}

		OutError = FString::Printf(
			TEXT("Generated object '%s' already exists with class '%s'."),
			*ObjectPath,
			*GetNameSafe(ExistingObject->GetClass()));
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!IsValid(Package))
	{
		OutError = FString::Printf(TEXT("Could not create package '%s'."), *PackageName);
		return nullptr;
	}

	TAsset* NewAsset = NewObject<TAsset>(
		Package,
		FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!IsValid(NewAsset))
	{
		OutError = FString::Printf(TEXT("Could not create asset '%s'."), *ObjectPath);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(NewAsset);
	NewAsset->MarkPackageDirty();
	bOutCreated = true;
	return NewAsset;
}

bool FWPLUTBaker::SavePackages(const TArray<UPackage*>& Packages, FString& OutError)
{
	if (Packages.IsEmpty())
	{
		return true;
	}

	TArray<UPackage*> FailedPackages;
	const FEditorFileUtils::EPromptReturnCode SaveResult =
		FEditorFileUtils::PromptForCheckoutAndSave(
			Packages,
			false,
			false,
			&FailedPackages,
			false,
			false);
	if (SaveResult == FEditorFileUtils::PR_Success && FailedPackages.IsEmpty())
	{
		return true;
	}

	OutError = SaveResult == FEditorFileUtils::PR_Cancelled
		? TEXT("Saving generated LUT assets was cancelled.")
		: TEXT("One or more generated LUT packages could not be checked out or saved.");
	return false;
}

bool FWPLUTBaker::ConfigureVolumeTexture(
	UVolumeTexture& VolumeTexture,
	const FWPLUTVolumeData& VolumeData,
	FString& OutError)
{
	if (!VolumeData.IsValid(&OutError))
	{
		return false;
	}

	VolumeTexture.PreEditChange(nullptr);
	VolumeTexture.SetModernSettingsForNewOrChangedTexture();
	VolumeTexture.SRGB = false;
	VolumeTexture.CompressionSettings = TC_HDR_F32;
	VolumeTexture.MipGenSettings = TMGS_NoMipmaps;
	VolumeTexture.NeverStream = true;
	VolumeTexture.VirtualTextureStreaming = false;
	VolumeTexture.Filter = TF_Trilinear;
	VolumeTexture.AddressMode = TA_Clamp;
	VolumeTexture.Source.Init(
		VolumeData.Dimensions.X,
		VolumeData.Dimensions.Y,
		VolumeData.Dimensions.Z,
		1,
		TSF_RGBA32F,
		reinterpret_cast<const uint8*>(VolumeData.Voxels.GetData()));
	VolumeTexture.PostEditChange();
	FinishVolumeTextureBuild(&VolumeTexture);
	VolumeTexture.MarkPackageDirty();

	if (VolumeTexture.GetSizeX() != VolumeData.Dimensions.X
		|| VolumeTexture.GetSizeY() != VolumeData.Dimensions.Y
		|| VolumeTexture.GetSizeZ() != VolumeData.Dimensions.Z
		|| VolumeTexture.GetPixelFormat() != PF_A32B32G32R32F)
	{
		OutError = TEXT("The generated volume texture did not compile as RGBA32F with the requested dimensions.");
		return false;
	}

	return true;
}

UWPLUTAsset* FWPLUTBaker::ValidateCandidate(
	UWPLUTAsset* Candidate,
	const FWPLUTDescriptor& Descriptor)
{
	if (!IsValid(Candidate))
	{
		return nullptr;
	}
	if (IsValid(Candidate->VolumeTexture))
	{
		FinishVolumeTextureBuild(Candidate->VolumeTexture);
	}
	FString Error;
	return Candidate->IsCompatibleWith(Descriptor, &Error) ? Candidate : nullptr;
}

void FWPLUTBaker::ResetRuntimeLUTCacheAfterBake()
{
	if (GEngine)
	{
		if (UWPLUTCacheSubsystem* Cache = GEngine->GetEngineSubsystem<UWPLUTCacheSubsystem>())
		{
			Cache->ResetCache();
		}
	}
}

void FWPLUTBaker::OpenBakeAllWidget()
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Bake All LUTs"))
		.ClientSize(FVector2D(520.0f, 360.0f))
		.SizingRule(ESizingRule::FixedSize)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	Window->SetContent(SNew(SWPLUTBakeWidget).ParentWindow(Window));
	FSlateApplication::Get().AddModalWindow(Window, nullptr);
}

void FWPLUTBaker::BakeAllInCurrentEditorWorld()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const FWPLUTDescriptor Descriptor = IsValid(Settings)
		? Settings->LUTDescriptor.GetSanitized()
		: FWPLUTDescriptor::MakeDefault().GetSanitized();

	BakeAllInCurrentEditorWorld(Descriptor, EWPLUTBakeDomainPreset::CurrentLevelAuto);
}

void FWPLUTBaker::BakeAllInCurrentEditorWorld(
	const FWPLUTDescriptor& RequestedDescriptor,
	const EWPLUTBakeDomainPreset DomainPreset)
{
	const double BakeStartSeconds = FPlatformTime::Seconds();
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!IsValid(EditorWorld))
	{
		ShowBakeNotification(
			LOCTEXT("NoEditorWorld", "LUT bake failed: no editor world is open."),
			SNotificationItem::CS_Fail);
		return;
	}

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
		ShowBakeNotification(FText::FromString(PathError), SNotificationItem::CS_Fail);
		return;
	}

	TArray<AWormholePortalActor*> PortalActors;
	for (TActorIterator<AWormholePortalActor> It(EditorWorld); It; ++It)
	{
		if (IsValid(*It) && !It->IsTemplate())
		{
			PortalActors.Add(*It);
		}
	}

	if (PortalActors.IsEmpty())
	{
		ShowBakeNotification(
			LOCTEXT("NoPortalActors", "No Wormhole Portal actors were found in the current level."),
			SNotificationItem::CS_Pending);
		return;
	}

	FScopedSlowTask SlowTask(4.0f, LOCTEXT("BakeProgress", "Baking shared LUT assets..."));
	SlowTask.MakeDialog(true);
	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("ScanPortals", "Scanning and deduplicating portal LUT requirements..."));

	FWPLUTDescriptor Descriptor = RequestedDescriptor.GetSanitized();
	TArray<float> PositiveRatios;
	PositiveRatios.Reserve(PortalActors.Num());
	FString InvalidActor;

	// Scan all portal actors for their T/rho ratios and validate their parameters.
	for (const AWormholePortalActor* Portal : PortalActors)
	{
		const float Rho = Portal->GetPortalRadius();
		const float HalfThroat = Portal->GetThroatHalfLength();
		const float Transition = Portal->GetTransitionLength();

		if (!FMath::IsFinite(Rho) || Rho <= UE_SMALL_NUMBER
			|| !FMath::IsFinite(HalfThroat) || HalfThroat < 0.0f
			|| !FMath::IsFinite(Transition) || Transition < 0.0f)
		{
			InvalidActor = Portal->GetActorNameOrLabel();
			break;
		}

		if (Transition <= UE_SMALL_NUMBER)
		{
			continue;
		}

		const float Ratio = Transition / Rho;
		if (!FMath::IsFinite(Ratio) || Ratio <= 0.0f)
		{
			InvalidActor = Portal->GetActorNameOrLabel();
			break;
		}

		PositiveRatios.Add(Ratio);
	}

	if (DomainPreset == EWPLUTBakeDomainPreset::CurrentLevelAuto && PositiveRatios.Num() > 0)
	{
		float MinRatio = PositiveRatios[0];
		float MaxRatio = PositiveRatios[0];
		for (const float Ratio : PositiveRatios)
		{
			MinRatio = FMath::Min(MinRatio, Ratio);
			MaxRatio = FMath::Max(MaxRatio, Ratio);
		}
		Descriptor.TransitionRatioMin = FMath::Max(0.01f, MinRatio / DomainPaddingFactor);
		Descriptor.TransitionRatioMax = FMath::Max(Descriptor.TransitionRatioMin * 1.01f, MaxRatio * DomainPaddingFactor);
	}

	Descriptor = Descriptor.GetSanitized();
	if (!InvalidActor.IsEmpty())
	{
		ShowBakeNotification(
			FText::Format(
				LOCTEXT("InvalidPortalMetric", "LUT bake failed: portal '{0}' has invalid rho, a, or T."),
				FText::FromString(InvalidActor)),
			SNotificationItem::CS_Fail);
		return;
	}

	for (int32 Index = 0; Index < PositiveRatios.Num(); ++Index)
	{
		if (!Descriptor.ContainsTransitionRatio(PositiveRatios[Index]))
		{
			ShowBakeNotification(
				FText::Format(
					LOCTEXT("UnsupportedRatio", "LUT bake failed: T/rho={0} is outside the supported normalized domain."),
					FText::AsNumber(PositiveRatios[Index])),
				SNotificationItem::CS_Fail);
			return;
		}
	}

	FString Error;
	if (!Descriptor.IsValid(&Error))
	{
		ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
		return;
	}
	if (SlowTask.ShouldCancel())
	{
		ShowBakeNotification(LOCTEXT("BakeCancelled", "LUT bake was cancelled."), SNotificationItem::CS_Pending);
		return;
	}

	// Determine the generated asset paths based on the descriptor.
	const FString BuildHash = Descriptor.MakeBuildHash();
	const FString TextureName = FString::Printf(TEXT("VT_WPLUT_%s"), *BuildHash);
	const FString LUTAssetName = FString::Printf(TEXT("DA_WPLUT_%s"), *BuildHash);
	const FString TexturePackageName = FString::Printf(TEXT("%s/%s"), *GeneratedRootPath, *TextureName);
	const FString LUTAssetPackageName = FString::Printf(TEXT("%s/%s"), *GeneratedRootPath, *LUTAssetName);
	const FString LUTAssetObjectPath = FString::Printf(TEXT("%s.%s"), *LUTAssetPackageName, *LUTAssetName);

	UWPLUTCatalog* ExistingCatalog = LoadObject<UWPLUTCatalog>(nullptr, *CatalogObjectPath);
	UWPLUTAsset* LUTAsset = nullptr;
	if (IsValid(ExistingCatalog))
	{
		FWPLUTDescriptor ActiveDescriptor;
		const TSoftObjectPtr<UWPLUTAsset> ActiveAsset = ExistingCatalog->GetActiveLUTAsset(&ActiveDescriptor);
		if (!ActiveAsset.IsNull() && ActiveDescriptor.GetSanitized() == Descriptor)
		{
			LUTAsset = ValidateCandidate(ActiveAsset.LoadSynchronous(), Descriptor);
		}
	}
	if (!IsValid(LUTAsset))
	{
		LUTAsset = ValidateCandidate(
			LoadObject<UWPLUTAsset>(nullptr, *LUTAssetObjectPath),
			Descriptor);
	}

	// build the volume data and create the assets.
	// If the LUT asset already exists and is valid, we can skip the build and save steps.
	const bool bReusedAsset = IsValid(LUTAsset);
	FWPLUTBuildStats BuildStats;
	if (!bReusedAsset)
	{
		SlowTask.EnterProgressFrame(1.0f, LOCTEXT("BuildVolume", "Computing the normalized volume in parallel..."));
		FWPLUTVolumeData VolumeData;
		if (!FWPLUTGenerator::BuildVolumeData(Descriptor, VolumeData, BuildStats))
		{
			const FString BuildError = BuildStats.Error.IsEmpty()
				? TEXT("The normalized LUT generator failed.")
				: BuildStats.Error;
			ShowBakeNotification(FText::FromString(BuildError), SNotificationItem::CS_Fail);
			return;
		}

		bool bTextureCreated = false;
		UVolumeTexture* VolumeTexture = LoadOrCreateAsset<UVolumeTexture>(
			TexturePackageName,
			TextureName,
			bTextureCreated,
			Error);
		bool bLUTAssetCreated = false;
		LUTAsset = LoadOrCreateAsset<UWPLUTAsset>(
			LUTAssetPackageName,
			LUTAssetName,
			bLUTAssetCreated,
			Error);

		if (!IsValid(VolumeTexture) || !IsValid(LUTAsset))
		{
			ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
			return;
		}

		if (!ConfigureVolumeTexture(*VolumeTexture, VolumeData, Error))
		{
			ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
			return;
		}

		LUTAsset->Modify();
		LUTAsset->Descriptor = Descriptor;
		LUTAsset->BuildHash = BuildHash;
		LUTAsset->VolumeTexture = VolumeTexture;
		LUTAsset->MarkPackageDirty();
		LUTAsset->PostEditChange();

		FString ValidationError;
		if (!LUTAsset->IsCompatibleWith(Descriptor, &ValidationError))
		{
			ShowBakeNotification(FText::FromString(ValidationError), SNotificationItem::CS_Fail);
			return;
		}

		SlowTask.EnterProgressFrame(1.0f, LOCTEXT("SaveLUTAssets", "Saving volume and LUT assets..."));
		TArray<UPackage*> ContentPackages;
		ContentPackages.AddUnique(VolumeTexture->GetOutermost());
		ContentPackages.AddUnique(LUTAsset->GetOutermost());
		if (!SavePackages(ContentPackages, Error))
		{
			ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
			return;
		}
	}
	else
	{
		SlowTask.EnterProgressFrame(2.0f, LOCTEXT("ReuseLUTAsset", "The matching LUT asset is current; skipping integration."));
	}

	if (SlowTask.ShouldCancel())
	{
		ShowBakeNotification(LOCTEXT("CatalogCancelled", "LUT bake was cancelled before catalog update."), SNotificationItem::CS_Pending);
		return;
	}

	// Update the LUT catalog with the new or reused LUT asset.
	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("SaveCatalog", "Updating the LUT catalog..."));
	const FString CatalogPackageName = FPackageName::ObjectPathToPackageName(CatalogObjectPath);
	const FString CatalogAssetName = FPackageName::ObjectPathToObjectName(CatalogObjectPath);
	bool bCatalogCreated = false;
	UWPLUTCatalog* Catalog = LoadOrCreateAsset<UWPLUTCatalog>(
		CatalogPackageName,
		CatalogAssetName,
		bCatalogCreated,
		Error);
	if (!IsValid(Catalog))
	{
		ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
		return;
	}

	FWPLUTDescriptor ExistingActiveDescriptor;
	const TSoftObjectPtr<UWPLUTAsset> ExistingActive = Catalog->GetActiveLUTAsset(&ExistingActiveDescriptor);
	const TSoftObjectPtr<UWPLUTAsset> DesiredAsset(LUTAsset);
	const bool bCatalogNeedsUpdate = bCatalogCreated
		|| ExistingActive.ToSoftObjectPath() != DesiredAsset.ToSoftObjectPath()
		|| ExistingActiveDescriptor.GetSanitized() != Descriptor
		|| Catalog->ActiveBuildHash != BuildHash;
	if (bCatalogNeedsUpdate)
	{
		Catalog->Modify();
		Catalog->AddOrUpdate(LUTAsset);
		Catalog->MarkPackageDirty();
		Catalog->PostEditChange();

		TArray<UPackage*> CatalogPackages;
		CatalogPackages.Add(Catalog->GetOutermost());
		if (!SavePackages(CatalogPackages, Error))
		{
			ShowBakeNotification(FText::FromString(Error), SNotificationItem::CS_Fail);
			return;
		}
	}

	// Reset the runtime LUT cache so that the new or updated LUT asset is used immediately.
	ResetRuntimeLUTCacheAfterBake();

	// Show a concise summary of the bake operation in the success notification.
	const double TotalSeconds = FPlatformTime::Seconds() - BakeStartSeconds;
	const double TextureMiB = static_cast<double>(Descriptor.GetDimensions().X)
		* Descriptor.GetDimensions().Y
		* Descriptor.GetDimensions().Z
		* sizeof(FLinearColor)
		/ (1024.0 * 1024.0);
	FNumberFormattingOptions NumberOptions;
	NumberOptions.SetMaximumFractionalDigits(2);
	const FText Summary = FText::Format(
		LOCTEXT(
			"BakeSummary",
			"LUT ready: {0} portals share 1 volume ({1}); {2}, {3} MiB, {4} s."),
		FText::AsNumber(PortalActors.Num()),
		FText::FromString(BuildHash.Left(12)),
		bReusedAsset ? LOCTEXT("Reused", "reused") : LOCTEXT("Built", "built"),
		FText::AsNumber(TextureMiB, &NumberOptions),
		FText::AsNumber(TotalSeconds, &NumberOptions));

	ShowBakeNotification(Summary, SNotificationItem::CS_Success);
}

#undef LOCTEXT_NAMESPACE

