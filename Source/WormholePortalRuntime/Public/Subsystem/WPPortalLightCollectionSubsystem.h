// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "UObject/ObjectKey.h"
#include "WPPortalLightCollectionSubsystem.generated.h"

class AActor;
class AWormholePortalActor;
class ULightComponent;
class UWPRegistrySubsystem;

/**
 * Internal state that tracks one Light Component discovered automatically in the World.
 *
 * It does not own the Light Component lifetime and holds only a weak pointer.
 * ObjectKey is used to look up TrackedLightKeys and remove invalid weak pointers.
 */
struct FWPTrackedLightRecord
{
	TObjectKey<ULightComponent> ObjectKey;
	TWeakObjectPtr<ULightComponent> LightComponent;
};

/**
 * List of Lights that may affect a specific Portal.
 *
 * This is a broad-phase result indicating whether a Light's influence volume overlaps the Portal Bounds.
 * Occlusion by intervening geometry is not tested at this stage.
 */
struct FWPPortalAffectingLightState
{
	TWeakObjectPtr<AWormholePortalActor> Portal;
	
	TArray<TWeakObjectPtr<ULightComponent>> AffectingLights;
	
	/**
	 * Used to compare AffectingLights membership.
	 * Uses TObjectKey, which identifies the UObject lifetime, rather than a raw pointer address.
	 */
	TSet<TObjectKey<ULightComponent>> AffectingLightKeys;
	
	/** Revision incremented whenever AffectingLights membership changes. */
	uint32 Revision = 0;
	
	/** Ensures that the initial state is published once even when it contains no Lights. */
	bool bInitialized = false;
};

/** Snapshot used by external Game Thread code to read per-Portal results. */
struct FWPPortalAffectingLightSnapshot
{
	TWeakObjectPtr<AWormholePortalActor> Portal;
	TArray<TWeakObjectPtr<ULightComponent>> AffectingLights;
	
	uint32 Revision = 0;
	
	bool IsValid() const
	{
		return Portal.IsValid();
	}
};

/**
 * Automatically discovers Point, Spot, and Directional Lights in the current World and maintains
 * the list of Lights that may affect each Portal registered with the Registry.
 *
 * This Subsystem is responsible only for Light discovery and broad-phase membership.
 * UWPPortalLightTransmissionSubsystem consumes these results to manage A-to-B and B-to-A routes,
 * exit-space transforms, proxy Lights, shadows, and occlusion.
 * This Subsystem does not generate Render Thread packets.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPPortalLightCollectionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	/**
	 * Returns the number of supported Light Components currently discovered and tracked automatically.
	 */
	int32 GetTrackedLightCount() const;
	
	/** Returns the number of Portals currently tracked from the Registry. */
	int32 GetTrackedPortalCount() const;
	
	/**
	 * Returns the current list of Lights that may affect a specific Portal.
	 *
	 * @param Portal Portal to query.
	 * @param OutLights Receives the current broad-phase Light list on success.
	 * @return True if Portal state was found. The list may be empty on success.
	 */
	bool GetAffectingLights(const AWormholePortalActor* Portal, TArray<TWeakObjectPtr<ULightComponent>>& OutLights) const;
	
	/**
	 * Returns the Light list and membership revision for a specific Portal.
	 *
	 * @param Portal Portal to query.
	 * @param OutSnapshot Receives the Light list and membership revision on success.
	 * @return True if Portal state was found.
	 */
	bool GetPortalLightSnapshot(const AWormholePortalActor* Portal, FWPPortalAffectingLightSnapshot& OutSnapshot) const;
	
private:
	/**
	 * Scans the current World for supported Light Components and adds any missing entries.
	 */
	void DiscoverAllLights();
	
	/** Discovers supported Light Components contained in a spawned Actor. */
	void DiscoverLightsOnActor(AActor* Actor);
	
	/** Processes at PostActorTick the Actors queued by the Actor-spawn callback. */
	void ProcessPendingSpawnedActors();
	
	/** Adds a supported Light Component to TrackedLights if it is not already tracked. */
	void TrackLight(ULightComponent* LightComponent);
	
	/**
	 * Removes Lights that were destroyed, moved to another World, or opted out from the tracking list.
	 */
	void CompactTrackedLights();
	
	/** Creates result state for a Portal registered with the Registry. */
	void RegisterPortal(AWormholePortalActor* Portal);
	
	/** Removes result state for a Portal unregistered from the Registry. */
	void UnregisterPortal(AWormholePortalActor* Portal);
	
	/** Removes state for Portals that were destroyed or moved to another World. */
	void CompactInvalidPortals();
	
	/** Recomputes AffectingLights for all currently registered Portals. */
	void RefreshAllPortalLights();
	
	/** Recomputes AffectingLights for one specified Portal. */
	void RefreshPortalLights(FWPPortalAffectingLightState& PortalState);
	
	/**
	 * Tests whether the Light is a trackable Point, Spot, or Directional Light in the current World.
	 */
	bool IsSupportedLight(const ULightComponent* LightComponent) const;
	
	/**
	 * Tests whether the Light is active and eligible as an actual influence candidate for the current frame.
	 */
	bool IsEligibleLight(const ULightComponent* LightComponent) const;
	
	/** Tests whether the Light type is currently eligible for collection. */
	static bool IsSupportedLightType(const ULightComponent& LightComponent);
	
	/** Tests whether two Light Key sets have identical membership. */
	static bool AreLightKeySetsEqual(const TSet<TObjectKey<ULightComponent>>& A, const TSet<TObjectKey<ULightComponent>>& B);
	
	void HandleActorSpawned(AActor* SpawnedActor);
	
	void HandleWorldPostActorTick(UWorld* TickedWorld, ELevelTick TickType, float DeltaSeconds);
	
	void HandlePortalRegistered(AWormholePortalActor* Portal);
	void HandlePortalUnregistered(AWormholePortalActor* Portal);
	
private:
	TWeakObjectPtr<UWPRegistrySubsystem> RegistrySubsystem;
	
	/** Weak-reference list that stores each supported Light in the World only once. */
	TArray<FWPTrackedLightRecord> TrackedLights;
	
	/** Lookup used to prevent duplicate registration of the same Light. */
	TSet<TObjectKey<ULightComponent>> TrackedLightKeys;
	
	/** Stores AffectingLights results for each Portal UObject lifetime. */
	TMap<TObjectKey<AWormholePortalActor>, FWPPortalAffectingLightState> PortalStates;
	
	/**
	 * Defers Component discovery until PostActorTick because construction and registration
	 * may not be complete when ActorSpawned fires.
	 */
	TArray<TWeakObjectPtr<AActor>> PendingSpawnedActors;
	
	/**
	 * Real-time timestamp for the next full scan that recovers Components missed through
	 * runtime AddComponent calls.
	 */
	double NextFullLightReconcileRealSeconds = 0.0;
	
	FDelegateHandle ActorSpawnedHandle;
	FDelegateHandle WorldPostActorTickHandle;
	
	FDelegateHandle PortalRegisteredHandle;
	FDelegateHandle PortalUnregisteredHandle;
	
	bool bDeinitializing = false;
};
