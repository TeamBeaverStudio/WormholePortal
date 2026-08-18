// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPLUTCacheSubsystem.h"

#include "Async/Async.h"
#include "Rendering/LUT/WPLUTAsset.h"
#include "Rendering/LUT/WPLUTCatalog.h"
#include "Rendering/LUT/WPLUTGenerator.h"
#include "Engine/AssetManager.h"
#include "Engine/VolumeTexture.h"
#include "Engine/StreamableManager.h"
#include "TextureResource.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "WormholePortalStats.h"
#include "WPLog.h"
#include "WPSettings.h"

namespace
{
	constexpr int32 MaxConcurrentFallbackBuilds = 1;

#if WITH_EDITOR
	void FinishEditorVolumeTextureBuild(UVolumeTexture* VolumeTexture)
	{
		if (!VolumeTexture)
		{
			return;
		}

		// Async package loading may complete before editor texture compilation.
		// Validation needs the real volume dimensions/format, not the placeholder.
		VolumeTexture->BlockOnAnyAsyncBuild();
	}
#endif

	enum class ECacheEntryState : uint8
	{
		LoadingAsset,
		QueuedFallback,
		BuildingFallback,
		Ready,
		Failed
	};

	FString MakeAssetCacheKey(const TSoftObjectPtr<UWPLUTAsset>& Asset)
	{
		return FString::Printf(TEXT("Asset|%s"), *Asset.ToSoftObjectPath().ToString());
	}

	FString MakeBuildCacheKey(const FWPLUTDescriptor& Descriptor)
	{
		return FString::Printf(TEXT("Build|%s"), *Descriptor.MakeBuildHash());
	}

	FWPLUTBinding MakeFailureBinding(const FString& Error)
	{
		FWPLUTBinding Binding;
		Binding.Error = Error;
		return Binding;
	}

	TSharedPtr<const FWPLUTVolumeData, ESPMode::ThreadSafe> CopyVolumeTextureToCPU(
		UVolumeTexture* Texture, const FWPLUTDescriptor& InDescriptor, FString& OutError)
	{
		OutError.Reset();
		if (!Texture || !Texture->GetPlatformData())
		{
			OutError = TEXT("Volume texture platform data is unavailable.");
			return nullptr;
		}

		const FWPLUTDescriptor Descriptor = InDescriptor.GetSanitized();
		const FIntVector ExpectedDimensions = Descriptor.GetDimensions();
		FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		if (PlatformData->PixelFormat != PF_A32B32G32R32F || PlatformData->Mips.Num() != 1
			|| PlatformData->SizeX != ExpectedDimensions.X || PlatformData->SizeY != ExpectedDimensions.Y)
		{
			OutError = TEXT("Volume texture platform layout does not match the LUT descriptor.");
			return nullptr;
		}

		FTexture2DMipMap& Mip = PlatformData->Mips[0];
		const int64 ExpectedBytes = static_cast<int64>(ExpectedDimensions.X) * ExpectedDimensions.Y
			* ExpectedDimensions.Z * sizeof(FLinearColor);
		if (Mip.SizeX != ExpectedDimensions.X || Mip.SizeY != ExpectedDimensions.Y || Mip.SizeZ != ExpectedDimensions.Z
			|| Mip.BulkData.GetBulkDataSize() != ExpectedBytes)
		{
			OutError = TEXT("Volume texture mip bytes do not match the expected RGBA32F volume.");
			return nullptr;
		}

		const void* Source = Mip.BulkData.LockReadOnly();
		if (!Source)
		{
			Mip.BulkData.Unlock();
			OutError = TEXT("Volume texture mip could not be locked for CPU prediction.");
			return nullptr;
		}

		TSharedPtr<FWPLUTVolumeData, ESPMode::ThreadSafe> Result = MakeShared<FWPLUTVolumeData, ESPMode::ThreadSafe>();
		Result->Descriptor = Descriptor;
		Result->Dimensions = ExpectedDimensions;
		Result->BuildHash = Descriptor.MakeBuildHash();
		Result->Voxels.SetNumUninitialized(ExpectedDimensions.X * ExpectedDimensions.Y * ExpectedDimensions.Z);
		FMemory::Memcpy(Result->Voxels.GetData(), Source, ExpectedBytes);
		Mip.BulkData.Unlock();
		return Result;
	}
}

struct UWPLUTCacheSubsystem::FWaiter
{
	uint64 RequestId = 0;
	float TransitionRatio = 1.0f;
	bool bAllowRuntimeFallback = true;

