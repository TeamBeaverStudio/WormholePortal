// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Rendering/LUT/WPLUTTypes.h"

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Transit/WPTransitTypes.h"
#include "Engine/EngineTypes.h"
#include "WPSettings.generated.h"

/**
 * One user-editable Dynamic Cubemap resolution tier.
 *
 * The Runtime compares the SafeProxy's projected screen-height percentage against
 * StartScreenHeightPercent. Once the threshold is reached, Resolution becomes the requested
 * Cubemap face size. Runtime validation clamps percentages to 0-100, rounds every positive
 * resolution up to the next multiple of eight in the 8-2048 range, and evaluates tiers in
 * ascending threshold order.
 */
USTRUCT()
struct WORMHOLEPORTALRUNTIME_API FWPCaptureResolutionTier
{
	GENERATED_BODY()

	/** SafeProxy screen-height percentage at which this tier starts. */
	UPROPERTY(EditAnywhere, Category = "Scene Capture|Dynamic Cubemap Resolution",
		meta = (DisplayName = "Start (%)", ClampMin = "0.0", ClampMax = "100.0",
			UIMin = "0.0", UIMax = "100.0"))
	float StartScreenHeightPercent = 100.0f;

	/** Cubemap face resolution requested after this tier's threshold is reached. */
	UPROPERTY(EditAnywhere, Category = "Scene Capture|Dynamic Cubemap Resolution",
		meta = (ClampMin = "1", ClampMax = "2048", UIMin = "1", UIMax = "2048",
			Delta = "1"))
	int32 Resolution = 512;
};

/**
 * @brief Project-wide settings for the Wormhole Portal plugin.
 *
 * Stores the Trace, Transit, Scene Capture, and LUT settings shared by all Worlds and Portals
 * in DefaultEngine.ini.
 */
