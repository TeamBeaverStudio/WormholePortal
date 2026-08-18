// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"

#include "WPPortalAudioSubsystem.generated.h"

class AActor;
class AWormholePortalActor;
class UAudioComponent;
class USoundBase;
class USoundClass;
class USoundSourceBus;
class UWPRegistrySubsystem;

struct FWPPortalPairSnapshot;
struct FWPTransform;

/**
 * Weak-reference record for an original Audio Component discovered automatically in the
 * World.
 */
struct FWPTrackedPortalAudioSource
{
	TObjectKey<UAudioComponent> ObjectKey;
	TWeakObjectPtr<UAudioComponent> AudioComponent;
	TStrongObjectPtr<USoundSourceBus> SourceBus;

	uint32 LastBoundPlayOrder = 0;
	bool bHasConfiguredSourceBusSend = false;
};

/**
 * Stores the current endpoints of one Registry Pair. PortalA and PortalB are not
 * inherently directional.
 */
struct FWPPortalAudioActivePairState
{
	TWeakObjectPtr<AWormholePortalActor> PortalA;
	TWeakObjectPtr<AWormholePortalActor> PortalB;
};

/**
 * Route key that combines the Pair lifetime, transmission direction, and original Audio
 * Component lifetime.
 */
struct FWPPortalAudioRouteKey
{
	FGuid PairId;
	TObjectKey<AWormholePortalActor> EntryPortalKey;
	TObjectKey<UAudioComponent> SourceAudioKey;

	friend bool operator==(const FWPPortalAudioRouteKey& A, const FWPPortalAudioRouteKey& B)
	{
		return A.PairId == B.PairId
			&& A.EntryPortalKey == B.EntryPortalKey
			&& A.SourceAudioKey == B.SourceAudioKey;
	}

	friend uint32 GetTypeHash(const FWPPortalAudioRouteKey& Key)
	{
		return HashCombineFast(
			GetTypeHash(Key.PairId),
			HashCombineFast(GetTypeHash(Key.EntryPortalKey), GetTypeHash(Key.SourceAudioKey)));
	}
};

/**
 * Current state for transmitting one original Audio Component through a unidirectional
 * Portal route.
 */
struct FWPPortalAudioRouteState
{
	TWeakObjectPtr<AWormholePortalActor> EntryPortal;
	TWeakObjectPtr<AWormholePortalActor> ExitPortal;
	TWeakObjectPtr<UAudioComponent> SourceAudio;
	TWeakObjectPtr<USoundSourceBus> SourceBus;
	TWeakObjectPtr<USoundBase> SourceSoundAtProxyCreation;
	TWeakObjectPtr<UAudioComponent> ProxyAudio;

	double NextOcclusionCheckRealSeconds = 0.0;
	uint32 LastSourcePlayOrder = 0;

	bool bUseSourceBus = false;
	bool bFallbackProxyStarted = false;
	bool bOccluded = false;
};

