// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/LUT/WPLUTEndpointManager.h"

#include "Rendering/LUT/WPLUTGenerator.h"
#include "Engine/Engine.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Subsystem/WPLUTCacheSubsystem.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "WPSettings.h"

namespace
{
	constexpr float WPLUTEndpointMinPortalRadiusCm = 1.0f;
 
	bool IsWPLUTEndpointRenderCapable(const UWorld* World)
	{
		return World && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer
			&& FApp::CanEverRender() && !IsRunningDedicatedServer();
	}

	const TCHAR* GetWPLUTEndpointSourceName(const EWPLUTSource Source)
	{
		switch (Source)
		{
		case EWPLUTSource::BakedAsset: return TEXT("BakedAsset");
		case EWPLUTSource::RuntimeFallback: return TEXT("RuntimeFallback");
		default: return TEXT("None");
		}
	}
}

UWorld* UWPLUTEndpointManager::GetWorld() const
{
	return ManagedWorld.Get();
}

void UWPLUTEndpointManager::Initialize(UWorld* InWorld)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	ManagedWorld = InWorld;
	bInitialized = IsValid(InWorld);
	bShuttingDown = false;

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Initialized. World=%s WorldType=%d NetMode=%d RenderCapable=%d Initialized=%d EndpointCount=%d CpuMs=%.3f"),
		*GetNameSafe(InWorld), InWorld ? static_cast<int32>(InWorld->WorldType) : INDEX_NONE,
		InWorld ? static_cast<int32>(InWorld->GetNetMode()) : INDEX_NONE,
		IsWPLUTEndpointRenderCapable(InWorld) ? 1 : 0, bInitialized ? 1 : 0,
		EndpointStates.Num(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPLUTEndpointManager::Shutdown(const TCHAR* Reason)
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;

#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 EndpointCountBeforeShutdown = EndpointStates.Num();
	const int32 PendingCountBeforeShutdown = GetPendingRequestCount();
#endif

	for (TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		CancelStateRequest(Pair.Value, TEXT("ManagerShutdown"));
		ClearBinding(Pair.Value, true);
	}
	EndpointStates.Reset();
	BoundTextures.Reset();
	EndpointChangedDelegate.Clear();
	bInitialized = false;

#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Shutdown complete. World=%s EndpointsReleased=%d PendingCancelled=%d BoundTextures=0 Registers=%llu Unregisters=%llu Requests=%llu Cancels=%llu Completions=%llu Failures=%llu StaleCompletions=%llu TotalRequestCpuMs=%.3f MaxRequestCpuMs=%.3f TotalCompletionCpuMs=%.3f MaxCompletionCpuMs=%.3f Reason=%s CpuMs=%.3f"),
		*GetNameSafe(ManagedWorld.Get()), EndpointCountBeforeShutdown, PendingCountBeforeShutdown,
		static_cast<unsigned long long>(RegisterCount), static_cast<unsigned long long>(UnregisterCount),
		static_cast<unsigned long long>(RequestCount), static_cast<unsigned long long>(CancelCount),
		static_cast<unsigned long long>(CompletionCount), static_cast<unsigned long long>(FailureCount),
		static_cast<unsigned long long>(StaleCompletionCount), TotalRequestCpuMs, MaxRequestCpuMs,
		TotalCompletionCpuMs, MaxCompletionCpuMs, Reason ? Reason : TEXT("Unspecified"), CpuMs);
#endif
	ManagedWorld.Reset();
}

