// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "WPPortalStreamingSubsystem.generated.h"

class AWormholePortalActor;
class UWPRegistrySubsystem;
enum class EWPPortalChangeType : uint8;

/**
 * World-scoped portal streaming demand owner.
 *
 * Stage 8 removes per-Actor Tick without changing World Partition behavior. This
 * subsystem evaluates authored portal streaming settings, reference-counts every
 * destination source, and releases all requests on link/lifetime changes. It ticks
 * on dedicated servers as well as clients because streaming readiness participates
 * in the gameplay transit gate.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPPortalStreamingSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	/** Test/diagnostic counts; Game Thread only. */
	int32 GetTrackedPortalCount() const { return PortalStates.Num(); }
	int32 GetActiveRequestCount() const;
	int32 GetActiveDestinationCount() const { return DestinationDemandCounts.Num(); }

private:
	struct FPortalStreamingState
	{
		TWeakObjectPtr<AWormholePortalActor> Portal;
		TWeakObjectPtr<AWormholePortalActor> RequestedDestination;
		TObjectKey<AWormholePortalActor> RequestedDestinationKey;
		float QueryElapsedSeconds = 0.0f;
		uint64 EvaluationCount = 0;
		bool bHasDestinationRequest = false;
	};

	void TrackPortal(AWormholePortalActor* Portal, const TCHAR* Reason);
	void UntrackPortal(AWormholePortalActor* Portal, const TCHAR* Reason);
	void EvaluatePortal(FPortalStreamingState& State);
	void SetDestinationRequest(
		FPortalStreamingState& State,
		AWormholePortalActor* Destination,
		bool bRequested,
		const TCHAR* Reason);
	void ReleaseDestinationRequest(FPortalStreamingState& State, const TCHAR* Reason);
	void ReleaseRequestsTargeting(AWormholePortalActor* Destination, const TCHAR* Reason);
	bool HasNearbyDemand(const AWormholePortalActor& Portal, float MaximumDistance);

	void HandlePortalRegistered(AWormholePortalActor* Portal);
	void HandlePortalUnregistered(AWormholePortalActor* Portal);
	void HandlePortalChanged(AWormholePortalActor* Portal, EWPPortalChangeType ChangeType);

private:
	TWeakObjectPtr<UWPRegistrySubsystem> RegistrySubsystem;
	TMap<TWeakObjectPtr<AWormholePortalActor>, FPortalStreamingState> PortalStates;
	TMap<TObjectKey<AWormholePortalActor>, int32> DestinationDemandCounts;
	FDelegateHandle PortalRegisteredHandle;
	FDelegateHandle PortalUnregisteredHandle;
	FDelegateHandle PortalChangedHandle;

	// Log-only: Accumulates counts and CPU costs for periodic and shutdown performance summaries.
	float SummaryElapsedSeconds = 0.0f;
	uint64 SummaryTickCount = 0;
	uint64 SummaryEvaluationCount = 0;
	uint64 SummaryDemandQueryCount = 0;
	uint64 SummaryRequestAcquireCount = 0;
	uint64 SummaryRequestReleaseCount = 0;
	double SummaryCpuMs = 0.0;
	double SummaryMaxTickCpuMs = 0.0;
	double SummaryDemandQueryCpuMs = 0.0;
	double SummaryMaxDemandQueryCpuMs = 0.0;
	bool bDeinitializing = false;
};