/**
 * Automatically discovers spatialized UAudioComponents in the current World and creates
 * reradiating
 * audio sources on the opposite side of each Portal Pair.
 *
 * Implementation contract:
 * - The Registry PairId is the sole topology identity for a route lifetime.
 * - Each Pair has independent A-to-B and B-to-A routes.
 * - PlayWhenSilent sources share their original playback state through a post-effect
 *   Source Bus.
 * - Other sources are transmitted through a best-effort fallback that creates a
 *   separate SoundBase instance.
 * - Independent radial legs on the Entry and Exit spheres reradiate audio in every
 *   direction from the Exit Portal.
 * - Instead of native Engine occlusion on the proxy, the Source-to-Entry and
 *   Exit-to-Listener segments are tested separately.
 * - Generated and Disabled tags control automatic discovery.
 *
 * The current scope supports one primary listener and one Portal hop.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPPortalAudioSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Audio")
	int32 GetTrackedSourceCount() const;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Audio")
	int32 GetProxyAudioCount() const;

private:
	void DiscoverAllAudioComponents();
	void DiscoverAudioComponentsOnActor(AActor* Actor);
	void ProcessPendingSpawnedActors();
	bool SilenceGeneratedTransitAudio(UAudioComponent* AudioComponent) const;
	void TrackAudioComponent(UAudioComponent* AudioComponent);
	void CompactTrackedAudioComponents();
	USoundSourceBus* EnsureSourceBus(
		FWPTrackedPortalAudioSource& SourceRecord,
		UAudioComponent& SourceAudio);
	void RefreshSourceBusSend(
		FWPTrackedPortalAudioSource& SourceRecord,
		UAudioComponent& SourceAudio);
	void ReleaseSourceBusSend(FWPTrackedPortalAudioSource& SourceRecord);

	bool IsSupportedSource(const UAudioComponent* AudioComponent) const;
	bool HasSupportedSpatialization(const UAudioComponent* AudioComponent) const;
	bool IsEligibleSource(const UAudioComponent* AudioComponent) const;
	bool CanSourceReachPortal(const UAudioComponent& AudioComponent, const AWormholePortalActor& EntryPortal) const;

	void RefreshPairTopology();
	void ReconcileAllRoutes();
	void ReconcileDirection(
		const FGuid& PairId,
		AWormholePortalActor* EntryPortal,
		AWormholePortalActor* ExitPortal,
		TSet<FWPPortalAudioRouteKey>& OutDesiredRoutes);

	AActor* EnsureProxyHost();
	UAudioComponent* CreateProxyAudio(
		const UAudioComponent& SourceAudio,
		USoundBase& ProxySound,
		bool bUseSourceBus,
		const FWPTransform& Mapping);
	void ConfigureProxyAudioBeforeRegistration(
		UAudioComponent& ProxyAudio,
		const UAudioComponent& SourceAudio,
		USoundBase& ProxySound,
		bool bUseSourceBus) const;
	float EstimateFallbackPlaybackStartTime(const UAudioComponent& SourceAudio) const;
	void SynchronizeFallbackPlayback(
		FWPPortalAudioRouteState& RouteState,
		const UAudioComponent& SourceAudio,
		UAudioComponent& ProxyAudio) const;

	void UpdateAllRoutes();
	void UpdateRoute(
		FWPPortalAudioRouteState& RouteState,
		const FVector& ListenerLocation,
		double NowRealSeconds);
	void UpdateRouteOcclusion(
		FWPPortalAudioRouteState& RouteState,
		const FVector& SourceLocation,
		const FVector& SourceEntrySurfacePoint,
		const FVector& ExitSurfacePoint,
		const FVector& ListenerLocation,
		const FVector& SourceDirection,
		const FVector& ExitDirection,
		double NowRealSeconds) const;

	bool ResolvePrimaryListener(FTransform& OutListenerTransform) const;
	bool IsSegmentOccluded(
		const FVector& Start,
		const FVector& End,
		const UAudioComponent& SourceAudio,
		const AWormholePortalActor& EntryPortal,
		const AWormholePortalActor& ExitPortal) const;

	void DestroyRouteProxy(FWPPortalAudioRouteState& RouteState);
	void DestroyRouteState(FWPPortalAudioRouteState& RouteState);
	void DestroyAllRouteStates();

	void HandleActorSpawned(AActor* SpawnedActor);
	void HandleWorldPostActorTick(UWorld* TickedWorld, ELevelTick TickType, float DeltaSeconds);
	void HandlePortalPairAdded(const FWPPortalPairSnapshot& PairSnapshot);
	void HandlePortalPairRemoved(const FWPPortalPairSnapshot& PairSnapshot);

private:
	TWeakObjectPtr<UWPRegistrySubsystem> RegistrySubsystem;
	TWeakObjectPtr<AActor> ProxyHostActor;

	UPROPERTY(Transient)
	TObjectPtr<USoundClass> PortalBusSoundClass;

	TArray<FWPTrackedPortalAudioSource> TrackedSources;
	TSet<TObjectKey<UAudioComponent>> TrackedSourceKeys;
	TArray<TWeakObjectPtr<AActor>> PendingSpawnedActors;

	TMap<FGuid, FWPPortalAudioActivePairState> ActivePairs;
	TMap<FWPPortalAudioRouteKey, FWPPortalAudioRouteState> RouteStates;

	double NextFullSourceReconcileRealSeconds = 0.0;

	FDelegateHandle ActorSpawnedHandle;
	FDelegateHandle WorldPostActorTickHandle;
	FDelegateHandle PortalPairAddedHandle;
	FDelegateHandle PortalPairRemovedHandle;

	bool bPairTopologyDirty = true;
	bool bDeinitializing = false;
};
