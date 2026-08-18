// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "Rendering/WPRenderTypes.h"
#include "Subsystem/WPLUTCacheSubsystem.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "WPLUTEndpointManager.generated.h"

class AWormholePortalActor;
class UWPLUTAsset;
class UVolumeTexture;
class UWorld;

/**
 * Per-endpoint request policy supplied by the World-scoped runtime coordinator.
 *
 * The default policy reads the project descriptor and fallback switch from UWPSettings. A caller that
 * has access to authored Actor data can additionally provide the endpoint's preferred baked asset.
 */
struct WORMHOLEPORTALRUNTIME_API FWPLUTEndpointRequestOptions
{
	TSoftObjectPtr<UWPLUTAsset> PreferredAsset;
	FWPLUTDescriptor DescriptorOverride = FWPLUTDescriptor::MakeDefault();
	FString DebugContext;
	bool bOverrideDescriptor = false;
	bool bOverrideRuntimeFallback = false;
	bool bAllowRuntimeFallback = true;
};

/** Immutable Game Thread snapshot returned to RuntimeSubsystem/renderer packet construction. */
struct WORMHOLEPORTALRUNTIME_API FWPLUTEndpointSnapshot
{
	TWeakObjectPtr<AWormholePortalActor> Portal;
	TWeakObjectPtr<UVolumeTexture> VolumeTexture;
	/** Shared CPU view of the bound LUT. This pointer does not duplicate voxel memory per Endpoint or Pair. */
	TSharedPtr<const FWPLUTVolumeData, ESPMode::ThreadSafe> CPUVolumeData;
	FWPLUTDescriptor RequestedDescriptor = FWPLUTDescriptor::MakeDefault();
	FWPLUTDescriptor BoundDescriptor = FWPLUTDescriptor::MakeDefault();
	FWPRayLUTContract Contract;
	float PortalRadiusCm = 0.0f;
	float ThroatHalfLengthCm = 0.0f;
	float TransitionLengthCm = 0.0f;
	float TransitionRatio = 0.0f;
	float RatioCoordinate01 = 0.0f;
	float NormalizedOuterRadius = 1.0f;
	float MetricOuterRadiusCm = 0.0f;
	uint32 BindingGeneration = 0;
	uint32 ResourceRevision = 0;
	uint32 EndpointRevision = 0;
	uint64 RequestGeneration = 0;
	bool bRegistered = false;
	bool bRequestPending = false;
	bool bAnalyticNoTransition = false;
	FString LastError;

	bool IsReady() const
	{
		return bRegistered && !bRequestPending
			&& (bAnalyticNoTransition || (VolumeTexture.IsValid() && Contract.IsValid()));
	}
};

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnWPLUTEndpointChanged,
	TWeakObjectPtr<AWormholePortalActor>,
	const FWPLUTEndpointSnapshot&);

/**
 * World-scoped owner of endpoint LUT request and binding state.
 *
 * UWPLUTCacheSubsystem remains the process-wide asset/fallback resource cache. This manager owns only
 * World endpoint waiters, request generations, binding identity, contract generation, and metric snapshots.
 * Actor identity is stored exclusively as a weak key; this UObject never keeps a Portal Actor alive.
 *
 * The intended owner is UWPRuntimeSubsystem. Registry integration remains explicit so initialization can
 * bind delegates before bootstrap and teardown can order pair rollback before endpoint release.
 */
UCLASS(Transient)
class WORMHOLEPORTALRUNTIME_API UWPLUTEndpointManager : public UObject
{
	GENERATED_BODY()

public:
	virtual UWorld* GetWorld() const override;

	void Initialize(UWorld* InWorld);
	void Shutdown(const TCHAR* Reason);

	/** Idempotently adds an endpoint and starts (or synchronously resolves) its current T/rho request. */
	bool RegisterEndpoint(
		AWormholePortalActor* Portal,
		const FWPLUTEndpointRequestOptions& Options = FWPLUTEndpointRequestOptions());

	/** Re-reads Actor metric values while preserving the endpoint's last request options. */
	bool RefreshEndpoint(AWormholePortalActor* Portal);

	/** Re-reads Actor metric values and atomically replaces the endpoint request options. */
	bool RefreshEndpoint(
		AWormholePortalActor* Portal,
		const FWPLUTEndpointRequestOptions& Options);

	/** Cancels the waiter, drops the binding, broadcasts a final unregistered snapshot, and erases the weak key. */
	bool UnregisterEndpoint(AWormholePortalActor* Portal, const TCHAR* Reason);

