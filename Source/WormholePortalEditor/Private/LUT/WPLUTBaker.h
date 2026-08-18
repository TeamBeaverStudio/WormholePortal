// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "Widgets/Notifications/SNotificationList.h"

class UPackage;
class UVolumeTexture;
class UWPLUTAsset;
class UWPLUTCatalog;

UENUM()
enum class EWPLUTBakeDomainPreset : uint8
{
	CurrentLevelAuto,
	Standard,
	Wide,
	Narrow
};

/** Editor-only entry point for building the shared normalized volume LUT assets. */
class FWPLUTBaker
{
public:
	/** Opens the editor modal used to choose quality/domain presets before baking. */
	static void OpenBakeAllWidget();

	/** Scans the current editor world and incrementally bakes every required LUT descriptor. */
	static void BakeAllInCurrentEditorWorld();

	/** Scans the current editor world and bakes the requested descriptor. */
	static void BakeAllInCurrentEditorWorld(
		const FWPLUTDescriptor& RequestedDescriptor,
		EWPLUTBakeDomainPreset DomainPreset);

private:
	static constexpr float DomainPaddingFactor = 1.05f;

	static void FinishVolumeTextureBuild(UVolumeTexture* VolumeTexture);
	static void ShowBakeNotification(
		const FText& Message,
		SNotificationItem::ECompletionState State);

	template <typename TAsset>
	static TAsset* LoadOrCreateAsset(
		const FString& PackageName,
		const FString& AssetName,
		bool& bOutCreated,
		FString& OutError);

	static bool SavePackages(const TArray<UPackage*>& Packages, FString& OutError);
	static bool ConfigureVolumeTexture(
		UVolumeTexture& VolumeTexture,
		const FWPLUTVolumeData& VolumeData,
		FString& OutError);
	static UWPLUTAsset* ValidateCandidate(
		UWPLUTAsset* Candidate,
		const FWPLUTDescriptor& Descriptor);
	static void ResetRuntimeLUTCacheAfterBake();
};

