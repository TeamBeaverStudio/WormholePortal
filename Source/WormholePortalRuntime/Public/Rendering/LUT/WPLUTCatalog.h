// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPtr.h"
#include "WPLUTCatalog.generated.h"

class UWPLUTAsset;

/** Persistent selector for the active baked LUT used by runtime rendering. */
UCLASS(BlueprintType)
class WORMHOLEPORTALRUNTIME_API UWPLUTCatalog : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** The single baked LUT selected for PIE/runtime. Users may swap this in the Content Browser. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LUT")
	TSoftObjectPtr<UWPLUTAsset> ActiveLUTAsset;

	/** Read-only metadata mirrored from ActiveLUTAsset. Do not edit descriptor values here. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LUT")
	FWPLUTDescriptor ActiveDescriptor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LUT")
	FString ActiveBuildHash;

	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	TSoftObjectPtr<UWPLUTAsset> GetActiveLUTAsset(FWPLUTDescriptor* OutResolvedDescriptor = nullptr) const;

	/** Editor baker helper. Sets the current active LUT and mirrors descriptor/hash metadata. */
	void SetActiveLUTAsset(UWPLUTAsset* Asset);

	/** Editor baker helper. Selects the given LUT as the active runtime asset. */
	void AddOrUpdate(UWPLUTAsset* Asset);

private:
	void RefreshActiveMetadataFromLoadedAsset(UWPLUTAsset* Asset);
};