	/** Cancels a pending waiter without removing an otherwise registered endpoint. */
	bool CancelEndpointRequest(AWormholePortalActor* Portal, const TCHAR* Reason);

	/** Removes invalid weak endpoints and cancels any waiters they left behind. */
	int32 CompactInvalidEndpoints(const TCHAR* Reason);

	bool GetEndpointSnapshot(
		const AWormholePortalActor* Portal,
		FWPLUTEndpointSnapshot& OutSnapshot) const;
	void GetEndpointSnapshots(TArray<FWPLUTEndpointSnapshot>& OutSnapshots) const;

	bool HasEndpoint(const AWormholePortalActor* Portal) const;
	int32 GetEndpointCount() const { return EndpointStates.Num(); }
	int32 GetPendingRequestCount() const;
	int32 GetReadyBindingCount() const;

	FOnWPLUTEndpointChanged& OnEndpointChanged() { return EndpointChangedDelegate; }

private:
	struct FEndpointState
	{
		TWeakObjectPtr<AWormholePortalActor> Portal;
		FWPLUTEndpointRequestOptions Options;
		FWPLUTRequestHandle RequestHandle;
		FWPLUTBinding Binding;
		FWPLUTDescriptor RequestedDescriptor = FWPLUTDescriptor::MakeDefault();
		FWPRayLUTContract Contract;
		TWeakObjectPtr<UVolumeTexture> PreviousTextureForRequest;
		FWPLUTDescriptor PreviousDescriptorForRequest = FWPLUTDescriptor::MakeDefault();
		uint32 PreviousRevisionForRequest = 0;
		float PortalRadiusCm = 0.0f;
		float ThroatHalfLengthCm = 0.0f;
		float TransitionLengthCm = 0.0f;
		float TransitionRatio = 0.0f;
		float RatioCoordinate01 = 0.0f;
		float NormalizedOuterRadius = 1.0f;
		float MetricOuterRadiusCm = 0.0f;
		uint32 BindingGeneration = 0;
		uint32 EndpointRevision = 0;
		uint64 RequestGeneration = 0;
		bool bRequestPending = false;
		bool bAnalyticNoTransition = false;
		FString LastError;
	};

	bool RefreshEndpointInternal(
		AWormholePortalActor* Portal,
		const FWPLUTEndpointRequestOptions& Options,
		bool bAllowAdd);
	void HandleRequestComplete(
		const FWPLUTBinding& Binding,
		TWeakObjectPtr<AWormholePortalActor> PortalKey,
		uint64 RequestGeneration);
	void CancelStateRequest(FEndpointState& State, const TCHAR* Reason);
	void ClearBinding(FEndpointState& State, bool bResetPreviousIdentity);
	void RefreshMetricSnapshot(FEndpointState& State, const AWormholePortalActor& Portal);
	void RebuildStrongTextureReferences();
	/** Log-only: StartSeconds is used only to calculate CPU time for state-broadcast logs. */
	void BroadcastState(const FEndpointState& State, const TCHAR* Phase, double StartSeconds);
	FWPLUTEndpointSnapshot MakeSnapshot(const FEndpointState& State, bool bRegistered) const;
	static FWPRayLUTContract MakeContract(const FEndpointState& State);
	static uint64 AdvanceGeneration(uint64 Generation);
	static uint32 AdvanceGeneration(uint32 Generation);
	static bool RequestOptionsEqual(
		const FWPLUTEndpointRequestOptions& A,
		const FWPLUTEndpointRequestOptions& B);

private:
	TWeakObjectPtr<UWorld> ManagedWorld;
	TMap<TWeakObjectPtr<AWormholePortalActor>, FEndpointState> EndpointStates;

	/** EndpointStates is not reflected, so this traced array is the binding lifetime owner. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVolumeTexture>> BoundTextures;

	FOnWPLUTEndpointChanged EndpointChangedDelegate;
	// Log-only: Aggregates endpoint request lifetime and CPU cost for shutdown and state logs.
	uint64 RegisterCount = 0;
	uint64 UnregisterCount = 0;
	uint64 RequestCount = 0;
	uint64 CancelCount = 0;
	uint64 CompletionCount = 0;
	uint64 FailureCount = 0;
	uint64 StaleCompletionCount = 0;
	double TotalRequestCpuMs = 0.0;
	double MaxRequestCpuMs = 0.0;
	double TotalCompletionCpuMs = 0.0;
	double MaxCompletionCpuMs = 0.0;
	bool bInitialized = false;
	bool bShuttingDown = false;
};
