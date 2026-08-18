// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "Subsystems/EngineSubsystem.h"
#include "UObject/SoftObjectPtr.h"
#include "WPLUTCacheSubsystem.generated.h"

struct FStreamableHandle;
class UWPLUTAsset;
class UWPLUTCatalog;

struct WORMHOLEPORTALRUNTIME_API FWPLUTRequest
{
	/** Sampling contract requested by the consumer. Ratio bounds may be resolved to a catalog asset's bounds. */
	FWPLUTDescriptor Descriptor = FWPLUTDescriptor::MakeDefault();
	/** T/rho. Must be positive; T==0 uses the caller's explicit no-lens path and never requests a LUT. */
	float TransitionRatio = 1.0f;
	TSoftObjectPtr<UWPLUTAsset> PreferredAsset;
	bool bAllowRuntimeFallback = true;
	/** Log-only: Context used to identify the request source in cache-hit and queue logs. */
	FString DebugContext;
};

struct WORMHOLEPORTALRUNTIME_API FWPLUTRequestHandle
{
	uint64 RequestId = 0;
	bool IsValid() const { return RequestId != 0; }
	void Reset() { RequestId = 0; }
};

DECLARE_DELEGATE_OneParam(FWPLUTRequestComplete, const FWPLUTBinding&);

/**
 * Process-wide normalized LUT cache shared by all worlds and PIE instances.
 * Asset loads and same-key fallback requests are coalesced. CPU fallback work
 * runs asynchronously and parallelizes rows; only UObject creation/upload is
 * returned to the game thread.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPLUTCacheSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Must be called on the game thread. Completion is always invoked on the game thread. */
	FWPLUTRequestHandle RequestLUT(
		const FWPLUTRequest& Request,
		FWPLUTRequestComplete Completion);

	void CancelRequest(FWPLUTRequestHandle Handle);

	/** Test/editor-session utility. Does not release resources owned by persistent assets. */
	void ResetCache();

private:
	struct FWaiter;
	struct FCacheEntry;

	FWPLUTRequestHandle RequestResolved(
		const FWPLUTRequest& Request,
		FWPLUTRequestComplete Completion,
		const FWPLUTDescriptor* ResolvedDescriptor,
		TSoftObjectPtr<UWPLUTAsset> ResolvedAsset);
	
	void AttachFallbackWaiter(FWaiter&& Waiter);
	void TryLoadDefaultCatalog();
	void StartAssetLoad(const FString& CacheKey, const TSharedPtr<FCacheEntry>& Entry);
	void HandleAssetLoadComplete(FString CacheKey, uint64 Generation);
	void QueueFallback(const FString& CacheKey, const TSharedPtr<FCacheEntry>& Entry);
	void PumpFallbackQueue();
	void StartFallbackBuild(const FString& CacheKey, const TSharedPtr<FCacheEntry>& Entry);
	void HandleFallbackBuildComplete(
		FString CacheKey,
		uint64 Generation,
		TSharedPtr<FWPLUTVolumeData, ESPMode::ThreadSafe> VolumeData,
		TSharedPtr<FWPLUTBuildStats, ESPMode::ThreadSafe> BuildStats,
		bool bSucceeded);
	
	void CompleteReadyEntry(const FString& CacheKey, const TSharedPtr<FCacheEntry>& Entry);
	void FailEntry(const FString& CacheKey, const TSharedPtr<FCacheEntry>& Entry, const FString& Error);
	FWPLUTBinding MakeBinding(const FCacheEntry& Entry, float TransitionRatio) const;
	static class UVolumeTexture* CreateTransientVolume(const FWPLUTVolumeData& VolumeData, const FString& CacheKey);

private:
	TMap<FString, TSharedPtr<FCacheEntry>> Entries;
	TArray<FString> PendingFallbackKeys;
	
	/** Generation tickets release their slot exactly once, including cancellation/reset races. */
	TSet<uint64> ActiveFallbackGenerations;
	uint64 NextRequestId = 1;
	uint64 NextEntryGeneration = 1;
	uint32 NextResourceRevision = 1;
	bool bShuttingDown = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UWPLUTAsset>> ResidentAssets;

	/** Tiny soft-reference index; volumes referenced by it still load asynchronously. */
	UPROPERTY(Transient)
	TObjectPtr<UWPLUTCatalog> DefaultCatalog = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UVolumeTexture>> ResidentTextures;
};