bool UWPLUTEndpointManager::RegisterEndpoint(
	AWormholePortalActor* Portal,
	const FWPLUTEndpointRequestOptions& Options)
{
	// Log-only: Measures endpoint registration CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
	if (!bInitialized || bShuttingDown || !IsInGameThread() || !IsValid(Portal)
		|| Portal->GetWorld() != ManagedWorld.Get())
	{
		WP_LOG(this, Error,
			TEXT("[LUTEndpointManager] Register rejected. World=%s Portal=%s PortalWorld=%s Initialized=%d ShuttingDown=%d GameThread=%d CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			*GetNameSafe(Portal ? Portal->GetWorld() : nullptr), bInitialized ? 1 : 0,
			bShuttingDown ? 1 : 0, IsInGameThread() ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	CompactInvalidEndpoints(TEXT("RegisterCompact"));
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	const bool bWasRegistered = EndpointStates.Contains(PortalKey);
	if (!bWasRegistered)
	{
		FEndpointState State;
		State.Portal = Portal;
		State.Options = Options;
		State.EndpointRevision = AdvanceGeneration(State.EndpointRevision);
		EndpointStates.Add(PortalKey, MoveTemp(State));
		++RegisterCount;
	}

	const bool bRefreshed = RefreshEndpointInternal(Portal, Options, false);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Endpoint registered. World=%s Portal=%s Added=%d Refreshed=%d EndpointCount=%d Pending=%d Ready=%d CpuMs=%.3f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal), bWasRegistered ? 0 : 1,
		bRefreshed ? 1 : 0, EndpointStates.Num(), GetPendingRequestCount(),
		GetReadyBindingCount(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return bRefreshed;
}

bool UWPLUTEndpointManager::RefreshEndpoint(AWormholePortalActor* Portal)
{
	const FEndpointState* State = EndpointStates.Find(TWeakObjectPtr<AWormholePortalActor>(Portal));
	if (!State)
	{
		return false;
	}
	// Refresh may synchronously complete and listeners may erase the map entry.
	const FWPLUTEndpointRequestOptions Options = State->Options;
	return RefreshEndpointInternal(Portal, Options, false);
}

bool UWPLUTEndpointManager::RefreshEndpoint(
	AWormholePortalActor* Portal,
	const FWPLUTEndpointRequestOptions& Options)
{
	return RefreshEndpointInternal(Portal, Options, false);
}

bool UWPLUTEndpointManager::RefreshEndpointInternal(
	AWormholePortalActor* Portal,
	const FWPLUTEndpointRequestOptions& Options,
	const bool bAllowAdd)
{
	// Log-only: Records request processing CPU time for detailed and aggregate log telemetry.
	const double StartSeconds = FPlatformTime::Seconds();
	if (!bInitialized || bShuttingDown || !IsInGameThread() || !IsValid(Portal)
		|| Portal->GetWorld() != ManagedWorld.Get())
	{
		return false;
	}

	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	FEndpointState* State = EndpointStates.Find(PortalKey);
	if (!State && bAllowAdd)
	{
		FEndpointState NewState;
		NewState.Portal = Portal;
		NewState.Options = Options;
		NewState.EndpointRevision = AdvanceGeneration(NewState.EndpointRevision);
		State = &EndpointStates.Add(PortalKey, MoveTemp(NewState));
		++RegisterCount;
	}
	if (!State)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[LUTEndpointManager] Refresh ignored for unregistered endpoint. World=%s Portal=%s CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const FWPLUTDescriptor DesiredDescriptor = (Options.bOverrideDescriptor
		? Options.DescriptorOverride
		: Portal->GetEffectiveLUTDescriptor()).GetSanitized();
	const bool bAllowRuntimeFallback = Options.bOverrideRuntimeFallback
		? Options.bAllowRuntimeFallback
		: Settings && Settings->bAllowRuntimeLUTFallback;
	const float PortalRadiusCm = FMath::Max(Portal->GetPortalRadius(), WPLUTEndpointMinPortalRadiusCm);
	const float TransitionLengthCm = FMath::Max(Portal->GetTransitionLength(), 0.0f);
	const float RawDesiredRatio = TransitionLengthCm > KINDA_SMALL_NUMBER
		? TransitionLengthCm / PortalRadiusCm
		: 0.0f;
	const float DesiredRatio = RawDesiredRatio > 0.0f
		? DesiredDescriptor.ClampTransitionRatio(RawDesiredRatio)
		: 0.0f;
	if (RawDesiredRatio > 0.0f && !FMath::IsNearlyEqual(
		RawDesiredRatio,
		DesiredRatio,
		FMath::Max(1.0e-5f, RawDesiredRatio * 1.0e-5f)))
	{
		// Keep the actor metric and LUT binding ratio identical. The setter's registry
		// notification re-enters this manager with the clamped metric.
		State->Options = Options;
		State->RequestedDescriptor = DesiredDescriptor;
		WP_LOG(this, Warning,
			TEXT("[LUTEndpointManager] Metric ratio clamped before LUT request. World=%s Portal=%s RawRatio=%.6f ClampedRatio=%.6f Domain=[%.6f, %.6f]"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal), RawDesiredRatio, DesiredRatio,
			DesiredDescriptor.TransitionRatioMin, DesiredDescriptor.TransitionRatioMax);
		// LUT-domain correction is an internal resource contract update, not a public
		// gameplay Metric mutation. It intentionally bypasses the runtime API guard.
		Portal->SetTransitionLengthInternal(DesiredRatio * PortalRadiusCm, true);
		if (Portal->bRuntimeMetricShapeInitialized)
		{
			Portal->CaptureRuntimeMetricShape(false);
			if (AWormholePortalActor* LinkedPortal = Portal->GetLinkedPortal();
				IsValid(LinkedPortal) && LinkedPortal->bRuntimeMetricShapeInitialized)
			{
				LinkedPortal->CaptureRuntimeMetricShape(false);
			}
		}
		return true;
	}
	const float RatioTolerance = FMath::Max(1.0e-5f, DesiredRatio * 1.0e-5f);
	const bool bRequestIdentityMatches = RequestOptionsEqual(State->Options, Options)
		&& State->RequestedDescriptor == DesiredDescriptor
		&& FMath::IsNearlyEqual(State->TransitionRatio, DesiredRatio, RatioTolerance);
	const float PreviousPortalRadiusCm = State->PortalRadiusCm;
	const float PreviousThroatHalfLengthCm = State->ThroatHalfLengthCm;
	const float PreviousTransitionLengthCm = State->TransitionLengthCm;
	const float PreviousMetricOuterRadiusCm = State->MetricOuterRadiusCm;
#if !UE_BUILD_SHIPPING
	const float PreviousThroatRatio = PreviousPortalRadiusCm > KINDA_SMALL_NUMBER
		? PreviousThroatHalfLengthCm / PreviousPortalRadiusCm
		: 0.0f;
	const float DesiredThroatRatio = PortalRadiusCm > KINDA_SMALL_NUMBER
		? FMath::Max(Portal->GetThroatHalfLength(), 0.0f) / PortalRadiusCm
		: 0.0f;
	const float ThroatRatioTolerance = FMath::Max(
		1.0e-5f, FMath::Max(FMath::Abs(PreviousThroatRatio), FMath::Abs(DesiredThroatRatio)) * 1.0e-5f);
	const bool bDimensionlessMetricShapeMatches = PreviousPortalRadiusCm > KINDA_SMALL_NUMBER
		&& FMath::IsNearlyEqual(PreviousThroatRatio, DesiredThroatRatio, ThroatRatioTolerance)
		&& FMath::IsNearlyEqual(State->TransitionRatio, DesiredRatio, RatioTolerance);
#endif
	RefreshMetricSnapshot(*State, *Portal);
	const bool bMetricChanged = !FMath::IsNearlyEqual(PreviousPortalRadiusCm, State->PortalRadiusCm)
		|| !FMath::IsNearlyEqual(PreviousThroatHalfLengthCm, State->ThroatHalfLengthCm)
		|| !FMath::IsNearlyEqual(PreviousTransitionLengthCm, State->TransitionLengthCm)
		|| !FMath::IsNearlyEqual(PreviousMetricOuterRadiusCm, State->MetricOuterRadiusCm);

	if (TransitionLengthCm <= KINDA_SMALL_NUMBER)
	{
		const bool bResourceStateChanged = State->bRequestPending || State->Binding.IsReady()
			|| !State->bAnalyticNoTransition;
		CancelStateRequest(*State, TEXT("AnalyticNoTransition"));
		ClearBinding(*State, true);
		State->Options = Options;
		State->RequestedDescriptor = DesiredDescriptor;
		State->TransitionRatio = 0.0f;
		State->RatioCoordinate01 = 0.0f;
		// T==0 removes only the variable-radius transition. The authored constant-radius
		// throat still extends by a, so the flat boundary is q=rho+a rather than rho.
		State->MetricOuterRadiusCm = PortalRadiusCm + State->ThroatHalfLengthCm;
		State->NormalizedOuterRadius = State->MetricOuterRadiusCm / PortalRadiusCm;
		State->bAnalyticNoTransition = true;
		State->LastError.Reset();
		if (bResourceStateChanged)
		{
			State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
			RebuildStrongTextureReferences();
			BroadcastState(*State, TEXT("AnalyticNoTransition"), StartSeconds);
		}
		else if (bMetricChanged)
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(this, VeryVerbose,
				TEXT("[LUTEndpointManager][MetricSnapshot] Analytic metric snapshot updated without LUT resource invalidation. World=%s Portal=%s Previous=(R=%.3f,A=%.3f,T=%.3f,Outer=%.3f) Current=(R=%.3f,A=%.3f,T=%.3f,Outer=%.3f) TransitionRatio=0 UniformMetricScale=%d EndpointRevision=%u RequestGeneration=%llu BindingGeneration=%u ResourceRevision=%u LUTRequestRestarted=0 SnapshotBroadcast=0 OwnershipInvalidationExpected=%d CpuMs=%.4f"),
				*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
				PreviousPortalRadiusCm, PreviousThroatHalfLengthCm,
				PreviousTransitionLengthCm, PreviousMetricOuterRadiusCm,
				State->PortalRadiusCm, State->ThroatHalfLengthCm,
				State->TransitionLengthCm, State->MetricOuterRadiusCm,
				bDimensionlessMetricShapeMatches ? 1 : 0, State->EndpointRevision,
				static_cast<unsigned long long>(State->RequestGeneration),
				State->BindingGeneration, State->Binding.ResourceRevision,
				bDimensionlessMetricShapeMatches ? 0 : 1,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		return true;
	}

	if (bRequestIdentityMatches && (State->bRequestPending || State->Binding.IsReady()))
	{
		if (bMetricChanged)
		{
			const float NormalizedOuterRadius = State->Binding.IsReady()
				? FMath::Max(State->Binding.NormalizedOuterRadius, 1.0f)
				: static_cast<float>(FMath::Max(
					FWPLUTGenerator::ComputeNormalizedOuterRadius(DesiredDescriptor, DesiredRatio), 1.0));
			State->NormalizedOuterRadius = NormalizedOuterRadius;
			State->MetricOuterRadiusCm = FMath::Max(PortalRadiusCm * NormalizedOuterRadius, PortalRadiusCm);
			// The actor's Registry Metric event already dirties its pair, so publishing a
			// second endpoint-resource event here is redundant. When T/rho matches, the
			// existing LUT request/binding remains valid; only its absolute metric snapshot
			// must track the current scale.
#if !UE_BUILD_SHIPPING
			WP_LOG(this, VeryVerbose,
				TEXT("[LUTEndpointManager][MetricSnapshot] Metric snapshot updated without LUT resource invalidation. World=%s Portal=%s Previous=(R=%.3f,A=%.3f,T=%.3f,Outer=%.3f) Current=(R=%.3f,A=%.3f,T=%.3f,Outer=%.3f) TransitionRatio=%.6f UniformMetricScale=%d EndpointRevision=%u RequestGeneration=%llu RequestPending=%d BindingGeneration=%u ResourceRevision=%u LUTRequestRestarted=0 SnapshotBroadcast=0 OwnershipInvalidationExpected=%d CpuMs=%.4f"),
				*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
				PreviousPortalRadiusCm, PreviousThroatHalfLengthCm,
				PreviousTransitionLengthCm, PreviousMetricOuterRadiusCm,
				State->PortalRadiusCm, State->ThroatHalfLengthCm,
				State->TransitionLengthCm, State->MetricOuterRadiusCm,
				State->TransitionRatio, bDimensionlessMetricShapeMatches ? 1 : 0,
				State->EndpointRevision,
				static_cast<unsigned long long>(State->RequestGeneration),
				State->bRequestPending ? 1 : 0, State->BindingGeneration,
				State->Binding.ResourceRevision,
				bDimensionlessMetricShapeMatches ? 0 : 1,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		return true;
	}

	if (!State->bRequestPending)
	{
		State->PreviousTextureForRequest = State->Binding.VolumeTexture.Get();
		State->PreviousDescriptorForRequest = State->Binding.Descriptor;
		State->PreviousRevisionForRequest = State->Binding.ResourceRevision;
	}
	CancelStateRequest(*State, TEXT("RequestSuperseded"));
	ClearBinding(*State, false);
	State->Options = Options;
	State->RequestedDescriptor = DesiredDescriptor;
	State->TransitionRatio = DesiredRatio;
	State->RatioCoordinate01 = 0.0f;
	State->NormalizedOuterRadius = static_cast<float>(FMath::Max(
		FWPLUTGenerator::ComputeNormalizedOuterRadius(DesiredDescriptor, DesiredRatio), 1.0));
	State->MetricOuterRadiusCm = FMath::Max(
		PortalRadiusCm * State->NormalizedOuterRadius, PortalRadiusCm);
	State->bAnalyticNoTransition = false;
	State->bRequestPending = true;
	State->LastError.Reset();
	State->RequestGeneration = AdvanceGeneration(State->RequestGeneration);
	const uint64 RequestGeneration = State->RequestGeneration;
	State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
	RebuildStrongTextureReferences();

	UWorld* World = ManagedWorld.Get();
	if (!IsWPLUTEndpointRenderCapable(World))
	{
		State->bRequestPending = false;
		State->LastError = TEXT("LUT endpoint bindings are disabled in a non-rendering World.");
		++FailureCount;
		BroadcastState(*State, TEXT("RenderDisabled"), StartSeconds);
		return true;
	}

	UWPLUTCacheSubsystem* Cache = GEngine
		? GEngine->GetEngineSubsystem<UWPLUTCacheSubsystem>()
		: nullptr;
	if (!Cache)
	{
		State->bRequestPending = false;
		State->LastError = TEXT("LUT cache subsystem is unavailable.");
		++FailureCount;
		BroadcastState(*State, TEXT("CacheUnavailable"), StartSeconds);
		return false;
	}

	FWPLUTRequest Request;
	Request.Descriptor = DesiredDescriptor;
	Request.TransitionRatio = DesiredRatio;
	Request.PreferredAsset = Options.PreferredAsset;
	Request.bAllowRuntimeFallback = bAllowRuntimeFallback;
	Request.DebugContext = Options.DebugContext.IsEmpty() ? Portal->GetPathName() : Options.DebugContext;
	const FWPLUTRequestHandle RequestHandle = Cache->RequestLUT(
		Request,
		FWPLUTRequestComplete::CreateUObject(
			this,
			&UWPLUTEndpointManager::HandleRequestComplete,
			PortalKey,
			RequestGeneration));

	// Ready cache hits and validation failures can complete synchronously and may re-enter listeners.
	State = EndpointStates.Find(PortalKey);
	if (State && State->bRequestPending && State->RequestGeneration == RequestGeneration)
	{
		State->RequestHandle = RequestHandle;
		if (!RequestHandle.IsValid())
		{
			State->bRequestPending = false;
			State->LastError = TEXT("LUT cache rejected the request without a completion.");
			State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
			++FailureCount;
			BroadcastState(*State, TEXT("RequestRejected"), StartSeconds);
		}
		else
		{
			BroadcastState(*State, TEXT("RequestPending"), StartSeconds);
		}
	}
	// EndpointChanged listeners are allowed to unregister the endpoint synchronously.
	State = EndpointStates.Find(PortalKey);

	++RequestCount;
#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TotalRequestCpuMs += CpuMs;
	MaxRequestCpuMs = FMath::Max(MaxRequestCpuMs, CpuMs);
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Request issued. World=%s Portal=%s RequestGeneration=%llu RequestId=%llu Ratio=%.6f PreferredAsset=%s RuntimeFallback=%d SynchronousCompletion=%d EndpointCount=%d RequestCpuMs=%.3f TotalRequestCpuMs=%.3f MaxRequestCpuMs=%.3f"),
		*GetNameSafe(World), *GetNameSafe(Portal), static_cast<unsigned long long>(RequestGeneration),
		static_cast<unsigned long long>(RequestHandle.RequestId), DesiredRatio,
		*Options.PreferredAsset.ToSoftObjectPath().ToString(), bAllowRuntimeFallback ? 1 : 0,
		State && !State->bRequestPending ? 1 : 0, EndpointStates.Num(), CpuMs,
		TotalRequestCpuMs, MaxRequestCpuMs);
#endif
	return true;
}

void UWPLUTEndpointManager::HandleRequestComplete(
	const FWPLUTBinding& Binding,
	const TWeakObjectPtr<AWormholePortalActor> PortalKey,
	const uint64 RequestGeneration)
{
	// Log-only: Records completion processing CPU time for detailed and aggregate log telemetry.
	const double StartSeconds = FPlatformTime::Seconds();
	FEndpointState* State = EndpointStates.Find(PortalKey);
	AWormholePortalActor* Portal = PortalKey.Get();
	if (bShuttingDown || !State || State->RequestGeneration != RequestGeneration
		|| !IsValid(Portal) || Portal->GetWorld() != ManagedWorld.Get())
	{
		++StaleCompletionCount;
		return;
	}

	const float CurrentPortalRadiusCm = FMath::Max(
		Portal->GetPortalRadius(), WPLUTEndpointMinPortalRadiusCm);
	const float CurrentTransitionLengthCm = FMath::Max(Portal->GetTransitionLength(), 0.0f);
	const float CurrentRatio = CurrentTransitionLengthCm > KINDA_SMALL_NUMBER
		? CurrentTransitionLengthCm / CurrentPortalRadiusCm
		: 0.0f;
	const float RatioTolerance = FMath::Max(1.0e-5f, CurrentRatio * 1.0e-5f);
	const bool bRequestStillMatchesActor = CurrentRatio > 0.0f
		&& FMath::IsNearlyEqual(State->TransitionRatio, CurrentRatio, RatioTolerance);
	const bool bBindingClampedRequest = Binding.IsReady()
		&& bRequestStillMatchesActor
		&& !FMath::IsNearlyEqual(Binding.TransitionRatio, CurrentRatio, RatioTolerance);
	if (bBindingClampedRequest)
	{
		State->bRequestPending = false;
		State->RequestHandle.Reset();
		State->LastError.Reset();
		State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
		WP_LOG(this, Warning,
			TEXT("[LUTEndpointManager] Loaded LUT domain clamped the in-flight request; applying the binding ratio to the actor metric. World=%s Portal=%s RequestGeneration=%llu RequestedRatio=%.6f BindingRatio=%.6f TransitionLength=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			static_cast<unsigned long long>(RequestGeneration), CurrentRatio,
			Binding.TransitionRatio, Binding.TransitionRatio * CurrentPortalRadiusCm);
		// Binding correction must remain available after the public Metric shape has
		// been locked, so use the manager-only internal path.
		Portal->SetTransitionLengthInternal(
			Binding.TransitionRatio * CurrentPortalRadiusCm, true);
		if (Portal->bRuntimeMetricShapeInitialized)
		{
			Portal->CaptureRuntimeMetricShape(false);
			if (AWormholePortalActor* LinkedPortal = Portal->GetLinkedPortal();
				IsValid(LinkedPortal) && LinkedPortal->bRuntimeMetricShapeInitialized)
			{
				LinkedPortal->CaptureRuntimeMetricShape(false);
			}
		}
		return;
	}
	if (CurrentRatio <= 0.0f
		|| !FMath::IsNearlyEqual(State->TransitionRatio, CurrentRatio, RatioTolerance)
		|| (Binding.IsReady()
			&& !FMath::IsNearlyEqual(Binding.TransitionRatio, CurrentRatio, RatioTolerance)))
	{
		State->bRequestPending = false;
		State->RequestHandle.Reset();
		State->LastError = TEXT("Endpoint metric changed while the LUT request was in flight.");
		State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
		const FWPLUTEndpointRequestOptions Options = State->Options;
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[LUTEndpointManager] Completion metric mismatch; refreshing. World=%s Portal=%s RequestGeneration=%llu RequestedRatio=%.6f BindingRatio=%.6f CurrentRatio=%.6f CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			static_cast<unsigned long long>(RequestGeneration), State->TransitionRatio,
			Binding.TransitionRatio, CurrentRatio,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		RefreshEndpointInternal(Portal, Options, false);
		return;
	}

	State->bRequestPending = false;
	State->RequestHandle.Reset();
	++CompletionCount;
	if (!Binding.IsReady())
	{
		ClearBinding(*State, true);
		State->LastError = Binding.Error.IsEmpty()
			? TEXT("LUT request completed without a ready binding.")
			: Binding.Error;
		State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
		++FailureCount;
		// Log-only: Copies the failure string so it remains safe to report across reentrant delegate broadcasts.
		const FString CompletionError = State->LastError;
		RebuildStrongTextureReferences();
		BroadcastState(*State, TEXT("RequestFailed"), StartSeconds);

		const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		TotalCompletionCpuMs += CpuMs;
		MaxCompletionCpuMs = FMath::Max(MaxCompletionCpuMs, CpuMs);
		WP_LOG(this, Error,
			TEXT("[LUTEndpointManager] Request failed. World=%s Portal=%s RequestGeneration=%llu Ratio=%.6f Error=%s Completions=%llu Failures=%llu CompletionCpuMs=%.3f TotalCompletionCpuMs=%.3f MaxCompletionCpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			static_cast<unsigned long long>(RequestGeneration), CurrentRatio, *CompletionError,
			static_cast<unsigned long long>(CompletionCount), static_cast<unsigned long long>(FailureCount),
			CpuMs, TotalCompletionCpuMs, MaxCompletionCpuMs);
		return;
	}

	const bool bResourceIdentityChanged = State->PreviousTextureForRequest.Get() != Binding.VolumeTexture.Get()
		|| State->PreviousRevisionForRequest != Binding.ResourceRevision
		|| State->PreviousDescriptorForRequest != Binding.Descriptor;
	State->Binding = Binding;
	State->RatioCoordinate01 = Binding.RatioCoordinate01;
	State->NormalizedOuterRadius = FMath::Max(Binding.NormalizedOuterRadius, 1.0f);
	State->MetricOuterRadiusCm = FMath::Max(
		CurrentPortalRadiusCm * State->NormalizedOuterRadius, CurrentPortalRadiusCm);
	State->TransitionRatio = Binding.TransitionRatio;
	State->LastError.Reset();
	if (bResourceIdentityChanged || State->BindingGeneration == 0)
	{
		State->BindingGeneration = AdvanceGeneration(State->BindingGeneration);
	}
	State->PreviousTextureForRequest.Reset();
	State->PreviousDescriptorForRequest = FWPLUTDescriptor::MakeDefault();
	State->PreviousRevisionForRequest = 0;
	State->Contract = MakeContract(*State);
	State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
	RefreshMetricSnapshot(*State, *Portal);
#if !UE_BUILD_SHIPPING
	const FWPLUTEndpointSnapshot ReadySnapshot = MakeSnapshot(*State, true);
#endif
	RebuildStrongTextureReferences();
	BroadcastState(*State, TEXT("BindingReady"), StartSeconds);

	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TotalCompletionCpuMs += CpuMs;
	MaxCompletionCpuMs = FMath::Max(MaxCompletionCpuMs, CpuMs);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Binding ready. World=%s Portal=%s RequestGeneration=%llu Source=%s Ratio=%.6f Z=%.6f NormalizedOuterRadius=%.6f MetricOuterRadiusCm=%.3f LayoutVersion=%u BindingGeneration=%u ResourceRevision=%u EndpointRevision=%u Texture=%s Extent=%dx%dx%d ContractValid=%d ResourceIdentityChanged=%d BoundTextures=%d CompletionCpuMs=%.3f TotalCompletionCpuMs=%.3f MaxCompletionCpuMs=%.3f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
		static_cast<unsigned long long>(RequestGeneration), GetWPLUTEndpointSourceName(Binding.Source),
		Binding.TransitionRatio, Binding.RatioCoordinate01, ReadySnapshot.NormalizedOuterRadius,
		ReadySnapshot.MetricOuterRadiusCm, ReadySnapshot.Contract.LayoutVersion,
		ReadySnapshot.BindingGeneration, Binding.ResourceRevision, ReadySnapshot.EndpointRevision,
		*GetNameSafe(Binding.VolumeTexture), ReadySnapshot.Contract.ExpectedExtent.X,
		ReadySnapshot.Contract.ExpectedExtent.Y, ReadySnapshot.Contract.ExpectedExtent.Z,
		ReadySnapshot.Contract.IsValid() ? 1 : 0,
		bResourceIdentityChanged ? 1 : 0, BoundTextures.Num(), CpuMs,
		TotalCompletionCpuMs, MaxCompletionCpuMs);
#endif
}

bool UWPLUTEndpointManager::UnregisterEndpoint(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	FEndpointState* State = EndpointStates.Find(PortalKey);
	if (!State)
	{
		return false;
	}

#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	CancelStateRequest(*State, Reason ? Reason : TEXT("EndpointUnregister"));
	ClearBinding(*State, true);
	State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
	const FWPLUTEndpointSnapshot FinalSnapshot = MakeSnapshot(*State, false);
	EndpointStates.Remove(PortalKey);
	RebuildStrongTextureReferences();
	++UnregisterCount;
	EndpointChangedDelegate.Broadcast(PortalKey, FinalSnapshot);

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[LUTEndpointManager] Endpoint unregistered. World=%s Portal=%s RequestGeneration=%llu EndpointRevision=%u RemainingEndpoints=%d Pending=%d Ready=%d BoundTextures=%d Reason=%s CpuMs=%.3f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
		static_cast<unsigned long long>(FinalSnapshot.RequestGeneration), FinalSnapshot.EndpointRevision,
		EndpointStates.Num(), GetPendingRequestCount(), GetReadyBindingCount(), BoundTextures.Num(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

bool UWPLUTEndpointManager::CancelEndpointRequest(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	FEndpointState* State = EndpointStates.Find(TWeakObjectPtr<AWormholePortalActor>(Portal));
	if (!State || !State->bRequestPending)
	{
		return false;
	}
	CancelStateRequest(*State, Reason ? Reason : TEXT("ExplicitCancel"));
	State->EndpointRevision = AdvanceGeneration(State->EndpointRevision);
	BroadcastState(*State, TEXT("RequestCancelled"), 0.0);
	return true;
}

int32 UWPLUTEndpointManager::CompactInvalidEndpoints(const TCHAR* Reason)
{
	// Log-only: Measures CPU time spent compacting invalid endpoints.
	const double StartSeconds = FPlatformTime::Seconds();
	TArray<TWeakObjectPtr<AWormholePortalActor>> InvalidKeys;
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		if (!Pair.Key.IsValid())
		{
			InvalidKeys.Add(Pair.Key);
		}
	}
	for (const TWeakObjectPtr<AWormholePortalActor>& Key : InvalidKeys)
	{
		if (FEndpointState* State = EndpointStates.Find(Key))
		{
			CancelStateRequest(*State, Reason ? Reason : TEXT("InvalidEndpointCompact"));
			ClearBinding(*State, true);
		}
		EndpointStates.Remove(Key);
		++UnregisterCount;
	}
	if (!InvalidKeys.IsEmpty())
	{
		RebuildStrongTextureReferences();
		WP_LOG(this, Warning,
			TEXT("[LUTEndpointManager] Invalid endpoints compacted. World=%s Removed=%d Remaining=%d Pending=%d Ready=%d BoundTextures=%d Reason=%s CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), InvalidKeys.Num(), EndpointStates.Num(),
			GetPendingRequestCount(), GetReadyBindingCount(), BoundTextures.Num(),
			Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
	return InvalidKeys.Num();
}

bool UWPLUTEndpointManager::GetEndpointSnapshot(
	const AWormholePortalActor* Portal,
	FWPLUTEndpointSnapshot& OutSnapshot) const
{
	if (!Portal)
	{
		return false;
	}
	const FEndpointState* State = EndpointStates.Find(
		TWeakObjectPtr<AWormholePortalActor>(const_cast<AWormholePortalActor*>(Portal)));
	if (!State)
	{
		return false;
	}
	OutSnapshot = MakeSnapshot(*State, true);
	return true;
}

void UWPLUTEndpointManager::GetEndpointSnapshots(
	TArray<FWPLUTEndpointSnapshot>& OutSnapshots) const
{
	OutSnapshots.Reset(EndpointStates.Num());
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		if (Pair.Key.IsValid())
		{
			OutSnapshots.Add(MakeSnapshot(Pair.Value, true));
		}
	}
}

bool UWPLUTEndpointManager::HasEndpoint(const AWormholePortalActor* Portal) const
{
	return Portal && EndpointStates.Contains(
		TWeakObjectPtr<AWormholePortalActor>(const_cast<AWormholePortalActor*>(Portal)));
}

int32 UWPLUTEndpointManager::GetPendingRequestCount() const
{
	int32 Count = 0;
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		Count += Pair.Value.bRequestPending ? 1 : 0;
	}
	return Count;
}

int32 UWPLUTEndpointManager::GetReadyBindingCount() const
{
	int32 Count = 0;
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		Count += !Pair.Value.bRequestPending
			&& (Pair.Value.bAnalyticNoTransition
				|| (Pair.Value.Binding.IsReady() && Pair.Value.Contract.IsValid()))
			? 1 : 0;
	}
	return Count;
}

void UWPLUTEndpointManager::CancelStateRequest(FEndpointState& State, const TCHAR* Reason)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	const bool bHadPendingRequest = State.bRequestPending || State.RequestHandle.IsValid();
	if (State.RequestHandle.IsValid() && GEngine)
	{
		if (UWPLUTCacheSubsystem* Cache =
			GEngine->GetEngineSubsystem<UWPLUTCacheSubsystem>())
		{
			Cache->CancelRequest(State.RequestHandle);
		}
	}
	if (bHadPendingRequest)
	{
		++CancelCount;
		State.RequestGeneration = AdvanceGeneration(State.RequestGeneration);
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[LUTEndpointManager] Request cancelled. World=%s Portal=%s NewRequestGeneration=%llu RequestId=%llu Cancels=%llu Reason=%s CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(State.Portal.Get()),
			static_cast<unsigned long long>(State.RequestGeneration),
			static_cast<unsigned long long>(State.RequestHandle.RequestId),
			static_cast<unsigned long long>(CancelCount), Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}
	State.RequestHandle.Reset();
	State.bRequestPending = false;
}

void UWPLUTEndpointManager::ClearBinding(
	FEndpointState& State,
	const bool bResetPreviousIdentity)
{
	State.Binding = FWPLUTBinding();
	State.Contract = FWPRayLUTContract();
	State.RatioCoordinate01 = 0.0f;
	State.NormalizedOuterRadius = 1.0f;
	if (bResetPreviousIdentity)
	{
		State.PreviousTextureForRequest.Reset();
		State.PreviousDescriptorForRequest = FWPLUTDescriptor::MakeDefault();
		State.PreviousRevisionForRequest = 0;
	}
}

void UWPLUTEndpointManager::RefreshMetricSnapshot(
	FEndpointState& State,
	const AWormholePortalActor& Portal)
{
	State.PortalRadiusCm = FMath::Max(Portal.GetPortalRadius(), WPLUTEndpointMinPortalRadiusCm);
	State.ThroatHalfLengthCm = FMath::Max(Portal.GetThroatHalfLength(), 0.0f);
	State.TransitionLengthCm = FMath::Max(Portal.GetTransitionLength(), 0.0f);
	State.TransitionRatio = State.TransitionLengthCm > KINDA_SMALL_NUMBER
		? State.TransitionLengthCm / State.PortalRadiusCm
		: 0.0f;
	const float NormalizedOuterRadius = State.Binding.IsReady()
		? FMath::Max(State.Binding.NormalizedOuterRadius, 1.0f)
		: State.TransitionLengthCm > KINDA_SMALL_NUMBER
			? static_cast<float>(FMath::Max(
				FWPLUTGenerator::ComputeNormalizedOuterRadius(
					State.RequestedDescriptor, State.TransitionRatio), 1.0))
			: 1.0f + State.ThroatHalfLengthCm / State.PortalRadiusCm;
	State.NormalizedOuterRadius = NormalizedOuterRadius;
	State.MetricOuterRadiusCm = FMath::Max(
		State.PortalRadiusCm * NormalizedOuterRadius, State.PortalRadiusCm);
}

void UWPLUTEndpointManager::RebuildStrongTextureReferences()
{
	BoundTextures.Reset();
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointState>& Pair : EndpointStates)
	{
		if (UVolumeTexture* Texture = Pair.Value.Binding.VolumeTexture.Get())
		{
			BoundTextures.AddUnique(Texture);
		}
	}
}

void UWPLUTEndpointManager::BroadcastState(
	const FEndpointState& State,
	const TCHAR*,
	const double)
{
	const FWPLUTEndpointSnapshot Snapshot = MakeSnapshot(State, true);
	const TWeakObjectPtr<AWormholePortalActor> PortalKey = Snapshot.Portal;
	EndpointChangedDelegate.Broadcast(PortalKey, Snapshot);
}

FWPLUTEndpointSnapshot UWPLUTEndpointManager::MakeSnapshot(
	const FEndpointState& State,
	const bool bRegistered) const
{
	FWPLUTEndpointSnapshot Snapshot;
	Snapshot.Portal = State.Portal;
	Snapshot.VolumeTexture = State.Binding.VolumeTexture.Get();
	Snapshot.CPUVolumeData = State.Binding.CPUVolumeData;
	Snapshot.RequestedDescriptor = State.RequestedDescriptor;
	Snapshot.BoundDescriptor = State.Binding.Descriptor;
	Snapshot.Contract = State.Contract;
	Snapshot.PortalRadiusCm = State.PortalRadiusCm;
	Snapshot.ThroatHalfLengthCm = State.ThroatHalfLengthCm;
	Snapshot.TransitionLengthCm = State.TransitionLengthCm;
	Snapshot.TransitionRatio = State.TransitionRatio;
	Snapshot.RatioCoordinate01 = State.RatioCoordinate01;
	Snapshot.NormalizedOuterRadius = State.NormalizedOuterRadius;
	Snapshot.MetricOuterRadiusCm = State.MetricOuterRadiusCm;
	Snapshot.BindingGeneration = State.BindingGeneration;
	Snapshot.ResourceRevision = State.Binding.ResourceRevision;
	Snapshot.EndpointRevision = State.EndpointRevision;
	Snapshot.RequestGeneration = State.RequestGeneration;
	Snapshot.bRegistered = bRegistered;
	Snapshot.bRequestPending = State.bRequestPending;
	Snapshot.bAnalyticNoTransition = State.bAnalyticNoTransition;
	Snapshot.LastError = State.LastError;
	return Snapshot;
}

FWPRayLUTContract UWPLUTEndpointManager::MakeContract(const FEndpointState& State)
{
	FWPRayLUTContract Contract;
	if (!State.Binding.IsReady() || State.BindingGeneration == 0)
	{
		return Contract;
	}
	const FWPLUTDescriptor Descriptor = State.Binding.Descriptor.GetSanitized();
	Contract.LayoutVersion = 1;
	Contract.Generation = State.BindingGeneration;
	Contract.Revision = State.Binding.ResourceRevision;
	Contract.ExpectedExtent = Descriptor.GetDimensions();
	Contract.ExpectedFormat = EWPRayLUTFormat::RGBA32Float;
	Contract.ExpectedMipCount = 1;
	Contract.ExpectedDimension = EWPRayLUTDimension::Texture3D;
	return Contract;
}

uint64 UWPLUTEndpointManager::AdvanceGeneration(const uint64 Generation)
{
	const uint64 Next = Generation + 1;
	return Next == 0 ? 1 : Next;
}

uint32 UWPLUTEndpointManager::AdvanceGeneration(const uint32 Generation)
{
	const uint32 Next = Generation + 1;
	return Next == 0 ? 1 : Next;
}

bool UWPLUTEndpointManager::RequestOptionsEqual(
	const FWPLUTEndpointRequestOptions& A,
	const FWPLUTEndpointRequestOptions& B)
{
	return A.PreferredAsset == B.PreferredAsset
		&& A.bOverrideDescriptor == B.bOverrideDescriptor
		&& (!A.bOverrideDescriptor || A.DescriptorOverride == B.DescriptorOverride)
		&& A.bOverrideRuntimeFallback == B.bOverrideRuntimeFallback
		&& (!A.bOverrideRuntimeFallback || A.bAllowRuntimeFallback == B.bAllowRuntimeFallback)
		&& A.DebugContext == B.DebugContext;
}