	// Log-only: Preserves context for logging; the current waiter completion path does not read it or use it for behavior.
	FString DebugContext;
	FWPLUTRequestComplete Completion;
};

struct UWPLUTCacheSubsystem::FCacheEntry
{
	FString CacheKey;
	FWPLUTDescriptor Descriptor;
	TSoftObjectPtr<UWPLUTAsset> Asset;
	TObjectPtr<UWPLUTAsset> LoadedAsset = nullptr;
	TObjectPtr<UVolumeTexture> Texture = nullptr;
	TSharedPtr<const FWPLUTVolumeData, ESPMode::ThreadSafe> CPUVolumeData;

	TArray<FWaiter> Waiters;
	TSharedPtr<FStreamableHandle> StreamableHandle;
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> CancelToken;

	ECacheEntryState State = ECacheEntryState::Failed;
	EWPLUTSource Source = EWPLUTSource::None;

	uint64 Generation = 0;
	uint32 ResourceRevision = 0;

	// Log-only: Baseline timestamp for measuring request lifetime through completion or failure.
	double CreatedSeconds = 0.0;
	FString Error;
};

template <typename TWaiterType>
void ClampWaiterTransitionRatio(
	TWaiterType& Waiter,
	const FWPLUTDescriptor& InDescriptor,
	const TCHAR* DomainSource)
{
	const FWPLUTDescriptor Descriptor = InDescriptor.GetSanitized();
	const float RequestedRatio = Waiter.TransitionRatio;
	Waiter.TransitionRatio = Descriptor.ClampTransitionRatio(RequestedRatio);
	if (!FMath::IsNearlyEqual(
		RequestedRatio,
		Waiter.TransitionRatio,
		FMath::Max(1.0e-5f, RequestedRatio * 1.0e-5f)))
	{
		WP_LOG(nullptr, Warning,
			TEXT("LUT transition ratio clamped. Source=%s Context=%s RequestedRatio=%.6f ClampedRatio=%.6f Domain=[%.6f, %.6f]"),
			DomainSource ? DomainSource : TEXT("Unknown"), *Waiter.DebugContext,
			RequestedRatio, Waiter.TransitionRatio,
			Descriptor.TransitionRatioMin, Descriptor.TransitionRatioMax);
	}
}

void UWPLUTCacheSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	bShuttingDown = false;
	TryLoadDefaultCatalog();
}

void UWPLUTCacheSubsystem::Deinitialize()
{
	bShuttingDown = true;
	for (TPair<FString, TSharedPtr<FCacheEntry>>& Pair : Entries)
	{
		const TSharedPtr<FCacheEntry>& Entry = Pair.Value;
		if (Entry->CancelToken)
		{
			Entry->CancelToken->Store(true);
		}
		if (Entry->StreamableHandle)
		{
			Entry->StreamableHandle->CancelHandle();
		}
	}
	
	Entries.Reset();
	PendingFallbackKeys.Reset();
	ResidentAssets.Reset();
	ResidentTextures.Reset();
	DefaultCatalog = nullptr;
	ActiveFallbackGenerations.Reset();
	
	Super::Deinitialize();
}

FWPLUTRequestHandle UWPLUTCacheSubsystem::RequestLUT(
	const FWPLUTRequest& Request,
	FWPLUTRequestComplete Completion)
{
	check(IsInGameThread());
	if (Request.PreferredAsset.IsNull())
	{
		TryLoadDefaultCatalog();
		if (DefaultCatalog)
		{
			FWPLUTDescriptor ActiveDescriptor;
			const TSoftObjectPtr<UWPLUTAsset> ActiveAsset =
				DefaultCatalog->GetActiveLUTAsset(&ActiveDescriptor);
			if (!ActiveAsset.IsNull())
			{
				return RequestResolved(Request, MoveTemp(Completion), &ActiveDescriptor, ActiveAsset);
			}
		}
	}
	return RequestResolved(Request, MoveTemp(Completion), nullptr, Request.PreferredAsset);
}

void UWPLUTCacheSubsystem::TryLoadDefaultCatalog()
{
	check(IsInGameThread());
	if (!DefaultCatalog && !bShuttingDown)
	{
		const UWPSettings* Settings = GetDefault<UWPSettings>();
		FString RootPath;
		FString CatalogObjectPath;
		FString PathError;
		if (!IsValid(Settings)
			|| !Settings->TryResolveGeneratedLUTPaths(RootPath, CatalogObjectPath, PathError))
		{
			WP_LOG(this, Warning, TEXT("%s"), *PathError);
			return;
		}
		DefaultCatalog = LoadObject<UWPLUTCatalog>(nullptr, *CatalogObjectPath);
	}
}

