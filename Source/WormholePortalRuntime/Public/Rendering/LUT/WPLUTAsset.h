// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "Engine/DataAsset.h"
#include "WPLUTAsset.generated.h"

class UVolumeTexture;

/** Persistent, cooked single-volume normalized LUT. */
UCLASS(BlueprintType)
class WORMHOLEPORTALRUNTIME_API UWPLUTAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LUT")
	FWPLUTDescriptor Descriptor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LUT")
	FString BuildHash;

	/** Hard reference intentionally keeps the generated texture in the same cook dependency graph. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LUT")
	TObjectPtr<UVolumeTexture> VolumeTexture = nullptr;

	bool IsCompatibleWith(const FWPLUTDescriptor& DesiredDescriptor, FString* OutError = nullptr) const;
	bool Validate(FString* OutError = nullptr) const;
};


