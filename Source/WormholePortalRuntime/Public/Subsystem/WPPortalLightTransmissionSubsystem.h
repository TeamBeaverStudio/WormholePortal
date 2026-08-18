// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WormholePortalActor.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"

#include "WPPortalLightTransmissionSubsystem.generated.h"

class AActor;
class AWormholePortalActor;
class ULightComponent;
class UMaterialInterface;
class UPrimitiveComponent;
class USceneCaptureComponent2D;
class UTexture;
class UTextureRenderTarget2D;
class UWPPortalLightCollectionSubsystem;
class UWPRegistrySubsystem;

struct FWPPortalPairSnapshot;
struct FWPTransform;

/**
 * Current endpoint state for one Registry Pair.
 *
 * PortalA and PortalB are the Registry's canonical endpoints and are not inherently directional.
 * Transmission creates separate A-to-B and B-to-A routes.
 */
struct FWPPortalLightActivePairState
{
	TWeakObjectPtr<AWormholePortalActor> PortalA;
	TWeakObjectPtr<AWormholePortalActor> PortalB;
};

/**
 * Runtime route key that identifies one generated proxy Light.
 *
 * PairId identifies the lifetime of the current Portal connection.
 * EntryPortalKey distinguishes the A-to-B and B-to-A directions.
 * SourceLightKey identifies the UObject lifetime of the original Light Component.
 *
 * This Key is used only within the Game Thread lifetime of the current World,
 * so a separate persistent SourceId is not currently required.
 */
struct FWPPortalLightRouteKey
{
	FGuid PairId;
	
	TObjectKey<AWormholePortalActor> EntryPortalKey;
	TObjectKey<ULightComponent> SourceLightKey;
	
	friend bool operator==(const FWPPortalLightRouteKey& A, const FWPPortalLightRouteKey& B)
	{
		return A.PairId == B.PairId && A.EntryPortalKey == B.EntryPortalKey && A.SourceLightKey == B.SourceLightKey;
	}
	
	friend uint32 GetTypeHash(const FWPPortalLightRouteKey& Key)
	{
		return HashCombineFast(GetTypeHash(Key.PairId), HashCombineFast(GetTypeHash(Key.EntryPortalKey), GetTypeHash(Key.SourceLightKey)));
	}
};

/** Current route state from an original Light to an exit proxy Light. */
struct FWPPortalLightTransmissionState
{
	TWeakObjectPtr<AWormholePortalActor> EntryPortal;
	TWeakObjectPtr<AWormholePortalActor> ExitPortal;
	
	TWeakObjectPtr<ULightComponent> SourceLight;
	TWeakObjectPtr<ULightComponent> ProxyLight;

	/**
	 * Portal-specific shadow-depth capture that views only the Entry Portal from the Source Light position.
	 */
	TWeakObjectPtr<USceneCaptureComponent2D> SourceShadowCapture;
	TWeakObjectPtr<UTextureRenderTarget2D> SourceShadowDepthTarget;

	FTransform LastCapturedSourceTransform = FTransform::Identity;
	FTransform LastCapturedEntryTransform = FTransform::Identity;

	float LastCapturedEntryRadius = 0.0f;
	float ExitPortalScreenDiameterPixels = 0.0f;

	double LastSourceShadowCaptureRealSeconds = -1.0;
	double ResolutionCandidateSinceRealSeconds = 0.0;

	int32 CurrentSourceShadowResolution = 0;
	int32 ResolutionCandidate = 0;

	bool bSourceShadowValid = false;
	bool bSourceShadowDirty = true;
	bool bSourceShadowActive = false;
	bool bExitPortalOnScreen = false;
};

/**
 * Weak-reference record that tracks each dynamic shadow caster in the World without duplicates.
 */
struct FWPPortalShadowCasterRecord
{
	TObjectKey<UPrimitiveComponent> ObjectKey;
	TWeakObjectPtr<UPrimitiveComponent> PrimitiveComponent;
};

/**
 * Transmits original Point and Spot Lights that affect a Portal into the space on the
 * opposite side.
 *
 * Responsibilities:
 * - Builds A-to-B and B-to-A routes for each Registry Pair.
 * - Consumes AffectingLights from the Collection Subsystem.
 * - Creates proxy Lights in exit space.
 * - Synchronizes the original Light transform and properties in real time.
 * - Removes proxies when the connection or original source lifetime ends.
 *
 * Current Point and Spot Light implementation:
 * - Uses a virtual-light transform that preserves ray-field continuity.
 * - Restricts illumination to the exit-sphere aperture and blocks direct light before
 *   the exit.
 * - Combines Entry-facing SceneDepth with native shadows from the Exit proxy for
 *   two-sided occlusion.
 *
 * Directional Lights remain in Collection results but are not transmitted through this
 * Subsystem's
 * per-light proxy path. Directional transmission is planned as a separate
 * environmental-lighting model.
 *
 * Currently unsupported:
 * - Lights inside a Portal, asymmetric radii, and translucent or colored shadows
 * - GI, Reflection, and Volumetric transmission
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPPortalLightTransmissionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual  void Deinitialize() override;
	
	/** Returns the number of currently valid generated proxy Lights. */
	int32 GetProxyLightCount() const;
	