FWPLUTRequestHandle UWPLUTCacheSubsystem::RequestResolved(
	const FWPLUTRequest& InRequest,
	FWPLUTRequestComplete Completion,
	const FWPLUTDescriptor* ResolvedDescriptor,
	TSoftObjectPtr<UWPLUTAsset> ResolvedAsset)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_LUTRequest);
	INC_DWORD_STAT(STAT_WP_LUTRequests);
	check(IsInGameThread());
#if !UE_BUILD_SHIPPING
	const double RequestStartSeconds = FPlatformTime::Seconds();
#endif
	FWPLUTRequestHandle Handle;
	
	if (bShuttingDown)
	{
		if (Completion.IsBound())
		{
			Completion.Execute(MakeFailureBinding(TEXT("LUT cache is shutting down.")));
		}
		return Handle;
	}
	
	if (IsRunningDedicatedServer())
	{
		if (Completion.IsBound())
		{
			Completion.Execute(MakeFailureBinding(TEXT("LUTs are disabled on dedicated servers.")));
		}
		return Handle;
	}

	FWPLUTRequest Request = InRequest;
	Request.Descriptor = Request.Descriptor.GetSanitized();
	FString ValidationError;
	if (!Request.Descriptor.IsValid(&ValidationError)
		|| !FMath::IsFinite(Request.TransitionRatio)
		|| Request.TransitionRatio <= 0.0f)
	{
		if (ValidationError.IsEmpty())
		{
			ValidationError = TEXT("TransitionRatio must be finite and positive.");
		}
		if (Completion.IsBound())
		{
			Completion.Execute(MakeFailureBinding(ValidationError));
		}
		return Handle;
	}

	FWaiter Waiter;
	Waiter.RequestId = NextRequestId++;
	Waiter.TransitionRatio = Request.TransitionRatio;
	Waiter.bAllowRuntimeFallback = Request.bAllowRuntimeFallback;
	Waiter.DebugContext = Request.DebugContext;
	Waiter.Completion = MoveTemp(Completion);
	Handle.RequestId = Waiter.RequestId;
	if (ResolvedDescriptor)
	{
		ClampWaiterTransitionRatio(Waiter, *ResolvedDescriptor, TEXT("ResolvedBakedAsset"));
	}
	else if (!ResolvedAsset.IsNull())
	{
		ClampWaiterTransitionRatio(Waiter, Request.Descriptor, TEXT("PreferredBakedAsset"));
	}

	if (!ResolvedAsset.IsNull())
	{
		const FString CacheKey = MakeAssetCacheKey(ResolvedAsset);
		TSharedPtr<FCacheEntry>* ExistingEntry = Entries.Find(CacheKey);
		
		// Cache hit
		if (ExistingEntry)
		{
			if ((*ExistingEntry)->State == ECacheEntryState::Ready)
			{
				ClampWaiterTransitionRatio(
					Waiter, (*ExistingEntry)->Descriptor, TEXT("ReadyBakedAsset"));
				const FWPLUTBinding ExactAssetBinding =
					MakeBinding(**ExistingEntry, Waiter.TransitionRatio);
				INC_DWORD_STAT(STAT_WP_LUTCacheHits);

				if (Waiter.Completion.IsBound())
				{
					Waiter.Completion.Execute(ExactAssetBinding);
				}
			}
			else
			{
				(*ExistingEntry)->Waiters.Add(MoveTemp(Waiter));
			}
			return Handle;
		}

		// Cache miss
		TSharedPtr<FCacheEntry> Entry = MakeShared<FCacheEntry>();
		Entry->CacheKey = CacheKey;
		Entry->Descriptor = ResolvedDescriptor
			? ResolvedDescriptor->GetSanitized()
			: Request.Descriptor;
		Entry->Asset = ResolvedAsset;
		Entry->Waiters.Add(MoveTemp(Waiter));
		Entry->Generation = NextEntryGeneration++;
		Entry->CreatedSeconds = FPlatformTime::Seconds();
		Entry->State = ECacheEntryState::LoadingAsset;
		Entries.Add(CacheKey, Entry);
#if !UE_BUILD_SHIPPING
		WP_LOG(this, VeryVerbose,
			TEXT("LUT asset request queued. RequestId=%llu Key=%s Resolution=ResolvedAssetExact AssetIdentityRequest=1 Context=%s CpuMs=%.4f"),
			Handle.RequestId, *CacheKey, *Request.DebugContext,
			(FPlatformTime::Seconds() - RequestStartSeconds) * 1000.0);
#endif
		StartAssetLoad(CacheKey, Entry);
		return Handle;
	}

	AttachFallbackWaiter(MoveTemp(Waiter));
	return Handle;
}