UCLASS(Config = Engine, DefaultConfig, meta = (DisplayName = "Wormhole Portal"))
class WORMHOLEPORTALRUNTIME_API UWPSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	/** Initializes or migrates the Dynamic Cubemap tier array after Config values are loaded. */
	virtual void PostInitProperties() override;

	/** Reapplies legacy-to-array migration when the settings Config is reloaded at Runtime. */
	virtual void PostReloadConfig(FProperty* PropertyThatWasLoaded) override;

	/**
	 * Converts any positive requested resolution to the next multiple of eight in the 8-2048 range.
	 * Zero and negative values use FallbackResolution before normalization.
	 */
	static int32 NormalizeCaptureResolution(
		int32 RequestedResolution,
		int32 FallbackResolution);

	UPROPERTY(Config, EditAnywhere, Category = "Trace")
	TEnumAsByte<ECollisionChannel> PortalTraceChannel = ECC_GameTraceChannel1;
	
	UPROPERTY(Config, EditAnywhere, Category = "Trace")
	FName PortalTraceChannelName = TEXT("WPPortalTrace");
	
	// Maximum angular tolerance for treating a candidate as tied with the closest tangent plane.
	UPROPERTY(Config, EditAnywhere, Category = "Transit|Plane Selection",
		meta = (ClampMin = "0.0", ClampMax = "90.0", UIMin = "0.0", UIMax = "90.0", Units = "deg"))
	float PlaneTieAngleDegrees = 1.0f;
	
	// Final plane priority used when tangent-plane and movement-direction angles cannot select a unique plane.
	UPROPERTY(Config, EditAnywhere, Category = "Transit|Plane Selection")
	EWPPlanePriority PlanePriority = EWPPlanePriority::YZ_XZ_XY;
	
	/**
	 * @brief Starting Custom Primitive Data index used by Visual Clip.
	 *
	 * Must match the Primitive Data index used by the baked material.
	 * A pre-bake validation step must verify that the four consecutive slots beginning
	 * at this index do not conflict with other Custom Primitive Data.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Transit|Visual Clip",
		meta = (ClampMin = "0", ClampMax = "28", UIMin = "0", UIMax = "28", Delta = "4"))
	int32 ClipBaseIndex = 28;
	
	// Path where generated Voxel Assets are stored.
	UPROPERTY(Config, EditAnywhere, Category = "Transit|Voxel Asset", meta = (LongPackageName))
	FString GeneratedVoxelAssetPath = TEXT("/WormholePortal/WormholePortal/Generated/Voxels");

	/** Resolves and validates the writable project path used for generated Voxel Assets. */
	bool TryResolveGeneratedVoxelPath(
		FString& OutRootPath,
		FString& OutError) const;

	/** Controls whether audio from spatialized Audio Components is transmitted to the other side of a linked Portal. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio")
	bool bEnablePortalAudio = true;

	/** Final linear volume multiplier applied to a Portal route. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio",
		meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
	float PortalAudioTransmissionGain = 1.0f;

	/** Controls Portal Audio occlusion by testing the Source-to-Entry and Exit-to-Listener segments independently. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion")
	bool bEnablePortalAudioOcclusion = true;

	/** General Scene Collision Channel used for occlusion tests in both Portal spaces. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion")
	TEnumAsByte<ECollisionChannel> PortalAudioOcclusionTraceChannel = ECC_Visibility;

	/** Interval at which occlusion is reevaluated for both segments of the same route. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion",
		meta = (ClampMin = "0.01", ClampMax = "2.0", UIMin = "0.01", UIMax = "0.5", Units = "s"))
	float PortalAudioOcclusionCheckInterval = 0.1f;

	/** Linear volume multiplier applied when either segment is blocked. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float PortalAudioOccludedVolumeMultiplier = 0.35f;

	/** Low Pass Filter frequency applied when either segment is blocked. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion",
		meta = (ClampMin = "20.0", ClampMax = "20000.0", UIMin = "100.0", UIMax = "20000.0", Units = "Hz"))
	float PortalAudioOcclusionLowPassFilterFrequency = 1200.0f;

	/** Distance by which each segment is shortened inward from its surface to avoid re-hitting the seam or endpoint collision. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Occlusion",
		meta = (ClampMin = "0.0", ClampMax = "25.0", UIMin = "0.0", UIMax = "5.0", Units = "cm"))
	float PortalAudioOcclusionSurfaceBias = 2.0f;

	/** Interval at which Audio Components are rediscovered, including components created at runtime without an Actor. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Audio|Performance",
		meta = (ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.05", UIMax = "2.0", Units = "s"))
	float PortalAudioSourceReconcileInterval = 0.25f;

	/** Controls whether caster visibility from Entry space is transmitted through the Exit Proxy Light Function. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows")
	bool bEnablePortalSourceShadows = true;

	/**
	 * Lowest Cubemap face resolution used while a visible SafeProxy occupies less than the
	 * Tier 1 screen-height threshold. A culled Pair also allocates this resolution while its
	 * first capture remains deferred. Runtime rounds every positive resolution up to the next
	 * multiple of eight in the 8-2048 range.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Dynamic Cubemap Resolution",
		meta = (DisplayName = "Lowest Visible Resolution", ClampMin = "1",
			ClampMax = "2048", UIMin = "1", UIMax = "2048", Delta = "1"))
	int32 CaptureLowestVisibleResolution = 64;

	/**
	 * Ordered collection of visible Dynamic Cubemap resolution tiers. Elements may be added,
	 * removed, or reordered in Project Settings. Runtime sorts a temporary policy by Start (%)
	 * so an accidentally reordered list remains safe. An empty list uses Lowest Visible
	 * Resolution for every camera distance.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Dynamic Cubemap Resolution",
		meta = (DisplayName = "Resolution Tiers", TitleProperty = "StartScreenHeightPercent"))
	TArray<FWPCaptureResolutionTier> CaptureResolutionTiers;

	/**
	 * Cubemap resolution requested while the Reference Camera is inside this Pair's SafeProxyRadius.
	 * It takes precedence over the screen-height tier, although the VRAM budget may reduce the
	 * requested resolution. No console CVar is used; this Project Settings value is the sole
	 * source of truth.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Dynamic Cubemap Resolution",
		meta = (DisplayName = "Inside SafeProxy Resolution", ClampMin = "1",
			ClampMax = "2048", UIMin = "1", UIMax = "2048", Delta = "1"))
	int32 CaptureInsideSafeProxyResolution = 768;

private:
	/** Current serialized layout version for CaptureResolutionTiers. Hidden from Project Settings. */
	UPROPERTY(Config)
	int32 CaptureResolutionTierConfigVersion = 0;

	// Legacy fixed-tier Config keys retained only so existing projects migrate without data loss.
	UPROPERTY(Config)
	float CaptureTier1ScreenHeightPercent = 30.0f;

	UPROPERTY(Config)
	int32 CaptureTier1Resolution = 128;

	UPROPERTY(Config)
	float CaptureTier2ScreenHeightPercent = 60.0f;

	UPROPERTY(Config)
	int32 CaptureTier2Resolution = 256;

	UPROPERTY(Config)
	float CaptureTier3ScreenHeightPercent = 100.0f;

	UPROPERTY(Config)
	int32 CaptureTier3Resolution = 512;

	/** Builds the array once from legacy fixed-tier keys when loading a pre-array Config. */
	void EnsureCaptureResolutionTierConfigMigrated();

	/** Snaps all stored scalar and array resolutions; returns the number of changed values. */
	int32 NormalizeStoredCaptureResolutions();