private:
	/** Synchronizes ActivePairs with the Registry's authoritative Pair list. */
	void RefreshPairTopology();
	
	/** Reconciles all routes against the current Pairs and Collection results. */
	void ReconcileAllRoutes();
	
	/**
	 * Reconciles a Pair's unidirectional Entry-to-Exit Light route and adds required route
	 * keys to OutDesiredRoutes.
	 */
	void ReconcileDirection(const FGuid& PairId, AWormholePortalActor* EntryPortal, AWormholePortalActor* ExitPortal, TSet<FWPPortalLightRouteKey>& OutDesiredRoutes);
	
	/**
	 * Creates the transient Actor that owns generated Light Components if it does not
	 * already exist.
	 */
	AActor* EnsureProxyHost();
	
	/**
	 * Creates a generated proxy Light of the same type as the original Point or Spot Light.
	 */
	ULightComponent* CreateProxyLight(const ULightComponent& SourceLight, const FWPTransform& Mapping, const AWormholePortalActor& ExitPortal);
	
	/**
	 * Applies the current transform and properties of an original Point or Spot Light to
	 * its proxy.
	 */
	void UpdateProxyLight(ULightComponent& ProxyLight, const ULightComponent& SourceLight, const FWPTransform& Mapping, const AWormholePortalActor& ExitPortal) const;
	
	/**
	 * Tests whether the Source Light is a Point or Spot Light supported by the per-light
	 * proxy path.
	 */
	static bool IsSupportedProxySourceType(const ULightComponent& SourceLight);
	
	/** Tests whether the proxy and current source have the same Light type. */
	static bool DoesProxyTypeMatchSource(const ULightComponent& ProxyLight, const ULightComponent& SourceLight);
	
	/** Removes the proxy and source-shadow resources owned by one route. */
	void DestroyRouteState(FWPPortalLightTransmissionState& RouteState);
	
	/** Removes all route states and their generated proxies. */
	void DestroyAllRouteStates();

	/**
	 * Applies the source-shadow capture budget to active routes and schedules required
	 * captures.
	 */
	void UpdateSourceShadowRoutes();

	bool IsSourceShadowRouteSupported(const FWPPortalLightTransmissionState& RouteState) const;
	bool EnsureSourceShadowResources(FWPPortalLightTransmissionState& RouteState, int32 Resolution);
	bool ConfigureSourceShadowCapture(FWPPortalLightTransmissionState& RouteState, int32 Resolution);
	void RebuildSourceShadowShowOnlyList(FWPPortalLightTransmissionState& RouteState) const;
	void UpdateSourceShadowMaterialParameters(FWPPortalLightTransmissionState& RouteState) const;
	void SetSourceShadowMaterialEnabled(FWPPortalLightTransmissionState& RouteState, bool bEnabled) const;
	void DestroySourceShadowResources(FWPPortalLightTransmissionState& RouteState) const;

	bool GetExitPortalScreenMetrics(const AWormholePortalActor& ExitPortal, float& OutDiameterPixels) const;
	static bool DoesBoundsIntersectSourceShadowCone(
		const FBoxSphereBounds& Bounds,
		const FVector& SourceLocation,
		const FVector& CaptureForward,
		float TanHalfFov,
		float MaxDistance);

	void DiscoverAllShadowCasters();
	void DiscoverShadowCastersOnActor(AActor* Actor);
	void ProcessPendingSpawnedActors();
	bool TrackShadowCaster(UPrimitiveComponent* PrimitiveComponent);
	bool CompactShadowCasters();
	bool IsEligibleShadowCaster(const UPrimitiveComponent* PrimitiveComponent) const;
	void MarkAllSourceShadowsDirty();

	void HandleActorSpawned(AActor* SpawnedActor);
	void HandleWorldPostActorTick(UWorld* TickedWorld, ELevelTick TickType, float DeltaSeconds);
	
	void HandlePortalPairAdded(const FWPPortalPairSnapshot& PairSnapshot);
	
	void HandlePortalPairRemoved(const FWPPortalPairSnapshot& PairSnapshot);
	
private:
	TWeakObjectPtr<UWPRegistrySubsystem> RegistrySubsystem;
	TWeakObjectPtr<UWPPortalLightCollectionSubsystem> LightCollectionSubsystem;

	/**
	 * Parent Material for the spherical Portal-gate MID created for each Point or Spot
	 * proxy.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PortalLocalLightMaskMaterial;

	/**
	 * Default Material texture restored when the source-shadow render-target override is
	 * removed.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UTexture> SourceShadowFallbackTexture;
	
	/** World-scoped transient Actor that owns generated Light Components. */
	TWeakObjectPtr<AActor> ProxyHostActor;
	
	/** Stores current endpoint state for each Registry PairId. */
	TMap<FGuid, FWPPortalLightActivePairState> ActivePairs;
	
	/** Stores generated proxy state for each transmission direction and Source Light. */
	TMap<FWPPortalLightRouteKey, FWPPortalLightTransmissionState> RouteStates;

	/** Dynamic shadow-caster registry used to build the SceneDepth show-only list. */
	TArray<FWPPortalShadowCasterRecord> ShadowCasters;
	TSet<TObjectKey<UPrimitiveComponent>> ShadowCasterKeys;
	TArray<TWeakObjectPtr<AActor>> PendingSpawnedActors;

	double NextFullShadowCasterReconcileRealSeconds = 0.0;
	
	FDelegateHandle PortalPairAddedHandle;
	FDelegateHandle PortalPairRemovedHandle;
	FDelegateHandle ActorSpawnedHandle;
	FDelegateHandle WorldPostActorTickHandle;
	
	bool bPairTopologyDirty = true;
	bool bDeinitializing = false;
};