void UWPLUTCacheSubsystem::AttachFallbackWaiter(FWaiter&& Waiter)
{
	check(IsInGameThread());
	if (!Waiter.bAllowRuntimeFallback)
	{
		if (Waiter.Completion.IsBound())
		{
			Waiter.Completion.Execute(MakeFailureBinding(TEXT("No compatible baked LUT and runtime fallback is disabled.")));
		}
		return;
	}

	FWPLUTDescriptor Descriptor = FWPLUTDescriptor::MakeDefault().GetSanitized();
	ClampWaiterTransitionRatio(Waiter, Descriptor, TEXT("BalancedStandardFallback"));
	const FString CacheKey = MakeBuildCacheKey(Descriptor);
	if (TSharedPtr<FCacheEntry>* ExistingEntry = Entries.Find(CacheKey))
	{
		if ((*ExistingEntry)->State == ECacheEntryState::Ready)
		{
			if (Waiter.Completion.IsBound())
			{
				Waiter.Completion.Execute(MakeBinding(**ExistingEntry, Waiter.TransitionRatio));
			}
		}
		else
		{
			(*ExistingEntry)->Waiters.Add(MoveTemp(Waiter));
		}
		return;
	}

	TSharedPtr<FCacheEntry> Entry = MakeShared<FCacheEntry>();
	Entry->CacheKey = CacheKey;
	Entry->Descriptor = Descriptor;
	Entry->Waiters.Add(MoveTemp(Waiter));
	Entry->Generation = NextEntryGeneration++;
	Entry->CreatedSeconds = FPlatformTime::Seconds();
	Entries.Add(CacheKey, Entry);
	QueueFallback(CacheKey, Entry);
}

void UWPLUTCacheSubsystem::StartAssetLoad(
	const FString& CacheKey,
	const TSharedPtr<FCacheEntry>& Entry)
{
	check(IsInGameThread());
	if (!Entry || Entry->Asset.IsNull())
	{
		FailEntry(CacheKey, Entry, TEXT("Baked LUT asset path is null."));
		return;
	}

	INC_DWORD_STAT(STAT_WP_LUTAssetLoads);
	if (UWPLUTAsset* AlreadyLoaded = Entry->Asset.Get())
	{
		HandleAssetLoadComplete(CacheKey, Entry->Generation);
		return;
	}

	const uint64 Generation = Entry->Generation;
	TWeakObjectPtr<UWPLUTCacheSubsystem> WeakThis(this);
	Entry->StreamableHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		Entry->Asset.ToSoftObjectPath(),
		FStreamableDelegate::CreateLambda([WeakThis, CacheKey, Generation]()
		{
			if (UWPLUTCacheSubsystem* StrongThis = WeakThis.Get())
			{
				StrongThis->HandleAssetLoadComplete(CacheKey, Generation);
			}
		}),
		FStreamableManager::AsyncLoadHighPriority,
		false,
		false,
		TEXT("WPLUTCache"));
	
	if (!Entry->StreamableHandle.IsValid())
	{
		HandleAssetLoadComplete(CacheKey, Generation);
	}
}

