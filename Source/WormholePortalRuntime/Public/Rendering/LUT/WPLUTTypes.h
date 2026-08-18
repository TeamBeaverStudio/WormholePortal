// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WPLUTTypes.generated.h"

class UVolumeTexture;

namespace WPLUT
{
	inline constexpr double LensingWidthToMassRatio = 1.42953;
	inline constexpr TCHAR CatalogAssetName[] = TEXT("DA_WPLUTCatalog");
}

UENUM(BlueprintType)
enum class EWPLUTSource : uint8
{
	None,
	BakedAsset,
	RuntimeFallback
};

/**
 * Scale-independent single-volume LUT contract.
 *
 * Logical axes (before half-texel remapping):
 *   U = branch-split raised-cosine impact coordinate
 *   V = transition-local coordinate (ell - a) / T
 *   W = logarithmic transition ratio T / rho
 *
 * The finite tail is always the current C2 smooth flat-space join, and the
 * stored texture format is always RGBA32F. Absolute rho and a do not belong in
 * the build key.
 */
USTRUCT(BlueprintType)
struct WORMHOLEPORTALRUNTIME_API FWPLUTDescriptor
{
	GENERATED_BODY()

	/** Number of samples along the impact-parameter axis. Must remain even so no inward texel lies on the U=0.5 separatrix. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resolution", meta = (ClampMin = "16", ClampMax = "2048"))
	int32 ImpactSamples = 512;

	/** Number of samples along the transition-local V axis, from the mouth boundary to the finite flat-space join. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resolution", meta = (ClampMin = "2", ClampMax = "512"))
	int32 TransitionSamples = 48;

	/** Number of logarithmic T/rho slices stored along the 3D LUT W axis. Higher values improve interpolation across differently sized portals. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resolution", meta = (ClampMin = "2", ClampMax = "128"))
	int32 RatioSamples = 24;

	/** Numerical quadrature steps used while baking each ray sample. Higher values reduce integration error but increase bake time. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Integration", meta = (ClampMin = "16", ClampMax = "2048"))
	int32 IntegrationSteps = 192;

	/** Minimum transition-length-to-throat-radius ratio covered by the LUT domain. Runtime portals below this ratio require another LUT or fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Domain", meta = (ClampMin = "0.01"))
	float TransitionRatioMin = 0.5f;

	/** Maximum transition-length-to-throat-radius ratio covered by the LUT domain. Runtime portals above this ratio require another LUT or fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Domain", meta = (ClampMin = "0.01"))
	float TransitionRatioMax = 8.0f;

	/** In the fixed C2 finite-tail profile, the strict curve is retained up to this transition fraction before smoothly joining flat space. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Metric", meta = (ClampMin = "0.1", ClampMax = "0.95"))
	float TailFlattenStartFraction = 0.5f;

	static FWPLUTDescriptor MakeDefault();

	/** Returns a deterministic, bounded descriptor suitable for hashing/building. */
	FWPLUTDescriptor GetSanitized() const;
	bool IsValid(FString* OutError = nullptr) const;

	bool ContainsTransitionRatio(float TransitionRatio, float RelativeTolerance = 1.0e-5f) const;
	float TransitionRatioFromCoordinate01(float Coordinate01) const;
	float ComputeRatioCoordinate01(float TransitionRatio) const;
	float RatioFromSlice(int32 SliceIndex) const;
	float ClampTransitionRatio(float TransitionRatio) const;
	FIntVector GetDimensions() const;

	/** Compares all build settings except ratio-domain endpoints. */
	bool IsSamplingCompatible(const FWPLUTDescriptor& Other) const;
	FString MakeBuildHash() const;

	bool operator==(const FWPLUTDescriptor& Other) const;
	bool operator!=(const FWPLUTDescriptor& Other) const { return !(*this == Other); }
};

WORMHOLEPORTALRUNTIME_API uint32 GetTypeHash(const FWPLUTDescriptor& Descriptor);

/** Immutable CPU build output. UObject creation intentionally happens elsewhere on the game thread. */
struct WORMHOLEPORTALRUNTIME_API FWPLUTVolumeData
{
	FWPLUTDescriptor Descriptor;
	FIntVector Dimensions = FIntVector::ZeroValue;
	TArray<FLinearColor> Voxels;
	FString BuildHash;

	bool IsValid(FString* OutError = nullptr) const;
	int64 GetByteCount() const { return static_cast<int64>(Voxels.Num()) * sizeof(FLinearColor); }
};

struct WORMHOLEPORTALRUNTIME_API FWPLUTBuildStats
{
	double PreflightMilliseconds = 0.0;
	double ProfileMilliseconds = 0.0;
	double IntegrationMilliseconds = 0.0;
	double TotalMilliseconds = 0.0;
	int64 TexelCount = 0;
	int64 TextureBytes = 0;
	int64 EstimatedQuadratureSamples = 0;
	bool bCancelled = false;
	FString Error;
};

/** Per-actor view of a shared texture. Only the scalar coordinates vary by actor. */
USTRUCT(BlueprintType)
struct WORMHOLEPORTALRUNTIME_API FWPLUTBinding
{
	GENERATED_BODY()

	/** Shared baked or transient 3D LUT texture currently bound for this actor. Null means the binding is not ready. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "LUT")
	TObjectPtr<UVolumeTexture> VolumeTexture = nullptr;

	/** Shared immutable CPU voxels used by capture-face prediction. One copy is owned per LUT cache entry, never per Pair. */
	TSharedPtr<const FWPLUTVolumeData, ESPMode::ThreadSafe> CPUVolumeData;

	/** Descriptor that produced the bound LUT texture. This may differ from the requested descriptor when runtime fallback is used. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	FWPLUTDescriptor Descriptor;

	/** Actor-specific T/rho value used to choose the logarithmic W slice inside the shared 3D LUT. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	float TransitionRatio = 1.0f;

	/** Logical [0,1] logarithmic slice coordinate, before half-texel remapping. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	float RatioCoordinate01 = 0.0f;

	/** Metric outer radius divided by rho for this actor and transition ratio. Used to size the ray/proxy contract consistently. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	float NormalizedOuterRadius = 1.0f;

	/** Monotonic texture-resource revision. Changes when the bound LUT texture object or generated fallback resource changes. */
	UPROPERTY(VisibleAnywhere, Category = "LUT")
	uint32 ResourceRevision = 0;

	/** Indicates whether this binding came from a baked asset, runtime fallback, or is still unavailable. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	EWPLUTSource Source = EWPLUTSource::None;

	/** Failure reason reported by the LUT cache when the binding is not ready. Empty for successful bindings. */
	UPROPERTY(BlueprintReadOnly, Category = "LUT")
	FString Error;

	bool IsReady() const { return VolumeTexture != nullptr && Source != EWPLUTSource::None; }
};
