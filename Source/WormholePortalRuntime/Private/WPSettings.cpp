// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPSettings.h"

#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "WPLog.h"

namespace
{
	constexpr int32 WPCaptureResolutionTierConfigVersion = 1;
	constexpr int32 WPMinimumEffectiveCaptureResolution = 8;
	constexpr int32 WPMaximumCaptureResolution = 2048;
	constexpr int32 WPCaptureResolutionAlignment = 8;

	bool TryResolveGeneratedProjectAssetPath(
		const FString& ConfiguredPath,
		const TCHAR* SettingDisplayName,
		FString& OutRootPath,
		FString& OutError)
	{
		OutRootPath = ConfiguredPath;
		OutRootPath.TrimStartAndEndInline();
		OutRootPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (OutRootPath.Len() > 1 && OutRootPath.EndsWith(TEXT("/")))
		{
			OutRootPath.LeftChopInline(1);
		}

		FText PathError;
		const bool bIsValidLongPackageName = FPackageName::IsValidLongPackageName(
			OutRootPath,
			false,
			&PathError);
		if (!OutRootPath.StartsWith(TEXT("/WormholePortal/"), ESearchCase::CaseSensitive)
			|| !bIsValidLongPackageName)
		{
			OutError = FString::Printf(
				TEXT("%s '%s' is invalid. Generated assets must use a /WormholePortal/... package path so they can be written to the project."),
				SettingDisplayName,
				*OutRootPath);
			if (!PathError.IsEmpty())
			{
				OutError += FString::Printf(TEXT(" %s"), *PathError.ToString());
			}
			return false;
		}

		OutError.Reset();
		return true;
	}
}

int32 UWPSettings::NormalizeCaptureResolution(
	const int32 RequestedResolution,
	const int32 FallbackResolution)
{
	const int32 SafeFallback = FallbackResolution > 0
		? FallbackResolution
		: WPMinimumEffectiveCaptureResolution;
	const int32 PositiveResolution = RequestedResolution > 0
		? RequestedResolution
		: SafeFallback;
	const int32 ClampedResolution = FMath::Clamp(
		PositiveResolution,
		1,
		WPMaximumCaptureResolution);
	const int32 AlignedResolution =
		((ClampedResolution + WPCaptureResolutionAlignment - 1)
			/ WPCaptureResolutionAlignment)
		* WPCaptureResolutionAlignment;
	return FMath::Clamp(
		AlignedResolution,
		WPMinimumEffectiveCaptureResolution,
		WPMaximumCaptureResolution);
}

void UWPSettings::PostInitProperties()
{
	Super::PostInitProperties();
	EnsureCaptureResolutionTierConfigMigrated();
	NormalizeStoredCaptureResolutions();
}

void UWPSettings::PostReloadConfig(FProperty* PropertyThatWasLoaded)
{
	Super::PostReloadConfig(PropertyThatWasLoaded);
	EnsureCaptureResolutionTierConfigMigrated();
	NormalizeStoredCaptureResolutions();
}

void UWPSettings::EnsureCaptureResolutionTierConfigMigrated()
{
	if (CaptureResolutionTierConfigVersion >= WPCaptureResolutionTierConfigVersion)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	CaptureResolutionTiers.Reset(3);
	const auto AddLegacyTier = [this](const float StartPercent, const int32 Resolution)
	{
		FWPCaptureResolutionTier& Tier = CaptureResolutionTiers.AddDefaulted_GetRef();
		Tier.StartScreenHeightPercent = StartPercent;
		Tier.Resolution = Resolution;
	};
	AddLegacyTier(CaptureTier1ScreenHeightPercent, CaptureTier1Resolution);
	AddLegacyTier(CaptureTier2ScreenHeightPercent, CaptureTier2Resolution);
	AddLegacyTier(CaptureTier3ScreenHeightPercent, CaptureTier3Resolution);
	CaptureResolutionTierConfigVersion = WPCaptureResolutionTierConfigVersion;

	WP_LOG(this, Log,
		TEXT("[Settings][CaptureResolutionTierMigration] Migrated legacy fixed tiers to array. ConfigVersion=%d TierCount=%d Tier1=%.3f%%/%d Tier2=%.3f%%/%d Tier3=%.3f%%/%d SavedAutomatically=0 CpuMs=%.4f"),
		CaptureResolutionTierConfigVersion,
		CaptureResolutionTiers.Num(),
		CaptureResolutionTiers[0].StartScreenHeightPercent,
		CaptureResolutionTiers[0].Resolution,
		CaptureResolutionTiers[1].StartScreenHeightPercent,
		CaptureResolutionTiers[1].Resolution,
		CaptureResolutionTiers[2].StartScreenHeightPercent,
		CaptureResolutionTiers[2].Resolution,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

int32 UWPSettings::NormalizeStoredCaptureResolutions()
{
	int32 AdjustedValueCount = 0;
	const auto NormalizeValue = [&AdjustedValueCount](
		int32& Value,
		const int32 FallbackResolution)
	{
		const int32 NormalizedValue = UWPSettings::NormalizeCaptureResolution(
			Value,
			FallbackResolution);
		if (Value != NormalizedValue)
		{
			Value = NormalizedValue;
			++AdjustedValueCount;
		}
	};

	NormalizeValue(CaptureLowestVisibleResolution, 64);
	for (FWPCaptureResolutionTier& Tier : CaptureResolutionTiers)
	{
		NormalizeValue(Tier.Resolution, 512);
	}
	NormalizeValue(CaptureInsideSafeProxyResolution, 768);
	return AdjustedValueCount;
}

#if WITH_EDITOR
void UWPSettings::PostEditChangeChainProperty(
	FPropertyChangedChainEvent& PropertyChangedEvent)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 AdjustedValueCount = NormalizeStoredCaptureResolutions();
	Super::PostEditChangeChainProperty(PropertyChangedEvent);

	if (AdjustedValueCount > 0)
	{
		WP_LOG(this, Log,
			TEXT("[Settings][CaptureResolutionNormalization] Edited values rounded up to the next 8-pixel multiple. ChangedProperty=%s AdjustedValueCount=%d TierCount=%d MinimumEffectiveResolution=%d Alignment=%d MaximumResolution=%d CpuMs=%.4f"),
			PropertyChangedEvent.Property
				? *PropertyChangedEvent.Property->GetName()
				: TEXT("None"),
			AdjustedValueCount,
			CaptureResolutionTiers.Num(),
			WPMinimumEffectiveCaptureResolution,
			WPCaptureResolutionAlignment,
			WPMaximumCaptureResolution,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
}
#endif

bool UWPSettings::TryResolveGeneratedVoxelPath(
	FString& OutRootPath,
	FString& OutError) const
{
	return TryResolveGeneratedProjectAssetPath(
		GeneratedVoxelAssetPath,
		TEXT("Generated Voxel Asset Path"),
		OutRootPath,
		OutError);
}

bool UWPSettings::TryResolveGeneratedLUTPaths(
	FString& OutRootPath,
	FString& OutCatalogObjectPath,
	FString& OutError) const
{
	if (!TryResolveGeneratedProjectAssetPath(
		GeneratedLUTAssetPath,
		TEXT("Generated LUT Asset Path"),
		OutRootPath,
		OutError))
	{
		OutCatalogObjectPath.Reset();
		return false;
	}

	const FString CatalogPackageName = FString::Printf(
		TEXT("%s/%s"),
		*OutRootPath,
		WPLUT::CatalogAssetName);
	OutCatalogObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*CatalogPackageName,
		WPLUT::CatalogAssetName);
	OutError.Reset();
	return true;
}