public:

	/** Controls only the Lumen Reflections pass for the Cube SceneCapture. No pass is created when Lumen parity mode uses Reflection=None, even if this setting is enabled. */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Lumen Reflections"))
	bool bSceneCaptureLumenReflections = true;

	/** Controls only the Screen Space Ambient Occlusion (SSAO) pass for the Cube SceneCapture. Does not affect Distance Field AO. */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Screen Space Ambient Occlusion (SSAO)"))
	bool bSceneCaptureScreenSpaceAO = true;

	/**
	 * Controls the Volumetric Cloud bundle for the Cube SceneCapture.
	 * This setting does not affect the Player Game View. Disabling it turns off not only Cloud
	 * ray marching, but also Cloud shadow maps, related filtering, VolumetricCloud output
	 * generation, and VolCloudComposeOverScene. If Atmosphere is disabled, Volumetric Clouds
	 * are not rendered even when this setting is enabled.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Volumetric Cloud Bundle"))
	bool bSceneCaptureCloud = true;

	/**
	 * Controls the dynamic-shadow bundle for the Cube SceneCapture.
	 * Disabling it turns off dynamic ShadowDepths, including VSM, and ShadowProjection.
	 * Contact, Capsule, and Distance Field Shadows each have individual Show Flags, but,
	 * depending on the render path, they may still be affected by this parent dynamic-shadow
	 * state. This setting does not change shadows in the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Dynamic Shadows Bundle"))
	bool bSceneCaptureDynamicShadows = true;

	/**
	 * Controls the Sky Light bundle for the Cube SceneCapture.
	 * It affects not only SkyLight diffuse, but also SkyLighting-dependent work such as
	 * DFAO-modulated Sky Light, Lumen sky-light processing, and the sky-light contribution to
	 * volumetric fog. This setting does not affect the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Sky Lighting Bundle"))
	bool bSceneCaptureSkyLighting = true;

	/**
	 * Controls the Sky Atmosphere bundle for the Cube SceneCapture.
	 * Disabling it affects atmosphere LUTs, sky rendering, and atmospheric lighting. It also
	 * removes a prerequisite for Volumetric Clouds, so Clouds are not rendered.
	 * This setting does not affect Atmosphere in the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Sky Atmosphere Bundle"))
	bool bSceneCaptureAtmosphere = true;

	/**
	 * Controls the deferred direct-lighting bundle for the Cube SceneCapture.
	 * It controls the deferred-lighting output of Directional, Point, Spot, and Rect Lights.
	 * However, setup work for DynamicShadows or LightGrid is not necessarily eliminated if
	 * another feature still requires it. This setting does not affect the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Deferred Lighting Bundle"))
	bool bSceneCaptureDeferredLighting = true;

	/**
	 * This is the top-level Show Flag that enables general lighting for the Cube SceneCapture.
	 * Disabling it removes the execution conditions for deferred/direct lighting, SSAO, and
	 * several other lighting features, so the result may be effectively close to Unlit.
	 * This setting does not change the Lighting state of the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Lighting Master Bundle"))
	bool bSceneCaptureLighting = true;

	/**
	 * Controls the top-level Fog bundle for the Cube SceneCapture.
	 * Disabling it turns off Exponential Height Fog, Local Fog Volumes, Volumetric Fog, and fog
	 * compositing behind water. Use the separate option below to disable only Volumetric Fog.
	 * This setting does not affect Fog in the Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Fog Master Bundle"))
	bool bSceneCaptureFog = true;

	/**
	 * Controls only Volumetric Fog for the Cube SceneCapture.
	 * Disabling it leaves Height Fog and Local Fog Volumes enabled as long as the parent Fog
	 * option remains enabled. Conversely, Volumetric Fog is not rendered when the parent Fog
	 * option is disabled, even if this setting is enabled. This setting does not affect the
	 * Player Game View.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Scene Capture|Bundled Show Flags",
		meta = (DisplayName = "Volumetric Fog"))
	bool bSceneCaptureVolumetricFog = true;

	/** Base resolution used for the first capture and for off-screen routes. Quantized at runtime to the 128, 256, or 512 tier. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "128", ClampMax = "512", UIMin = "128", UIMax = "512"))
	int32 PortalSourceShadowBaseResolution = 256;

	/** Source-shadow capture update frequency for visible Exit Portals. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "1.0", ClampMax = "60.0", UIMin = "1.0", UIMax = "60.0", Units = "Hz"))
	float PortalSourceShadowUpdateRateHz = 30.0f;

	/** Maximum number of source-shadow SceneCaptures that can be scheduled on the GPU per frame. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "0", ClampMax = "16", UIMin = "0", UIMax = "8"))
	int32 MaxPortalSourceShadowCapturesPerFrame = 2;

	/** Maximum number of source-shadow routes that can keep capture resources resident simultaneously. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "0", ClampMax = "64", UIMin = "0", UIMax = "16"))
	int32 MaxActivePortalSourceShadowRoutes = 8;

	/** Bias applied when comparing Captured View-Z against the Portal boundary to prevent shadow acne. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "0.0", ClampMax = "100.0", UIMin = "0.0", UIMax = "10.0", Units = "cm"))
	float PortalSourceShadowDepthBiasCm = 1.0f;

	/** Radius, in texels, of the 4-tap PCF offset used by the Light Function. */
	UPROPERTY(Config, EditAnywhere, Category = "Portal Light|Source Shadows",
		meta = (ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
	float PortalSourceShadowPCFRadiusTexels = 1.0f;

	/** Path where generated LUT Assets are stored. */
	UPROPERTY(Config, EditAnywhere, Category = "LUT|LUT Asset", meta = (LongPackageName))
	FString GeneratedLUTAssetPath = TEXT("/WormholePortal/WormholePortal/Generated/LUT");

	/** Shared normalized Volume LUT bake and runtime contract. */
	UPROPERTY(Config, EditAnywhere, Category = LUT)
	FWPLUTDescriptor LUTDescriptor;

	/** Allows an asynchronous transient fallback when no compatible baked asset exists. */
	UPROPERTY(Config, EditAnywhere, Category = LUT)
	bool bAllowRuntimeLUTFallback = true;

	bool TryResolveGeneratedLUTPaths(
		FString& OutRootPath,
		FString& OutCatalogObjectPath,
		FString& OutError) const;

#if WITH_EDITOR
	/** Snaps edited scalar and array resolutions upward to their effective eight-pixel alignment. */
	virtual void PostEditChangeChainProperty(
		FPropertyChangedChainEvent& PropertyChangedEvent) override;

	virtual FName GetCategoryName() const override
	{
		return TEXT("Plugins");
	}
#endif
	
};