void UWPLUTCacheSubsystem::HandleAssetLoadComplete(const FString CacheKey, const uint64 Generation)
{
	check(IsInGameThread());
	SCOPE_CYCLE_COUNTER(STAT_WP_LUTAssetLoadComplete);
	// Log-only: Measures baked asset validation CPU time.
	const double CompletionStartSeconds = FPlatformTime::Seconds();
	TSharedPtr<FCacheEntry>* FoundEntry = Entries.Find(CacheKey);
	if (!FoundEntry || !FoundEntry->IsValid() || (*FoundEntry)->Generation != Generation || bShuttingDown)
	{
		return;
	}
	const TSharedPtr<FCacheEntry> Entry = *FoundEntry;
	Entry->StreamableHandle.Reset();
	UWPLUTAsset* LoadedAsset = Entry->Asset.Get();

#if WITH_EDITOR
	// UVolumeTexture async package load can finish before its editor platform data.
	// Exact size/format validation must not inspect the temporary default texture.
	if (LoadedAsset && LoadedAsset->VolumeTexture)
	{
		FinishEditorVolumeTextureBuild(LoadedAsset->VolumeTexture);
	}
#endif

	FString Error;
	const bool bAssetValid = LoadedAsset && LoadedAsset->Validate(&Error);
	
	if (!bAssetValid)
	{
		if (Error.IsEmpty())
		{
			Error = TEXT("Baked LUT asset failed to load or validate.");
		}

		// Log-only: Reports how long the rejected asset entry remained in the cache.
		const double LifetimeMs = Entry->CreatedSeconds > 0.0
			? (FPlatformTime::Seconds() - Entry->CreatedSeconds) * 1000.0
			: 0.0;
		WP_LOG(this, Warning,
			TEXT("Baked LUT rejected; using runtime fallback when allowed. Asset=%s Loaded=%d Texture=%s Error=%s Waiters=%d LifetimeMs=%.3f ValidationCpuMs=%.3f"),
			*Entry->Asset.ToSoftObjectPath().ToString(),
			LoadedAsset ? 1 : 0,
			*GetNameSafe(LoadedAsset ? LoadedAsset->VolumeTexture : nullptr),
			*Error,
			Entry->Waiters.Num(),
			LifetimeMs,
			(FPlatformTime::Seconds() - CompletionStartSeconds) * 1000.0);

		TArray<FWaiter> Waiters = MoveTemp(Entry->Waiters);
		Entries.Remove(CacheKey);
		for (FWaiter& Waiter : Waiters)
		{
			if (Waiter.bAllowRuntimeFallback)
			{
				AttachFallbackWaiter(MoveTemp(Waiter));
			}
			else if (Waiter.Completion.IsBound())
			{
				Waiter.Completion.Execute(MakeFailureBinding(Error));
			}
		}
		return;
	}

	Entry->LoadedAsset = LoadedAsset;
	Entry->Texture = LoadedAsset->VolumeTexture;
	Entry->Descriptor = LoadedAsset->Descriptor.GetSanitized();
	const double CPUCopyStartSeconds = FPlatformTime::Seconds();
	FString CPUCopyError;
	Entry->CPUVolumeData = CopyVolumeTextureToCPU(LoadedAsset->VolumeTexture, Entry->Descriptor, CPUCopyError);
	if (!Entry->CPUVolumeData.IsValid())
	{
		WP_LOG(this, Warning,
			TEXT("Baked LUT CPU prediction copy unavailable; face prediction will fail open. Asset=%s Error=%s CpuMs=%.3f"),
			*Entry->Asset.ToSoftObjectPath().ToString(), *CPUCopyError,
			(FPlatformTime::Seconds() - CPUCopyStartSeconds) * 1000.0);
	}
	else
	{
		WP_LOG(this, Verbose,
			TEXT("Baked LUT CPU prediction copy ready. Asset=%s Bytes=%lld MiB=%.3f SharedPerCacheEntry=1 CpuMs=%.3f"),
			*Entry->Asset.ToSoftObjectPath().ToString(), Entry->CPUVolumeData->GetByteCount(),
			static_cast<double>(Entry->CPUVolumeData->GetByteCount()) / (1024.0 * 1024.0),
			(FPlatformTime::Seconds() - CPUCopyStartSeconds) * 1000.0);
	}
	Entry->Source = EWPLUTSource::BakedAsset;
	Entry->State = ECacheEntryState::Ready;
	Entry->ResourceRevision = NextResourceRevision++;
	ResidentAssets.AddUnique(LoadedAsset);
	ResidentTextures.AddUnique(LoadedAsset->VolumeTexture);

	for (FWaiter& Waiter : Entry->Waiters)
	{
		ClampWaiterTransitionRatio(Waiter, Entry->Descriptor, TEXT("LoadedBakedAsset"));
	}
	CompleteReadyEntry(CacheKey, Entry);
}

void UWPLUTCacheSubsystem::QueueFallback(
	const FString& CacheKey,
	const TSharedPtr<FCacheEntry>& Entry)
{
	check(IsInGameThread());
	if (!Entry || bShuttingDown)
	{
		return;
	}
	Entry->State = ECacheEntryState::QueuedFallback;
	Entry->CancelToken = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
	PendingFallbackKeys.AddUnique(CacheKey);
	PumpFallbackQueue();
}

