// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPRuntimeSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Rendering/IWPRenderer.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Rendering/WPCaptureManager.h"
#include "Rendering/LUT/WPLUTEndpointManager.h"
#include "Subsystem/WPRegistrySubsystem.h"
#include "Subsystem/WPTransitSubsystem.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "WPSettings.h"
#include "Transit/WPTransitTypes.h"
#include "WormholePortalStats.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	constexpr double WPLegacyAtomicCaptureCadenceSeconds = 0.03;
	constexpr double WPMinimumTargetEndpointHz = 5.0;
	constexpr double WPMaximumTargetEndpointHz = 120.0;
	// Renderer의 analytic proxy safety shell과 같은 계약입니다.
	constexpr double WPCaptureProxySafetyShellCm = 1.0;

	TAutoConsoleVariable<int32> CVarWPRuntimeEnabled(
		TEXT("wp.RuntimeEnabled"),
		1,
		TEXT("Canonical switch for the production RenderPacket pipeline.\n")
		TEXT("0: production renderer disabled, 1: enabled (default)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarWPCaptureSchedulerMode(
		TEXT("wp.CaptureSchedulerMode"),
		2,
		TEXT("Runtime-owned production pair capture mode.\n")
		TEXT("0: deprecated alias of mode 1, 1: legacy atomic pair capture every 30 ms, ")
		TEXT("2: steady-state endpoint stagger at the configured endpoint Hz (default). ")
		TEXT("Invalid values fail closed to the same Runtime production path; Actor Tick is never used."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureTargetEndpointHz(
		TEXT("wp.CaptureTargetEndpointHz"),
		30.0f,
		TEXT("Target refresh rate per visible endpoint for scheduler mode 2. ")
		TEXT("Values are clamped to 30..120 Hz. Warmup, Transit, and accumulated two-endpoint cadence debt use atomic pair fallback."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarWPCaptureSchedulerFailureRollbackThreshold(
		TEXT("wp.CaptureSchedulerFailureRollbackThreshold"),
		1,
		TEXT("Consecutive Runtime pair submission failures before manager-owned authority/resource recovery. Minimum 1. The historical CVar name is retained for existing settings."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureVisibilityInvisibleHoldSeconds(
		TEXT("wp.CaptureVisibilityInvisibleHoldSeconds"),
		0.5f,
		TEXT("Fresh both-endpoints-invisible time required before steady-state pair capture pauses. Minimum 0."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureVisibilityFeedbackMaxAgeSeconds(
		TEXT("wp.CaptureVisibilityFeedbackMaxAgeSeconds"),
		0.25f,
		TEXT("Maximum age of Renderer visibility feedback accepted by the capture scheduler. Stale feedback fails open. Minimum 0.05 seconds."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureVisibilityHiddenRefreshHz(
		TEXT("wp.CaptureVisibilityHiddenRefreshHz"),
		0.0f,
		TEXT("Deprecated compatibility input. Production frustum policy always resolves this value to 0 Hz so V0 strictly blocks A+B capture. The requested value is retained only for diagnostics."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureOcclusionTraceIntervalSeconds(
		TEXT("wp.CaptureOcclusionTraceIntervalSeconds"),
		0.1f,
		TEXT("Minimum seconds between CPU SafeProxy occlusion evaluations per pair. ")
		TEXT("Clamped to 0.02..1.0 seconds."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureResolutionUpgradeHoldSeconds(
		TEXT("wp.CaptureResolutionUpgradeHoldSeconds"),
		0.75f,
		TEXT("Continuous screen-size time required before a VRAM-increasing resolution ")
		TEXT("transition. Minimum 0 seconds."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureResolutionDowngradeHoldSeconds(
		TEXT("wp.CaptureResolutionDowngradeHoldSeconds"),
		0.15f,
		TEXT("Continuous screen-size time required before a VRAM-reducing resolution ")
		TEXT("transition. Minimum 0 seconds."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureResolutionMinimumDwellSeconds(
		TEXT("wp.CaptureResolutionMinimumDwellSeconds"),
		0.5f,
		TEXT("Minimum time between completed Pair resolution transitions. A required ")
		TEXT("downgrade caused by the hard budget or invisibility bypasses this dwell."),
		ECVF_Default);

	// 로그 전용: renderer 미가용 경고의 반복 출력 간격만 제한합니다.
	constexpr double RendererUnavailableLogIntervalSeconds = 5.0;
	constexpr double PacketHeartbeatSeconds = 1.0;
	constexpr float CameraPublishDistanceCm = 0.1f;
	constexpr float CaptureVisibilityCameraLocationToleranceCm = 0.1f;
	constexpr float CaptureVisibilityCameraRotationToleranceDegrees = 0.01f;
	constexpr float CaptureVisibilityCameraFOVToleranceDegrees = 0.01f;
	// Frustum visibility와 CPU SafeProxy occlusion은 production 기본 계약입니다.
	// on/off CVar를 제공하지 않으며 모든 Runtime pair에 항상 적용합니다.
	constexpr bool WPCaptureVisibilityPolicyAlwaysEnabled = true;
	constexpr double WPCaptureOcclusionMinimumIntervalSeconds = 0.02;
	constexpr double WPCaptureOcclusionMaximumIntervalSeconds = 1.0;
	constexpr uint8 WPCaptureEndpointAMask = 1u << 0;
	constexpr uint8 WPCaptureEndpointBMask = 1u << 1;
	constexpr uint8 WPCaptureBothEndpointsMask =
		WPCaptureEndpointAMask | WPCaptureEndpointBMask;
	constexpr float TransformTolerance = 1.0e-4f;
	constexpr float SupportedScaleTolerance = 1.0e-3f;

	/**
	 * One normalized array element used by the Runtime capture scheduler. SourceIndex is kept
	 * only to diagnose whether the Project Settings list required threshold reordering.
	 */
	struct FWPDynamicCaptureResolutionTierPolicy
	{
		double StartRatio = 1.0;
		uint32 Resolution = 512;
		int32 SourceIndex = INDEX_NONE;
	};

	/**
	 * Project Settings screen percentages and Cubemap resolutions converted into the Scheduler
	 * contract. Tiers are stable-sorted by threshold, and resolutions are normalized to a
	 * nondecreasing power-of-two sequence so malformed user input cannot oscillate resources.
	 */
	struct FWPDynamicCaptureResolutionPolicy
	{
		TArray<FWPDynamicCaptureResolutionTierPolicy, TInlineAllocator<8>> Tiers;
		uint32 LowestVisibleResolution = 64;
		uint32 InsideResolution = 768;
		bool bThresholdOrderAdjusted = false;
		bool bResolutionOrderAdjusted = false;
	};

	uint32 NormalizeWPCaptureResolution(
		const int32 RequestedResolution,
		const int32 DefaultResolution)
	{
		return static_cast<uint32>(UWPSettings::NormalizeCaptureResolution(
			RequestedResolution,
			DefaultResolution));
	}

	FWPDynamicCaptureResolutionPolicy ResolveWPDynamicCaptureResolutionPolicy()
	{
		const UWPSettings* Settings = GetDefault<UWPSettings>();
		const auto SanitizePercent = [](const double Value, const double DefaultValue)
		{
			return FMath::IsFinite(Value)
				? FMath::Clamp(Value, 0.0, 100.0)
				: DefaultValue;
		};

		const int32 RawLowestVisibleResolution = Settings
			? Settings->CaptureLowestVisibleResolution : 64;
		const int32 RawInsideResolution = Settings
			? Settings->CaptureInsideSafeProxyResolution : 768;

		FWPDynamicCaptureResolutionPolicy Result;
		Result.LowestVisibleResolution = NormalizeWPCaptureResolution(
			RawLowestVisibleResolution, 64);
		Result.InsideResolution = FMath::Max(
			NormalizeWPCaptureResolution(RawInsideResolution, 768),
			Result.LowestVisibleResolution);
		Result.bResolutionOrderAdjusted =
			static_cast<uint32>(RawLowestVisibleResolution)
				!= Result.LowestVisibleResolution
			|| static_cast<uint32>(RawInsideResolution) != Result.InsideResolution;

		const auto AddTier = [&Result, &SanitizePercent](
			const double RawStartPercent,
			const int32 RawResolution,
			const int32 SourceIndex)
		{
			const double EffectiveStartPercent = SanitizePercent(RawStartPercent, 100.0);
			FWPDynamicCaptureResolutionTierPolicy& Tier = Result.Tiers.AddDefaulted_GetRef();
			Tier.StartRatio = EffectiveStartPercent / 100.0;
			Tier.Resolution = NormalizeWPCaptureResolution(RawResolution, 512);
			Tier.SourceIndex = SourceIndex;
			Result.bThresholdOrderAdjusted |= !FMath::IsNearlyEqual(
				RawStartPercent, EffectiveStartPercent);
			Result.bResolutionOrderAdjusted |= RawResolution
				!= static_cast<int32>(Tier.Resolution);
		};

		if (Settings)
		{
			Result.Tiers.Reserve(Settings->CaptureResolutionTiers.Num());
			for (int32 TierIndex = 0;
				TierIndex < Settings->CaptureResolutionTiers.Num();
				++TierIndex)
			{
				const FWPCaptureResolutionTier& SourceTier =
					Settings->CaptureResolutionTiers[TierIndex];
				AddTier(
					static_cast<double>(SourceTier.StartScreenHeightPercent),
					SourceTier.Resolution,
					TierIndex);
			}
		}
		else
		{
			AddTier(30.0, 128, 0);
			AddTier(60.0, 256, 1);
			AddTier(100.0, 512, 2);
		}

		Result.Tiers.StableSort(
			[](const FWPDynamicCaptureResolutionTierPolicy& Left,
				const FWPDynamicCaptureResolutionTierPolicy& Right)
			{
				return Left.StartRatio < Right.StartRatio;
			});

		uint32 PreviousResolution = Result.LowestVisibleResolution;
		for (int32 TierIndex = 0; TierIndex < Result.Tiers.Num(); ++TierIndex)
		{
			FWPDynamicCaptureResolutionTierPolicy& Tier = Result.Tiers[TierIndex];
			Result.bThresholdOrderAdjusted |= Tier.SourceIndex != TierIndex;
			const uint32 OrderedResolution = FMath::Max(Tier.Resolution, PreviousResolution);
			Result.bResolutionOrderAdjusted |= OrderedResolution != Tier.Resolution;
			Tier.Resolution = OrderedResolution;
			PreviousResolution = OrderedResolution;
		}
		return Result;
	}

	FString DescribeWPDynamicCaptureResolutionTiers(
		const FWPDynamicCaptureResolutionPolicy& Policy)
	{
		FString Description(TEXT("["));
		for (int32 TierIndex = 0; TierIndex < Policy.Tiers.Num(); ++TierIndex)
		{
			const FWPDynamicCaptureResolutionTierPolicy& Tier = Policy.Tiers[TierIndex];
			Description += FString::Printf(
				TEXT("%s%d:%.3f%%/%u"),
				TierIndex > 0 ? TEXT(",") : TEXT(""),
				TierIndex + 1,
				Tier.StartRatio * 100.0,
				Tier.Resolution);
		}
		Description += TEXT("]");
		return Description;
	}

	const TCHAR* GetWPCaptureResolutionPhaseName(
		const EWPCaptureResolutionTransitionPhase Phase)
	{
		switch (Phase)
		{
		case EWPCaptureResolutionTransitionPhase::Stable: return TEXT("Stable");
		case EWPCaptureResolutionTransitionPhase::AwaitingUnavailablePublication:
			return TEXT("AwaitingUnavailablePublication");
		case EWPCaptureResolutionTransitionPhase::WaitingForReleaseFence:
			return TEXT("WaitingForReleaseFence");
		case EWPCaptureResolutionTransitionPhase::AllocatingEndpointA:
			return TEXT("AllocatingEndpointA");
		case EWPCaptureResolutionTransitionPhase::WaitingForEndpointA:
			return TEXT("WaitingForEndpointA");
		case EWPCaptureResolutionTransitionPhase::AllocatingEndpointB:
			return TEXT("AllocatingEndpointB");
		case EWPCaptureResolutionTransitionPhase::WaitingForEndpointB:
			return TEXT("WaitingForEndpointB");
		case EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointA:
			return TEXT("SeamlessAllocatingEndpointA");
		case EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointA:
			return TEXT("SeamlessWaitingForEndpointA");
		case EWPCaptureResolutionTransitionPhase::SeamlessAllocatingEndpointB:
			return TEXT("SeamlessAllocatingEndpointB");
		case EWPCaptureResolutionTransitionPhase::SeamlessWaitingForEndpointB:
			return TEXT("SeamlessWaitingForEndpointB");
		case EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointA:
			return TEXT("SeamlessCapturingEndpointA");
		case EWPCaptureResolutionTransitionPhase::SeamlessCapturingEndpointB:
			return TEXT("SeamlessCapturingEndpointB");
		case EWPCaptureResolutionTransitionPhase::AwaitingSeamlessPublication:
			return TEXT("AwaitingSeamlessPublication");
		case EWPCaptureResolutionTransitionPhase::WaitingForRetiredReleaseFence:
			return TEXT("WaitingForRetiredReleaseFence");
		default: return TEXT("Unknown");
		}
	}

	uint32 ResolveWPDynamicCaptureTier(
		const double ScreenDiameterRatio,
		const FWPDynamicCaptureResolutionPolicy& Policy)
	{
		if (!FMath::IsFinite(ScreenDiameterRatio))
		{
			return Policy.LowestVisibleResolution;
		}

		uint32 Resolution = Policy.LowestVisibleResolution;
		for (const FWPDynamicCaptureResolutionTierPolicy& Tier : Policy.Tiers)
		{
			if (ScreenDiameterRatio < Tier.StartRatio)
			{
				break;
			}
			Resolution = Tier.Resolution;
		}
		return Resolution;
	}

	uint32 ResolveWPInsideCaptureResolution(
		const FWPDynamicCaptureResolutionPolicy& Policy)
	{
		return Policy.InsideResolution;
	}

	bool IsWPRuntimeEnabled(const int32 RuntimeEnabledRaw)
	{
		return RuntimeEnabledRaw != 0;
	}

	bool IsFiniteReferenceLocation(const FVector& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	bool IsFiniteReferenceRotation(const FRotator& Value)
	{
		return FMath::IsFinite(Value.Pitch)
			&& FMath::IsFinite(Value.Yaw)
			&& FMath::IsFinite(Value.Roll);
	}

	bool IsValidWPCaptureVisibilityFOV(const float FOVDegrees)
	{
		return FMath::IsFinite(FOVDegrees)
			&& FOVDegrees > 0.0f
			&& FOVDegrees < 180.0f;
	}

	double ResolveWPEffectiveHiddenRefreshHz(const double RequestedHz)
	{
		(void)RequestedHz;
		// The production contract is strict: a fresh V0 frustum result pauses both
		// captures. Keep the legacy CVar readable for diagnostics, but never let a
		// process-global console value silently turn "stop" back into a heartbeat.
		return 0.0;
	}

	bool ShouldBlockWPFrustumCapture(const bool bCaptureVisibilityPaused)
	{
		// V0 is the only state that marks the pair paused. V1 and V2 intentionally
		// stay active because both cubemaps must keep updating whenever either
		// portal surface intersects the primary view frustum.
		return bCaptureVisibilityPaused;
	}

	bool HasWPEffectiveVisibleEndpoint(
		const uint32 FrustumVisibleEndpointCount,
		const bool bOcclusionValid,
		const uint8 OcclusionVisibleEndpointMask,
		const bool bInsideSelected)
	{
		if (bInsideSelected)
		{
			return true;
		}
		// Occlusion이 유효하지 않으면 기존 frustum 결과를 그대로 사용해 fail-open합니다.
		return FrustumVisibleEndpointCount > 0
			&& (!bOcclusionValid || OcclusionVisibleEndpointMask != 0);
	}

	enum class EWPCaptureVisibilityFreshness : uint8
	{
		FeatureDisabled,
		IncoherentSnapshot,
		MissingSample,
		EpochMismatch,
		MissingRequiredPacketFloor,
		PacketBeforeRequiredFloor,
		PacketAfterPublishedState,
		InvalidOrStaleReceipt,
		Fresh
	};

	const TCHAR* GetWPCaptureVisibilityFreshnessName(
		const EWPCaptureVisibilityFreshness Freshness)
	{
		switch (Freshness)
		{
		case EWPCaptureVisibilityFreshness::FeatureDisabled:
			return TEXT("FeatureDisabled");
		case EWPCaptureVisibilityFreshness::IncoherentSnapshot:
			return TEXT("IncoherentSnapshot");
		case EWPCaptureVisibilityFreshness::MissingSample:
			return TEXT("MissingSample");
		case EWPCaptureVisibilityFreshness::EpochMismatch:
			return TEXT("EpochMismatch");
		case EWPCaptureVisibilityFreshness::MissingRequiredPacketFloor:
			return TEXT("MissingRequiredPacketFloor");
		case EWPCaptureVisibilityFreshness::PacketBeforeRequiredFloor:
			return TEXT("PacketBeforeRequiredFloor");
		case EWPCaptureVisibilityFreshness::PacketAfterPublishedState:
			return TEXT("PacketAfterPublishedState");
		case EWPCaptureVisibilityFreshness::InvalidOrStaleReceipt:
			return TEXT("InvalidOrStaleReceipt");
		case EWPCaptureVisibilityFreshness::Fresh:
			return TEXT("Fresh");
		default:
			return TEXT("Unknown");
		}
	}

	EWPCaptureVisibilityFreshness EvaluateWPCaptureVisibilityFreshness(
		const bool bFeatureEnabled,
		const bool bSnapshotCoherent,
		const uint64 FeedbackOwnershipEpoch,
		const uint64 CurrentOwnershipEpoch,
		const uint64 FeedbackPacketSequence,
		const uint64 RequiredPacketSequence,
		const uint64 PublishedPacketSequence,
		const uint64 FeedbackSampleSequence,
		const double FeedbackAgeSeconds,
		const double FeedbackMaxAgeSeconds)
	{
		// This first branch is the strict A/B baseline: callers need not inspect
		// any visibility mailbox field when the feature is disabled.
		if (!bFeatureEnabled)
		{
			return EWPCaptureVisibilityFreshness::FeatureDisabled;
		}
		if (!bSnapshotCoherent)
		{
			return EWPCaptureVisibilityFreshness::IncoherentSnapshot;
		}
		if (FeedbackOwnershipEpoch == 0 || FeedbackPacketSequence == 0
			|| FeedbackSampleSequence == 0)
		{
			return EWPCaptureVisibilityFreshness::MissingSample;
		}
		if (FeedbackOwnershipEpoch != CurrentOwnershipEpoch)
		{
			return EWPCaptureVisibilityFreshness::EpochMismatch;
		}
		if (RequiredPacketSequence == 0)
		{
			return EWPCaptureVisibilityFreshness::MissingRequiredPacketFloor;
		}
		if (FeedbackPacketSequence < RequiredPacketSequence)
		{
			return EWPCaptureVisibilityFreshness::PacketBeforeRequiredFloor;
		}
		if (FeedbackPacketSequence > PublishedPacketSequence)
		{
			return EWPCaptureVisibilityFreshness::PacketAfterPublishedState;
		}
		if (!FMath::IsFinite(FeedbackAgeSeconds)
			|| !FMath::IsFinite(FeedbackMaxAgeSeconds)
			|| FeedbackAgeSeconds < 0.0
			|| FeedbackMaxAgeSeconds < 0.0
			|| FeedbackAgeSeconds > FeedbackMaxAgeSeconds)
		{
			return EWPCaptureVisibilityFreshness::InvalidOrStaleReceipt;
		}
		return EWPCaptureVisibilityFreshness::Fresh;
	}

	bool IsWPCaptureVisibilitySampleGapContinuous(
		const uint64 PreviousOwnershipEpoch,
		const uint64 PreviousSampleSequence,
		const uint64 CurrentOwnershipEpoch,
		const double SampleGapSeconds,
		const double FeedbackMaxAgeSeconds)
	{
		return PreviousOwnershipEpoch == CurrentOwnershipEpoch
			&& PreviousOwnershipEpoch != 0
			&& PreviousSampleSequence != 0
			&& FMath::IsFinite(SampleGapSeconds)
			&& FMath::IsFinite(FeedbackMaxAgeSeconds)
			&& SampleGapSeconds >= 0.0
			&& FeedbackMaxAgeSeconds >= 0.0
			&& SampleGapSeconds <= FeedbackMaxAgeSeconds;
	}

	bool IsWPCaptureVisibilitySampleBeyondRejectBarrier(
		const uint64 CurrentOwnershipEpoch,
		const uint64 CurrentSampleSequence,
		const uint64 RejectedOwnershipEpoch,
		const uint64 RejectedThroughSampleSequence)
	{
		if (RejectedOwnershipEpoch == 0
			|| RejectedThroughSampleSequence == 0)
		{
			return true;
		}
		// The feedback mailbox exposes only its latest monotonic sample per
		// ownership epoch. A different current-epoch tuple is therefore the next
		// usable observation; inequality also stays correct across uint64 wrap.
		return CurrentOwnershipEpoch != RejectedOwnershipEpoch
			|| CurrentSampleSequence != RejectedThroughSampleSequence;
	}

	enum class EWPCaptureVisibilityCameraGuard : uint8
	{
		NotRequired,
		ViewUnavailable,
		InvalidViewActor,
		InvalidLocation,
		InvalidRotation,
		InvalidFOV,
		SnapshotMissing,
		ViewActorChanged,
		LocationChanged,
		RotationChanged,
		FOVChanged,
		Stable
	};

	const TCHAR* GetWPCaptureVisibilityCameraGuardName(
		const EWPCaptureVisibilityCameraGuard Guard)
	{
		switch (Guard)
		{
		case EWPCaptureVisibilityCameraGuard::NotRequired:
			return TEXT("NotRequired");
		case EWPCaptureVisibilityCameraGuard::ViewUnavailable:
			return TEXT("ViewUnavailable");
		case EWPCaptureVisibilityCameraGuard::InvalidViewActor:
			return TEXT("InvalidViewActor");
		case EWPCaptureVisibilityCameraGuard::InvalidLocation:
			return TEXT("InvalidLocation");
		case EWPCaptureVisibilityCameraGuard::InvalidRotation:
			return TEXT("InvalidRotation");
		case EWPCaptureVisibilityCameraGuard::InvalidFOV:
			return TEXT("InvalidFOV");
		case EWPCaptureVisibilityCameraGuard::SnapshotMissing:
			return TEXT("SnapshotMissing");
		case EWPCaptureVisibilityCameraGuard::ViewActorChanged:
			return TEXT("ViewActorChanged");
		case EWPCaptureVisibilityCameraGuard::LocationChanged:
			return TEXT("LocationChanged");
		case EWPCaptureVisibilityCameraGuard::RotationChanged:
			return TEXT("RotationChanged");
		case EWPCaptureVisibilityCameraGuard::FOVChanged:
			return TEXT("FOVChanged");
		case EWPCaptureVisibilityCameraGuard::Stable:
			return TEXT("Stable");
		default:
			return TEXT("Unknown");
		}
	}

	EWPCaptureVisibilityCameraGuard EvaluateWPCaptureVisibilityCameraGuard(
		const bool bGuardRequired,
		const bool bReferenceViewAvailable,
		const uint32 CurrentViewActorId,
		const FVector& CurrentCameraLocation,
		const FRotator& CurrentCameraRotation,
		const float CurrentCameraFOVDegrees,
		const bool bGuardSnapshotInitialized,
		const uint32 GuardViewActorId,
		const FVector& GuardCameraLocation,
		const FRotator& GuardCameraRotation,
		const float GuardCameraFOVDegrees)
	{
		if (!bGuardRequired)
		{
			return EWPCaptureVisibilityCameraGuard::NotRequired;
		}
		if (!bReferenceViewAvailable)
		{
			return EWPCaptureVisibilityCameraGuard::ViewUnavailable;
		}
		if (CurrentViewActorId == 0)
		{
			return EWPCaptureVisibilityCameraGuard::InvalidViewActor;
		}
		if (!IsFiniteReferenceLocation(CurrentCameraLocation))
		{
			return EWPCaptureVisibilityCameraGuard::InvalidLocation;
		}
		if (!IsFiniteReferenceRotation(CurrentCameraRotation))
		{
			return EWPCaptureVisibilityCameraGuard::InvalidRotation;
		}
		if (!IsValidWPCaptureVisibilityFOV(CurrentCameraFOVDegrees))
		{
			return EWPCaptureVisibilityCameraGuard::InvalidFOV;
		}
		if (!bGuardSnapshotInitialized)
		{
			return EWPCaptureVisibilityCameraGuard::SnapshotMissing;
		}
		if (CurrentViewActorId != GuardViewActorId)
		{
			return EWPCaptureVisibilityCameraGuard::ViewActorChanged;
		}
		if (FVector::Dist(CurrentCameraLocation, GuardCameraLocation)
			> CaptureVisibilityCameraLocationToleranceCm)
		{
			return EWPCaptureVisibilityCameraGuard::LocationChanged;
		}
		if (!CurrentCameraRotation.Equals(
			GuardCameraRotation,
			CaptureVisibilityCameraRotationToleranceDegrees))
		{
			return EWPCaptureVisibilityCameraGuard::RotationChanged;
		}
		if (FMath::Abs(CurrentCameraFOVDegrees - GuardCameraFOVDegrees)
			> CaptureVisibilityCameraFOVToleranceDegrees)
		{
			return EWPCaptureVisibilityCameraGuard::FOVChanged;
		}
		return EWPCaptureVisibilityCameraGuard::Stable;
	}

	bool ShouldAdvanceWPCaptureVisibilityPacketFloor(
		const bool bVisibilityFeedbackEnabled,
		const bool bInitial,
		const bool bResourceReadinessChanged,
		const bool bMetricChanged,
		const bool bTransformChanged,
		const bool bOwnershipChanged)
	{
		return bVisibilityFeedbackEnabled
			&& (bInitial
				|| bResourceReadinessChanged
				|| bMetricChanged
				|| bTransformChanged
				|| bOwnershipChanged);
	}

	bool ResolvePrimaryLocalReferenceView(
		const UWorld* World,
		FVector& OutCameraLocation,
		FRotator& OutCameraRotation,
		AActor*& OutViewActor,
		const APlayerController*& OutPlayerController,
		float& OutCameraFOVDegrees)
	{
		OutPlayerController = nullptr;
		OutCameraFOVDegrees = 0.0f;
		if (!World || World->GetNetMode() == NM_DedicatedServer)
		{
			return false;
		}

		// A listen server can enumerate a remote controller first. Match the capture manager's
		// local-player-first policy so render packets and cube captures use the same camera.
		for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			const APlayerController* PlayerController = Iterator->Get();
			const APlayerCameraManager* CameraManager =
				PlayerController && PlayerController->IsLocalController()
					? PlayerController->PlayerCameraManager
					: nullptr;
			if (!CameraManager)
			{
				continue;
			}

			const FVector CandidateLocation = CameraManager->GetCameraLocation();
			const FRotator CandidateRotation = CameraManager->GetCameraRotation();
			if (!IsFiniteReferenceLocation(CandidateLocation))
			{
				continue;
			}
			// CaptureManager selects this first finite local camera. If its rotation is corrupt,
			// fail closed instead of pairing this capture with a different local player's view.
			if (!IsFiniteReferenceRotation(CandidateRotation))
			{
				return false;
			}

			OutCameraLocation = CandidateLocation;
			OutCameraRotation = CandidateRotation;
			OutViewActor = PlayerController->GetViewTarget();
			OutPlayerController = PlayerController;
			// A corrupt FOV must not invalidate the established reference-camera
			// path. Only the optional visibility pause consumes/validates it.
			OutCameraFOVDegrees = CameraManager->GetFOVAngle();
			return true;
		}
		return false;
	}

	enum class EWPRegistryPairReconcileAction : uint8
	{
		Add,
		Keep,
		Replace
	};

	EWPRegistryPairReconcileAction EvaluateWPRegistryPairReconcileAction(
		const bool bHasExistingState,
		const FGuid& ExistingPairId,
		const FGuid& RegistryPairId)
	{
		if (!bHasExistingState)
		{
			return EWPRegistryPairReconcileAction::Add;
		}

		return ExistingPairId == RegistryPairId
			? EWPRegistryPairReconcileAction::Keep
			: EWPRegistryPairReconcileAction::Replace;
	}

	struct FWPCaptureSchedulerModePolicyResult
	{
		const TCHAR* DecisionReason = TEXT("InvalidModeRuntimeProductionCapture");
		bool bModeValid = false;
		bool bRuntimeActive = false;
		bool bDeprecatedAlias = false;
		bool bStaggeredEndpointSubmission = false;
	};

	FWPCaptureSchedulerModePolicyResult EvaluateWPCaptureSchedulerModePolicy(
		const int32 ModeRaw,
		const bool bRenderPacketPipelineActive)
	{
		(void)bRenderPacketPipelineActive;
		FWPCaptureSchedulerModePolicyResult Result;
		Result.bModeValid = ModeRaw == 0 || ModeRaw == 1 || ModeRaw == 2;
		Result.bRuntimeActive = true;
		if (ModeRaw == 2)
		{
			Result.DecisionReason = TEXT("RuntimeStaggeredEndpointCapture");
			Result.bStaggeredEndpointSubmission = true;
			return Result;
		}
		if (ModeRaw == 1)
		{
			Result.DecisionReason = TEXT("RuntimeLegacyAtomicCapture");
			return Result;
		}
		Result.DecisionReason = ModeRaw == 0
			? TEXT("DeprecatedAliasRuntimeLegacyAtomicCapture")
			: TEXT("InvalidModeRuntimeProductionCapture");
		Result.bDeprecatedAlias = ModeRaw == 0;
		return Result;
	}

	double GetWPCaptureTargetEndpointHz()
	{
		const double RawHz =
			static_cast<double>(CVarWPCaptureTargetEndpointHz.GetValueOnGameThread());
		return FMath::Clamp(
			FMath::IsFinite(RawHz) ? RawHz : WPMinimumTargetEndpointHz,
			WPMinimumTargetEndpointHz,
			WPMaximumTargetEndpointHz);
	}

	double GetWPEndpointCaptureCadenceSeconds()
	{
		return 1.0 / GetWPCaptureTargetEndpointHz();
	}

	double GetWPStaggeredSubmissionCadenceSeconds()
	{
		return 0.5 / GetWPCaptureTargetEndpointHz();
	}

	const TCHAR* GetWPManagedSubmissionModeName(
		const EWPManagedCaptureSubmissionMode SubmissionMode)
	{
		switch (SubmissionMode)
		{
		case EWPManagedCaptureSubmissionMode::AtomicPair:
			return TEXT("AtomicPair");
		case EWPManagedCaptureSubmissionMode::EndpointA:
			return TEXT("EndpointA");
		case EWPManagedCaptureSubmissionMode::EndpointB:
			return TEXT("EndpointB");
		default:
			return TEXT("Unknown");
		}
	}

	enum class EWPFixedCaptureDecision : uint8
	{
		IntervalPending,
		IntervalDue,
		CommitFreshCameraWait,
		NonFiniteElapsed
	};

	struct FWPFixedCaptureCadenceResult
	{
		EWPFixedCaptureDecision Decision = EWPFixedCaptureDecision::IntervalPending;
		const TCHAR* DecisionReason = TEXT("IntervalPending");
		bool bWouldSubmit = false;
		bool bIntervalDue = false;
	};

	FWPFixedCaptureCadenceResult EvaluateWPFixedCaptureCadence(
		const double ElapsedSeconds,
		const double CadenceSeconds,
		const bool bCommitWaitingForFreshCamera)
	{
		FWPFixedCaptureCadenceResult Result;
		if (bCommitWaitingForFreshCamera)
		{
			Result.Decision = EWPFixedCaptureDecision::CommitFreshCameraWait;
			Result.DecisionReason = TEXT("CommitFreshCameraWait");
			return Result;
		}
		if (!FMath::IsFinite(ElapsedSeconds)
			|| !FMath::IsFinite(CadenceSeconds) || CadenceSeconds <= 0.0)
		{
			Result.Decision = EWPFixedCaptureDecision::NonFiniteElapsed;
			Result.DecisionReason = TEXT("NonFiniteElapsed");
			return Result;
		}
		if (ElapsedSeconds >= CadenceSeconds)
		{
			Result.Decision = EWPFixedCaptureDecision::IntervalDue;
			Result.DecisionReason = TEXT("IntervalDue");
			Result.bWouldSubmit = true;
			Result.bIntervalDue = true;
		}
		return Result;
	}

	bool ShouldUseWPCaptureCadenceDebtAtomicFallback(
		const bool bStaggeredEndpointSubmission,
		const bool bWarmupRequiresAtomicPair,
		const double CaptureCadenceElapsedSeconds,
		const double EndpointCadenceSeconds)
	{
		return bStaggeredEndpointSubmission
			&& !bWarmupRequiresAtomicPair
			&& FMath::IsFinite(CaptureCadenceElapsedSeconds)
			&& FMath::IsFinite(EndpointCadenceSeconds)
			&& EndpointCadenceSeconds > 0.0
			&& CaptureCadenceElapsedSeconds >= EndpointCadenceSeconds;
	}

	struct FWPAtomicPairSubmissionResult
	{
		uint32 GenerationDeltaA = 0;
		uint32 GenerationDeltaB = 0;
		bool bPairEpochCoherent = false;
		bool bAtomicPairSubmission = false;
	};

	uint64 AdvanceWPCapturePairEpoch(const uint64 Epoch)
	{
		uint64 Result = Epoch + 1;
		if (Result == 0)
		{
			++Result;
		}
		return Result;
	}

	FWPAtomicPairSubmissionResult EvaluateWPAtomicPairSubmission(
		const uint32 GenerationABefore,
		const uint32 GenerationAAfter,
		const uint32 GenerationBBefore,
		const uint32 GenerationBAfter,
		const uint64 PairEpochABefore,
		const uint64 PairEpochAAfter,
		const uint64 PairEpochBBefore,
		const uint64 PairEpochBAfter)
	{
		FWPAtomicPairSubmissionResult Result;
		Result.GenerationDeltaA = GenerationAAfter - GenerationABefore;
		Result.GenerationDeltaB = GenerationBAfter - GenerationBBefore;
		Result.bPairEpochCoherent =
			PairEpochABefore == PairEpochBBefore
			&& PairEpochAAfter == PairEpochBAfter
			&& PairEpochAAfter == AdvanceWPCapturePairEpoch(PairEpochABefore);
		Result.bAtomicPairSubmission = Result.GenerationDeltaA == 1
			&& Result.GenerationDeltaB == 1
			&& Result.bPairEpochCoherent;
		return Result;
	}

	/**
	 * Packet publication의 순수 정책 입력입니다. CaptureGeneration 변화는 진단 정보로만 전달하며,
	 * capture-ready의 최초 전환과 resource/state 변화만 즉시 publish 사유가 됩니다.
	 */
	struct FWPPublishPolicyInput
	{
		bool bHasPublished = false;
		bool bDirty = false;
		bool bInitial = false;
		bool bResourceReadinessChanged = false;
		bool bCaptureReadinessChanged = false;
		bool bMetricChanged = false;
		bool bCubeContractChanged = false;
		bool bLUTContractChanged = false;
		bool bTransformChanged = false;
		bool bTransitChanged = false;
		bool bOwnershipChanged = false;
		bool bCaptureGenerationChanged = false;
		double SecondsSinceLastPublish = 0.0;
	};

	struct FWPPublishPolicyResult
	{
		bool bImmediate = false;
		bool bHeartbeat = false;
		bool bShouldPublish = false;
		bool bCaptureDiagnosticCoalesced = false;
	};

	bool HasWPEndpointCaptureReadinessChanged(
		const uint32 CurrentGeneration,
		const uint32 LastPublishedGeneration)
	{
		// CaptureGeneration is content metadata, but its zero/non-zero boundary is a
		// resource usability transition. Keep that boundary per endpoint so a local-
		// cube-only mode does not wait for the other endpoint or the 1 Hz heartbeat.
		return (CurrentGeneration != 0) != (LastPublishedGeneration != 0);
	}

	FWPPublishPolicyResult EvaluateWPPublishPolicy(
		const FWPPublishPolicyInput& Input)
	{
		FWPPublishPolicyResult Result;
		Result.bImmediate = Input.bDirty
			|| Input.bInitial
			|| Input.bResourceReadinessChanged
			|| Input.bCaptureReadinessChanged
			|| Input.bMetricChanged
			|| Input.bCubeContractChanged
			|| Input.bLUTContractChanged
			|| Input.bTransformChanged
			|| Input.bTransitChanged
			|| Input.bOwnershipChanged;
		Result.bHeartbeat = Input.bHasPublished
			&& Input.SecondsSinceLastPublish >= PacketHeartbeatSeconds;
		Result.bShouldPublish = Result.bImmediate || Result.bHeartbeat;
		Result.bCaptureDiagnosticCoalesced = Input.bCaptureGenerationChanged
			&& !Result.bShouldPublish;
		return Result;
	}

	uint64 AdvanceWPOwnershipEpoch(const uint64 CurrentEpoch)
	{
		// Saturation is safer than wrapping to zero, which is reserved for the never-requested default.
		return CurrentEpoch == MAX_uint64 ? MAX_uint64 : CurrentEpoch + 1;
	}

	struct FWPOwnershipPolicyInput
	{
		EWPPairOwnershipMode CurrentRequested = EWPPairOwnershipMode::Disabled;
		EWPPairOwnershipMode CurrentEffective = EWPPairOwnershipMode::Disabled;
		EWPPairOwnershipMode DesiredRequested = EWPPairOwnershipMode::Disabled;
		uint64 CurrentEpoch = 0;
		uint64 WarmupSucceededEpoch = 0;
		uint64 ProductionFailedEpoch = 0;
		bool bRequestedChanged = false;
		bool bResourceIdentityChanged = false;
		bool bInputsReady = false;
		bool bPreviousInputsReady = false;
	};

	struct FWPOwnershipPolicyResult
	{
		EWPPairOwnershipMode Requested = EWPPairOwnershipMode::Disabled;
		EWPPairOwnershipMode Effective = EWPPairOwnershipMode::Disabled;
		uint64 Epoch = 0;
		const TCHAR* DecisionReason = TEXT("Unchanged");
		bool bStateChanged = false;
		bool bStartedWarmup = false;
		bool bCommittedProduction = false;
		bool bRestartedWarmup = false;
	};

	/**
	 * Production compositor의 순수 상태 정책입니다. Warmup->Production만 같은 epoch을 유지하며,
	 * request/resource/failure invalidation은 stale RT feedback을 막기 위해 새 epoch을 만듭니다.
	 */
	FWPOwnershipPolicyResult EvaluateWPOwnershipPolicy(
		const FWPOwnershipPolicyInput& Input)
	{
		FWPOwnershipPolicyResult Result;
		Result.Requested = Input.DesiredRequested;
		Result.Effective = Input.CurrentEffective;
		Result.Epoch = Input.CurrentEpoch;

		const bool bControlChanged = Input.bRequestedChanged;
		const bool bWasProductionPathActive = Input.CurrentEffective
			!= EWPPairOwnershipMode::Disabled;

		if (Input.DesiredRequested == EWPPairOwnershipMode::Disabled)
		{
			Result.Effective = EWPPairOwnershipMode::Disabled;
			if (bControlChanged || bWasProductionPathActive)
			{
				Result.Epoch = AdvanceWPOwnershipEpoch(Input.CurrentEpoch);
			}
			Result.DecisionReason = TEXT("RuntimePipelineDisabled");
			Result.bRestartedWarmup = false;
		}
		else
		{
			// Only a committed Production state can produce a production failure.
			// Warmup failures remain on the same epoch and are retried when a fresh
			// packet arrives; advancing every frame would create packet/log churn.
			const bool bProductionFailed =
				Input.CurrentEffective == EWPPairOwnershipMode::Production
				&& Input.CurrentEpoch != 0
				&& Input.ProductionFailedEpoch == Input.CurrentEpoch;
			const bool bReadinessLost = (Input.bPreviousInputsReady && !Input.bInputsReady)
				|| (Input.CurrentEffective == EWPPairOwnershipMode::Production
					&& !Input.bInputsReady);
			const bool bStartWarmupEpoch = Input.CurrentEpoch == 0
				|| Input.CurrentEffective == EWPPairOwnershipMode::Disabled
				|| bControlChanged
				|| Input.bResourceIdentityChanged
				|| bReadinessLost
				|| bProductionFailed;

			if (bStartWarmupEpoch)
			{
				Result.Epoch = AdvanceWPOwnershipEpoch(Input.CurrentEpoch);
				Result.Effective = EWPPairOwnershipMode::Warmup;
				Result.bStartedWarmup = true;
				Result.bRestartedWarmup = Input.CurrentEffective
					== EWPPairOwnershipMode::Production;
				if (bProductionFailed)
				{
					Result.DecisionReason = TEXT("SameEpochProductionFailure");
				}
				else if (bReadinessLost)
				{
					Result.DecisionReason = TEXT("OwnershipInputsLost");
				}
				else if (Input.bResourceIdentityChanged)
				{
					Result.DecisionReason = TEXT("ResourceIdentityChanged");
				}
				else if (bControlChanged)
				{
					Result.DecisionReason = TEXT("RequestChanged");
				}
				else
				{
					Result.DecisionReason = TEXT("ProductionWarmupRequested");
				}
			}
			else if (Input.CurrentEffective == EWPPairOwnershipMode::Warmup
				&& Input.bInputsReady
				&& Input.WarmupSucceededEpoch == Input.CurrentEpoch
				&& Input.ProductionFailedEpoch != Input.CurrentEpoch)
			{
				// Exact same-epoch acknowledgement is the only commit path.
				Result.Effective = EWPPairOwnershipMode::Production;
				Result.bCommittedProduction = true;
				Result.DecisionReason = TEXT("SameEpochWarmupSucceeded");
			}
			else if (!Input.bInputsReady)
			{
				Result.Effective = EWPPairOwnershipMode::Warmup;
				Result.DecisionReason = TEXT("WaitingForBothEndpoints");
			}
			else
			{
				Result.DecisionReason = Input.CurrentEffective
					== EWPPairOwnershipMode::Production
					? TEXT("ProductionHealthy")
					: TEXT("WaitingForSameEpochWarmup");
			}
		}

		Result.bStateChanged = Result.Requested != Input.CurrentRequested
			|| Result.Effective != Input.CurrentEffective
			|| Result.Epoch != Input.CurrentEpoch;
		return Result;
	}

	enum class EWPTransitLifecyclePhase : uint8
	{
		Unknown,
		Started,
		Committed,
		Cancelled
	};

	struct FWPTransitOrderDecision
	{
		bool bShouldApply = false;
		const TCHAR* DecisionReason = TEXT("None");
	};

	EWPTransitLifecyclePhase ParseWPTransitLifecyclePhase(const TCHAR* Phase)
	{
		if (FCString::Stricmp(Phase, TEXT("Started")) == 0)
		{
			return EWPTransitLifecyclePhase::Started;
		}
		if (FCString::Stricmp(Phase, TEXT("Committed")) == 0)
		{
			return EWPTransitLifecyclePhase::Committed;
		}
		if (FCString::Stricmp(Phase, TEXT("Cancelled")) == 0)
		{
			return EWPTransitLifecyclePhase::Cancelled;
		}
		return EWPTransitLifecyclePhase::Unknown;
	}

	FWPTransitOrderDecision EvaluateWPTransitEventOrder(
		const uint64 EventSequence,
		const uint64 LastEventSequence,
		const bool bTransitActive,
		const EWPTransitLifecyclePhase Phase)
	{
		if (EventSequence == 0)
		{
			return { false, TEXT("InvalidZeroSequence") };
		}
		if (Phase == EWPTransitLifecyclePhase::Unknown)
		{
			return { false, TEXT("UnknownPhase") };
		}

		if (Phase == EWPTransitLifecyclePhase::Started)
		{
			if (EventSequence < LastEventSequence)
			{
				return { false, TEXT("StaleSequence") };
			}
			if (EventSequence == LastEventSequence)
			{
				return { false, TEXT("DuplicateStarted") };
			}
			return {
				true,
				bTransitActive ? TEXT("StartedSupersededActiveRun") : TEXT("StartedNewSequence")
			};
		}

		// TransitSubsystem assigns one sequence to the entire run, so its terminal event must
		// reuse the Started sequence while that run is still active.
		if (EventSequence < LastEventSequence)
		{
			return { false, TEXT("StaleSequence") };
		}
		if (EventSequence == LastEventSequence && !bTransitActive)
		{
			return { false, TEXT("DuplicateTerminal") };
		}
		return {
			true,
			EventSequence > LastEventSequence
				? TEXT("RecoveredTerminalWithoutStarted")
				: TEXT("TerminalForActiveRun")
		};
	}

	void ApplyWPTransitLifecycleState(
		const EWPTransitLifecyclePhase Phase,
		const uint64 EventSequence,
		const EWPSide SourceSide,
		const EWPSide DestinationSide,
		uint64& InOutLastEventSequence,
		EWPSide& InOutEntrySide,
		EWPSide& InOutCurrentSide,
		bool& bInOutTransitActive)
	{
		InOutLastEventSequence = EventSequence;
		if (Phase == EWPTransitLifecyclePhase::Started)
		{
			InOutEntrySide = SourceSide;
			InOutCurrentSide = SourceSide;
			bInOutTransitActive = true;
		}
		else if (Phase == EWPTransitLifecyclePhase::Committed)
		{
			InOutCurrentSide = DestinationSide;
			InOutEntrySide = EWPSide::None;
			bInOutTransitActive = false;
		}
		else
		{
			InOutCurrentSide = SourceSide;
			InOutEntrySide = EWPSide::None;
			bInOutTransitActive = false;
		}
	}

	FString PairIdToString(const FGuid& PairId)
	{
		return PairId.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	const TCHAR* GetWorldTypeName(const EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::None: return TEXT("None");
		case EWorldType::Game: return TEXT("Game");
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview: return TEXT("GamePreview");
		case EWorldType::GameRPC: return TEXT("GameRPC");
		case EWorldType::Inactive: return TEXT("Inactive");
		default: return TEXT("Unknown");
		}
	}

#if !UE_BUILD_SHIPPING
	const TCHAR* GetRuntimePortalChangeTypeName(const EWPPortalChangeType ChangeType)
	{
		switch (ChangeType)
		{
		case EWPPortalChangeType::Link: return TEXT("Link");
		case EWPPortalChangeType::Metric: return TEXT("Metric");
		case EWPPortalChangeType::Visual: return TEXT("Visual");
		case EWPPortalChangeType::RenderResources: return TEXT("RenderResources");
		default: return TEXT("Unknown");
		}
	}
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPTransitLifecycleSequenceTest,
	"WormholePortal.Runtime.TransitLifecycleSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPTransitLifecycleSequenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	uint64 LastSequence = 0;
	EWPSide EntrySide = EWPSide::None;
	EWPSide CurrentSide = EWPSide::None;
	bool bTransitActive = false;

	const FWPTransitOrderDecision Started = EvaluateWPTransitEventOrder(
		1, LastSequence, bTransitActive, EWPTransitLifecyclePhase::Started);
	TestTrue(TEXT("A new Started sequence is accepted."), Started.bShouldApply);
	ApplyWPTransitLifecycleState(
		EWPTransitLifecyclePhase::Started,
		1,
		EWPSide::SideA,
		EWPSide::SideB,
		LastSequence,
		EntrySide,
		CurrentSide,
		bTransitActive);
	TestEqual(TEXT("Started stores its run sequence."), LastSequence, uint64(1));
	TestEqual(TEXT("Started stores the source as entry side."),
		static_cast<int32>(EntrySide), static_cast<int32>(EWPSide::SideA));
	TestEqual(TEXT("Started stores the source as current side."),
		static_cast<int32>(CurrentSide), static_cast<int32>(EWPSide::SideA));
	TestTrue(TEXT("Started marks the lifecycle active."), bTransitActive);

	const FWPTransitOrderDecision Committed = EvaluateWPTransitEventOrder(
		1, LastSequence, bTransitActive, EWPTransitLifecyclePhase::Committed);
	TestTrue(TEXT("Committed may close the active run with the same sequence."), Committed.bShouldApply);
	ApplyWPTransitLifecycleState(
		EWPTransitLifecyclePhase::Committed,
		1,
		EWPSide::SideA,
		EWPSide::SideB,
		LastSequence,
		EntrySide,
		CurrentSide,
		bTransitActive);
	TestEqual(TEXT("Committed keeps the run sequence."), LastSequence, uint64(1));
	TestEqual(TEXT("Committed clears the entry side."),
		static_cast<int32>(EntrySide), static_cast<int32>(EWPSide::None));
	TestEqual(TEXT("Committed stores the destination side."),
		static_cast<int32>(CurrentSide), static_cast<int32>(EWPSide::SideB));
	TestFalse(TEXT("Committed closes the lifecycle."), bTransitActive);

	LastSequence = 2;
	EntrySide = EWPSide::SideB;
	CurrentSide = EWPSide::SideB;
	bTransitActive = true;
	const FWPTransitOrderDecision Cancelled = EvaluateWPTransitEventOrder(
		2, LastSequence, bTransitActive, EWPTransitLifecyclePhase::Cancelled);
	TestTrue(TEXT("Cancelled may close the active run with the same sequence."), Cancelled.bShouldApply);
	ApplyWPTransitLifecycleState(
		EWPTransitLifecyclePhase::Cancelled,
		2,
		EWPSide::SideB,
		EWPSide::SideA,
		LastSequence,
		EntrySide,
		CurrentSide,
		bTransitActive);
	TestEqual(TEXT("Cancelled clears the entry side."),
		static_cast<int32>(EntrySide), static_cast<int32>(EWPSide::None));
	TestEqual(TEXT("Cancelled restores the source side."),
		static_cast<int32>(CurrentSide), static_cast<int32>(EWPSide::SideB));
	TestFalse(TEXT("Cancelled closes the lifecycle."), bTransitActive);

	const FWPTransitOrderDecision DuplicateStarted = EvaluateWPTransitEventOrder(
		1, 1, true, EWPTransitLifecyclePhase::Started);
	TestFalse(TEXT("A duplicate Started event is rejected."), DuplicateStarted.bShouldApply);
	TestEqual(TEXT("Duplicate Started rejection reason is diagnostic."),
		FString(DuplicateStarted.DecisionReason), FString(TEXT("DuplicateStarted")));

	const FWPTransitOrderDecision DuplicateTerminal = EvaluateWPTransitEventOrder(
		1, 1, false, EWPTransitLifecyclePhase::Committed);
	TestFalse(TEXT("A duplicate terminal event is rejected after the run closes."),
		DuplicateTerminal.bShouldApply);
	TestEqual(TEXT("Duplicate terminal rejection reason is diagnostic."),
		FString(DuplicateTerminal.DecisionReason), FString(TEXT("DuplicateTerminal")));

	const FWPTransitOrderDecision StaleTerminal = EvaluateWPTransitEventOrder(
		1, 2, true, EWPTransitLifecyclePhase::Cancelled);
	TestFalse(TEXT("A terminal event older than the active sequence is rejected."),
		StaleTerminal.bShouldApply);

	const FWPTransitOrderDecision RecoveredTerminal = EvaluateWPTransitEventOrder(
		3, 2, false, EWPTransitLifecyclePhase::Committed);
	TestTrue(TEXT("A newer terminal event recovers state when Started was missed."),
		RecoveredTerminal.bShouldApply);
	TestEqual(TEXT("Recovered terminal decision reason is diagnostic."),
		FString(RecoveredTerminal.DecisionReason), FString(TEXT("RecoveredTerminalWithoutStarted")));
	uint64 RecoveredSequence = 2;
	EWPSide RecoveredEntrySide = EWPSide::SideA;
	EWPSide RecoveredCurrentSide = EWPSide::SideA;
	bool bRecoveredTransitActive = true;
	ApplyWPTransitLifecycleState(
		EWPTransitLifecyclePhase::Committed,
		3,
		EWPSide::SideA,
		EWPSide::SideB,
		RecoveredSequence,
		RecoveredEntrySide,
		RecoveredCurrentSide,
		bRecoveredTransitActive);
	TestEqual(TEXT("Recovered terminal advances to its sequence."), RecoveredSequence, uint64(3));
	TestEqual(TEXT("Recovered terminal clears the stale entry side."),
		static_cast<int32>(RecoveredEntrySide), static_cast<int32>(EWPSide::None));
	TestEqual(TEXT("Recovered committed terminal stores the destination side."),
		static_cast<int32>(RecoveredCurrentSide), static_cast<int32>(EWPSide::SideB));
	TestFalse(TEXT("Recovered terminal converges to an inactive lifecycle."), bRecoveredTransitActive);

	const FWPTransitOrderDecision NextStarted = EvaluateWPTransitEventOrder(
		2, 1, false, EWPTransitLifecyclePhase::Started);
	TestTrue(TEXT("The next monotonic Started sequence is accepted."), NextStarted.bShouldApply);

	const FWPTransitOrderDecision ZeroSequence = EvaluateWPTransitEventOrder(
		0, 0, false, EWPTransitLifecyclePhase::Started);
	TestFalse(TEXT("Sequence zero is rejected."), ZeroSequence.bShouldApply);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPRayLUTContractTest,
	"WormholePortal.Runtime.RayLUTContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPRayLUTContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FWPRayLUTContract Contract;
	TestFalse(TEXT("A default LUT contract is not ready."), Contract.IsValid());
	Contract.LayoutVersion = 6;
	Contract.Generation = 3;
	Contract.Revision = 0x12345678u;
	Contract.ExpectedExtent = FIntVector(512, 48, 24);
	Contract.ExpectedFormat = EWPRayLUTFormat::RGBA32Float;
	Contract.ExpectedMipCount = 1;
	Contract.ExpectedDimension = EWPRayLUTDimension::Texture3D;
	TestTrue(TEXT("A fully populated LUT contract is ready."), Contract.IsValid());

	FWPRayLUTContract RecreatedContract = Contract;
	TestTrue(TEXT("An exact LUT contract copy compares equal."), RecreatedContract == Contract);
	++RecreatedContract.Generation;
	TestTrue(TEXT("A resource regeneration changes contract identity even with identical content revision."),
		RecreatedContract != Contract);
	TestEqual(TEXT("Resource regeneration does not change the content revision."),
		RecreatedContract.Revision, Contract.Revision);

	FWPRayLUTContract InvalidExtentContract = Contract;
	InvalidExtentContract.ExpectedExtent.Y = 0;
	TestFalse(TEXT("A LUT contract with a zero expected extent is rejected."),
		InvalidExtentContract.IsValid());

	FWPRayLUTContract InvalidDepthContract = Contract;
	InvalidDepthContract.ExpectedExtent.Z = 0;
	TestFalse(TEXT("A volume LUT contract with zero expected depth is rejected."),
		InvalidDepthContract.IsValid());
	FWPRayLUTContract InvalidMipContract = Contract;
	InvalidMipContract.ExpectedMipCount = 0;
	TestFalse(TEXT("A volume LUT contract with no expected mip is rejected."),
		InvalidMipContract.IsValid());
	FWPRayLUTContract InvalidDimensionContract = Contract;
	InvalidDimensionContract.ExpectedDimension = EWPRayLUTDimension::Unknown;
	TestFalse(TEXT("A non-3D LUT contract is rejected."), InvalidDimensionContract.IsValid());

	FWPMetricSettings Metric;
	Metric.PortalRadiusCm = 100.0f;
	Metric.ThroatHalfLengthCm = 50.0f;
	Metric.TransitionLengthCm = 100.0f;
	Metric.MouthRadiusCm = 150.0f;
	Metric.MetricOuterRadiusCm = 125.0f;
	Metric.OuterRadiusCm = 250.0f;
	Metric.Revision = 1;
	TestTrue(TEXT("A metric accepts a radius distinct from the larger authored proxy bounds."),
		Metric.IsFiniteAndValid());

	FWPMetricSettings DifferentMetric = Metric;
	DifferentMetric.MetricOuterRadiusCm += 1.0f;
	TestFalse(TEXT("Metric compatibility detects a differing shader metric outer radius."),
		Metric.IsCompatibleWith(DifferentMetric));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPCubeContractTest,
	"WormholePortal.Runtime.CubeContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPCubeContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FWPCubeContract Contract;
	TestFalse(TEXT("A default cube contract is not ready."), Contract.IsValid());
	Contract.CubeLayoutVersion = 1;
	Contract.ResourceGeneration = 7;
	Contract.ExpectedExtent = FIntPoint(512, 512);
	Contract.ExpectedFormat = EWPCubeFormat::RGBA16Float;
	Contract.ExpectedMipCount = 1;
	Contract.ExpectedDimension = EWPCubeDimension::TextureCube;
	TestTrue(TEXT("A fully populated cube contract is ready."), Contract.IsValid());

	FWPCubeContract ReallocatedContract = Contract;
	++ReallocatedContract.ResourceGeneration;
	TestTrue(TEXT("A new allocation changes stable cube contract identity."),
		ReallocatedContract != Contract);
	TestEqual(TEXT("Reallocation preserves the declared layout version."),
		ReallocatedContract.CubeLayoutVersion, Contract.CubeLayoutVersion);

	FWPRenderPacket ContentOnlyUpdate;
	ContentOnlyUpdate.CubeContractA = Contract;
	ContentOnlyUpdate.CaptureGenerationA = 41;
	ContentOnlyUpdate.bCaptureReady = true;
	const bool bPreviouslyCaptureReady = true;
	const uint32 PreviouslyObservedCaptureGeneration = ContentOnlyUpdate.CaptureGenerationA;
	const FWPCubeContract BeforeCapture = ContentOnlyUpdate.CubeContractA;
	++ContentOnlyUpdate.CaptureGenerationA;
	TestTrue(TEXT("A content capture generation update does not mutate allocation identity."),
		ContentOnlyUpdate.CubeContractA == BeforeCapture);

	FWPPublishPolicyInput ContentOnlyPolicyInput;
	ContentOnlyPolicyInput.bHasPublished = true;
	ContentOnlyPolicyInput.bCaptureGenerationChanged =
		ContentOnlyUpdate.CaptureGenerationA != PreviouslyObservedCaptureGeneration;
	ContentOnlyPolicyInput.bCaptureReadinessChanged =
		ContentOnlyUpdate.bCaptureReady != bPreviouslyCaptureReady;
	ContentOnlyPolicyInput.SecondsSinceLastPublish = PacketHeartbeatSeconds * 0.5;
	const FWPPublishPolicyResult ContentOnlyPolicy =
		EvaluateWPPublishPolicy(ContentOnlyPolicyInput);
	TestFalse(TEXT("A capture generation-only change is not an immediate packet publication."),
		ContentOnlyPolicy.bImmediate);
	TestFalse(TEXT("A capture generation-only change waits for the diagnostic heartbeat."),
		ContentOnlyPolicy.bShouldPublish);
	TestTrue(TEXT("A suppressed capture generation advance is classified as coalesced diagnostic data."),
		ContentOnlyPolicy.bCaptureDiagnosticCoalesced);

	FWPPublishPolicyInput CaptureReadyPolicyInput = ContentOnlyPolicyInput;
	const bool bInitialCaptureReady = false;
	CaptureReadyPolicyInput.bCaptureReadinessChanged =
		ContentOnlyUpdate.bCaptureReady != bInitialCaptureReady;
	const FWPPublishPolicyResult CaptureReadyPolicy =
		EvaluateWPPublishPolicy(CaptureReadyPolicyInput);
	TestTrue(TEXT("The initial capture readiness transition publishes immediately."),
		CaptureReadyPolicy.bImmediate && CaptureReadyPolicy.bShouldPublish);

	const bool bEndpointAOnlyStartup = HasWPEndpointCaptureReadinessChanged(1, 0)
		&& !HasWPEndpointCaptureReadinessChanged(0, 0);
	FWPPublishPolicyInput EndpointAOnlyStartupPolicyInput = ContentOnlyPolicyInput;
	EndpointAOnlyStartupPolicyInput.bCaptureReadinessChanged = bEndpointAOnlyStartup;
	const FWPPublishPolicyResult EndpointAOnlyStartupPolicy =
		EvaluateWPPublishPolicy(EndpointAOnlyStartupPolicyInput);
	TestTrue(TEXT("Endpoint A becoming content-ready while B remains unready publishes immediately."),
		EndpointAOnlyStartupPolicy.bImmediate
			&& EndpointAOnlyStartupPolicy.bShouldPublish);

	const bool bEndpointBOnlyStartup = !HasWPEndpointCaptureReadinessChanged(9, 9)
		&& HasWPEndpointCaptureReadinessChanged(1, 0);
	FWPPublishPolicyInput EndpointBOnlyStartupPolicyInput = ContentOnlyPolicyInput;
	EndpointBOnlyStartupPolicyInput.bCaptureReadinessChanged = bEndpointBOnlyStartup;
	const FWPPublishPolicyResult EndpointBOnlyStartupPolicy =
		EvaluateWPPublishPolicy(EndpointBOnlyStartupPolicyInput);
	TestTrue(TEXT("Endpoint B becoming content-ready while A remains ready publishes immediately."),
		EndpointBOnlyStartupPolicy.bImmediate
			&& EndpointBOnlyStartupPolicy.bShouldPublish);

	TestFalse(TEXT("A non-zero capture content advance does not change endpoint readiness."),
		HasWPEndpointCaptureReadinessChanged(42, 41));
	TestTrue(TEXT("An endpoint resource reset back to generation zero changes readiness."),
		HasWPEndpointCaptureReadinessChanged(0, 41));

	FWPPublishPolicyInput HeartbeatPolicyInput = ContentOnlyPolicyInput;
	HeartbeatPolicyInput.SecondsSinceLastPublish = PacketHeartbeatSeconds;
	const FWPPublishPolicyResult HeartbeatPolicy =
		EvaluateWPPublishPolicy(HeartbeatPolicyInput);
	TestFalse(TEXT("A diagnostic heartbeat is not classified as an immediate state change."),
		HeartbeatPolicy.bImmediate);
	TestTrue(TEXT("A diagnostic heartbeat publishes coalesced capture metadata."),
		HeartbeatPolicy.bHeartbeat && HeartbeatPolicy.bShouldPublish);

	FWPPublishPolicyInput ReallocationPolicyInput = ContentOnlyPolicyInput;
	ReallocationPolicyInput.bCubeContractChanged = ReallocatedContract != Contract;
	const FWPPublishPolicyResult ReallocationPolicy =
		EvaluateWPPublishPolicy(ReallocationPolicyInput);
	TestTrue(TEXT("A cube resource generation change publishes immediately."),
		ReallocationPolicy.bImmediate && ReallocationPolicy.bShouldPublish);

	FWPCubeContract InvalidExtent = Contract;
	InvalidExtent.ExpectedExtent.Y = 256;
	TestFalse(TEXT("A non-square cube extent is rejected."), InvalidExtent.IsValid());

	FWPCubeContract InvalidMipCount = Contract;
	InvalidMipCount.ExpectedMipCount = 0;
	TestFalse(TEXT("A zero-mip cube contract is rejected."), InvalidMipCount.IsValid());

	FWPCubeContract InvalidFormat = Contract;
	InvalidFormat.ExpectedFormat = EWPCubeFormat::Unknown;
	TestFalse(TEXT("An unknown cube format is rejected."), InvalidFormat.IsValid());

	FWPCubeContract InvalidDimension = Contract;
	InvalidDimension.ExpectedDimension = EWPCubeDimension::Unknown;
	TestFalse(TEXT("A non-cube dimension is rejected."), InvalidDimension.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPPairOwnershipPolicyTest,
	"WormholePortal.Runtime.PairOwnershipPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPPairOwnershipPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("The Runtime switch enables production."),
		IsWPRuntimeEnabled(1));
	TestFalse(TEXT("The Runtime switch disables production."),
		IsWPRuntimeEnabled(0));
	FWPPairOwnershipSnapshot Snapshot;
	Snapshot.PairId = FGuid(1, 2, 3, 4);
	Snapshot.RequestedOwnership = EWPPairOwnershipMode::Production;
	Snapshot.EffectiveOwnership = EWPPairOwnershipMode::Warmup;
	Snapshot.OwnershipEpoch = 1;
	Snapshot.bEndpointAReady = true;
	Snapshot.bEndpointBReady = true;
	Snapshot.bOwnershipInputsReady = true;
	TestTrue(TEXT("A complete warmup packet activates the production path without Actor primitive identity."),
		Snapshot.ShouldRunProductionPath() && Snapshot.IsReadyForRendering(1));
	TestFalse(TEXT("Warmup is not a committed production frame."),
		Snapshot.IsProductionCommitted());
	Snapshot.EffectiveOwnership = EWPPairOwnershipMode::Production;
	TestTrue(TEXT("A committed Production packet remains renderer-ready."),
		Snapshot.IsProductionCommitted() && Snapshot.IsReadyForRendering(1));
	Snapshot.EffectiveOwnership = EWPPairOwnershipMode::Disabled;
	TestFalse(TEXT("Disabled never activates the production renderer."),
		Snapshot.ShouldRunProductionPath() || Snapshot.IsReadyForRendering(1));

	FWPOwnershipPolicyInput Input;
	FWPOwnershipPolicyResult Result = EvaluateWPOwnershipPolicy(Input);
	TestEqual(TEXT("Default policy remains Disabled."),
		static_cast<int32>(Result.Effective),
		static_cast<int32>(EWPPairOwnershipMode::Disabled));
	TestEqual(TEXT("Default policy performs no ownership work."), Result.Epoch, uint64(0));

	Input.DesiredRequested = EWPPairOwnershipMode::Production;
	Input.bRequestedChanged = true;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("A Production request always starts in warmup."), Result.bStartedWarmup);
	TestEqual(TEXT("The first warmup owns epoch one."), Result.Epoch, uint64(1));
	TestEqual(TEXT("Warmup becomes effective before production commit."),
		static_cast<int32>(Result.Effective),
		static_cast<int32>(EWPPairOwnershipMode::Warmup));

	Input.CurrentRequested = Result.Requested;
	Input.CurrentEffective = Result.Effective;
	Input.CurrentEpoch = Result.Epoch;
	Input.bRequestedChanged = false;
	Input.bInputsReady = true;
	Input.WarmupSucceededEpoch = 2;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestEqual(TEXT("A stale/future acknowledgement cannot commit Production."),
		static_cast<int32>(Result.Effective),
		static_cast<int32>(EWPPairOwnershipMode::Warmup));
	TestEqual(TEXT("Waiting for acknowledgement does not churn the epoch."), Result.Epoch, uint64(1));
	FWPOwnershipPolicyInput WarmupFailureInput = Input;
	WarmupFailureInput.WarmupSucceededEpoch = 0;
	WarmupFailureInput.ProductionFailedEpoch = 1;
	const FWPOwnershipPolicyResult WarmupFailureResult =
		EvaluateWPOwnershipPolicy(WarmupFailureInput);
	TestFalse(TEXT("A warmup pass failure stays on the same epoch instead of churning packets."),
		WarmupFailureResult.bStartedWarmup);
	TestEqual(TEXT("Warmup failure preserves its retry epoch."),
		WarmupFailureResult.Epoch, uint64(1));
	TestEqual(TEXT("Warmup remains effective while its retry is latched."),
		static_cast<int32>(WarmupFailureResult.Effective),
		static_cast<int32>(EWPPairOwnershipMode::Warmup));

	Input.WarmupSucceededEpoch = 1;
	Input.ProductionFailedEpoch = 1;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestFalse(TEXT("A racy same-epoch failure observation blocks stale warmup success."),
		Result.bCommittedProduction);
	TestEqual(TEXT("The failed warmup remains on its retry epoch."),
		Result.Epoch, uint64(1));
	Input.ProductionFailedEpoch = 0;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("An exact same-epoch warmup acknowledgement commits Production."),
		Result.bCommittedProduction);
	TestEqual(TEXT("Warmup commit intentionally retains its acknowledged epoch."),
		Result.Epoch, uint64(1));

	Input.CurrentEffective = Result.Effective;
	Input.ProductionFailedEpoch = 1;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("A same-epoch production failure restarts with a fresh warmup."),
		Result.bRestartedWarmup && Result.bStartedWarmup);
	TestEqual(TEXT("Production failure invalidates stale feedback with a new epoch."),
		Result.Epoch, uint64(2));

	Input.CurrentEffective = Result.Effective;
	Input.CurrentEpoch = Result.Epoch;
	Input.ProductionFailedEpoch = 0;
	Input.WarmupSucceededEpoch = 1;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestFalse(TEXT("The preceding epoch's success cannot recover a new warmup."),
		Result.bCommittedProduction);

	Input.WarmupSucceededEpoch = 2;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("The replacement epoch can commit after its own success."),
		Result.bCommittedProduction);

	Input.CurrentEffective = Result.Effective;
	Input.bResourceIdentityChanged = true;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("Resource identity replacement creates a new warmup generation."),
		Result.bStartedWarmup && Result.bRestartedWarmup);
	TestEqual(TEXT("Resource replacement advances the epoch."), Result.Epoch, uint64(3));

	Input.CurrentEffective = EWPPairOwnershipMode::Production;
	Input.CurrentEpoch = Result.Epoch;
	Input.bResourceIdentityChanged = false;
	Input.bPreviousInputsReady = true;
	Input.bInputsReady = false;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestTrue(TEXT("Either endpoint losing readiness immediately restarts warmup."),
		Result.bStartedWarmup && Result.bRestartedWarmup);
	TestEqual(TEXT("Readiness loss invalidates the production epoch."), Result.Epoch, uint64(4));

	Input.CurrentRequested = EWPPairOwnershipMode::Production;
	Input.CurrentEffective = Result.Effective;
	Input.CurrentEpoch = Result.Epoch;
	Input.DesiredRequested = EWPPairOwnershipMode::Disabled;
	Input.bRequestedChanged = true;
	Input.bPreviousInputsReady = false;
	Result = EvaluateWPOwnershipPolicy(Input);
	TestEqual(TEXT("Disabling the Runtime pipeline disables the production path."),
		static_cast<int32>(Result.Effective),
		static_cast<int32>(EWPPairOwnershipMode::Disabled));
	TestTrue(TEXT("Leaving an active warmup invalidates its epoch."), Result.Epoch > uint64(4));

	TestEqual(TEXT("Epoch zero advances to one."), AdvanceWPOwnershipEpoch(0), uint64(1));
	TestEqual(TEXT("Epoch overflow saturates instead of returning reserved zero."),
		AdvanceWPOwnershipEpoch(MAX_uint64), MAX_uint64);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPFixedCaptureCadencePolicyTest,
	"WormholePortal.Runtime.FixedCaptureCadencePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPFixedCaptureCadencePolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FWPCaptureSchedulerModePolicyResult RuntimeMode =
		EvaluateWPCaptureSchedulerModePolicy(1, true);
	TestTrue(TEXT("Mode one selects legacy Runtime atomic capture."),
		RuntimeMode.bModeValid && RuntimeMode.bRuntimeActive
			&& !RuntimeMode.bDeprecatedAlias
			&& !RuntimeMode.bStaggeredEndpointSubmission);
	const FWPCaptureSchedulerModePolicyResult StaggeredMode =
		EvaluateWPCaptureSchedulerModePolicy(2, true);
	TestTrue(TEXT("Mode two selects steady-state endpoint staggering."),
		StaggeredMode.bModeValid && StaggeredMode.bRuntimeActive
			&& StaggeredMode.bStaggeredEndpointSubmission);
	const FWPCaptureSchedulerModePolicyResult DeprecatedAliasMode =
		EvaluateWPCaptureSchedulerModePolicy(0, true);
	TestTrue(TEXT("Mode zero is a deprecated alias of the Runtime production capture path."),
		DeprecatedAliasMode.bModeValid && DeprecatedAliasMode.bRuntimeActive
			&& DeprecatedAliasMode.bDeprecatedAlias);
	const FWPCaptureSchedulerModePolicyResult InvalidMode =
		EvaluateWPCaptureSchedulerModePolicy(17, true);
	TestTrue(TEXT("An invalid scheduler mode fails closed without reviving Actor Tick."),
		!InvalidMode.bModeValid && InvalidMode.bRuntimeActive);
	const FWPCaptureSchedulerModePolicyResult RenderPipelineDisabledMode =
		EvaluateWPCaptureSchedulerModePolicy(1, false);
	TestTrue(TEXT("Disabling the production render pipeline does not stop Runtime-owned capture."),
		RenderPipelineDisabledMode.bModeValid && RenderPipelineDisabledMode.bRuntimeActive);

	FWPFixedCaptureCadenceResult Result =
		EvaluateWPFixedCaptureCadence(
			0.029, WPLegacyAtomicCaptureCadenceSeconds, false);
	TestFalse(TEXT("A pair remains pending before 30 ms."), Result.bWouldSubmit);
	Result = EvaluateWPFixedCaptureCadence(
		WPLegacyAtomicCaptureCadenceSeconds,
		WPLegacyAtomicCaptureCadenceSeconds, false);
	TestTrue(TEXT("Every pair becomes due at exactly 30 ms."),
		Result.bWouldSubmit && Result.bIntervalDue);
	const double EndpointCadenceSeconds = 1.0 / WPMinimumTargetEndpointHz;
	const double StaggeredSubmissionCadenceSeconds = 0.5 / WPMinimumTargetEndpointHz;
	Result = EvaluateWPFixedCaptureCadence(
		StaggeredSubmissionCadenceSeconds - 0.001,
		StaggeredSubmissionCadenceSeconds, false);
	TestFalse(TEXT("A staggered endpoint remains pending before half of the endpoint period."),
		Result.bWouldSubmit);
	Result = EvaluateWPFixedCaptureCadence(
		StaggeredSubmissionCadenceSeconds,
		StaggeredSubmissionCadenceSeconds, false);
	TestTrue(TEXT("One staggered endpoint becomes due at 60 submissions per second."),
		Result.bWouldSubmit && Result.bIntervalDue);
	TestFalse(TEXT("One staggered deadline does not require an atomic debt catch-up."),
		ShouldUseWPCaptureCadenceDebtAtomicFallback(
			true, false,
			StaggeredSubmissionCadenceSeconds, EndpointCadenceSeconds));
	TestTrue(TEXT("Two staggered deadlines require one atomic A+B debt catch-up."),
		ShouldUseWPCaptureCadenceDebtAtomicFallback(
			true, false,
			EndpointCadenceSeconds, EndpointCadenceSeconds));

	double JitterElapsedSeconds = 0.0;
	double JitterTotalSeconds = 0.0;
	bool bJitterNextEndpointA = true;
	int32 JitterEndpointASubmissions = 0;
	int32 JitterEndpointBSubmissions = 0;
	int32 JitterAtomicCatchUps = 0;
	for (int32 FrameIndex = 0; FrameIndex < 6000; ++FrameIndex)
	{
		const double JitterDeltaSeconds =
			(FrameIndex % 2) == 0 ? 0.0165 : 0.0168;
		JitterElapsedSeconds += JitterDeltaSeconds;
		JitterTotalSeconds += JitterDeltaSeconds;
		const bool bCadenceDebtAtomicFallback =
			ShouldUseWPCaptureCadenceDebtAtomicFallback(
				true, false,
				JitterElapsedSeconds, EndpointCadenceSeconds);
		const double JitterActiveCadenceSeconds =
			bCadenceDebtAtomicFallback
				? EndpointCadenceSeconds
				: StaggeredSubmissionCadenceSeconds;
		const FWPFixedCaptureCadenceResult JitterDecision =
			EvaluateWPFixedCaptureCadence(
				JitterElapsedSeconds, JitterActiveCadenceSeconds, false);
		if (!JitterDecision.bWouldSubmit)
		{
			continue;
		}
		if (bCadenceDebtAtomicFallback)
		{
			++JitterEndpointASubmissions;
			++JitterEndpointBSubmissions;
			++JitterAtomicCatchUps;
		}
		else if (bJitterNextEndpointA)
		{
			++JitterEndpointASubmissions;
			bJitterNextEndpointA = false;
		}
		else
		{
			++JitterEndpointBSubmissions;
			bJitterNextEndpointA = true;
		}
		JitterElapsedSeconds = FMath::Fmod(
			JitterElapsedSeconds, JitterActiveCadenceSeconds);
	}
	const int32 ExpectedJitterEndpointSubmissions = FMath::RoundToInt(
		JitterTotalSeconds * WPMinimumTargetEndpointHz);
	TestEqual(TEXT("Boundary jitter preserves endpoint A 30 Hz debt."),
		JitterEndpointASubmissions, ExpectedJitterEndpointSubmissions);
	TestEqual(TEXT("Boundary jitter preserves endpoint B 30 Hz debt."),
		JitterEndpointBSubmissions, ExpectedJitterEndpointSubmissions);
	TestTrue(TEXT("Boundary jitter exercises atomic debt catch-up."),
		JitterAtomicCatchUps > 0);
	Result = EvaluateWPFixedCaptureCadence(
		1.0, EndpointCadenceSeconds, true);
	TestFalse(TEXT("Commit fresh-camera wait blocks the normal cadence submission."),
		Result.bWouldSubmit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPAtomicPairSubmissionPolicyTest,
	"WormholePortal.Runtime.AtomicPairSubmissionPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPAtomicPairSubmissionPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWPAtomicPairSubmissionResult Actual = EvaluateWPAtomicPairSubmission(
		10, 11, 10, 11, 4, 5, 4, 5);
	TestTrue(TEXT("An exact +1/+1 generation and coherent epoch step is atomic."),
		Actual.bAtomicPairSubmission);

	Actual = EvaluateWPAtomicPairSubmission(10, 11, 10, 10, 4, 4, 4, 4);
	TestTrue(TEXT("A one-endpoint generation advance is rejected as non-atomic."),
		!Actual.bAtomicPairSubmission);

	Actual = EvaluateWPAtomicPairSubmission(
		MAX_uint32, 0, MAX_uint32, 0, MAX_uint64, 1, MAX_uint64, 1);
	TestTrue(TEXT("Generation wrap and reserved-zero epoch skip remain one exact submission."),
		Actual.bAtomicPairSubmission);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPRegistryPairAuthorityPolicyTest,
	"WormholePortal.Runtime.RegistryPairAuthorityPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPRegistryPairAuthorityPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGuid OriginalRegistryPairId(1, 2, 3, 4);
	const FGuid RelinkedRegistryPairId(5, 6, 7, 8);

	TestEqual(
		TEXT("A Registry snapshot creates state when no state exists."),
		static_cast<uint8>(EvaluateWPRegistryPairReconcileAction(
			false, FGuid(), OriginalRegistryPairId)),
		static_cast<uint8>(EWPRegistryPairReconcileAction::Add));
	TestEqual(
		TEXT("An idempotent bootstrap/event overlap preserves the same Registry PairId state."),
		static_cast<uint8>(EvaluateWPRegistryPairReconcileAction(
			true, OriginalRegistryPairId, OriginalRegistryPairId)),
		static_cast<uint8>(EWPRegistryPairReconcileAction::Keep));
	TestEqual(
		TEXT("Unlink/relink of the same endpoints replaces every old-lifetime state."),
		static_cast<uint8>(EvaluateWPRegistryPairReconcileAction(
			true, OriginalRegistryPairId, RelinkedRegistryPairId)),
		static_cast<uint8>(EWPRegistryPairReconcileAction::Replace));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPCaptureVisibilitySafetyPolicyTest,
	"WormholePortal.Runtime.CaptureVisibilitySafetyPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPCaptureVisibilitySafetyPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double TestStartSeconds = FPlatformTime::Seconds();

	TestEqual(TEXT("Strict V0 frustum policy keeps the default hidden refresh at zero."),
		ResolveWPEffectiveHiddenRefreshHz(0.0), 0.0);
	TestEqual(TEXT("A stale process-global 24 Hz override cannot bypass strict V0 capture stop."),
		ResolveWPEffectiveHiddenRefreshHz(24.0), 0.0);
	TestTrue(TEXT("V0 paused state blocks both cubemap submissions."),
		ShouldBlockWPFrustumCapture(true));
	TestFalse(TEXT("V1/V2 active states keep A+B cubemap submissions eligible."),
		ShouldBlockWPFrustumCapture(false));
	TestTrue(TEXT("Invalid CPU occlusion fails open to a visible frustum endpoint."),
		HasWPEffectiveVisibleEndpoint(1, false, 0, false));
	TestFalse(TEXT("A valid all-blocked CPU occlusion result suppresses visible frustum endpoints."),
		HasWPEffectiveVisibleEndpoint(2, true, 0, false));
	TestTrue(TEXT("One open SafeProxy endpoint keeps the pair active."),
		HasWPEffectiveVisibleEndpoint(2, true, WPCaptureEndpointAMask, false));
	TestTrue(TEXT("The camera-inside selected pair bypasses frustum and occlusion pause."),
		HasWPEffectiveVisibleEndpoint(0, true, 0, true));
	TestFalse(TEXT("Frustum V0 remains invisible when no inside override exists."),
		HasWPEffectiveVisibleEndpoint(0, false, WPCaptureBothEndpointsMask, false));

	TestEqual(TEXT("The pure freshness helper preserves its disabled-policy baseline."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			false, false, 0, 0, 0, 0, 0, 0, -1.0, -1.0)),
		static_cast<uint8>(EWPCaptureVisibilityFreshness::FeatureDisabled));
	TestEqual(TEXT("An incoherent seqlock snapshot fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, false, 7, 7, 100, 100, 100, 1, 0.01, 0.25)),
		static_cast<uint8>(EWPCaptureVisibilityFreshness::IncoherentSnapshot));
	TestEqual(TEXT("A visibility sample from another ownership epoch fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 6, 7, 100, 100, 100, 1, 0.01, 0.25)),
		static_cast<uint8>(EWPCaptureVisibilityFreshness::EpochMismatch));
	TestEqual(TEXT("No published visibility-contract floor fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 7, 7, 100, 0, 100, 1, 0.01, 0.25)),
		static_cast<uint8>(
			EWPCaptureVisibilityFreshness::MissingRequiredPacketFloor));
	TestEqual(TEXT("Feedback older than the latest geometry contract fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 7, 7, 99, 100, 100, 1, 0.01, 0.25)),
		static_cast<uint8>(
			EWPCaptureVisibilityFreshness::PacketBeforeRequiredFloor));
	TestEqual(TEXT("Feedback newer than GT successful-publish state fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 7, 7, 101, 100, 100, 1, 0.01, 0.25)),
		static_cast<uint8>(
			EWPCaptureVisibilityFreshness::PacketAfterPublishedState));
	TestEqual(TEXT("A stale sample receipt fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 7, 7, 100, 100, 100, 1, 0.251, 0.25)),
		static_cast<uint8>(
			EWPCaptureVisibilityFreshness::InvalidOrStaleReceipt));
	TestEqual(TEXT("A coherent current-epoch sample inside the packet floor and age is fresh."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityFreshness(
			true, true, 7, 7, 100, 100, 100, 1, 0.25, 0.25)),
		static_cast<uint8>(EWPCaptureVisibilityFreshness::Fresh));

	TestTrue(TEXT("A sample gap at the max-age boundary remains continuous."),
		IsWPCaptureVisibilitySampleGapContinuous(7, 3, 7, 0.25, 0.25));
	TestFalse(TEXT("A hitch-sized sample gap resets the invisible hold chain."),
		IsWPCaptureVisibilitySampleGapContinuous(7, 3, 7, 0.251, 0.25));
	TestFalse(TEXT("An ownership epoch change resets the invisible hold chain."),
		IsWPCaptureVisibilitySampleGapContinuous(7, 3, 8, 0.01, 0.25));
	TestFalse(TEXT("A negative sample gap resets the invisible hold chain."),
		IsWPCaptureVisibilitySampleGapContinuous(
			7, 3, 7, -0.001, 0.25));
	TestTrue(TEXT("No reject barrier accepts a fresh visibility sample."),
		IsWPCaptureVisibilitySampleBeyondRejectBarrier(7, 10, 0, 0));
	TestFalse(TEXT("The rejected sample tuple cannot seed a new camera guard."),
		IsWPCaptureVisibilitySampleBeyondRejectBarrier(7, 10, 7, 10));
	TestTrue(TEXT("A new sample in the same epoch crosses the reject barrier."),
		IsWPCaptureVisibilitySampleBeyondRejectBarrier(7, 11, 7, 10));
	TestTrue(TEXT("A new ownership epoch is outside the old sample domain."),
		IsWPCaptureVisibilitySampleBeyondRejectBarrier(8, 1, 7, 10));

	const FVector CameraLocation(100.0, 200.0, 300.0);
	const FRotator CameraRotation(10.0, 20.0, 0.0);
	TestEqual(TEXT("Camera guard is a zero-work decision before an invisible hold exists."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			false, false, 0, FVector::ZeroVector, FRotator::ZeroRotator, 0.0f,
			false, 0, FVector::ZeroVector, FRotator::ZeroRotator, 0.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::NotRequired));
	TestEqual(TEXT("A stable actor/location/rotation/FOV guard permits the hold."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 42, CameraLocation, CameraRotation, 90.0f,
			true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::Stable));
	TestEqual(TEXT("An unavailable primary reference view immediately fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, false, 42, CameraLocation, CameraRotation, 90.0f,
			true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::ViewUnavailable));
	TestEqual(TEXT("A primary view actor change immediately fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 43, CameraLocation, CameraRotation, 90.0f,
			true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::ViewActorChanged));
	TestEqual(TEXT("Location movement over 0.1 cm immediately fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 42, CameraLocation + FVector(0.101, 0.0, 0.0),
			CameraRotation, 90.0f, true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::LocationChanged));
	TestEqual(TEXT("Rotation movement over 0.01 degrees immediately fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 42, CameraLocation, CameraRotation + FRotator(0.011, 0.0, 0.0),
			90.0f, true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::RotationChanged));
	TestEqual(TEXT("FOV movement over 0.01 degrees immediately fails open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 42, CameraLocation, CameraRotation, 90.011f,
			true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::FOVChanged));
	TestEqual(TEXT("An invalid FOV affects only visibility pause and fails it open."),
		static_cast<uint8>(EvaluateWPCaptureVisibilityCameraGuard(
			true, true, 42, CameraLocation, CameraRotation, 0.0f,
			true, 42, CameraLocation, CameraRotation, 90.0f)),
		static_cast<uint8>(EWPCaptureVisibilityCameraGuard::InvalidFOV));

	TestTrue(TEXT("Initial successful publication establishes the visibility floor."),
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			true, true, false, false, false, false));
	TestTrue(TEXT("A transform publication advances the visibility floor."),
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			true, false, false, false, true, false));
	TestTrue(TEXT("An ownership or visibility-feedback-toggle publication advances the floor."),
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			true, false, false, false, false, true));
	TestFalse(TEXT("A heartbeat/camera diagnostic/capture-generation-only publication does not advance the floor."),
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			true, false, false, false, false, false));
	TestFalse(TEXT("Feature-off publication never establishes a visibility floor."),
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			false, true, true, true, true, true));
	AddInfo(FString::Printf(
		TEXT("Capture visibility safety policy completed. StrictFrustumHiddenStop=1 CpuMs=%.4f"),
		(FPlatformTime::Seconds() - TestStartSeconds) * 1000.0));
	return true;
}
#endif

void UWPRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// 로그 전용: subsystem 초기화 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	Super::Initialize(Collection);
	bDeinitializing = false;
	Telemetry.Reset();
	const UWorld* World = GetWorld();
	bRenderRuntimeEnabled = IsValid(World)
		&& World->IsGameWorld()
		&& World->GetNetMode() != NM_DedicatedServer
		&& FApp::CanEverRender();
	if (!bRenderRuntimeEnabled)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[RuntimeGate] Render coordinator disabled. World=%s WorldType=%s IsGameWorld=%d NetMode=%d CanEverRender=%d CaptureManagerCreated=0 LUTEndpointManagerCreated=0 RegistryRenderDelegates=0 TransitRenderDelegates=0 RendererHandles=0 DedicatedServerRenderAllocations=0 CpuMs=%.4f"),
			*GetNameSafe(World), World ? GetWorldTypeName(World->WorldType) : TEXT("NoWorld"),
			World && World->IsGameWorld() ? 1 : 0,
			World ? static_cast<int32>(World->GetNetMode()) : -1,
			FApp::CanEverRender() ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return;
	}
	CaptureManager = NewObject<UWPCaptureManager>(
		this, TEXT("WPCaptureManager"), RF_Transient);
	if (CaptureManager)
	{
		CaptureManager->Initialize(GetWorld());
	}
	CaptureScheduler = MakeUnique<FWPRuntimeCaptureScheduler>(
		*this, CaptureManager, PairStates, bRenderPacketPipelineActive, bPairsDirty);
	LUTEndpointManager = NewObject<UWPLUTEndpointManager>(
		this, TEXT("WPLUTEndpointManager"), RF_Transient);
	if (LUTEndpointManager)
	{
		LUTEndpointManager->Initialize(GetWorld());
		LUTEndpointChangedHandle = LUTEndpointManager->OnEndpointChanged().AddUObject(
			this, &UWPRuntimeSubsystem::HandleLUTEndpointChanged);
	}
	RenderPublication = MakeUnique<FWPRuntimeRenderPublication>(
		*this, CaptureManager, LUTEndpointManager, Telemetry);

	RegistrySubsystem = Collection.InitializeDependency<UWPRegistrySubsystem>();
	TransitSubsystem = Collection.InitializeDependency<UWPTransitSubsystem>();

	bool bRegistryPairBootstrapCompleted = false;
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		// Pair lifecycle delegates must be live before the read-only bootstrap. This closes the
		// late-subscriber gap and makes Registry the only topology/PairId authority.
		PortalPairAddedHandle = Registry->OnPortalPairAdded().AddUObject(
			this, &UWPRuntimeSubsystem::HandlePortalPairAdded);
		PortalPairRemovedHandle = Registry->OnPortalPairRemoved().AddUObject(
			this, &UWPRuntimeSubsystem::HandlePortalPairRemoved);
		RebuildPairs();
		bRegistryPairBootstrapCompleted = true;

		PortalRegisteredHandle = Registry->OnPortalRegistered().AddUObject(
			this, &UWPRuntimeSubsystem::HandlePortalRegistered);
		PortalUnregisteredHandle = Registry->OnPortalUnregistered().AddUObject(
			this, &UWPRuntimeSubsystem::HandlePortalUnregistered);
		PortalChangedHandle = Registry->OnPortalChanged().AddUObject(
			this, &UWPRuntimeSubsystem::HandlePortalChanged);

		// A late-created runtime subsystem must own resources for endpoints that were
		// registered before its delegates were bound. Registry remains the membership source.
		TArray<AWormholePortalActor*> BootstrapPortals;
		Registry->GetRegisteredPortals(BootstrapPortals);
		for (AWormholePortalActor* Portal : BootstrapPortals)
		{
			if (!IsValid(Portal))
			{
				continue;
			}
			if (CaptureManager)
			{
				CaptureManager->EnsureEndpointResources(
					Portal,
					ResolveWPDynamicCaptureResolutionPolicy().LowestVisibleResolution);
			}
			if (LUTEndpointManager)
			{
				FWPLUTEndpointRequestOptions Options;
				Options.PreferredAsset = Portal->LUTAssetOverride;
				Options.DebugContext = Portal->GetPathName();
				LUTEndpointManager->RegisterEndpoint(Portal, Options);
			}
		}
	}

	if (UWPTransitSubsystem* Transit = TransitSubsystem.Get())
	{
		TransitStartedHandle = Transit->OnTransitStarted().AddUObject(
			this, &UWPRuntimeSubsystem::HandleTransitStarted);
		TransitCommittedHandle = Transit->OnTransitCommitted().AddUObject(
			this, &UWPRuntimeSubsystem::HandleTransitCommitted);
		TransitCancelledHandle = Transit->OnTransitCancelled().AddUObject(
			this, &UWPRuntimeSubsystem::HandleTransitCancelled);
	}
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UWPRuntimeSubsystem::HandleWorldPostActorTick);

	bPairsDirty = !bRegistryPairBootstrapCompleted;
	const int32 RuntimeEnabledRaw = CVarWPRuntimeEnabled.GetValueOnGameThread();
	bRenderPacketPipelineActive = IsWPRuntimeEnabled(RuntimeEnabledRaw);
	LastRuntimeEnabledRaw = RuntimeEnabledRaw;
	const int32 CaptureModeRaw = CVarWPCaptureSchedulerMode.GetValueOnGameThread();
	const FWPCaptureSchedulerModePolicyResult CaptureModePolicy =
		EvaluateWPCaptureSchedulerModePolicy(
			CaptureModeRaw,
			bRenderPacketPipelineActive);
	CaptureScheduler->Initialize(CaptureModePolicy.bRuntimeActive);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Runtime] Initialized. World=%s WorldType=%s RegistryValid=%d TransitValid=%d RendererAvailable=%d CaptureManagerValid=%d ManagedCaptureEndpoints=%d RegistryPairDelegatesBoundBeforeBootstrap=%d RegistryPairBootstrapCompleted=%d BootstrapPairCount=%d RegistryPairIdAuthority=1 SelfGeneratedPairIds=0 PortalLinkPairRebuild=0 RuntimeEnabledRaw=%d EffectiveEnablePolicy=RuntimeEnabled RenderPacketPipelineEnabled=%d CaptureSchedulerModeRaw=%d CaptureSchedulerRuntimeActive=%d CaptureSchedulerAuthority=%s CaptureSchedulerExecutionChanged=%d StaggeredSteadyState=%d TargetEndpointHz=%.3f EndpointCadenceMs=%.3f StaggeredSubmissionCadenceMs=%.3f LegacyAtomicCadenceMs=%.3f TransitImmediate=0 TransitForcesAtomic=0 CommitFreshCameraWait=1 CaptureSchedulerPostActorDelegateValid=%d ActorCaptureExecution=0 ActorTickDependency=0 ActorFallbackAvailable=0 OwnershipPolicy=AllRegisteredPairsProduction PairIndependentWarmup=1 RasterProxyDependency=0 RasterFallbackAvailable=0 CpuMs=%.3f"),
		*GetNameSafe(GetWorld()),
		GetWorld() ? GetWorldTypeName(GetWorld()->WorldType) : TEXT("NoWorld"),
		RegistrySubsystem.IsValid() ? 1 : 0,
		TransitSubsystem.IsValid() ? 1 : 0,
		IWPRenderer::Find() ? 1 : 0,
		CaptureManager ? 1 : 0,
		CaptureManager ? CaptureManager->GetEndpointCount() : 0,
		PortalPairAddedHandle.IsValid() && PortalPairRemovedHandle.IsValid() ? 1 : 0,
		bRegistryPairBootstrapCompleted ? 1 : 0,
		PairStates.Num(),
		RuntimeEnabledRaw,
		bRenderPacketPipelineActive ? 1 : 0,
		CaptureModeRaw, CaptureScheduler->IsRuntimeActive() ? 1 : 0,
		TEXT("RuntimeManager"),
		1,
		CaptureModePolicy.bStaggeredEndpointSubmission ? 1 : 0,
		GetWPCaptureTargetEndpointHz(),
		GetWPEndpointCaptureCadenceSeconds() * 1000.0,
		GetWPStaggeredSubmissionCadenceSeconds() * 1000.0,
		WPLegacyAtomicCaptureCadenceSeconds * 1000.0,
		WorldPostActorTickHandle.IsValid() ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	const float RequestedHiddenRefreshHz = FMath::Max(
		CVarWPCaptureVisibilityHiddenRefreshHz.GetValueOnGameThread(),
		0.0f);
	if (RequestedHiddenRefreshHz > 0.0f)
	{
		WP_LOG(this, Warning,
			TEXT("[CaptureScheduler][StrictFrustumHiddenStop] Legacy hidden refresh override ignored. World=%s HiddenRefreshHzRequested=%.3f HiddenRefreshHzEffective=0.000 FrustumState=V0Policy CaptureA=0 CaptureB=0 ResourcesRetained=1 EngineModified=0 CpuMs=%.4f"),
			*GetNameSafe(GetWorld()),
			RequestedHiddenRefreshHz,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
}

void UWPRuntimeSubsystem::Deinitialize()
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: 종료 전 상태와 전체 cleanup CPU 시간을 최종 로그에 보존합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 PairCountBeforeCleanup = PairStates.Num();
#endif
	bDeinitializing = true;

	if (WorldPostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
		WorldPostActorTickHandle.Reset();
	}

	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		Registry->OnPortalPairAdded().Remove(PortalPairAddedHandle);
		Registry->OnPortalPairRemoved().Remove(PortalPairRemovedHandle);
		PortalPairAddedHandle.Reset();
		PortalPairRemovedHandle.Reset();
		Registry->OnPortalRegistered().Remove(PortalRegisteredHandle);
		Registry->OnPortalUnregistered().Remove(PortalUnregisteredHandle);
		Registry->OnPortalChanged().Remove(PortalChangedHandle);
	}

	if (UWPTransitSubsystem* Transit = TransitSubsystem.Get())
	{
		Transit->OnTransitStarted().Remove(TransitStartedHandle);
		Transit->OnTransitCommitted().Remove(TransitCommittedHandle);
		Transit->OnTransitCancelled().Remove(TransitCancelledHandle);
	}

	for (TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
	{
		RemovePair(Pair.Key, Pair.Value, TEXT("SubsystemDeinitialize"));
	}
	PairStates.Reset();
	if (CaptureScheduler)
	{
		CaptureScheduler->Reset();
	}
#if !UE_BUILD_SHIPPING
	const bool bWasRenderRuntimeEnabled = bRenderRuntimeEnabled;
	const bool bHadCaptureManager = CaptureManager != nullptr;
	const bool bHadLUTEndpointManager = LUTEndpointManager != nullptr;
	const int32 ManagedEndpointsBeforeShutdown = CaptureManager
		? CaptureManager->GetEndpointCount() : 0;
	const int32 LUTEndpointsBeforeShutdown = LUTEndpointManager
		? LUTEndpointManager->GetEndpointCount() : 0;
#endif
	if (LUTEndpointManager)
	{
		LUTEndpointManager->OnEndpointChanged().Remove(LUTEndpointChangedHandle);
		LUTEndpointChangedHandle.Reset();
		LUTEndpointManager->Shutdown(TEXT("RuntimeSubsystemDeinitialize"));
		LUTEndpointManager = nullptr;
	}
	if (CaptureManager)
	{
		CaptureManager->Shutdown(TEXT("RuntimeSubsystemDeinitialize"));
		CaptureManager = nullptr;
	}
	RenderPublication.Reset();
	RegistrySubsystem.Reset();
	TransitSubsystem.Reset();
	bRenderRuntimeEnabled = false;
	bRenderPacketPipelineActive = false;
	LastRuntimeEnabledRaw = MIN_int32;

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Runtime] Deinitialized. World=%s RenderRuntimeWasEnabled=%d RemovedPairs=%d LifetimePublishedPackets=%llu RegistryPairDelegatesRemoved=%d RemainingRegistryPairStates=0 RegistryPairIdAuthority=1 CaptureSchedulerPostActorDelegateRemoved=%d CaptureSchedulerStateEntriesAfterCleanup=0 ManagedCaptureEndpointsReleased=%d CaptureManagerDestroyed=%d LUTEndpointsReleased=%d LUTEndpointManagerDestroyed=%d ActorRenderResourceOwner=0 CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), bWasRenderRuntimeEnabled ? 1 : 0,
		PairCountBeforeCleanup, Telemetry.PublishedPacketCount,
		!PortalPairAddedHandle.IsValid() && !PortalPairRemovedHandle.IsValid() ? 1 : 0,
		WorldPostActorTickHandle.IsValid() ? 0 : 1,
		ManagedEndpointsBeforeShutdown, bHadCaptureManager ? 1 : 0,
		LUTEndpointsBeforeShutdown, bHadLUTEndpointManager ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	CaptureScheduler.Reset();

	Super::Deinitialize();
}

bool UWPRuntimeSubsystem::GetPairOwnershipSnapshot(
	const AWormholePortalActor* Portal,
	FWPPairOwnershipSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = FWPPairOwnershipSnapshot();
	if (!IsValid(Portal))
	{
		return false;
	}

	for (const TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
	{
		const FWPPortalPairState& PairState = Pair.Value;
		if (PairState.Identity.PortalA.Get() != Portal && PairState.Identity.PortalB.Get() != Portal)
		{
			continue;
		}

		OutSnapshot.PairId = PairState.Identity.PairId;
		OutSnapshot.StableSelectorNameA = PairState.Identity.StableSelectorNameA;
		OutSnapshot.StableSelectorNameB = PairState.Identity.StableSelectorNameB;
		OutSnapshot.RequestedOwnership = PairState.Ownership.RequestedOwnership;
		OutSnapshot.EffectiveOwnership = PairState.Ownership.EffectiveOwnership;
		OutSnapshot.OwnershipEpoch = PairState.Ownership.OwnershipEpoch;
		OutSnapshot.bEndpointAReady = PairState.Ownership.bOwnershipEndpointAReady;
		OutSnapshot.bEndpointBReady = PairState.Ownership.bOwnershipEndpointBReady;
		OutSnapshot.bOwnershipInputsReady = PairState.Ownership.bOwnershipInputsReady;
		return true;
	}
	return false;
}

bool UWPRuntimeSubsystem::GetLUTEndpointSnapshot(
	const AWormholePortalActor* Portal,
	FWPLUTEndpointSnapshot& OutSnapshot) const
{
	return LUTEndpointManager
		&& LUTEndpointManager->GetEndpointSnapshot(Portal, OutSnapshot);
}

bool UWPRuntimeSubsystem::GetCaptureEndpointSnapshot(
	const AWormholePortalActor* Portal,
	FWPCaptureEndpointSnapshot& OutSnapshot) const
{
	return CaptureManager
		&& CaptureManager->GetEndpointSnapshot(Portal, OutSnapshot);
}

void UWPRuntimeSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_RuntimeTick);
	SET_DWORD_STAT(STAT_WP_ActivePairs, PairStates.Num());

	const int32 RuntimeEnabledRaw = CVarWPRuntimeEnabled.GetValueOnGameThread();
	const bool bRenderPacketPipelineEnabled = IsWPRuntimeEnabled(RuntimeEnabledRaw);
#if !UE_BUILD_SHIPPING
	// 로그 전용: effective policy가 같아도 raw CVar 변경 사실을 한 번 기록합니다.
	const bool bPipelineControlChanged = RuntimeEnabledRaw != LastRuntimeEnabledRaw;
#endif
	const bool bPipelineEffectiveChanged =
		bRenderPacketPipelineEnabled != bRenderPacketPipelineActive;
	if (bPipelineEffectiveChanged
#if !UE_BUILD_SHIPPING
		|| bPipelineControlChanged
#endif
	)
	{
#if !UE_BUILD_SHIPPING
		// 로그 전용: pipeline toggle 처리 CPU 시간을 측정합니다.
		const double ToggleStartSeconds = FPlatformTime::Seconds();
		int32 ReleasedHandleCount = 0;
#endif
		if (bPipelineEffectiveChanged)
		{
			bRenderPacketPipelineActive = bRenderPacketPipelineEnabled;
			if (!bRenderPacketPipelineEnabled)
			{
				IWPRenderer* ToggleRenderer = IWPRenderer::Find();
				for (TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
				{
					if (Pair.Value.Ownership.RenderHandle.IsValid())
					{
#if !UE_BUILD_SHIPPING
						++ReleasedHandleCount;
#endif
						if (ToggleRenderer
							&& Pair.Value.Ownership.RenderHandle.ServiceId == ToggleRenderer->GetServiceId())
						{
							ToggleRenderer->UnregisterPair(Pair.Value.Ownership.RenderHandle);
						}
						Pair.Value.Ownership.RenderHandle.Reset();
					}
					// Unregistering the Renderer invalidates both the mailbox
					// sequence domain and the visibility geometry packet floor.
					// Preserve paused itself so PostActorTick emits the normal
					// Paused->Active transition and same-callback atomic resume.
					Pair.Value.Ownership.LastOwnershipFeedback =
						FWPPairOwnershipFeedback();
					Pair.Value.Capture.Visibility.RequiredPacketSequence = 0;
					Pair.Value.Capture.Visibility.LastOwnershipEpoch = 0;
					Pair.Value.Capture.Visibility.LastSampleSequence = 0;
					Pair.Value.Capture.Visibility.LastSampleReceiptSeconds = -1.0e30;
					Pair.Value.Capture.Visibility.LastVisibleEndpointMask = 0x3;
					Pair.Value.Capture.Visibility.RejectedOwnershipEpoch = 0;
					Pair.Value.Capture.Visibility.RejectedThroughSampleSequence = 0;
					Pair.Value.Capture.Visibility.InvisibleElapsedSeconds = 0.0;
					Pair.Value.Capture.Visibility.LastInvisibleSampleReceiptSeconds =
						-1.0e30;
					Pair.Value.Capture.Visibility.LastInvisibleOwnershipEpoch = 0;
					Pair.Value.Capture.Visibility.LastInvisibleSampleSequence = 0;
					Pair.Value.Capture.Visibility.GuardViewActorId = 0;
					Pair.Value.Capture.Visibility.GuardCameraLocation =
						FVector::ZeroVector;
					Pair.Value.Capture.Visibility.GuardCameraRotation =
						FRotator::ZeroRotator;
					Pair.Value.Capture.Visibility.GuardCameraFOVDegrees = 0.0f;
					Pair.Value.Capture.Visibility.bGuardInitialized = false;
					Pair.Value.Capture.Visibility.bAwaitingPostGuardSample = false;
					if (Pair.Value.Ownership.EffectiveOwnership
						!= EWPPairOwnershipMode::Disabled)
					{
						Pair.Value.Ownership.OwnershipEpoch =
							AdvanceWPOwnershipEpoch(Pair.Value.Ownership.OwnershipEpoch);
					}
					Pair.Value.Ownership.RequestedOwnership = EWPPairOwnershipMode::Disabled;
					Pair.Value.Ownership.EffectiveOwnership = EWPPairOwnershipMode::Disabled;
					Pair.Value.Ownership.bOwnershipInputsReady = false;
					Pair.Value.Publication.bDirty = true;
					Pair.Value.Publication.bHasPublished = false;
				}
			}
			else
			{
				bPairsDirty = true;
				for (TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
				{
					Pair.Value.Publication.bDirty = true;
				}
			}
		}

#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Runtime] Production RenderPacket pipeline control changed. World=%s RuntimeEnabledRaw=%d EffectiveEnablePolicy=RuntimeEnabled ControlChanged=%d EffectiveChanged=%d Enabled=%d PairCount=%d ReleasedHandles=%d ProductionPathDisabled=%d RasterProxyDependency=0 RasterFallbackAvailable=0 CaptureSchedulerUnaffected=1 CapturePairStatesRemoved=0 ActorTickDependency=0 CpuMs=%.3f"),
			*GetNameSafe(GetWorld()), RuntimeEnabledRaw,
			bPipelineControlChanged ? 1 : 0, bPipelineEffectiveChanged ? 1 : 0,
			bRenderPacketPipelineEnabled ? 1 : 0,
			PairStates.Num(), ReleasedHandleCount,
			bRenderPacketPipelineEnabled ? 0 : 1,
			(FPlatformTime::Seconds() - ToggleStartSeconds) * 1000.0);
#endif
	}
	LastRuntimeEnabledRaw = RuntimeEnabledRaw;

	if (!bRenderPacketPipelineEnabled)
	{
		return;
	}

	if (bPairsDirty)
	{
		RebuildPairs();
	}

	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	AActor* ReferenceViewActor = nullptr;
	float CameraFOVDegrees = 0.0f;
	const bool bHasReferenceView = ResolveReferenceView(
		CameraLocation, CameraRotation, ReferenceViewActor, CameraFOVDegrees);
	const double NowSeconds = FPlatformTime::Seconds();
	if (CaptureScheduler)
	{
		CaptureScheduler->UpdateVisibilityAndOcclusion(
			CameraLocation, ReferenceViewActor, bHasReferenceView, NowSeconds);
	}
	IWPRenderer* Renderer = IWPRenderer::Find();
	if (!Renderer && !PairStates.IsEmpty()
		&& NowSeconds - LastRendererUnavailableLogSeconds >= RendererUnavailableLogIntervalSeconds)
	{
		LastRendererUnavailableLogSeconds = NowSeconds;
		WP_LOG(this, Warning,
			TEXT("[Runtime] Renderer unavailable; production output is disabled while manager-owned capture continues. World=%s PairCount=%d ProductionOutputAvailable=0 RasterProxyDependency=0 RasterFallbackAvailable=0 RetryAfterSeconds=%.1f CpuMs=%.4f"),
			*GetNameSafe(GetWorld()), PairStates.Num(), RendererUnavailableLogIntervalSeconds,
			(FPlatformTime::Seconds() - NowSeconds) * 1000.0);
	}

	for (TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
	{
		FWPPortalPairState& PairState = PairEntry.Value;
		AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
		AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
		if (!IsValid(PortalA) || !IsValid(PortalB))
		{
			// Never reconstruct or validate topology from Actor links here. A missed abnormal
			// lifetime cleanup is reconciled against Registry snapshots on the next pass.
			bPairsDirty = true;
			continue;
		}

		RenderPublication->EnsureRendererRegistration(PairState);
		if (bHasReferenceView)
		{
			RenderPublication->UpdateReferenceViewState(PairState, CameraLocation);
		}

		FWPRenderPacket Packet = RenderPublication->BuildRenderPacket(
			PairState, CameraLocation, ReferenceViewActor, bHasReferenceView);
		RenderPublication->UpdatePairOwnership(PairState, Packet, Renderer);
#if !UE_BUILD_SHIPPING
		if (!PairState.Validation.bValidationStateInitialized
			|| PairState.Validation.bLastResourcesReady != Packet.bResourcesReady
			|| PairState.Validation.bLastMetricCompatible != Packet.bMetricCompatible
			|| PairState.Validation.bLastCaptureReady != Packet.bCaptureReady
			|| PairState.Validation.bLastScaleSupported != Packet.bScaleSupported)
		{
			// 로그 전용: validation 변경 로그를 구성하는 CPU 시간을 측정합니다.
			const double ValidationStartSeconds = FPlatformTime::Seconds();
			PairState.Validation.bValidationStateInitialized = true;
			PairState.Validation.bLastResourcesReady = Packet.bResourcesReady;
			PairState.Validation.bLastMetricCompatible = Packet.bMetricCompatible;
			PairState.Validation.bLastCaptureReady = Packet.bCaptureReady;
			PairState.Validation.bLastScaleSupported = Packet.bScaleSupported;
			WP_LOG(this, Verbose,
				TEXT("[Runtime][CubeContract] Pair cube contract validation changed. PairId=%s PortalA=%s PortalB=%s CubeRefA=%d CubeRefB=%d CubeLayoutA=%u CubeLayoutB=%u ResourceGenerationA=%u ResourceGenerationB=%u CaptureGenerationA=%u CaptureGenerationB=%u ExtentA=%dx%d ExtentB=%dx%d FormatA=%s FormatB=%s MipCountA=%u MipCountB=%u DimensionA=%s DimensionB=%s ContractValidA=%d ContractValidB=%d ResourcesReady=%d ContentOnlyCaptureRepublish=Suppressed CpuMs=%.4f"),
				*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
				Packet.CubeA.IsValid() ? 1 : 0, Packet.CubeB.IsValid() ? 1 : 0,
				Packet.CubeContractA.CubeLayoutVersion, Packet.CubeContractB.CubeLayoutVersion,
				Packet.CubeContractA.ResourceGeneration, Packet.CubeContractB.ResourceGeneration,
				Packet.CaptureGenerationA, Packet.CaptureGenerationB,
				Packet.CubeContractA.ExpectedExtent.X, Packet.CubeContractA.ExpectedExtent.Y,
				Packet.CubeContractB.ExpectedExtent.X, Packet.CubeContractB.ExpectedExtent.Y,
				GetWPCubeFormatName(Packet.CubeContractA.ExpectedFormat),
				GetWPCubeFormatName(Packet.CubeContractB.ExpectedFormat),
				Packet.CubeContractA.ExpectedMipCount, Packet.CubeContractB.ExpectedMipCount,
				GetWPCubeDimensionName(Packet.CubeContractA.ExpectedDimension),
				GetWPCubeDimensionName(Packet.CubeContractB.ExpectedDimension),
				Packet.CubeContractA.IsValid() ? 1 : 0, Packet.CubeContractB.IsValid() ? 1 : 0,
				Packet.bResourcesReady ? 1 : 0,
				(FPlatformTime::Seconds() - ValidationStartSeconds) * 1000.0);
			const bool bBaseEligible = Packet.MetricA.IsFiniteAndValid()
				&& Packet.MetricB.IsFiniteAndValid()
				&& Packet.bResourcesReady
				&& Packet.bMetricCompatible
				&& Packet.bCaptureReady
				&& Packet.bScaleSupported;
			const bool bExpectedCaptureWarmup = Packet.MetricA.IsFiniteAndValid()
				&& Packet.MetricB.IsFiniteAndValid()
				&& Packet.bResourcesReady
				&& Packet.bMetricCompatible
				&& !Packet.bCaptureReady
				&& Packet.bScaleSupported;
			FWPLUTEndpointSnapshot LUTSnapshotA;
			FWPLUTEndpointSnapshot LUTSnapshotB;
			const bool bHasLUTSnapshotA = LUTEndpointManager
				&& LUTEndpointManager->GetEndpointSnapshot(PortalA, LUTSnapshotA);
			const bool bHasLUTSnapshotB = LUTEndpointManager
				&& LUTEndpointManager->GetEndpointSnapshot(PortalB, LUTSnapshotB);
			const bool bExpectedLUTWarmup = Packet.MetricA.IsFiniteAndValid()
				&& Packet.MetricB.IsFiniteAndValid()
				&& Packet.bMetricCompatible
				&& Packet.bScaleSupported
				&& Packet.CubeA.IsValid()
				&& Packet.CubeB.IsValid()
				&& Packet.CubeContractA.IsValid()
				&& Packet.CubeContractB.IsValid()
				&& !Packet.bResourcesReady
				&& bHasLUTSnapshotA
				&& bHasLUTSnapshotB
				&& LUTSnapshotA.LastError.IsEmpty()
				&& LUTSnapshotB.LastError.IsEmpty()
				&& (LUTSnapshotA.bRequestPending || LUTSnapshotA.IsReady())
				&& (LUTSnapshotB.bRequestPending || LUTSnapshotB.IsReady())
				&& (LUTSnapshotA.bRequestPending || LUTSnapshotB.bRequestPending);
			if (bBaseEligible)
			{
				WP_LOG(this, Verbose,
					TEXT("[Runtime] Pair validation changed. PairId=%s PortalA=%s PortalB=%s BaseEligible=1 ResourcesReady=1 MetricCompatible=1 CaptureSubmitted=1 ScaleSupported=1 CubeA=%d CubeB=%d LUTA=%d LUTB=%d AnalyticNoTransitionA=%d AnalyticNoTransitionB=%d CaptureA=%u CaptureB=%u Reason=Ready RasterProxyDependency=0 CpuMs=%.4f"),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
					Packet.CubeA.IsValid() ? 1 : 0, Packet.CubeB.IsValid() ? 1 : 0,
					Packet.RayLUTA.IsValid() ? 1 : 0, Packet.RayLUTB.IsValid() ? 1 : 0,
					Packet.bAnalyticNoTransitionA ? 1 : 0,
					Packet.bAnalyticNoTransitionB ? 1 : 0,
					Packet.CaptureGenerationA, Packet.CaptureGenerationB,
					(FPlatformTime::Seconds() - ValidationStartSeconds) * 1000.0);
			}
			else if (bExpectedLUTWarmup)
			{
				WP_LOG(this, Verbose,
					TEXT("[Runtime] Pair validation changed. PairId=%s PortalA=%s PortalB=%s BaseEligible=0 MetricAValid=1 MetricBValid=1 ResourcesReady=0 MetricCompatible=1 CaptureSubmitted=%d ScaleSupported=1 CubeA=1 CubeB=1 LUTA=%d LUTB=%d LUTPendingA=%d LUTPendingB=%d LUTReadyA=%d LUTReadyB=%d AnalyticNoTransitionA=%d AnalyticNoTransitionB=%d CaptureA=%u CaptureB=%u Reason=ExpectedAsyncLUTWarmup RasterProxyDependency=0 CpuMs=%.4f"),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
					Packet.bCaptureReady ? 1 : 0,
					Packet.RayLUTA.IsValid() ? 1 : 0, Packet.RayLUTB.IsValid() ? 1 : 0,
					LUTSnapshotA.bRequestPending ? 1 : 0,
					LUTSnapshotB.bRequestPending ? 1 : 0,
					LUTSnapshotA.IsReady() ? 1 : 0, LUTSnapshotB.IsReady() ? 1 : 0,
					Packet.bAnalyticNoTransitionA ? 1 : 0,
					Packet.bAnalyticNoTransitionB ? 1 : 0,
					Packet.CaptureGenerationA, Packet.CaptureGenerationB,
					(FPlatformTime::Seconds() - ValidationStartSeconds) * 1000.0);
			}
			else if (bExpectedCaptureWarmup)
			{
				WP_LOG(this, Verbose,
					TEXT("[Runtime] Pair validation changed. PairId=%s PortalA=%s PortalB=%s BaseEligible=0 MetricAValid=%d MetricBValid=%d ResourcesReady=%d MetricCompatible=%d CaptureSubmitted=%d ScaleSupported=%d CubeA=%d CubeB=%d LUTA=%d LUTB=%d AnalyticNoTransitionA=%d AnalyticNoTransitionB=%d CaptureA=%u CaptureB=%u Reason=ExpectedCaptureWarmup RasterProxyDependency=0 CpuMs=%.4f"),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
					Packet.MetricA.IsFiniteAndValid() ? 1 : 0, Packet.MetricB.IsFiniteAndValid() ? 1 : 0,
					Packet.bResourcesReady ? 1 : 0, Packet.bMetricCompatible ? 1 : 0,
					Packet.bCaptureReady ? 1 : 0, Packet.bScaleSupported ? 1 : 0,
					Packet.CubeA.IsValid() ? 1 : 0, Packet.CubeB.IsValid() ? 1 : 0,
					Packet.RayLUTA.IsValid() ? 1 : 0, Packet.RayLUTB.IsValid() ? 1 : 0,
					Packet.bAnalyticNoTransitionA ? 1 : 0,
					Packet.bAnalyticNoTransitionB ? 1 : 0,
					Packet.CaptureGenerationA, Packet.CaptureGenerationB,
					(FPlatformTime::Seconds() - ValidationStartSeconds) * 1000.0);
			}
			else
			{
				WP_LOG(this, Verbose,
					TEXT("[Runtime] Pair validation changed. PairId=%s PortalA=%s PortalB=%s BaseEligible=0 MetricAValid=%d MetricBValid=%d ResourcesReady=%d MetricCompatible=%d CaptureSubmitted=%d ScaleSupported=%d CubeA=%d CubeB=%d LUTA=%d LUTB=%d LUTSnapshotA=%d LUTSnapshotB=%d LUTPendingA=%d LUTPendingB=%d LUTErrorA=\"%s\" LUTErrorB=\"%s\" AnalyticNoTransitionA=%d AnalyticNoTransitionB=%d CaptureA=%u CaptureB=%u Reason=UnexpectedValidationFailure RasterProxyDependency=0 CpuMs=%.4f"),
					*PairIdToString(PairState.Identity.PairId), *GetNameSafe(PortalA), *GetNameSafe(PortalB),
					Packet.MetricA.IsFiniteAndValid() ? 1 : 0, Packet.MetricB.IsFiniteAndValid() ? 1 : 0,
					Packet.bResourcesReady ? 1 : 0, Packet.bMetricCompatible ? 1 : 0,
					Packet.bCaptureReady ? 1 : 0, Packet.bScaleSupported ? 1 : 0,
					Packet.CubeA.IsValid() ? 1 : 0, Packet.CubeB.IsValid() ? 1 : 0,
					Packet.RayLUTA.IsValid() ? 1 : 0, Packet.RayLUTB.IsValid() ? 1 : 0,
					bHasLUTSnapshotA ? 1 : 0, bHasLUTSnapshotB ? 1 : 0,
					LUTSnapshotA.bRequestPending ? 1 : 0,
					LUTSnapshotB.bRequestPending ? 1 : 0,
					*LUTSnapshotA.LastError, *LUTSnapshotB.LastError,
					Packet.bAnalyticNoTransitionA ? 1 : 0,
					Packet.bAnalyticNoTransitionB ? 1 : 0,
					Packet.CaptureGenerationA, Packet.CaptureGenerationB,
					(FPlatformTime::Seconds() - ValidationStartSeconds) * 1000.0);
			}
		}
#endif

		const FTransform TransformA = PortalA->GetActorTransform();
		const FTransform TransformB = PortalB->GetActorTransform();
		if (!Renderer || !PairState.Ownership.RenderHandle.IsValid())
		{
			continue;
		}
		const FWPPublishDecision PublishDecision = RenderPublication->MakePublishDecision(
			PairState, Packet, TransformA, TransformB, NowSeconds);
		PairState.Publication.LastObservedCaptureGenerationA = Packet.CaptureGenerationA;
		PairState.Publication.LastObservedCaptureGenerationB = Packet.CaptureGenerationB;
		PairState.Publication.bCaptureGenerationObservationInitialized = true;
		if (!PublishDecision.bShouldPublish)
		{
			continue;
		}
		if (Renderer->UpdatePair(PairState.Ownership.RenderHandle, Packet))
		{
			RenderPublication->CommitPublishedState(
				PairState, Packet, TransformA, TransformB, CameraLocation,
				PublishDecision, NowSeconds);
		}
		else
		{
			PairState.Publication.bDirty = true;
		}
	}

}

TStatId UWPRuntimeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWPRuntimeSubsystem, STATGROUP_Tickables);
}

bool UWPRuntimeSubsystem::IsTickable() const
{
	const UWorld* World = GetWorld();
	return !HasAnyFlags(RF_ClassDefaultObject) && IsInitialized() && bRenderRuntimeEnabled && World
		&& World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer;
}

// Capture policy helpers/CVars remain private to this translation unit; the implementation
// below belongs to the composed FWPRuntimeCaptureScheduler object.
#include "Subsystem/WPRuntimeCaptureScheduler.inl"

void UWPRuntimeSubsystem::HandleWorldPostActorTick(
	UWorld* World,
	const ELevelTick TickType,
	const float DeltaSeconds)
{
	if (CaptureScheduler)
	{
		CaptureScheduler->HandleWorldPostActorTick(World, TickType, DeltaSeconds);
	}
}

void UWPRuntimeSubsystem::RebuildPairs()
{
	SCOPE_CYCLE_COUNTER(STAT_WP_PairRebuild);
	// Registry reconcile 완료 로그에 전체 CPU 시간을 포함합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	bPairsDirty = false;

	UWPRegistrySubsystem* Registry = RegistrySubsystem.Get();
	if (!Registry)
	{
		WP_LOG(this, Error,
			TEXT("[Runtime] Pair rebuild failed. World=%s Reason=RegistryUnavailable CpuMs=%.3f"),
			*GetNameSafe(GetWorld()), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return;
	}

	TArray<FWPPortalPairSnapshot> RegistryPairs;
	Registry->GetRegisteredPortalPairs(RegistryPairs);
	TMap<FGuid, FWPPortalPairSnapshot> ValidRegistryPairs;
	TArray<FGuid> ValidRegistryPairOrder;
	TSet<const AWormholePortalActor*> SeenRegistryEndpoints;
	ValidRegistryPairs.Reserve(RegistryPairs.Num());
	ValidRegistryPairOrder.Reserve(RegistryPairs.Num());
	SeenRegistryEndpoints.Reserve(RegistryPairs.Num() * 2);
#if !UE_BUILD_SHIPPING
	int32 AddedPairCount = 0;
	int32 KeptPairCount = 0;
	int32 ReplacedPairCount = 0;
	int32 RejectedSnapshotCount = 0;
#endif
	for (const FWPPortalPairSnapshot& RegistryPair : RegistryPairs)
	{
		if (!RegistryPair.IsStructurallyValid())
		{
#if !UE_BUILD_SHIPPING
			++RejectedSnapshotCount;
#endif
			WP_LOG(this, Error,
				TEXT("[Runtime][RegistryPairAuthority] Registry snapshot rejected. World=%s PairId=%s Reason=StructurallyInvalid RegistryPairIdAuthority=1 CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), *PairIdToString(RegistryPair.PairId),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			continue;
		}

		AWormholePortalActor* PortalA = RegistryPair.PortalA.Get();
		AWormholePortalActor* PortalB = RegistryPair.PortalB.Get();
		const bool bPortalAValid = IsValid(PortalA);
		const bool bPortalBValid = IsValid(PortalB);
		const bool bSameWorld = bPortalAValid && bPortalBValid
			&& PortalA->GetWorld() == GetWorld() && PortalB->GetWorld() == GetWorld();
		if (!bPortalAValid || !bPortalBValid || !bSameWorld)
		{
#if !UE_BUILD_SHIPPING
			++RejectedSnapshotCount;
#endif
			WP_LOG(this, Error,
				TEXT("[Runtime][RegistryPairAuthority] Registry snapshot rejected. World=%s PairId=%s PortalA=%s PortalB=%s PortalAValid=%d PortalBValid=%d SameWorld=%d Reason=EndpointInvalidOrForeignWorld RegistryPairIdAuthority=1 CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), *PairIdToString(RegistryPair.PairId),
				*GetNameSafe(PortalA), *GetNameSafe(PortalB),
				bPortalAValid ? 1 : 0, bPortalBValid ? 1 : 0,
				bSameWorld ? 1 : 0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			continue;
		}

		const bool bDuplicatePairId = ValidRegistryPairs.Contains(RegistryPair.PairId);
		const bool bDuplicateEndpoint = SeenRegistryEndpoints.Contains(PortalA)
			|| SeenRegistryEndpoints.Contains(PortalB);
		if (bDuplicatePairId || bDuplicateEndpoint)
		{
#if !UE_BUILD_SHIPPING
			++RejectedSnapshotCount;
#endif
			WP_LOG(this, Error,
				TEXT("[Runtime][RegistryPairAuthority] Registry snapshot rejected. World=%s PairId=%s DuplicateEndpoint=%d DuplicatePairId=%d Reason=DuplicateRegistrySnapshot RegistryPairIdAuthority=1 CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), *PairIdToString(RegistryPair.PairId),
				bDuplicateEndpoint ? 1 : 0, bDuplicatePairId ? 1 : 0,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			continue;
		}
		SeenRegistryEndpoints.Add(PortalA);
		SeenRegistryEndpoints.Add(PortalB);
		ValidRegistryPairs.Add(RegistryPair.PairId, RegistryPair);
		ValidRegistryPairOrder.Add(RegistryPair.PairId);
	}

	// Remove old lifetimes first. A relink can reuse the same endpoints, but Registry assigns a
	// new PairId, so no renderer/packet/ownership/scheduler state is allowed to cross that boundary.
	TArray<FGuid> RemovedPairIds;
	for (const TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
	{
		if (!ValidRegistryPairs.Contains(Pair.Key))
		{
			RemovedPairIds.Add(Pair.Key);
		}
	}
	for (const FGuid& RemovedPairId : RemovedPairIds)
	{
		FWPPortalPairState* RemovedState = PairStates.Find(RemovedPairId);
		if (!RemovedState)
		{
			continue;
		}

		FGuid ReplacementPairId;
		for (const FGuid& CandidatePairId : ValidRegistryPairOrder)
		{
			const FWPPortalPairSnapshot* Candidate = ValidRegistryPairs.Find(CandidatePairId);
			if (!Candidate)
			{
				continue;
			}
			const bool bSameEndpoints =
				(RemovedState->Identity.PortalA == Candidate->PortalA
					&& RemovedState->Identity.PortalB == Candidate->PortalB)
				|| (RemovedState->Identity.PortalA == Candidate->PortalB
					&& RemovedState->Identity.PortalB == Candidate->PortalA);
			if (bSameEndpoints)
			{
				ReplacementPairId = CandidatePairId;
				break;
			}
		}

		if (ReplacementPairId.IsValid())
		{
#if !UE_BUILD_SHIPPING
			++ReplacedPairCount;
			WP_LOG(this, Verbose,
				TEXT("[Runtime][RegistryPairAuthority] Pair lifetime replaced. World=%s OldPairId=%s NewPairId=%s OldRendererStateRemoved=1 OldPacketSequenceRemoved=1 OldOwnershipStateRemoved=1 OldCaptureSchedulerStateRemoved=1 PairStateMapKeySource=RegistryPairId Reason=SameEndpointsNewRegistryPairId CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), *PairIdToString(RemovedPairId),
				*PairIdToString(ReplacementPairId),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		RemovePair(RemovedPairId, *RemovedState,
			ReplacementPairId.IsValid()
				? TEXT("RegistryPairIdReplaced")
				: TEXT("AbsentFromRegistrySnapshot"));
		PairStates.Remove(RemovedPairId);
	}

	for (const FGuid& RegistryPairId : ValidRegistryPairOrder)
	{
		const FWPPortalPairSnapshot* RegistryPair = ValidRegistryPairs.Find(RegistryPairId);
		if (!RegistryPair)
		{
			continue;
		}
		AWormholePortalActor* PortalA = RegistryPair->PortalA.Get();
		AWormholePortalActor* PortalB = RegistryPair->PortalB.Get();
		FWPPortalPairState* ExistingState = PairStates.Find(RegistryPairId);
		const EWPRegistryPairReconcileAction ReconcileAction =
			EvaluateWPRegistryPairReconcileAction(
				ExistingState != nullptr,
				ExistingState ? ExistingState->Identity.PairId : FGuid(),
				RegistryPairId);
		if (ReconcileAction == EWPRegistryPairReconcileAction::Replace)
		{
			RemovePair(RegistryPairId, *ExistingState, TEXT("CorruptPairIdInvariant"));
			PairStates.Remove(RegistryPairId);
			ExistingState = nullptr;
#if !UE_BUILD_SHIPPING
			++ReplacedPairCount;
#endif
		}

		if (!ExistingState)
		{
			FWPPortalPairState& NewState = PairStates.Add(RegistryPairId);
			NewState.Identity.PairId = RegistryPairId;
			NewState.Identity.PortalA = PortalA;
			NewState.Identity.PortalB = PortalB;
			NewState.Identity.StableSelectorNameA = PortalA->GetStableSelectorName();
			NewState.Identity.StableSelectorNameB = PortalB->GetStableSelectorName();
			NewState.Publication.bDirty = true;
#if !UE_BUILD_SHIPPING
			++AddedPairCount;
			WP_LOG(this, Verbose,
				TEXT("[Runtime] Pair created. PairId=%s PairStateMapKey=%s PairSortKey=%llu PortalA=%s PortalB=%s SelectorA=%s SelectorB=%s PairIdSource=Registry PairStateMapKeySource=RegistryPairId RegistryPairIdAuthority=1 PortalLinkPairRebuild=0 SelfGeneratedPairIds=0 CanonicalEndpointsSource=Registry OwnershipDefault=Disabled OwnershipEpoch=0 RasterProxyDependency=0 RasterFallbackAvailable=0 ReconcileElapsedCpuMs=%.4f"),
				*PairIdToString(NewState.Identity.PairId), *PairIdToString(RegistryPairId),
				MakePairSortKey(RegistryPairId), *GetNameSafe(PortalA),
				*GetNameSafe(PortalB), *NewState.Identity.StableSelectorNameA.ToString(),
				*NewState.Identity.StableSelectorNameB.ToString(),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		else
		{
			const bool bEndpointsChanged = ExistingState->Identity.PortalA.Get() != PortalA || ExistingState->Identity.PortalB.Get() != PortalB;
			ExistingState->Identity.PortalA = PortalA;
			ExistingState->Identity.PortalB = PortalB;
			ExistingState->Publication.bDirty |= bEndpointsChanged;
#if !UE_BUILD_SHIPPING
			++KeptPairCount;
#endif
		}
	}

#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	WP_LOG(this, Verbose,
		TEXT("[Runtime][RegistryPairAuthority] Pair reconcile complete. World=%s RegistrySnapshots=%d ActivePairs=%d AddedPairs=%d KeptPairs=%d ReplacedPairLifetimes=%d RemovedPairs=%d RejectedSnapshots=%d RegistryPairIdAuthority=1 PairLifecycleAuthority=Registry PortalLinkReads=0 SelfGeneratedPairIds=0 CpuMs=%.3f"),
		*GetNameSafe(GetWorld()), RegistryPairs.Num(), PairStates.Num(), AddedPairCount,
		KeptPairCount, ReplacedPairCount, RemovedPairIds.Num(), RejectedSnapshotCount, CpuMs);
#endif
}

void UWPRuntimeSubsystem::RemovePair(
	const FGuid& PairStateKey,
	FWPPortalPairState& PairState,
	const TCHAR* Reason)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: 제거 전 authority와 cleanup 결과/CPU 시간을 최종 pair 로그에 보존합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const bool bPairIdInvariantValid = PairStateKey == PairState.Identity.PairId;
	const EWPCaptureAuthority CaptureAuthorityBeforeRemoval = PairState.Capture.Authority;
#endif
	// Pair-owned capture/transit state and renderer ownership are removed while both endpoints
	// still exist. Endpoint resources are released only by the later Registry unregister path.
#if !UE_BUILD_SHIPPING
	const bool bCapturePairStateRemoved = CaptureManager
		&& CaptureManager->RemovePairCaptureState(PairState.Identity.PairId, Reason);
	bool bRendererUnregistered = false;
#else
	if (CaptureManager)
	{
		CaptureManager->RemovePairCaptureState(PairState.Identity.PairId, Reason);
	}
#endif
	if (IWPRenderer* Renderer = IWPRenderer::Find())
	{
		if (PairState.Ownership.RenderHandle.IsValid() && PairState.Ownership.RenderHandle.ServiceId == Renderer->GetServiceId())
		{
			Renderer->UnregisterPair(PairState.Ownership.RenderHandle);
#if !UE_BUILD_SHIPPING
			bRendererUnregistered = true;
#endif
		}
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[Runtime] Pair removed. PairId=%s PairStateMapKey=%s PairIdInvariantValid=%d PairStateMapKeySource=RegistryPairId PortalA=%s PortalB=%s SelectorA=%s SelectorB=%s Handle=%llu ServiceId=%llu LastSequence=%llu RequestedOwnership=%s EffectiveOwnership=%s OwnershipEpoch=%llu CaptureCadenceElapsedMs=%.3f CaptureLastSuccessfulFrame=%llu CaptureLastFailedFrame=%llu TransitForcedCaptureSequence=%llu CaptureSchedulerStateRemoved=%d RendererUnregistered=%d CaptureAuthorityBeforeRemoval=%s ActorFallbackRestore=0 ActorTickDependency=0 ManagerPairStateRemovedBeforeEndpointRelease=1 ManagedResourcesRetainedUntilEndpointUnregister=1 TransitForcesCapture=%d TransitForcedSequence=%llu Reason=%s CpuMs=%.3f"),
		*PairIdToString(PairState.Identity.PairId), *PairIdToString(PairStateKey),
		bPairIdInvariantValid ? 1 : 0, *GetNameSafe(PairState.Identity.PortalA.Get()),
		*GetNameSafe(PairState.Identity.PortalB.Get()), *PairState.Identity.StableSelectorNameA.ToString(),
		*PairState.Identity.StableSelectorNameB.ToString(), PairState.Ownership.RenderHandle.Value,
		PairState.Ownership.RenderHandle.ServiceId, PairState.Publication.PacketSequence,
		GetWPPairOwnershipModeName(PairState.Ownership.RequestedOwnership),
		GetWPPairOwnershipModeName(PairState.Ownership.EffectiveOwnership),
		PairState.Ownership.OwnershipEpoch, PairState.Capture.CadenceElapsedSeconds * 1000.0,
		static_cast<unsigned long long>(PairState.Capture.LastSuccessfulSubmissionFrame),
		static_cast<unsigned long long>(PairState.Capture.LastFailedSubmissionFrame),
		0ull,
		bCapturePairStateRemoved ? 1 : 0,
		bRendererUnregistered ? 1 : 0,
		FWPRuntimeCaptureScheduler::GetCaptureAuthorityName(CaptureAuthorityBeforeRemoval),
		0,
		0ull,
		Reason,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	PairState.Ownership.RenderHandle.Reset();
}

int32 UWPRuntimeSubsystem::MarkPairDirtyForPortal(
	AWormholePortalActor* Portal, const bool bTopologyChanged)
{
	if (bTopologyChanged)
	{
		bPairsDirty = true;
	}

	int32 DirtiedPairCount = 0;
	for (TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
	{
		if (Pair.Value.Identity.PortalA.Get() == Portal || Pair.Value.Identity.PortalB.Get() == Portal)
		{
			Pair.Value.Publication.bDirty = true;
			++DirtiedPairCount;
		}
	}
	return DirtiedPairCount;
}

#include "Subsystem/WPRuntimeRenderPublication.inl"

bool UWPRuntimeSubsystem::ResolveReferenceView(
	FVector& OutCameraLocation,
	FRotator& OutCameraRotation,
	AActor*& OutViewActor,
	float& OutCameraFOVDegrees) const
{
	return RenderPublication
		&& RenderPublication->ResolveReferenceView(
			OutCameraLocation,
			OutCameraRotation,
			OutViewActor,
			OutCameraFOVDegrees);
}

void UWPRuntimeSubsystem::HandlePortalRegistered(AWormholePortalActor* Portal)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: event 처리 CPU 시간과 dirty 처리 개수를 출력합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 DirtiedPairCount = MarkPairDirtyForPortal(Portal, false);
#else
	MarkPairDirtyForPortal(Portal, false);
#endif
	const bool bEndpointMayBeAllocated = IsValid(Portal)
		&& Portal->HasActorBegunPlay();
#if !UE_BUILD_SHIPPING
	const bool bManagedEndpointReady = bEndpointMayBeAllocated
		&& CaptureManager
		&& CaptureManager->EnsureEndpointResources(
			Portal,
			ResolveWPDynamicCaptureResolutionPolicy().LowestVisibleResolution);
	bool bLUTEndpointRegistered = false;
#else
	if (bEndpointMayBeAllocated && CaptureManager)
	{
		CaptureManager->EnsureEndpointResources(
			Portal,
			ResolveWPDynamicCaptureResolutionPolicy().LowestVisibleResolution);
	}
#endif
	if (bEndpointMayBeAllocated && LUTEndpointManager)
	{
		FWPLUTEndpointRequestOptions Options;
		Options.PreferredAsset = Portal->LUTAssetOverride;
		Options.DebugContext = Portal->GetPathName();
#if !UE_BUILD_SHIPPING
		bLUTEndpointRegistered = LUTEndpointManager->RegisterEndpoint(Portal, Options);
#else
		LUTEndpointManager->RegisterEndpoint(Portal, Options);
#endif
	}
#if !UE_BUILD_SHIPPING
	if (!bManagedEndpointReady)
	{
		WP_LOG(this, Verbose,
			TEXT("[RuntimeEvent] Portal registered without a managed capture endpoint. Portal=%s PortalValid=%d ActorBegunPlay=%d CaptureManagerValid=%d PairCount=%d FailureOwner=RuntimeCaptureManager ActorFallback=0 CpuMs=%.3f"),
			*GetNameSafe(Portal), IsValid(Portal) ? 1 : 0,
			IsValid(Portal) && Portal->HasActorBegunPlay() ? 1 : 0,
			CaptureManager ? 1 : 0, PairStates.Num(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
	WP_LOG(this, Verbose,
		TEXT("[RuntimeEvent] Portal registered observed. Portal=%s TopologyDirty=0 DirtiedPairs=%d PairCount=%d PairTopologyAuthority=RegistryPairEvents PairCreatedFromPortalEvent=0 ManagedCaptureEndpointReady=%d ManagedCaptureEndpointCount=%d LUTEndpointRegistered=%d LUTEndpointCount=%d LUTReadyCount=%d LiveReregisterRepairSupported=1 ActorRenderResourceOwner=0 CpuMs=%.3f"),
		*GetNameSafe(Portal), DirtiedPairCount, PairStates.Num(),
		bManagedEndpointReady ? 1 : 0,
		CaptureManager ? CaptureManager->GetEndpointCount() : 0,
		bLUTEndpointRegistered ? 1 : 0,
		LUTEndpointManager ? LUTEndpointManager->GetEndpointCount() : 0,
		LUTEndpointManager ? LUTEndpointManager->GetReadyBindingCount() : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRuntimeSubsystem::HandlePortalUnregistered(AWormholePortalActor* Portal)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: unregister cleanup CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	// Registry broadcasts generic Unregistered before its PairRemoved delegate. Tear down
	// every pair state that references this endpoint first so renderer handles and capture
	// pair state can never outlive the resources released below. The later PairRemoved event
	// is intentionally idempotent.
	TArray<FGuid> PairIdsToRemove;
	for (const TPair<FGuid, FWPPortalPairState>& PairEntry : PairStates)
	{
		if (PairEntry.Value.Identity.PortalA.Get() == Portal
			|| PairEntry.Value.Identity.PortalB.Get() == Portal)
		{
			PairIdsToRemove.Add(PairEntry.Key);
		}
	}
	for (const FGuid& PairId : PairIdsToRemove)
	{
		if (FWPPortalPairState* PairState = PairStates.Find(PairId))
		{
			RemovePair(PairId, *PairState, TEXT("RegistryPortalUnregisteredPreResourceRelease"));
			PairStates.Remove(PairId);
		}
	}
	bPairsDirty = true;
#if !UE_BUILD_SHIPPING
	const bool bReleasedManagedCaptureEndpoint = CaptureManager
		&& CaptureManager->ReleaseEndpointResources(
			Portal, TEXT("RegistryPortalUnregistered"));
	const bool bReleasedLUTEndpoint = LUTEndpointManager
		&& LUTEndpointManager->UnregisterEndpoint(
			Portal, TEXT("RegistryPortalUnregistered"));
#else
	if (CaptureManager)
	{
		CaptureManager->ReleaseEndpointResources(
			Portal, TEXT("RegistryPortalUnregistered"));
	}
	if (LUTEndpointManager)
	{
		LUTEndpointManager->UnregisterEndpoint(
			Portal, TEXT("RegistryPortalUnregistered"));
	}
#endif
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[RuntimeEvent] Portal unregistered observed. Portal=%s PairsRemovedBeforeResources=%d PairCount=%d PairTopologyAuthority=RegistryPairEvents LaterPairRemovedEventIdempotent=1 ManagedCaptureEndpointReleased=%d RemainingManagedCaptureEndpoints=%d LUTEndpointReleased=%d RemainingLUTEndpoints=%d RendererBeforeResourceRelease=1 ActorRenderResourceOwner=0 CpuMs=%.3f"),
		*GetNameSafe(Portal), PairIdsToRemove.Num(), PairStates.Num(),
		bReleasedManagedCaptureEndpoint ? 1 : 0,
		CaptureManager ? CaptureManager->GetEndpointCount() : 0,
		bReleasedLUTEndpoint ? 1 : 0,
		LUTEndpointManager ? LUTEndpointManager->GetEndpointCount() : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRuntimeSubsystem::HandlePortalChanged(
	AWormholePortalActor* Portal, const EWPPortalChangeType ChangeType)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: event 처리 CPU 시간과 변경 결과를 출력합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const bool bTopologyChanged = ChangeType == EWPPortalChangeType::Link;
	const int32 DirtiedPairCount = MarkPairDirtyForPortal(Portal, false);
	bool bLUTRefreshRequested = false;
#else
	MarkPairDirtyForPortal(Portal, false);
#endif
	if (ChangeType == EWPPortalChangeType::Metric
		&& IsValid(Portal) && LUTEndpointManager)
	{
		FWPLUTEndpointRequestOptions Options;
		Options.PreferredAsset = Portal->LUTAssetOverride;
		Options.DebugContext = Portal->GetPathName();
#if !UE_BUILD_SHIPPING
		bLUTRefreshRequested = LUTEndpointManager->RefreshEndpoint(Portal, Options);
#else
		LUTEndpointManager->RefreshEndpoint(Portal, Options);
#endif
	}
#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	if (ChangeType != EWPPortalChangeType::RenderResources
		&& ChangeType != EWPPortalChangeType::Visual)
	{
		WP_LOG(this, Verbose,
			TEXT("[RuntimeEvent] Portal change observed. Portal=%s ChangeType=%s TopologyDirty=0 TopologyChangeObserved=%d DirtiedPairs=%d PairCount=%d PairTopologyAuthority=RegistryPairEvents LUTRefreshRequested=%d CpuMs=%.3f"),
			*GetNameSafe(Portal), GetRuntimePortalChangeTypeName(ChangeType),
			bTopologyChanged ? 1 : 0,
			DirtiedPairCount, PairStates.Num(), bLUTRefreshRequested ? 1 : 0, CpuMs);
	}
#endif
}

void UWPRuntimeSubsystem::HandleLUTEndpointChanged(
	TWeakObjectPtr<AWormholePortalActor> PortalKey,
	const FWPLUTEndpointSnapshot& Snapshot)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: endpoint event 처리 CPU 시간과 dirty/notify 결과를 출력합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	AWormholePortalActor* Portal = PortalKey.Get();
#if !UE_BUILD_SHIPPING
	const int32 DirtiedPairCount = MarkPairDirtyForPortal(Portal, false);
	bool bRegistryNotified = false;
#else
	MarkPairDirtyForPortal(Portal, false);
#endif
	if (Snapshot.bRegistered && IsValid(Portal))
	{
		if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
		{
			Registry->NotifyPortalChanged(Portal, EWPPortalChangeType::RenderResources);
#if !UE_BUILD_SHIPPING
			bRegistryNotified = true;
#endif
		}
	}
#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[RuntimeEvent][LUTEndpoint] Endpoint snapshot changed. World=%s Portal=%s Registered=%d Pending=%d Ready=%d AnalyticNoTransition=%d TransitionRatio=%.6f Z=%.6f MetricOuterRadiusCm=%.3f BindingGeneration=%u ResourceRevision=%u EndpointRevision=%u RequestGeneration=%llu ContractValid=%d Texture=%s Error=\"%s\" DirtiedPairs=%d RegistryRenderResourcesNotified=%d ActorLUTOwner=0 CpuMs=%.4f"),
		*GetNameSafe(GetWorld()), *GetNameSafe(Portal), Snapshot.bRegistered ? 1 : 0,
		Snapshot.bRequestPending ? 1 : 0, Snapshot.IsReady() ? 1 : 0,
		Snapshot.bAnalyticNoTransition ? 1 : 0, Snapshot.TransitionRatio,
		Snapshot.RatioCoordinate01, Snapshot.MetricOuterRadiusCm,
		Snapshot.BindingGeneration, Snapshot.ResourceRevision, Snapshot.EndpointRevision,
		static_cast<unsigned long long>(Snapshot.RequestGeneration),
		Snapshot.Contract.IsValid() ? 1 : 0,
		*GetNameSafe(Snapshot.VolumeTexture.Get()), *Snapshot.LastError,
		DirtiedPairCount, bRegistryNotified ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRuntimeSubsystem::HandlePortalPairAdded(
	const FWPPortalPairSnapshot& PairSnapshot)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: Registry pair reconcile CPU 시간과 reconcile 후 존재 여부를 출력합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	bPairsDirty = true;
	RebuildPairs();

#if !UE_BUILD_SHIPPING
	const bool bPresentAfterReconcile = PairStates.Contains(PairSnapshot.PairId);

	WP_LOG(this, Verbose,
		TEXT("[RuntimeEvent][RegistryPairAuthority] PairAdded observed. EventPairId=%s PresentAfterRegistrySnapshotReconcile=%d ActivePairs=%d EventEndpointPointersRead=0 PairLifecycleAuthority=Registry CurrentSnapshotWins=1 IdempotentAdd=1 CpuMs=%.3f"),
		*PairIdToString(PairSnapshot.PairId), bPresentAfterReconcile ? 1 : 0,
		PairStates.Num(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRuntimeSubsystem::HandlePortalPairRemoved(
	const FWPPortalPairSnapshot& PairSnapshot)
{
#if !UE_BUILD_SHIPPING
	// 로그 전용: immediate cleanup과 전체 reconcile CPU 시간을 구분해 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	bool bRemovedState = false;
#endif
	if (FWPPortalPairState* RemovedState = PairStates.Find(PairSnapshot.PairId))
	{
		RemovePair(PairSnapshot.PairId, *RemovedState, TEXT("RegistryPairRemovedEvent"));
		PairStates.Remove(PairSnapshot.PairId);
#if !UE_BUILD_SHIPPING
		bRemovedState = true;
#endif
	}

#if !UE_BUILD_SHIPPING
	const double ImmediateCleanupCpuMs =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
#endif
	// The event endpoint weak pointers are deliberately never dereferenced. PairId is the only
	// cleanup key, and the authoritative full snapshot then resolves stale/duplicate events.
	bPairsDirty = true;
	RebuildPairs();
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[RuntimeEvent][RegistryPairAuthority] PairRemoved observed. EventPairId=%s RemovedStateCount=%d DuplicateOrAlreadyRemoved=%d ActivePairsAfterReconcile=%d ImmediateRendererAndSchedulerCleanup=1 EventEndpointPointersRead=0 RemovalLookup=PairId CurrentSnapshotWins=1 IdempotentRemove=1 ImmediateCleanupCpuMs=%.4f TotalCpuMs=%.3f"),
		*PairIdToString(PairSnapshot.PairId), bRemovedState ? 1 : 0,
		bRemovedState ? 0 : 1, PairStates.Num(), ImmediateCleanupCpuMs,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPRuntimeSubsystem::HandleTransitStarted(const FWPTransitEvent& Event)
{
	ApplyTransitEvent(Event, TEXT("Started"));
}

void UWPRuntimeSubsystem::HandleTransitCommitted(const FWPTransitEvent& Event)
{
	ApplyTransitEvent(Event, TEXT("Committed"));
}

void UWPRuntimeSubsystem::HandleTransitCancelled(const FWPTransitEvent& Event)
{
	ApplyTransitEvent(Event, TEXT("Cancelled"));
}

void UWPRuntimeSubsystem::ApplyTransitEvent(const FWPTransitEvent& Event, const TCHAR* Phase)
{
	// 로그 전용: transit event 처리 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	const EWPTransitLifecyclePhase LifecyclePhase = ParseWPTransitLifecyclePhase(Phase);
	AActor* TransitActor = Event.Actor.Get();
	if (!IsRelevantReferenceActor(TransitActor))
	{
		return;
	}

	AWormholePortalActor* SourcePortal = Event.SourcePortal.Get();
	AWormholePortalActor* DestinationPortal = Event.DestinationPortal.Get();
	if (!IsValid(SourcePortal) || !IsValid(DestinationPortal) || SourcePortal == DestinationPortal)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Transit] Event rejected. Phase=%s EventSequence=%llu Actor=%s Source=%s Destination=%s SourceValid=%d DestinationValid=%d SameEndpoint=%d Reason=InvalidPortalEndpoints CpuMs=%.3f"),
			Phase, Event.Sequence, *GetNameSafe(TransitActor),
			*GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal),
			IsValid(SourcePortal) ? 1 : 0, IsValid(DestinationPortal) ? 1 : 0,
			SourcePortal == DestinationPortal ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return;
	}
	for (TPair<FGuid, FWPPortalPairState>& Pair : PairStates)
	{
		FWPPortalPairState& PairState = Pair.Value;
		const bool bMatchesForward = PairState.Identity.PortalA.Get() == SourcePortal
			&& PairState.Identity.PortalB.Get() == DestinationPortal;
		const bool bMatchesReverse = PairState.Identity.PortalA.Get() == DestinationPortal
			&& PairState.Identity.PortalB.Get() == SourcePortal;
		const bool bMatches = bMatchesForward || bMatchesReverse;
		if (!bMatches)
		{
			continue;
		}

		const uint64 PreviousEventSequence = PairState.Transit.LastTransitEventSequence;
		const bool bWasTransitActive = PairState.Transit.bTransitActive;
		const FWPTransitOrderDecision OrderDecision = EvaluateWPTransitEventOrder(
			Event.Sequence,
			PreviousEventSequence,
			bWasTransitActive,
			LifecyclePhase);
		if (!OrderDecision.bShouldApply)
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose,
				TEXT("[Transit] Event rejected. Phase=%s EventSequence=%llu PreviousEventSequence=%llu PreviousTransitActive=%d PairId=%s Actor=%s Source=%s Destination=%s Reason=%s CpuMs=%.3f"),
				Phase, Event.Sequence, PreviousEventSequence, bWasTransitActive ? 1 : 0,
				*PairIdToString(PairState.Identity.PairId), *GetNameSafe(TransitActor),
				*GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal),
				OrderDecision.DecisionReason,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return;
		}

		auto ResolveSide = [&PairState](const AWormholePortalActor* Portal)
		{
			if (PairState.Identity.PortalA.Get() == Portal)
			{
				return EWPSide::SideA;
			}
			if (PairState.Identity.PortalB.Get() == Portal)
			{
				return EWPSide::SideB;
			}
			return EWPSide::None;
		};

		const EWPSide SourceSide = ResolveSide(SourcePortal);
		const EWPSide DestinationSide = ResolveSide(DestinationPortal);
		PairState.Transit.TransitActorId = TransitActor ? TransitActor->GetUniqueID() : 0;
		ApplyWPTransitLifecycleState(
			LifecyclePhase,
			Event.Sequence,
			SourceSide,
			DestinationSide,
			PairState.Transit.LastTransitEventSequence,
			PairState.Transit.EntrySide,
			PairState.Transit.CurrentSide,
			PairState.Transit.bTransitActive);
		PairState.Publication.bDirty = true;
		EWPCaptureTransitPhase CaptureTransitPhase =
			EWPCaptureTransitPhase::Started;
		switch (LifecyclePhase)
		{
		case EWPTransitLifecyclePhase::Committed:
			CaptureTransitPhase = EWPCaptureTransitPhase::Committed;
			break;
		case EWPTransitLifecyclePhase::Cancelled:
			CaptureTransitPhase = EWPCaptureTransitPhase::Cancelled;
			break;
		default:
			break;
		}
		// ApplyPairTransitEvent 호출은 기능 동작이고, 반환값은 로그에만 사용합니다.
#if !UE_BUILD_SHIPPING
		const bool bCaptureManagerApplied = CaptureManager
			&& CaptureManager->ApplyPairTransitEvent(
				PairState.Identity.PairId, Event, CaptureTransitPhase);
#else
		if (CaptureManager)
		{
			CaptureManager->ApplyPairTransitEvent(
				PairState.Identity.PairId, Event, CaptureTransitPhase);
		}
#endif

#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Transit] Reference traversal state changed. Phase=%s EventSequence=%llu PreviousEventSequence=%llu PreviousTransitActive=%d PairId=%s Actor=%s ActorId=%u Source=%s Destination=%s SourceSide=%d DestinationSide=%d StoredEntrySide=%d CurrentSide=%d TransitActive=%d TransitForcesCapture=%d TransitForcedSequence=%llu CaptureManagerApplied=%d ActorTransitCaptureState=0 CaptureCadenceElapsedMs=%.3f TransitElapsedMs=%.3f SequencePolicy=RunLifecycle Decision=%s CpuMs=%.3f"),
			Phase, Event.Sequence, PreviousEventSequence, bWasTransitActive ? 1 : 0,
			*PairIdToString(PairState.Identity.PairId), *GetNameSafe(TransitActor),
			PairState.Transit.TransitActorId, *GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal),
			static_cast<int32>(SourceSide), static_cast<int32>(DestinationSide),
			static_cast<int32>(PairState.Transit.EntrySide), static_cast<int32>(PairState.Transit.CurrentSide),
			PairState.Transit.bTransitActive ? 1 : 0,
			0,
			0ull,
			bCaptureManagerApplied ? 1 : 0,
			PairState.Capture.CadenceElapsedSeconds * 1000.0,
			Event.TransitElapsedMs,
			OrderDecision.DecisionReason,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return;
	}

	WP_LOG(this, Warning,
		TEXT("[Transit] Event rejected. Phase=%s EventSequence=%llu Actor=%s Source=%s Destination=%s PairCount=%d Reason=NoExactMatchingPair CpuMs=%.3f"),
		Phase, Event.Sequence, *GetNameSafe(TransitActor), *GetNameSafe(SourcePortal),
		*GetNameSafe(DestinationPortal), PairStates.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

bool UWPRuntimeSubsystem::IsRelevantReferenceActor(const AActor* Actor) const
{
	const UWorld* World = GetWorld();
	if (!IsValid(Actor) || !World || World->GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}

	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	AActor* ViewActor = nullptr;
	const APlayerController* PlayerController = nullptr;
	float CameraFOVDegrees = 0.0f;
	return ResolvePrimaryLocalReferenceView(
		World, CameraLocation, CameraRotation, ViewActor, PlayerController,
		CameraFOVDegrees)
		&& PlayerController
		&& (PlayerController->GetPawn() == Actor || ViewActor == Actor);
}

uint64 UWPRuntimeSubsystem::MakePairSortKey(const FGuid& PairId)
{
	const uint64 High = (static_cast<uint64>(PairId.A) << 32)
		| static_cast<uint64>(PairId.B);
	const uint64 Low = (static_cast<uint64>(PairId.C) << 32)
		| static_cast<uint64>(PairId.D);
	// Stable process-independent tie-break key. PairStates itself remains keyed by the full FGuid.
	return High ^ (Low + 0x9e3779b97f4a7c15ull + (High << 6) + (High >> 2));
}

FWPMetricSettings UWPRuntimeSubsystem::MakeMetricSettings(
	const AWormholePortalActor& Portal) const
{
	FWPMetricSettings Metric;
	Metric.PortalRadiusCm = Portal.GetPortalRadius();
	Metric.ThroatHalfLengthCm = Portal.GetThroatHalfLength();
	Metric.TransitionLengthCm = Portal.GetTransitionLength();
	Metric.MouthRadiusCm = Portal.GetMouthRadius();
	FWPLUTEndpointSnapshot LUTSnapshot;
	const bool bHasLUTSnapshot = LUTEndpointManager
		&& LUTEndpointManager->GetEndpointSnapshot(&Portal, LUTSnapshot);
	Metric.MetricOuterRadiusCm = bHasLUTSnapshot
		&& FMath::IsFinite(LUTSnapshot.MetricOuterRadiusCm)
		&& LUTSnapshot.MetricOuterRadiusCm >= Metric.PortalRadiusCm
		? LUTSnapshot.MetricOuterRadiusCm
		: Metric.PortalRadiusCm;
	Metric.OuterRadiusCm = Portal.GetTransitionRadius();
	// Ownership resources depend on the dimensionless metric shape, not its absolute
	// scale. Quantizing both ratios keeps a uniform rho/a/T animation stable across
	// harmless floating-point division noise while still detecting authored ratio changes.
	constexpr float MetricRatioIdentityPrecision = 100000.0f;
	const int32 QuantizedThroatRatio = FMath::RoundToInt(
		(Metric.ThroatHalfLengthCm / Metric.PortalRadiusCm) * MetricRatioIdentityPrecision);
	const int32 QuantizedTransitionRatio = FMath::RoundToInt(
		(Metric.TransitionLengthCm / Metric.PortalRadiusCm) * MetricRatioIdentityPrecision);
	Metric.ResourceIdentityRevision = HashCombineFast(
		GetTypeHash(QuantizedThroatRatio), GetTypeHash(QuantizedTransitionRatio));
	if (Metric.ResourceIdentityRevision == 0)
	{
		Metric.ResourceIdentityRevision = 1;
	}
	Metric.Revision = HashCombineFast(HashCombineFast(
		HashCombineFast(GetTypeHash(Metric.PortalRadiusCm), GetTypeHash(Metric.ThroatHalfLengthCm)),
		GetTypeHash(Metric.TransitionLengthCm)), GetTypeHash(Metric.MetricOuterRadiusCm));
	if (Metric.Revision == 0)
	{
		Metric.Revision = 1;
	}
	return Metric;
}