void UWPLUTCacheSubsystem::PumpFallbackQueue()
{
	check(IsInGameThread());
	while (!bShuttingDown
		&& ActiveFallbackGenerations.Num() < MaxConcurrentFallbackBuilds
		&& PendingFallbackKeys.Num() > 0)
	{
		const FString CacheKey = PendingFallbackKeys[0];
		PendingFallbackKeys.RemoveAt(0, 1, EAllowShrinking::No);
		TSharedPtr<FCacheEntry>* FoundEntry = Entries.Find(CacheKey);
		if (!FoundEntry || !FoundEntry->IsValid()
			|| (*FoundEntry)->State != ECacheEntryState::QueuedFallback)
		{
			continue;
		}
		StartFallbackBuild(CacheKey, *FoundEntry);
	}
}

void UWPLUTCacheSubsystem::StartFallbackBuild(
	const FString& CacheKey,
	const TSharedPtr<FCacheEntry>& Entry)
{
	check(IsInGameThread());
	INC_DWORD_STAT(STAT_WP_LUTFallbackStarts);
	Entry->State = ECacheEntryState::BuildingFallback;
	ActiveFallbackGenerations.Add(Entry->Generation);

	const FWPLUTDescriptor Descriptor = Entry->Descriptor;
	const uint64 Generation = Entry->Generation;
	const TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> CancelToken = Entry->CancelToken;
	TWeakObjectPtr<UWPLUTCacheSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, CacheKey, Generation, Descriptor, CancelToken]()
	{
		SCOPE_CYCLE_COUNTER(STAT_WP_LUTFallbackBuild);
		TSharedPtr<FWPLUTVolumeData, ESPMode::ThreadSafe> VolumeData =
			MakeShared<FWPLUTVolumeData, ESPMode::ThreadSafe>();
		TSharedPtr<FWPLUTBuildStats, ESPMode::ThreadSafe> BuildStats =
			MakeShared<FWPLUTBuildStats, ESPMode::ThreadSafe>();
		const bool bSucceeded = FWPLUTGenerator::BuildVolumeData(
			Descriptor,
			*VolumeData,
			*BuildStats,
			[CancelToken]() { return CancelToken && CancelToken->Load(); });

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, CacheKey, Generation, VolumeData, BuildStats, bSucceeded]()
			{
				if (UWPLUTCacheSubsystem* StrongThis = WeakThis.Get())
				{
					StrongThis->HandleFallbackBuildComplete(
						CacheKey, Generation, VolumeData, BuildStats, bSucceeded);
				}
			});
	});
}

void UWPLUTCacheSubsystem::HandleFallbackBuildComplete(
	const FString CacheKey,
	const uint64 Generation,
	TSharedPtr<FWPLUTVolumeData, ESPMode::ThreadSafe> VolumeData,
	TSharedPtr<FWPLUTBuildStats, ESPMode::ThreadSafe> BuildStats,
	const bool bSucceeded)
{
	check(IsInGameThread());
	SCOPE_CYCLE_COUNTER(STAT_WP_LUTFallbackUpload);
	ActiveFallbackGenerations.Remove(Generation);
	TSharedPtr<FCacheEntry>* FoundEntry = Entries.Find(CacheKey);
	
	if (!FoundEntry || !FoundEntry->IsValid() || (*FoundEntry)->Generation != Generation || bShuttingDown)
	{
		PumpFallbackQueue();
		return;
	}
	
	const TSharedPtr<FCacheEntry> Entry = *FoundEntry;
	if (!bSucceeded || !VolumeData.IsValid())
	{
		const FString Error = BuildStats.IsValid() && !BuildStats->Error.IsEmpty()
			? BuildStats->Error
			: TEXT("Runtime LUT generation failed.");
		FailEntry(CacheKey, Entry, Error);
		PumpFallbackQueue();
		return;
	}

	UVolumeTexture* Texture = CreateTransientVolume(*VolumeData, CacheKey);
	if (!Texture)
	{
		FailEntry(CacheKey, Entry, TEXT("Runtime volume texture creation failed."));
		PumpFallbackQueue();
		return;
	}

	Entry->Texture = Texture;
	Entry->Descriptor = VolumeData->Descriptor;
	Entry->CPUVolumeData = VolumeData;
	Entry->Source = EWPLUTSource::RuntimeFallback;
	Entry->State = ECacheEntryState::Ready;
	Entry->ResourceRevision = NextResourceRevision++;
	Entry->CancelToken.Reset();
	ResidentTextures.Add(Texture);
	CompleteReadyEntry(CacheKey, Entry);
	PumpFallbackQueue();
}

void UWPLUTCacheSubsystem::CompleteReadyEntry(
	const FString& CacheKey,
	const TSharedPtr<FCacheEntry>& Entry)
{
	check(IsInGameThread());
	if (!Entry || Entry->State != ECacheEntryState::Ready || !Entry->Texture)
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	const double CompletionStartSeconds = FPlatformTime::Seconds();
#endif
	TArray<FWaiter> Waiters = MoveTemp(Entry->Waiters);
	Entry->Waiters.Reset();
	for (FWaiter& Waiter : Waiters)
	{
		if (Waiter.Completion.IsBound())
		{
			Waiter.Completion.Execute(MakeBinding(*Entry, Waiter.TransitionRatio));
		}
	}
#if !UE_BUILD_SHIPPING
	const double CallbackCpuMs =
		(FPlatformTime::Seconds() - CompletionStartSeconds) * 1000.0;
	const double LifetimeMs = Entry->CreatedSeconds > 0.0
		? (FPlatformTime::Seconds() - Entry->CreatedSeconds) * 1000.0
		: 0.0;
	WP_LOG(this, Verbose,
		TEXT("LUT ready. Key=%s Source=%s Waiters=%d Size=%dx%dx%d LifetimeMs=%.3f CallbackCpuMs=%.3f"),
		*CacheKey,
		Entry->Source == EWPLUTSource::BakedAsset ? TEXT("Asset") : TEXT("Fallback"),
		Waiters.Num(),
		Entry->Descriptor.ImpactSamples,
		Entry->Descriptor.TransitionSamples,
		Entry->Descriptor.RatioSamples,
		LifetimeMs,
		CallbackCpuMs);
#endif
}

void UWPLUTCacheSubsystem::FailEntry(
	const FString& CacheKey,
	const TSharedPtr<FCacheEntry>& Entry,
	const FString& Error)
{
	check(IsInGameThread());
	// Log-only: Measures failure callback CPU time and entry lifetime.
	const double FailureStartSeconds = FPlatformTime::Seconds();
	if (!Entry)
	{
		return;
	}
	Entry->State = ECacheEntryState::Failed;
	Entry->Error = Error;
	TArray<FWaiter> Waiters = MoveTemp(Entry->Waiters);
	Entry->Waiters.Reset();
	PendingFallbackKeys.Remove(CacheKey);
	ActiveFallbackGenerations.Remove(Entry->Generation);
	// Entries.Remove performs the functional removal; only its returned removal count is used for logging.
	const int32 RemovedEntryCount = Entries.Remove(CacheKey);

	// Completion code can synchronously issue the same request. The failed entry must no longer
	// occupy the key before callbacks run, otherwise that reentrant waiter is stranded on it.
	for (FWaiter& Waiter : Waiters)
	{
		if (Waiter.Completion.IsBound())
		{
			Waiter.Completion.Execute(MakeFailureBinding(Error));
		}
	}
	// Log-only: Holds the measured failure callback duration and total entry lifetime.
	const double CallbackCpuMs =
		(FPlatformTime::Seconds() - FailureStartSeconds) * 1000.0;
	const double LifetimeMs = Entry->CreatedSeconds > 0.0
		? (FPlatformTime::Seconds() - Entry->CreatedSeconds) * 1000.0
		: 0.0;
	WP_LOG(this, Error,
		TEXT("LUT request failed. Key=%s Error=%s Waiters=%d EntryRemovedBeforeCallbacks=%d ReentrantRequestSafe=1 LifetimeMs=%.3f CallbackCpuMs=%.3f"),
		*CacheKey, *Error, Waiters.Num(), RemovedEntryCount > 0 ? 1 : 0,
		LifetimeMs, CallbackCpuMs);
}

FWPLUTBinding UWPLUTCacheSubsystem::MakeBinding(
	const FCacheEntry& Entry,
	const float TransitionRatio) const
{
	FWPLUTBinding Binding;
	Binding.VolumeTexture = Entry.Texture;
	Binding.CPUVolumeData = Entry.CPUVolumeData;
	Binding.Descriptor = Entry.Descriptor;
	Binding.TransitionRatio = TransitionRatio;
	Binding.RatioCoordinate01 = Entry.Descriptor.ComputeRatioCoordinate01(TransitionRatio);
	Binding.NormalizedOuterRadius = static_cast<float>(
		FWPLUTGenerator::ComputeNormalizedOuterRadius(Entry.Descriptor, TransitionRatio));
	Binding.ResourceRevision = Entry.ResourceRevision;
	Binding.Source = Entry.Source;
	return Binding;
}

void UWPLUTCacheSubsystem::CancelRequest(const FWPLUTRequestHandle Handle)
{
	check(IsInGameThread());
	if (!Handle.IsValid())
	{
		return;
	}
	FString EmptyEntryKey;
	for (TPair<FString, TSharedPtr<FCacheEntry>>& Pair : Entries)
	{
		const TSharedPtr<FCacheEntry>& Entry = Pair.Value;
		const int32 Removed = Entry->Waiters.RemoveAll(
			[Handle](const FWaiter& Waiter) { return Waiter.RequestId == Handle.RequestId; });
		if (Removed > 0 && Entry->Waiters.IsEmpty() && Entry->State != ECacheEntryState::Ready)
		{
			if (Entry->CancelToken)
			{
				Entry->CancelToken->Store(true);
			}
			if (Entry->StreamableHandle)
			{
				Entry->StreamableHandle->CancelHandle();
			}
			if (Entry->State == ECacheEntryState::BuildingFallback)
			{
				ActiveFallbackGenerations.Remove(Entry->Generation);
			}
			EmptyEntryKey = Pair.Key;
		}
		if (Removed > 0)
		{
			break;
		}
	}
	if (!EmptyEntryKey.IsEmpty())
	{
		PendingFallbackKeys.Remove(EmptyEntryKey);
		Entries.Remove(EmptyEntryKey);
		PumpFallbackQueue();
	}
}

void UWPLUTCacheSubsystem::ResetCache()
{
	check(IsInGameThread());
	for (TPair<FString, TSharedPtr<FCacheEntry>>& Pair : Entries)
	{
		if (Pair.Value->CancelToken)
		{
			Pair.Value->CancelToken->Store(true);
		}
		if (Pair.Value->StreamableHandle)
		{
			Pair.Value->StreamableHandle->CancelHandle();
		}
	}
	Entries.Reset();
	PendingFallbackKeys.Reset();
	ActiveFallbackGenerations.Reset();
	ResidentAssets.Reset();
	ResidentTextures.Reset();
	DefaultCatalog = nullptr;
}

UVolumeTexture* UWPLUTCacheSubsystem::CreateTransientVolume(
	const FWPLUTVolumeData& VolumeData,
	const FString& CacheKey)
{
	check(IsInGameThread());
	FString Error;
	if (!VolumeData.IsValid(&Error))
	{
		WP_LOG(nullptr, Error, TEXT("Invalid fallback volume: %s"), *Error);
		return nullptr;
	}

	const FIntVector Size = VolumeData.Dimensions;
	const FString ShortHash = VolumeData.BuildHash.Left(12);
	UVolumeTexture* Texture = UVolumeTexture::CreateTransient(
		Size.X, Size.Y, Size.Z, PF_A32B32G32R32F,
		FName(*FString::Printf(TEXT("WP_LUT_%s"), *ShortHash)));
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() != 1)
	{
		return nullptr;
	}
	const FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	if (PlatformData->PixelFormat != PF_A32B32G32R32F
		|| PlatformData->SizeX != Size.X
		|| PlatformData->SizeY != Size.Y)
	{
		return nullptr;
	}

	Texture->SRGB = false;
	Texture->Filter = TF_Trilinear;
	Texture->CompressionSettings = TC_HDR;
	Texture->LODGroup = TEXTUREGROUP_Effects;
	Texture->AddressMode = TA_Clamp;
	Texture->NeverStream = true;
#if WITH_EDITORONLY_DATA
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif

	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	if (Mip.SizeX != Size.X || Mip.SizeY != Size.Y || Mip.SizeZ != Size.Z
		|| Mip.BulkData.GetBulkDataSize() != VolumeData.GetByteCount())
	{
		return nullptr;
	}
	void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE);
	if (!Destination)
	{
		Mip.BulkData.Unlock();
		return nullptr;
	}
	FMemory::Memcpy(Destination, VolumeData.Voxels.GetData(), VolumeData.GetByteCount());
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

