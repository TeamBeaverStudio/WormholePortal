// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/WPCaptureManager.h"

#include "Rendering/IWPRenderer.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/SceneComponent.h"
#include "CoreGlobals.h"
#include "Engine/Scene.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderCommandFence.h"
#include "SceneRenderBuilderInterface.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "WPSettings.h"
#include "Transit/WPTransitTypes.h"
#include "WormholePortalStats.h"

#include "WPTransform.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	constexpr int32 WPMinimumCaptureResolution = 8;
	constexpr uint32 WPCaptureLayoutVersion = 2;
	constexpr uint32 WPCaptureMipCount = 1;
	constexpr uint32 WPCaptureFaceCount = 6;
	constexpr uint32 WPBytesPerPixelRGBA16F = 8;
	constexpr uint8 WPStaggeredEndpointAMask = 1u << 0;
	constexpr uint8 WPStaggeredEndpointBMask = 1u << 1;
	constexpr uint8 WPStaggeredCompleteMask =
		WPStaggeredEndpointAMask | WPStaggeredEndpointBMask;
	constexpr float WPSourceSwitchHysteresisCm = 100.0f;
	constexpr double WPCaptureProxySafetyShellCm = 1.0;
	constexpr float WPMinimumCaptureMaxViewDistanceCm = 1'000.0f;
	constexpr float WPMaximumCaptureMaxViewDistanceCm = 10'000'000.0f;
	constexpr float WPMinimumCaptureLODDistanceFactor = 1.0f;
	constexpr float WPMaximumCaptureLODDistanceFactor = 10.0f;

	TAutoConsoleVariable<int32> CVarWPCubeAADirectPublish(
		TEXT("wp.CubeAADirectPublish"),
		1,
		TEXT("Cube AA output path. 1 alternates two endpoint UAV cubes, writes the filtered result ")
		TEXT("directly to the non-captured physical cube, then retargets a stable published texture ")
		TEXT("reference and eliminates copy-back. 0 is a validation fallback that filters to a ")
		TEXT("transient cube and copies back in-place. A live change recreates endpoint outputs."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureMaxViewDistanceCm(
		TEXT("wp.CaptureMaxViewDistanceCm"),
		-1.0f,
		TEXT("Maximum primitive render distance in centimeters for managed cube captures. ")
		TEXT("Positive values are clamped to 1000..10000000 cm; zero or negative disables ")
		TEXT("the override. The code-safe default is unlimited; the validated single-pair ")
		TEXT("project preset is 10000 cm (100 m). ")
		TEXT("A live change is applied before the next capture without resource recreation."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCaptureLODDistanceFactor(
		TEXT("wp.CaptureLODDistanceFactor"),
		1.0f,
		TEXT("Distance multiplier used for mesh LOD selection in managed cube captures. ")
		TEXT("Values are clamped to 1..10; values above 1 select lower LODs sooner. ")
		TEXT("The code-safe default is 1.0; the validated single-pair project preset is 2.0. ")
		TEXT("A live change is applied before ")
		TEXT("the next capture without resource recreation."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarWPCaptureFiniteFarPlane(
		TEXT("wp.CaptureFiniteFarPlane"),
		0,
		TEXT("Projection far-plane diagnostic for managed cube captures. ")
		TEXT("0 keeps the infinite projection while CaptureMaxViewDistanceCm still culls ")
		TEXT("distant primitives. 1 makes the projection finite at that distance. ")
		TEXT("Keep 0 for the production single-pair preset because a finite projection can ")
		TEXT("clip distant translucent sky, horizon, and cloud content."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarWPCubeLumenParityMode(
		TEXT("wp.CubeLumenParityMode"),
		0,
		TEXT("Cube-capture Lumen parity diagnostic. 0 keeps the established capture contract: "
			"Dynamic GI=None, Reflection=None. 1 enables Lumen Dynamic GI while keeping "
			"Reflection=None. 2 enables Lumen Dynamic GI and Lumen Reflection. All modes "
			"disable Lumen screen traces to avoid screen-space/view-dependent lighting during "
			"cubemap-to-SceneColor comparison. Surface Cache resolution is controlled separately "
			"by wp.CubeLumenSurfaceCacheResolution. "
			"A live change is applied safely immediately before every managed cube capture."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCubeLumenSurfaceCacheResolution(
		TEXT("wp.CubeLumenSurfaceCacheResolution"),
		0.5f,
		TEXT("Lumen surface-cache resolution used by managed cube captures when Lumen parity is enabled. ")
			TEXT("Values are clamped to UE's supported 0.5..1.0 range. 0.5 matches the engine ")
			TEXT("SceneCapture default; 1.0 is the full-resolution diagnostic reference."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCubeLumenFinalGatherQuality(
		TEXT("wp.CubeLumenFinalGatherQuality"),
		0.25f,
		TEXT("Capture-only Lumen Final Gather quality used by managed cube captures. ")
			TEXT("Values are clamped to 0.25..2.0. 0.25 reduces the screen-probe tracing ")
			TEXT("octahedron from 8x8 to 4x4 while preserving the 768+ final cube resolution. ")
			TEXT("The value is ignored while wp.CubeLumenParityMode=0."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWPCubeLumenSceneLightingQuality(
		TEXT("wp.CubeLumenSceneLightingQuality"),
		1.0f,
		TEXT("Capture-only Lumen Scene Lighting quality used by managed cube captures. ")
			TEXT("Values are clamped to the renderer's effective 0.5..2.0 range. ")
			TEXT("Keep 1.0 unless the Final Gather-only candidate cannot meet the frame budget. ")
			TEXT("The value is ignored while wp.CubeLumenParityMode=0."),
		ECVF_Default);

	struct FWPCubeLumenParityState
	{
		int32 RequestedMode = 0;
		int32 AppliedMode = 0;
		EDynamicGlobalIlluminationMethod::Type DynamicGlobalIlluminationMethod =
			EDynamicGlobalIlluminationMethod::None;
		EReflectionMethod::Type ReflectionMethod = EReflectionMethod::None;
		bool bLumenFinalGatherScreenTraces = false;
		bool bLumenReflectionsScreenTraces = false;
		float RequestedLumenSurfaceCacheResolution = 0.5f;
		float LumenSurfaceCacheResolution = 0.5f;
		float RequestedLumenFinalGatherQuality = 0.25f;
		float LumenFinalGatherQuality = 0.25f;
		float RequestedLumenSceneLightingQuality = 1.0f;
		float LumenSceneLightingQuality = 1.0f;
		double ApplyCpuUs = 0.0;
	};

	FWPCubeLumenParityState ApplyWPCubeLumenParity(
		USceneCaptureComponentCube& CaptureComponent)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WP_CubeLumenParityApply);
#if !UE_BUILD_SHIPPING
		const double StartSeconds = FPlatformTime::Seconds();
#endif

		FWPCubeLumenParityState State;
		State.RequestedMode = CVarWPCubeLumenParityMode.GetValueOnGameThread();
		State.AppliedMode = FMath::Clamp(State.RequestedMode, 0, 2);
		State.DynamicGlobalIlluminationMethod = State.AppliedMode >= 1
			? EDynamicGlobalIlluminationMethod::Lumen
			: EDynamicGlobalIlluminationMethod::None;
		State.ReflectionMethod = State.AppliedMode >= 2
			? EReflectionMethod::Lumen
			: EReflectionMethod::None;
		State.RequestedLumenSurfaceCacheResolution =
			CVarWPCubeLumenSurfaceCacheResolution.GetValueOnGameThread();
		State.LumenSurfaceCacheResolution = FMath::Clamp(
			State.RequestedLumenSurfaceCacheResolution, 0.5f, 1.0f);
		State.RequestedLumenFinalGatherQuality =
			CVarWPCubeLumenFinalGatherQuality.GetValueOnGameThread();
		State.LumenFinalGatherQuality = FMath::Clamp(
			State.RequestedLumenFinalGatherQuality, 0.25f, 2.0f);
		State.RequestedLumenSceneLightingQuality =
			CVarWPCubeLumenSceneLightingQuality.GetValueOnGameThread();
		State.LumenSceneLightingQuality = FMath::Clamp(
			State.RequestedLumenSceneLightingQuality, 0.5f, 2.0f);
		const bool bLumenEnabled = State.AppliedMode >= 1;

		FPostProcessSettings& PostProcessSettings = CaptureComponent.PostProcessSettings;
		CaptureComponent.PostProcessBlendWeight = 1.0f;
		PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = 1;
		PostProcessSettings.DynamicGlobalIlluminationMethod = State.DynamicGlobalIlluminationMethod;
		PostProcessSettings.bOverride_ReflectionMethod = 1;
		PostProcessSettings.ReflectionMethod = State.ReflectionMethod;
		PostProcessSettings.bOverride_LumenFinalGatherScreenTraces = 1;
		PostProcessSettings.LumenFinalGatherScreenTraces = 0;
		PostProcessSettings.bOverride_LumenReflectionsScreenTraces = 1;
		PostProcessSettings.LumenReflectionsScreenTraces = 0;
		PostProcessSettings.bOverride_LumenSurfaceCacheResolution = 1;
		PostProcessSettings.LumenSurfaceCacheResolution =
			State.LumenSurfaceCacheResolution;
		PostProcessSettings.bOverride_LumenFinalGatherQuality = bLumenEnabled ? 1 : 0;
		PostProcessSettings.LumenFinalGatherQuality =
			State.LumenFinalGatherQuality;
		PostProcessSettings.bOverride_LumenSceneLightingQuality = bLumenEnabled ? 1 : 0;
		PostProcessSettings.LumenSceneLightingQuality =
			State.LumenSceneLightingQuality;

#if !UE_BUILD_SHIPPING
		State.bLumenFinalGatherScreenTraces =
			PostProcessSettings.LumenFinalGatherScreenTraces != 0;
		State.bLumenReflectionsScreenTraces =
			PostProcessSettings.LumenReflectionsScreenTraces != 0;
		State.LumenSurfaceCacheResolution =
			PostProcessSettings.LumenSurfaceCacheResolution;
		State.LumenFinalGatherQuality =
			PostProcessSettings.LumenFinalGatherQuality;
		State.LumenSceneLightingQuality =
			PostProcessSettings.LumenSceneLightingQuality;
		State.ApplyCpuUs = (FPlatformTime::Seconds() - StartSeconds) * 1'000'000.0;
#endif
		return State;
	}

	/**
	 * Project Settings의 Bundled Show Flags 범주에 노출된 모든 ShowFlag를
	 * manager-owned cube SceneCapture에 적용한 결과입니다. 같은 설정 범주에 표시되더라도
	 * Lumen Reflections와 Screen Space AO는 각각 독립적으로 제어되고, 나머지 Bundle 옵션은
	 * 설명에 명시된 관련 패스를 함께 제어합니다.
	 * 모든 값은 CaptureComponent.ShowFlags에만 기록되므로 Player Game View의 FEngineShowFlags는
	 * 변경하지 않습니다. 상위 Flag가 꺼져 하위 Flag가 실질적으로 실행될 수 없는 조합도 원래
	 * 요청값을 그대로 보존하고, Effective 값으로 실제 renderer gate를 별도로 기록합니다.
	 */
	struct FWPCaptureShowFlagState
	{
		bool bLumenReflections = true;
		bool bScreenSpaceAO = true;
		bool bCloud = true;
		bool bDynamicShadows = true;
		bool bSkyLighting = true;
		bool bAtmosphere = true;
		bool bDeferredLighting = true;
		bool bLighting = true;
		bool bFog = true;
		bool bVolumetricFog = true;
		bool bEffectiveScreenSpaceAO = true;
		bool bEffectiveCloud = true;
		bool bEffectiveSkyLighting = true;
		bool bEffectiveDeferredLighting = true;
		bool bEffectiveVolumetricFog = true;
		bool bSettingsChanged = false;
		double ApplyCpuUs = 0.0;
	};

	FWPCaptureShowFlagState ApplyWPCaptureShowFlags(
		USceneCaptureComponentCube& CaptureComponent)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WP_CaptureShowFlagsApply);
#if !UE_BUILD_SHIPPING
		const double StartSeconds = FPlatformTime::Seconds();
#endif

		FWPCaptureShowFlagState State;
		if (const UWPSettings* Settings = GetDefault<UWPSettings>())
		{
			State.bLumenReflections = Settings->bSceneCaptureLumenReflections;
			State.bScreenSpaceAO = Settings->bSceneCaptureScreenSpaceAO;
			State.bCloud = Settings->bSceneCaptureCloud;
			State.bDynamicShadows = Settings->bSceneCaptureDynamicShadows;
			State.bSkyLighting = Settings->bSceneCaptureSkyLighting;
			State.bAtmosphere = Settings->bSceneCaptureAtmosphere;
			State.bDeferredLighting = Settings->bSceneCaptureDeferredLighting;
			State.bLighting = Settings->bSceneCaptureLighting;
			State.bFog = Settings->bSceneCaptureFog;
			State.bVolumetricFog = Settings->bSceneCaptureVolumetricFog;
		}

#if !UE_BUILD_SHIPPING
		State.bSettingsChanged =
			CaptureComponent.ShowFlags.LumenReflections != State.bLumenReflections
			|| CaptureComponent.ShowFlags.ScreenSpaceAO != State.bScreenSpaceAO
			|| CaptureComponent.ShowFlags.Cloud != State.bCloud
			|| CaptureComponent.ShowFlags.DynamicShadows != State.bDynamicShadows
			|| CaptureComponent.ShowFlags.SkyLighting != State.bSkyLighting
			|| CaptureComponent.ShowFlags.Atmosphere != State.bAtmosphere
			|| CaptureComponent.ShowFlags.DeferredLighting != State.bDeferredLighting
			|| CaptureComponent.ShowFlags.Lighting != State.bLighting
			|| CaptureComponent.ShowFlags.Fog != State.bFog
			|| CaptureComponent.ShowFlags.VolumetricFog != State.bVolumetricFog;
#endif

		CaptureComponent.ShowFlags.SetLumenReflections(State.bLumenReflections);
		CaptureComponent.ShowFlags.SetScreenSpaceAO(State.bScreenSpaceAO);
		CaptureComponent.ShowFlags.SetCloud(State.bCloud);
		CaptureComponent.ShowFlags.SetDynamicShadows(State.bDynamicShadows);
		CaptureComponent.ShowFlags.SetSkyLighting(State.bSkyLighting);
		CaptureComponent.ShowFlags.SetAtmosphere(State.bAtmosphere);
		CaptureComponent.ShowFlags.SetDeferredLighting(State.bDeferredLighting);
		CaptureComponent.ShowFlags.SetLighting(State.bLighting);
		CaptureComponent.ShowFlags.SetFog(State.bFog);
		CaptureComponent.ShowFlags.SetVolumetricFog(State.bVolumetricFog);

#if !UE_BUILD_SHIPPING
		State.bEffectiveScreenSpaceAO = State.bLighting && State.bScreenSpaceAO;
		State.bEffectiveCloud = State.bAtmosphere && State.bCloud;
		State.bEffectiveSkyLighting = State.bLighting && State.bSkyLighting;
		State.bEffectiveDeferredLighting = State.bLighting && State.bDeferredLighting;
		State.bEffectiveVolumetricFog = State.bFog && State.bVolumetricFog;

		State.ApplyCpuUs = (FPlatformTime::Seconds() - StartSeconds) * 1'000'000.0;
#endif
		return State;
	}

	float ResolveWPCaptureMaxViewDistanceCm(const float RequestedDistanceCm)
	{
		if (!FMath::IsFinite(RequestedDistanceCm) || RequestedDistanceCm <= 0.0f)
		{
			return -1.0f;
		}
		return FMath::Clamp(
			RequestedDistanceCm,
			WPMinimumCaptureMaxViewDistanceCm,
			WPMaximumCaptureMaxViewDistanceCm);
	}

	float ResolveWPCaptureLODDistanceFactor(const float RequestedFactor)
	{
		return FMath::IsFinite(RequestedFactor)
			? FMath::Clamp(
				RequestedFactor,
				WPMinimumCaptureLODDistanceFactor,
				WPMaximumCaptureLODDistanceFactor)
			: WPMinimumCaptureLODDistanceFactor;
	}

	struct FWPCaptureDistanceLODState
	{
		float RequestedMaxViewDistanceCm = -1.0f;
		float AppliedMaxViewDistanceCm = -1.0f;
		float RequestedLODDistanceFactor = 1.0f;
		float AppliedLODDistanceFactor = 1.0f;
		int32 RequestedFiniteFarPlane = 0;
		bool bAppliedFiniteFarPlane = false;
		bool bSettingsChanged = false;
		double ApplyCpuUs = 0.0;
	};

	FWPCaptureDistanceLODState ApplyWPCaptureDistanceLOD(
		USceneCaptureComponentCube& CaptureComponent)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WP_CaptureDistanceLODApply);
#if !UE_BUILD_SHIPPING
		const double StartSeconds = FPlatformTime::Seconds();
#endif

		FWPCaptureDistanceLODState State;
		State.RequestedMaxViewDistanceCm =
			CVarWPCaptureMaxViewDistanceCm.GetValueOnGameThread();
		State.AppliedMaxViewDistanceCm =
			ResolveWPCaptureMaxViewDistanceCm(State.RequestedMaxViewDistanceCm);
		State.RequestedLODDistanceFactor =
			CVarWPCaptureLODDistanceFactor.GetValueOnGameThread();
		State.AppliedLODDistanceFactor =
			ResolveWPCaptureLODDistanceFactor(State.RequestedLODDistanceFactor);
		State.RequestedFiniteFarPlane =
			CVarWPCaptureFiniteFarPlane.GetValueOnGameThread();
		State.bAppliedFiniteFarPlane =
			State.RequestedFiniteFarPlane != 0
			&& State.AppliedMaxViewDistanceCm > 0.0f;

#if !UE_BUILD_SHIPPING
		State.bSettingsChanged =
			!FMath::IsNearlyEqual(
				CaptureComponent.MaxViewDistanceOverride,
				State.AppliedMaxViewDistanceCm)
			|| !FMath::IsNearlyEqual(
				CaptureComponent.LODDistanceFactor,
				State.AppliedLODDistanceFactor)
			|| CaptureComponent.bFiniteFarPlane != State.bAppliedFiniteFarPlane;
#endif

		CaptureComponent.MaxViewDistanceOverride = State.AppliedMaxViewDistanceCm;
		CaptureComponent.LODDistanceFactor = State.AppliedLODDistanceFactor;
		CaptureComponent.bFiniteFarPlane = State.bAppliedFiniteFarPlane;
#if !UE_BUILD_SHIPPING
		State.ApplyCpuUs = (FPlatformTime::Seconds() - StartSeconds) * 1'000'000.0;
#endif
		return State;
	}

	uint32 ResolveWPCaptureResolution(const uint32 RequestedResolution)
	{
		const int32 SafeRequestedResolution = RequestedResolution
			> static_cast<uint32>(MAX_int32)
			? MAX_int32
			: static_cast<int32>(RequestedResolution);
		return static_cast<uint32>(UWPSettings::NormalizeCaptureResolution(
			SafeRequestedResolution,
			WPMinimumCaptureResolution));
	}

	bool IsWPCubeAADirectPublishEnabled()
	{
		return CVarWPCubeAADirectPublish.GetValueOnGameThread() != 0;
	}

	double GetWPEndpointColorMemoryMiB(const uint32 Resolution)
	{
		// UTextureRenderTargetCube owns the six-face cube SRV plus one persistent 2D
		// render surface used for per-face rendering/resolves (TextureRenderTargetCube.cpp).
		return static_cast<double>(
			static_cast<uint64>(Resolution) * Resolution
			* (WPCaptureFaceCount + 1ull) * WPBytesPerPixelRGBA16F)
			/ (1024.0 * 1024.0);
	}

	uint64 GetWPCubeColorBytes(const uint32 Resolution)
	{
		return static_cast<uint64>(Resolution) * Resolution
			* WPCaptureFaceCount * WPBytesPerPixelRGBA16F;
	}

	double GetWPEndpointWorkMegapixels(const uint32 Resolution)
	{
		return static_cast<double>(Resolution) * Resolution
			* WPCaptureFaceCount / 1'000'000.0;
	}

	const TCHAR* GetWPCaptureSubmissionModeName(
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

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool HasRenderTargetColorContract(
		const UTextureRenderTargetCube* Target,
		const uint32 ExpectedResolution,
		const bool bRequireUAV)
	{
		if (!IsValid(Target))
		{
			return false;
		}

		return ExpectedResolution >= static_cast<uint32>(WPMinimumCaptureResolution)
			&& Target->SizeX == ExpectedResolution
			&& Target->GetFormat() == PF_FloatRGBA
			&& Target->GetNumMips() == WPCaptureMipCount
			&& Target->bHDR
			&& Target->bForceLinearGamma
			&& !Target->bAutoGenerateMips
			&& (!bRequireUAV || Target->bSupportsUAV);
	}

	bool HasCaptureColorContract(
		const USceneCaptureComponentCube* Capture,
		const UTextureRenderTargetCube* Target,
		const uint32 ExpectedResolution)
	{
		return IsValid(Capture)
			&& HasRenderTargetColorContract(Target, ExpectedResolution, false)
			&& Capture->TextureTarget == Target
			&& Capture->CaptureSource == SCS_SceneColorHDRNoAlpha
			&& !Capture->bUseRayTracingIfEnabled;
	}
	bool GetPrimaryViewCameraLocation(const UWorld* World, FVector& OutCameraLocation)
	{
		if (!World || World->GetNetMode() == NM_DedicatedServer)
		{
			return false;
		}

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
			if (IsFiniteVector(CandidateLocation))
			{
				OutCameraLocation = CandidateLocation;
				return true;
			}
		}
		return false;
	}

	bool ResolveFirstPersonLocalView(
		const UWorld* World,
		const AActor* TransitActor,
		FVector& OutCameraLocation,
		const TCHAR*& OutReason)
	{
		OutCameraLocation = FVector::ZeroVector;
		OutReason = TEXT("Unknown");
		if (!World || World->GetNetMode() == NM_DedicatedServer || !IsValid(TransitActor))
		{
			OutReason = TEXT("InvalidWorldOrTransitActor");
			return false;
		}

		for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			const APlayerController* PlayerController = Iterator->Get();
			if (!PlayerController || !PlayerController->IsLocalController()
				|| PlayerController->GetViewTarget() != TransitActor
				|| PlayerController->GetPawn() != TransitActor)
			{
				continue;
			}

			TInlineComponentArray<UCameraComponent*> CameraComponents;
			TransitActor->GetComponents(CameraComponents);
			bool bFoundActiveCamera = false;
			for (const UCameraComponent* CameraComponent : CameraComponents)
			{
				if (!IsValid(CameraComponent) || !CameraComponent->IsActive())
				{
					continue;
				}
				bFoundActiveCamera = true;
				for (const USceneComponent* Attachment = CameraComponent; Attachment;
					Attachment = Attachment->GetAttachParent())
				{
					if (Cast<USpringArmComponent>(Attachment))
					{
						OutReason = TEXT("SpringArmCameraUnsupportedFirstPersonOnly");
						return false;
					}
				}
			}
			if (!bFoundActiveCamera)
			{
				OutReason = TEXT("NoActiveCameraComponentOnTransitActor");
				return false;
			}

			const APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager;
			if (!CameraManager || !IsFiniteVector(CameraManager->GetCameraLocation()))
			{
				OutReason = CameraManager
					? TEXT("NonFinitePlayerCameraLocation")
					: TEXT("PlayerCameraManagerUnavailable");
				return false;
			}
			OutCameraLocation = CameraManager->GetCameraLocation();
			OutReason = TEXT("FirstPersonDirectCameraComponent");
			return true;
		}

		OutReason = TEXT("NotLocalPossessedViewTarget");
		return false;
	}

	bool MapCameraAlongTransitPath(
		const FVector& SourceCameraLocation,
		const FWPTransform& Mapping,
		const FVector& EntryPointWorld,
		const EWPTransitPlane SelectedPlane,
		FVector& OutDestinationCameraLocation)
	{
		if (!IsFiniteVector(SourceCameraLocation) || !IsFiniteVector(EntryPointWorld) || !IsValidTransitPlane(SelectedPlane)
			|| Mapping.SourceRotation.ContainsNaN() || Mapping.TransportRotation.ContainsNaN())
		{
			return false;
		}
		
		const FTransform SourceCameraTransform(FQuat::Identity, SourceCameraLocation, FVector::OneVector);
		OutDestinationCameraLocation = Mapping.MapTransform(SourceCameraTransform, EntryPointWorld, SelectedPlane).GetLocation();
		
		return IsFiniteVector(OutDestinationCameraLocation);
	}

	bool UnmapCameraAlongTransitPath(
		const FVector& DestinationCameraLocation,
		const FWPTransform& Mapping,
		const FVector& EntryPointWorld,
		const EWPTransitPlane SelectedPlane,
		FVector& OutSourceCameraLocation)
	{
		if (!IsFiniteVector(DestinationCameraLocation) || !IsFiniteVector(EntryPointWorld)
			|| !IsValidTransitPlane(SelectedPlane) || Mapping.SourceRotation.ContainsNaN()
			|| Mapping.TransportRotation.ContainsNaN())
		{
			return false;
		}
		const FVector DestinationSurface = Mapping.MapExit(EntryPointWorld, SelectedPlane);
		if (!IsFiniteVector(DestinationSurface)) return false;
		
		// MapTransform에서 적용한 출구 기준 이동을 역순으로 되돌린다.
		OutSourceCameraLocation = EntryPointWorld + Mapping.TransportRotation.Inverse().RotateVector(
			DestinationCameraLocation - DestinationSurface);
		return IsFiniteVector(OutSourceCameraLocation);
	}
}

bool FWPCaptureEndpointSnapshot::IsReadyForSubmission(const UWorld* ExpectedWorld) const
{
	const USceneCaptureComponentCube* Capture = CaptureComponent.Get();
	const UTextureRenderTargetCube* CurrentCaptureTarget = CaptureTarget.Get();
	const UTextureRenderTargetCube* PublishedTarget = RenderTarget.Get();
	const bool bManualCaptureOnly = IsValid(Capture)
		&& !Capture->bCaptureEveryFrame
		&& !Capture->bCaptureOnMovement;
	const bool bTickDisabled = IsValid(Capture)
		&& !Capture->PrimaryComponentTick.bCanEverTick
		&& !Capture->PrimaryComponentTick.bStartWithTickEnabled
		&& !Capture->IsComponentTickEnabled();
	return IsValid(ExpectedWorld) && IsValid(Capture)
		&& IsValid(CurrentCaptureTarget) && IsValid(PublishedTarget)
		&& (bCubeAADirectPublish || CurrentCaptureTarget == PublishedTarget)
		&& bCubeAADirectPublish == IsWPCubeAADirectPublishEnabled()
		&& CurrentCaptureTarget->bSupportsUAV == bCubeAADirectPublish
		&& PublishedTarget->bSupportsUAV == bCubeAADirectPublish
		&& ResourceEpoch != 0 && CubeContract.IsValid()
		&& Capture->IsRegistered() && Capture->GetWorld() == ExpectedWorld
		&& Capture->IsVisible() && Capture->TextureTarget == CurrentCaptureTarget
		&& bManualCaptureOnly && bTickDisabled
		&& HasCaptureColorContract(
			Capture, CurrentCaptureTarget,
			static_cast<uint32>(CubeContract.ExpectedExtent.X))
		&& HasRenderTargetColorContract(
			PublishedTarget,
			static_cast<uint32>(CubeContract.ExpectedExtent.X),
			bCubeAADirectPublish)
		&& static_cast<int32>(Capture->DetailMode) <= ExpectedWorld->GetDetailMode();
}

bool FWPManagedPairCaptureResult::WasSuccessful() const
{
	const uint32 GenerationDeltaA =
		CaptureGenerationAAfter - CaptureGenerationABefore;
	const uint32 GenerationDeltaB =
		CaptureGenerationBAfter - CaptureGenerationBBefore;
	if (!bPairEpochCoherent)
	{
		return false;
	}

	switch (SubmissionMode)
	{
	case EWPManagedCaptureSubmissionMode::AtomicPair:
		return bSubmittedA && bSubmittedB
			&& GenerationDeltaA == 1u && GenerationDeltaB == 1u
			&& bPairCycleCompleted && StaggeredCompletionMaskAfter == 0;
	case EWPManagedCaptureSubmissionMode::EndpointA:
	{
		const uint8 CombinedMask =
			StaggeredCompletionMaskBefore | WPStaggeredEndpointAMask;
		const bool bExpectedCycleCompletion =
			CombinedMask == WPStaggeredCompleteMask;
		const uint8 ExpectedMaskAfter =
			bExpectedCycleCompletion ? 0 : CombinedMask;
		return bSubmittedA && !bSubmittedB
			&& GenerationDeltaA == 1u && GenerationDeltaB == 0u
			&& (StaggeredCompletionMaskBefore & WPStaggeredEndpointAMask) == 0
			&& StaggeredCompletionMaskAfter == ExpectedMaskAfter
			&& bPairCycleCompleted == bExpectedCycleCompletion;
	}
	case EWPManagedCaptureSubmissionMode::EndpointB:
	{
		const uint8 CombinedMask =
			StaggeredCompletionMaskBefore | WPStaggeredEndpointBMask;
		const bool bExpectedCycleCompletion =
			CombinedMask == WPStaggeredCompleteMask;
		const uint8 ExpectedMaskAfter =
			bExpectedCycleCompletion ? 0 : CombinedMask;
		return !bSubmittedA && bSubmittedB
			&& GenerationDeltaA == 0u && GenerationDeltaB == 1u
			&& (StaggeredCompletionMaskBefore & WPStaggeredEndpointBMask) == 0
			&& StaggeredCompletionMaskAfter == ExpectedMaskAfter
			&& bPairCycleCompleted == bExpectedCycleCompletion;
	}
	default:
		return false;
	}
}

int32 FWPManagedPairCaptureResult::GetSubmittedEndpointCount() const
{
	return (bSubmittedA ? 1 : 0) + (bSubmittedB ? 1 : 0);
}

int32 FWPManagedPairCaptureResult::GetSubmittedFaceCount() const
{
	int32 Count = 0;
	const uint8 CombinedMasks[2] = { SubmittedFaceMaskA, SubmittedFaceMaskB };
	for (const uint8 Mask : CombinedMasks)
	{
		for (uint32 FaceIndex = 0; FaceIndex < WPCaptureFaceCount; ++FaceIndex)
		{
			Count += (Mask & static_cast<uint8>(1u << FaceIndex)) != 0 ? 1 : 0;
		}
	}
	return Count;
}

UWorld* UWPCaptureManager::GetWorld() const
{
	return ManagedWorld.Get();
}

void UWPCaptureManager::Initialize(UWorld* InWorld)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	ManagedWorld = InWorld;
	bInitialized = IsValid(InWorld)
		&& InWorld->IsGameWorld()
		&& InWorld->GetNetMode() != NM_DedicatedServer
		&& FApp::CanEverRender();
	bShuttingDown = false;
	if (bInitialized)
	{
#if !UE_BUILD_SHIPPING
		const IConsoleVariable* CubeSinglePassCVar =
			IConsoleManager::Get().FindConsoleVariable(
				TEXT("r.SceneCapture.CubeSinglePass"));
		const int32 CubeSinglePassValue =
			CubeSinglePassCVar ? CubeSinglePassCVar->GetInt() : -1;
		const bool bDirectPublish = IsWPCubeAADirectPublishEnabled();
		const UWPSettings* CaptureSettings = GetDefault<UWPSettings>();
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager] Initialized. World=%s WorldType=%d NetMode=%d Initialized=1 ResolutionSource=ProjectSettingsDynamicPolicy SceneCaptureCubeSinglePass=%d CaptureSource=SceneColorHDRNoAlpha EnginePostProcessAA=NoneByCaptureContract CustomCubeAA=%s CopyBackEliminated=%d DistanceFieldAOPreserved=1 CaptureShowFlagsScope=ManagedCubeSceneCaptureOnly PlayerGameViewChanged=0 LumenReflections=%d ScreenSpaceAO=%d CloudBundle=%d DynamicShadowsBundle=%d SkyLightingBundle=%d AtmosphereBundle=%d DeferredLightingBundle=%d LightingMaster=%d FogMaster=%d VolumetricFog=%d PersistRenderingStatePreserved=1 EndpointCount=%d EstimatedResidentColorMemoryMiB=%.2f CpuMs=%.3f"),
			*GetNameSafe(InWorld), static_cast<int32>(InWorld->WorldType),
			static_cast<int32>(InWorld->GetNetMode()),
			CubeSinglePassValue,
			bDirectPublish ? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			bDirectPublish ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureLumenReflections ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureScreenSpaceAO ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureCloud ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureDynamicShadows ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureSkyLighting ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureAtmosphere ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureDeferredLighting ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureLighting ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureFog ? 1 : 0,
			CaptureSettings && CaptureSettings->bSceneCaptureVolumetricFog ? 1 : 0,
			EndpointRecords.Num(),
			GetEstimatedResidentColorMemoryMiB(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}
	else
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager] Initialization disabled by render-capability gate. World=%s WorldType=%d IsGameWorld=%d NetMode=%d CanEverRender=%d Initialized=0 EndpointCount=0 DedicatedServerRenderAllocations=0 CpuMs=%.3f"),
			*GetNameSafe(InWorld), InWorld ? static_cast<int32>(InWorld->WorldType) : -1,
			InWorld && InWorld->IsGameWorld() ? 1 : 0,
			InWorld ? static_cast<int32>(InWorld->GetNetMode()) : -1,
			FApp::CanEverRender() ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}
}

uint64 UWPCaptureManager::AllocateResourceEpoch()
{
	++NextResourceEpoch;
	if (NextResourceEpoch == 0)
	{
		++NextResourceEpoch;
	}
	return NextResourceEpoch;
}

uint32 UWPCaptureManager::AllocateResourceGeneration()
{
	++NextResourceGeneration;
	if (NextResourceGeneration == 0)
	{
		++NextResourceGeneration;
	}
	return NextResourceGeneration;
}

UTextureRenderTargetCube* UWPCaptureManager::CreateEndpointCubeTarget(
	AWormholePortalActor* Portal,
	const TCHAR* Role,
	const uint32 CaptureResolution,
	const bool bSupportsUAV)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const TCHAR* EffectiveRole = Role ? Role : TEXT("Unknown");
	const FName TargetBaseName(*FString::Printf(
		TEXT("WPCube_%s_%s"), EffectiveRole, *GetNameSafe(Portal)));
	const FName TargetName = MakeUniqueObjectName(
		this, UTextureRenderTargetCube::StaticClass(), TargetBaseName);
	UTextureRenderTargetCube* Target = NewObject<UTextureRenderTargetCube>(
		this, TargetName, RF_Transient);
	if (!Target)
	{
		++AllocationFailureCount;
		WP_LOG(this, Error,
			TEXT("[CaptureManager][CubeTarget] Endpoint cube allocation failed. World=%s Portal=%s Role=%s Resolution=%u SupportsUAV=%d AllocationFailures=%llu CpuMs=%.4f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			EffectiveRole, CaptureResolution, bSupportsUAV ? 1 : 0,
			static_cast<unsigned long long>(AllocationFailureCount),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return nullptr;
	}

	Target->bHDR = true;
	Target->bForceLinearGamma = true;
	Target->bAutoGenerateMips = false;
	Target->bSupportsUAV = bSupportsUAV;
	Target->ClearColor = FLinearColor::Black;
	Target->Init(CaptureResolution, PF_FloatRGBA);
	// Runtime resolution changes must never wait for the Render Thread. The owning
	// pending-allocation state places a fence after this command and polls it later.
	Target->UpdateResource();
	if (!HasRenderTargetColorContract(
		Target, CaptureResolution, bSupportsUAV)
		|| Target->bSupportsUAV != bSupportsUAV)
	{
		++AllocationFailureCount;
		Target->ReleaseResource();
		WP_LOG(this, Error,
			TEXT("[CaptureManager][CubeTarget] Endpoint cube contract failed. World=%s Portal=%s Role=%s Target=%s Resolution=%u Format=%d Mips=%d HDR=%d Linear=%d AutoMips=%d SupportsUAV=%d RequiredUAV=%d AllocationFailures=%llu CpuMs=%.4f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			EffectiveRole, *GetNameSafe(Target), CaptureResolution,
			static_cast<int32>(Target->GetFormat()),
			Target->GetNumMips(), Target->bHDR ? 1 : 0,
			Target->bForceLinearGamma ? 1 : 0,
			Target->bAutoGenerateMips ? 1 : 0,
			Target->bSupportsUAV ? 1 : 0, bSupportsUAV ? 1 : 0,
			static_cast<unsigned long long>(AllocationFailureCount),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return nullptr;
	}

	ManagedRenderTargets.Add(Target);
	++CubeTargetAllocationCount;
#if !UE_BUILD_SHIPPING
	const double AllocationCpuMs =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TotalCubeTargetAllocationCpuMs += AllocationCpuMs;
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][CubeTarget] Endpoint cube allocation enqueued. World=%s Portal=%s Role=%s Target=%s Resolution=%u Format=PF_FloatRGBA HDR=1 Linear=1 Mips=1 SupportsUAV=%d Ownership=Endpoint PingPongEligible=%d CrossEndpointReuse=0 CommandOrdering=AsyncAllocateFenceThenCaptureThenAA AsyncCompute=0 GameThreadWait=0 RHIAllocationEnqueued=1 CubeMemoryMiB=%.2f TargetAllocations=%llu TargetReleases=%llu StrongRenderTargets=%d AllocationCpuMs=%.4f TotalTargetAllocationCpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
		EffectiveRole, *GetNameSafe(Target), CaptureResolution,
		bSupportsUAV ? 1 : 0, bSupportsUAV ? 1 : 0,
		GetWPEndpointColorMemoryMiB(CaptureResolution),
		static_cast<unsigned long long>(CubeTargetAllocationCount),
		static_cast<unsigned long long>(CubeTargetReleaseCount),
		ManagedRenderTargets.Num(), AllocationCpuMs,
		TotalCubeTargetAllocationCpuMs);
#endif
	return Target;
}

bool UWPCaptureManager::EnsureEndpointResources(
	AWormholePortalActor* Portal,
	const uint32 RequestedResolution)

{
	return EnsureEndpointResourcesInternal(
		Portal, RequestedResolution, false);
}

bool UWPCaptureManager::EnsureEndpointTransitionResources(
	AWormholePortalActor* Portal,
	const uint32 RequestedResolution)
{
	return EnsureEndpointResourcesInternal(
		Portal, RequestedResolution, true);
}

bool UWPCaptureManager::EnsureEndpointResourcesInternal(
	AWormholePortalActor* Portal,
	const uint32 RequestedResolution,
	const bool bPreserveExistingForResolutionTransition)
{
	// 로그 전용: endpoint allocation/repair CPU 시간을 측정하고 누적합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	UWorld* World = ManagedWorld.Get();
	const uint32 DesiredCaptureResolution =
		ResolveWPCaptureResolution(RequestedResolution);
	const bool bDesiredDirectPublish = IsWPCubeAADirectPublishEnabled();
	const bool bRenderCapable = IsValid(World) && World->IsGameWorld()
		&& World->GetNetMode() != NM_DedicatedServer && FApp::CanEverRender();
	if (!bInitialized || bShuttingDown || !IsInGameThread() || !bRenderCapable
		|| !IsValid(Portal) || Portal->GetWorld() != World)
	{
		++AllocationFailureCount;
		WP_LOG(this, Error,
			TEXT("[CaptureManager] Allocation rejected. World=%s Portal=%s Initialized=%d ShuttingDown=%d GameThread=%d RenderCapable=%d IsGameWorld=%d NetMode=%d CanEverRender=%d PortalWorld=%s ExpectedWorld=%s DedicatedServerRenderAllocations=0 AllocationFailures=%llu CpuMs=%.3f"),
			*GetNameSafe(World), *GetNameSafe(Portal), bInitialized ? 1 : 0,
			bShuttingDown ? 1 : 0, IsInGameThread() ? 1 : 0, bRenderCapable ? 1 : 0,
			World && World->IsGameWorld() ? 1 : 0,
			World ? static_cast<int32>(World->GetNetMode()) : -1,
			FApp::CanEverRender() ? 1 : 0,
			*GetNameSafe(Portal ? Portal->GetWorld() : nullptr), *GetNameSafe(World),
			static_cast<unsigned long long>(AllocationFailureCount),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	if (FEndpointRecord* Existing = EndpointRecords.Find(PortalKey))
	{
		USceneCaptureComponentCube* ExistingCapture = Existing->CaptureComponent.Get();
		UTextureRenderTargetCube* ExistingPublishedTarget = Existing->RenderTarget.Get();
		UTextureRenderTargetCube* ExistingCaptureTarget = Existing->CaptureTarget.Get();
		UTextureRenderTargetCube* ExistingAlternateTarget =
			Existing->AlternateRenderTarget.Get();
		const bool bRegistered = IsValid(ExistingCapture) && ExistingCapture->IsRegistered();
		const bool bWorldMatches = bRegistered && ExistingCapture->GetWorld() == World;
		const bool bTargetMatches = IsValid(ExistingCapture)
			&& ExistingCapture->TextureTarget == ExistingCaptureTarget;
		const bool bTickDisabled = IsValid(ExistingCapture)
			&& !ExistingCapture->PrimaryComponentTick.bCanEverTick
			&& !ExistingCapture->PrimaryComponentTick.bStartWithTickEnabled
			&& !ExistingCapture->IsComponentTickEnabled();
		const bool bManualCaptureOnly = IsValid(ExistingCapture)
			&& !ExistingCapture->bCaptureEveryFrame
			&& !ExistingCapture->bCaptureOnMovement;
		const bool bPublishedTargetContract =
			HasRenderTargetColorContract(
				ExistingPublishedTarget, Existing->CaptureResolution,
				bDesiredDirectPublish)
			&& IsValid(ExistingPublishedTarget)
			&& ExistingPublishedTarget->bSupportsUAV == bDesiredDirectPublish;
		const bool bCaptureTargetContract =
			HasRenderTargetColorContract(
				ExistingCaptureTarget, Existing->CaptureResolution,
				bDesiredDirectPublish)
			&& IsValid(ExistingCaptureTarget)
			&& ExistingCaptureTarget->bSupportsUAV == bDesiredDirectPublish;
		const bool bAlternateTargetContract = bDesiredDirectPublish
			? HasRenderTargetColorContract(
				ExistingAlternateTarget, Existing->CaptureResolution, true)
				&& ExistingAlternateTarget != ExistingCaptureTarget
				&& ExistingAlternateTarget->bSupportsUAV
			: !IsValid(ExistingAlternateTarget);
		const bool bTargetTopologyValid = bDesiredDirectPublish
			? ExistingCaptureTarget != ExistingAlternateTarget
				&& (ExistingPublishedTarget == ExistingCaptureTarget
					|| ExistingPublishedTarget == ExistingAlternateTarget)
			: ExistingCaptureTarget == ExistingPublishedTarget
				&& !IsValid(ExistingAlternateTarget);
		const bool bComplete = IsValid(ExistingCapture)
			&& IsValid(ExistingCaptureTarget)
			&& IsValid(ExistingPublishedTarget)
			&& Existing->ResourceEpoch != 0
			&& Existing->ResourceGeneration != 0
			&& Existing->CaptureResolution == DesiredCaptureResolution
			&& Existing->bCubeAADirectPublish == bDesiredDirectPublish
			&& bPublishedTargetContract
			&& bCaptureTargetContract
			&& bAlternateTargetContract
			&& bTargetTopologyValid
			&& bRegistered
			&& bWorldMatches
			&& bTargetMatches
			&& bManualCaptureOnly
			&& bTickDisabled
			&& HasCaptureColorContract(
				ExistingCapture, ExistingCaptureTarget,
				Existing->CaptureResolution);
		if (bComplete)
		{
			return true;
		}
		// 해상도만 달라진 정상 record는 seamless 전환 동안 파괴하지 않습니다.
		// 새 세대가 commit될 때 retired publication record로 옮겨 Renderer가 계속
		// sample하게 하고, Capture submission만 새 active record를 사용합니다.
		const bool bHealthyExistingForResolutionTransition =
			IsValid(ExistingCapture)
			&& IsValid(ExistingCaptureTarget)
			&& IsValid(ExistingPublishedTarget)
			&& Existing->ResourceEpoch != 0
			&& Existing->ResourceGeneration != 0
			&& Existing->CaptureResolution != DesiredCaptureResolution
			&& Existing->bCubeAADirectPublish == bDesiredDirectPublish
			&& bPublishedTargetContract
			&& bCaptureTargetContract
			&& bAlternateTargetContract
			&& bTargetTopologyValid
			&& bRegistered
			&& bWorldMatches
			&& bTargetMatches
			&& bManualCaptureOnly
			&& bTickDisabled
			&& HasCaptureColorContract(
				ExistingCapture, ExistingCaptureTarget,
				Existing->CaptureResolution);
		if (bPreserveExistingForResolutionTransition
			&& bHealthyExistingForResolutionTransition)
		{
			if (RetiredResolutionEndpointRecords.Contains(PortalKey))
			{
				++AllocationFailureCount;
#if !UE_BUILD_SHIPPING
				WP_LOG(this, Verbose,
					TEXT("[CaptureManager][ResolutionTransition] Nested seamless allocation rejected. World=%s Portal=%s ExistingResolution=%u DesiredResolution=%u RetiredRecordAlreadyPresent=1 AllocationFailures=%llu CpuMs=%.4f"),
					*GetNameSafe(World), *GetNameSafe(Portal),
					Existing->CaptureResolution, DesiredCaptureResolution,
					static_cast<unsigned long long>(AllocationFailureCount),
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
				return false;
			}
#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose,
				TEXT("[CaptureManager][ResolutionTransition] Healthy active generation preserved while replacement allocation advances. World=%s Portal=%s OldResolution=%u DesiredResolution=%u OldResourceGeneration=%u OldCaptureGeneration=%u RendererContinuesSamplingOld=1 OldCaptureFrozen=1 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f CpuMs=%.4f"),
				*GetNameSafe(World), *GetNameSafe(Portal),
				Existing->CaptureResolution, DesiredCaptureResolution,
				Existing->ResourceGeneration, Existing->CaptureGeneration,
				GetEstimatedResidentColorMemoryMiB(),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		else
		{
		WP_LOG(this, Warning,
			TEXT("[CaptureManager] Incomplete endpoint record detected; repairing. World=%s Portal=%s ResourceEpoch=%llu ResourceGeneration=%u CaptureGeneration=%u ExistingResolution=%u DesiredResolution=%u ExistingDirectPublish=%d DesiredDirectPublish=%d CaptureValid=%d CaptureTargetValid=%d PublishedTargetValid=%d AlternateTargetValid=%d CaptureAlternateDistinct=%d PublishedOwnerInPhysicalSet=%d Registered=%d WorldMatches=%d TextureTargetMatchesCapture=%d CaptureUAV=%d PublishedUAV=%d AlternateUAV=%d CaptureContract=%d PublishedContract=%d AlternateContract=%d TopologyValid=%d ManualCaptureOnly=%d CaptureEveryFrame=%d CaptureOnMovement=%d TickDisabled=%d ResourceOwner=CaptureManager CpuMs=%.3f"),
			*GetNameSafe(World), *GetNameSafe(Portal),
			static_cast<unsigned long long>(Existing->ResourceEpoch),
			Existing->ResourceGeneration, Existing->CaptureGeneration,
			Existing->CaptureResolution, DesiredCaptureResolution,
			Existing->bCubeAADirectPublish ? 1 : 0,
			bDesiredDirectPublish ? 1 : 0,
			IsValid(ExistingCapture) ? 1 : 0,
			IsValid(ExistingCaptureTarget) ? 1 : 0,
			IsValid(ExistingPublishedTarget) ? 1 : 0,
			IsValid(ExistingAlternateTarget) ? 1 : 0,
			ExistingAlternateTarget != ExistingCaptureTarget ? 1 : 0,
			ExistingPublishedTarget == ExistingCaptureTarget
				|| ExistingPublishedTarget == ExistingAlternateTarget ? 1 : 0,
			bRegistered ? 1 : 0, bWorldMatches ? 1 : 0, bTargetMatches ? 1 : 0,
			IsValid(ExistingCaptureTarget) && ExistingCaptureTarget->bSupportsUAV ? 1 : 0,
			IsValid(ExistingPublishedTarget) && ExistingPublishedTarget->bSupportsUAV ? 1 : 0,
			IsValid(ExistingAlternateTarget) && ExistingAlternateTarget->bSupportsUAV ? 1 : 0,
			bCaptureTargetContract ? 1 : 0, bPublishedTargetContract ? 1 : 0,
			bAlternateTargetContract ? 1 : 0, bTargetTopologyValid ? 1 : 0,
			bManualCaptureOnly ? 1 : 0,
			IsValid(ExistingCapture) && ExistingCapture->bCaptureEveryFrame ? 1 : 0,
			IsValid(ExistingCapture) && ExistingCapture->bCaptureOnMovement ? 1 : 0,
			bTickDisabled ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		ReleaseRecord(*Existing, TEXT("RepairIncompleteEndpoint"));
		EndpointRecords.Remove(PortalKey);
		SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
		SET_FLOAT_STAT(
			STAT_WP_CubeRenderTargetMiB,
			static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
		}
	}

	auto ReleaseNewCubeTarget =
		[this, Portal](UTextureRenderTargetCube* Target, const TCHAR* Role,
			const TCHAR* Reason)
	{
		if (!Target)
		{
			return;
		}
#if !UE_BUILD_SHIPPING
		const double ReleaseStartSeconds = FPlatformTime::Seconds();
#endif
		Target->ReleaseResource();
		ManagedRenderTargets.RemoveSingleSwap(Target);
		++CubeTargetReleaseCount;
#if !UE_BUILD_SHIPPING
		const double ReleaseCpuMs =
			(FPlatformTime::Seconds() - ReleaseStartSeconds) * 1000.0;
		TotalCubeTargetReleaseCpuMs += ReleaseCpuMs;
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][CubeTarget] Uncommitted endpoint cube released. World=%s Portal=%s Role=%s Target=%s TargetAllocations=%llu TargetReleases=%llu TargetBalance=%lld StrongRenderTargets=%d Reason=%s CpuMs=%.4f TotalTargetReleaseCpuMs=%.4f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
			Role ? Role : TEXT("Unknown"), *GetNameSafe(Target),
			static_cast<unsigned long long>(CubeTargetAllocationCount),
			static_cast<unsigned long long>(CubeTargetReleaseCount),
			static_cast<long long>(CubeTargetAllocationCount)
				- static_cast<long long>(CubeTargetReleaseCount),
			ManagedRenderTargets.Num(), Reason ? Reason : TEXT("Unspecified"),
			ReleaseCpuMs, TotalCubeTargetReleaseCpuMs);
#endif
	};

	FPendingEndpointAllocation* Pending = PendingEndpointAllocations.Find(PortalKey);
	if (Pending && (Pending->CaptureResolution != DesiredCaptureResolution
		|| Pending->bCubeAADirectPublish != bDesiredDirectPublish
		|| Pending->bPreserveExistingForResolutionTransition
			!= bPreserveExistingForResolutionTransition))
	{
		ReleasePendingAllocation(*Pending, TEXT("PendingResolutionSuperseded"));
		PendingEndpointAllocations.Remove(PortalKey);
		Pending = nullptr;
	}
	if (!Pending)
	{
		FPendingEndpointAllocation& NewPending =
			PendingEndpointAllocations.Add(PortalKey);
		NewPending.Portal = Portal;
		NewPending.ResourceEpoch = AllocateResourceEpoch();
		NewPending.ResourceGeneration = AllocateResourceGeneration();
		NewPending.CaptureResolution = DesiredCaptureResolution;
		NewPending.bCubeAADirectPublish = bDesiredDirectPublish;
		NewPending.bPreserveExistingForResolutionTransition =
			bPreserveExistingForResolutionTransition;
		NewPending.Stage = EEndpointAllocationStage::CreateCurrent;
#if !UE_BUILD_SHIPPING
		NewPending.StartSeconds = FPlatformTime::Seconds();
#endif
		Pending = &NewPending;
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][AsyncAllocation] Endpoint allocation started. World=%s Portal=%s ResourceEpoch=%llu ResourceGeneration=%u Resolution=%u DirectPublish=%d PreserveExistingForResolutionTransition=%d Stage=CreateCurrent PersistentCubeAllocationsThisFrame=0 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f CpuMs=%.4f"),
			*GetNameSafe(World), *GetNameSafe(Portal),
			static_cast<unsigned long long>(Pending->ResourceEpoch),
			Pending->ResourceGeneration, Pending->CaptureResolution,
			Pending->bCubeAADirectPublish ? 1 : 0,
			Pending->bPreserveExistingForResolutionTransition ? 1 : 0,
			GetEstimatedResidentColorMemoryMiB(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}

	if (Pending->Stage == EEndpointAllocationStage::CreateCurrent)
	{
		UTextureRenderTargetCube* Current = CreateEndpointCubeTarget(
			Portal, TEXT("Current"), DesiredCaptureResolution, bDesiredDirectPublish);
		if (!Current)
		{
			ReleasePendingAllocation(*Pending, TEXT("CurrentAllocationFailed"));
			PendingEndpointAllocations.Remove(PortalKey);
			return false;
		}
		Pending->CurrentTarget = Current;
		Pending->AllocationFence = MakeShared<FRenderCommandFence>();
		Pending->AllocationFence->BeginFence();
		Pending->Stage = EEndpointAllocationStage::WaitCurrent;
#if !UE_BUILD_SHIPPING
		WP_LOG(this, VeryVerbose,
			TEXT("[CaptureManager][AsyncAllocation] Stage advanced. World=%s Portal=%s Resolution=%u PreviousStage=CreateCurrent CurrentStage=WaitCurrent PersistentCubeAllocationsThisFrame=1 AllocationFenceBegun=1 GameThreadWait=0 WallMs=%.3f CpuMs=%.4f"),
			*GetNameSafe(World), *GetNameSafe(Portal), DesiredCaptureResolution,
			(FPlatformTime::Seconds() - Pending->StartSeconds) * 1000.0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}
	if (Pending->Stage == EEndpointAllocationStage::WaitCurrent)
	{
		if (!Pending->AllocationFence
			|| !Pending->AllocationFence->IsFenceComplete())
		{
			return false;
		}
		Pending->AllocationFence.Reset();
		Pending->Stage = bDesiredDirectPublish
			? EEndpointAllocationStage::CreateAlternate
			: EEndpointAllocationStage::Commit;
		return false;
	}
	if (Pending->Stage == EEndpointAllocationStage::CreateAlternate)
	{
		UTextureRenderTargetCube* Alternate = CreateEndpointCubeTarget(
			Portal, TEXT("Alternate"), DesiredCaptureResolution, true);
		if (!Alternate)
		{
			ReleasePendingAllocation(*Pending, TEXT("AlternateAllocationFailed"));
			PendingEndpointAllocations.Remove(PortalKey);
			return false;
		}
		Pending->AlternateTarget = Alternate;
		Pending->AllocationFence = MakeShared<FRenderCommandFence>();
		Pending->AllocationFence->BeginFence();
		Pending->Stage = EEndpointAllocationStage::WaitAlternate;
#if !UE_BUILD_SHIPPING
		WP_LOG(this, VeryVerbose,
			TEXT("[CaptureManager][AsyncAllocation] Stage advanced. World=%s Portal=%s Resolution=%u PreviousStage=CreateAlternate CurrentStage=WaitAlternate PersistentCubeAllocationsThisFrame=1 AllocationFenceBegun=1 GameThreadWait=0 WallMs=%.3f CpuMs=%.4f"),
			*GetNameSafe(World), *GetNameSafe(Portal), DesiredCaptureResolution,
			(FPlatformTime::Seconds() - Pending->StartSeconds) * 1000.0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}
	if (Pending->Stage == EEndpointAllocationStage::WaitAlternate)
	{
		if (!Pending->AllocationFence
			|| !Pending->AllocationFence->IsFenceComplete())
		{
			return false;
		}
		Pending->AllocationFence.Reset();
		Pending->Stage = EEndpointAllocationStage::Commit;
		return false;
	}

	UTextureRenderTargetCube* CurrentTarget = Pending->CurrentTarget.Get();
	UTextureRenderTargetCube* AlternateTarget = Pending->AlternateTarget.Get();
	const uint64 ResourceEpoch = Pending->ResourceEpoch;
	const uint32 ResourceGeneration = Pending->ResourceGeneration;
	const bool bCommitPreservesExistingForResolutionTransition =
		Pending->bPreserveExistingForResolutionTransition;
#if !UE_BUILD_SHIPPING
	const double AsyncAllocationWallMs =
		(FPlatformTime::Seconds() - Pending->StartSeconds) * 1000.0;
#endif
	const FName CaptureBaseName(*FString::Printf(TEXT("WPCapture_%s"), *Portal->GetName()));
	const FName CaptureName = MakeUniqueObjectName(
		World, USceneCaptureComponentCube::StaticClass(), CaptureBaseName);
	USceneCaptureComponentCube* CaptureComponent = NewObject<USceneCaptureComponentCube>(
		World, CaptureName, RF_Transient);
	if (!CaptureComponent)
	{
		++AllocationFailureCount;
		ReleaseNewCubeTarget(
			AlternateTarget, TEXT("Alternate"), TEXT("CaptureComponentAllocationFailed"));
		ReleaseNewCubeTarget(
			CurrentTarget, TEXT("Current"), TEXT("CaptureComponentAllocationFailed"));
		PendingEndpointAllocations.Remove(PortalKey);
		WP_LOG(this, Error,
			TEXT("[CaptureManager] Capture component allocation failed. World=%s Portal=%s ResourceEpoch=%llu CurrentTargetValid=%d AlternateTargetValid=%d DirectPublish=%d AllocationFailures=%llu CpuMs=%.3f"),
			*GetNameSafe(World), *GetNameSafe(Portal),
			static_cast<unsigned long long>(ResourceEpoch),
			IsValid(CurrentTarget) ? 1 : 0, IsValid(AlternateTarget) ? 1 : 0,
			bDesiredDirectPublish ? 1 : 0,
			static_cast<unsigned long long>(AllocationFailureCount),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	ManagedCaptureComponents.Add(CaptureComponent);

	// Configure every automatic submission switch and the exact target before registration. A newly
	// registered scene capture must never enqueue an implicit frame between registration and manager record commit.
	CaptureComponent->TextureTarget = CurrentTarget;
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->bCaptureRotation = false;
	CaptureComponent->bAlwaysPersistRenderingState = true;
	CaptureComponent->bUseRayTracingIfEnabled = false;
	CaptureComponent->CaptureSource = SCS_SceneColorHDRNoAlpha;
#if !UE_BUILD_SHIPPING
	const FWPCubeLumenParityState PreRegisterLumenParity =
		ApplyWPCubeLumenParity(*CaptureComponent);
#else
	(void)ApplyWPCubeLumenParity(*CaptureComponent);
#endif
	ApplyWPCaptureShowFlags(*CaptureComponent);
#if !UE_BUILD_SHIPPING
	const FWPCaptureDistanceLODState PreRegisterDistanceLOD =
		ApplyWPCaptureDistanceLOD(*CaptureComponent);
#else
	(void)ApplyWPCaptureDistanceLOD(*CaptureComponent);
#endif
	CaptureComponent->DetailMode = DM_Low;
	CaptureComponent->PrimaryComponentTick.bCanEverTick = false;
	CaptureComponent->PrimaryComponentTick.bStartWithTickEnabled = false;
	CaptureComponent->SetComponentTickEnabled(false);
	CaptureComponent->SetVisibility(true, false);
	CaptureComponent->RegisterComponentWithWorld(World);
	// RegisterComponent can apply class defaults; enforce and verify the manager-only manual-capture contract again.
	CaptureComponent->SetComponentTickEnabled(false);
#if !UE_BUILD_SHIPPING
	const FWPCubeLumenParityState PostRegisterLumenParity =
		ApplyWPCubeLumenParity(*CaptureComponent);
#else
	(void)ApplyWPCubeLumenParity(*CaptureComponent);
#endif
	(void)ApplyWPCaptureShowFlags(*CaptureComponent);
	const FWPCaptureDistanceLODState PostRegisterDistanceLOD =
		ApplyWPCaptureDistanceLOD(*CaptureComponent);
	const bool bRegistered = CaptureComponent->IsRegistered();
	const bool bRegisteredWorldMatches = bRegistered && CaptureComponent->GetWorld() == World;
	const bool bTextureTargetMatches = CaptureComponent->TextureTarget == CurrentTarget;
	const bool bManualCaptureOnly = !CaptureComponent->bCaptureEveryFrame
		&& !CaptureComponent->bCaptureOnMovement;
	const bool bTickDisabled = !CaptureComponent->PrimaryComponentTick.bCanEverTick
		&& !CaptureComponent->PrimaryComponentTick.bStartWithTickEnabled
		&& !CaptureComponent->IsComponentTickEnabled();
	const bool bDistanceLODContractValid =
		FMath::IsNearlyEqual(
			CaptureComponent->MaxViewDistanceOverride,
			PostRegisterDistanceLOD.AppliedMaxViewDistanceCm)
		&& FMath::IsNearlyEqual(
			CaptureComponent->LODDistanceFactor,
			PostRegisterDistanceLOD.AppliedLODDistanceFactor)
		&& CaptureComponent->bFiniteFarPlane
			== PostRegisterDistanceLOD.bAppliedFiniteFarPlane;
	const bool bColorContractValid =
		HasCaptureColorContract(
			CaptureComponent, CurrentTarget, DesiredCaptureResolution)
		&& HasRenderTargetColorContract(
			CurrentTarget, DesiredCaptureResolution, bDesiredDirectPublish)
		&& CurrentTarget->bSupportsUAV == bDesiredDirectPublish
		&& (bDesiredDirectPublish
			? HasRenderTargetColorContract(
				AlternateTarget, DesiredCaptureResolution, true)
				&& AlternateTarget != CurrentTarget
				&& AlternateTarget->bSupportsUAV
			: AlternateTarget == nullptr);
	if (!bRegisteredWorldMatches || !bTextureTargetMatches
		|| !bManualCaptureOnly || !bTickDisabled || !bDistanceLODContractValid
		|| !bColorContractValid)
	{
		++AllocationFailureCount;
		const bool bCaptureEveryFrame = CaptureComponent->bCaptureEveryFrame;
		const bool bCaptureOnMovement = CaptureComponent->bCaptureOnMovement;
		const bool bTickCanEver = CaptureComponent->PrimaryComponentTick.bCanEverTick;
		const bool bTickStartEnabled = CaptureComponent->PrimaryComponentTick.bStartWithTickEnabled;
		const bool bTickEnabled = CaptureComponent->IsComponentTickEnabled();
		CaptureComponent->TextureTarget = nullptr;
		CaptureComponent->DestroyComponent();
		ManagedCaptureComponents.RemoveSingleSwap(CaptureComponent);
		ReleaseNewCubeTarget(
			AlternateTarget, TEXT("Alternate"), TEXT("EndpointPreflightFailed"));
		ReleaseNewCubeTarget(
			CurrentTarget, TEXT("Current"), TEXT("EndpointPreflightFailed"));
		PendingEndpointAllocations.Remove(PortalKey);
		WP_LOG(this, Error,
			TEXT("[CaptureManager] Endpoint record preflight failed. World=%s Portal=%s ResourceEpoch=%llu Registered=%d RegisteredWorldMatches=%d TextureTargetMatchesCurrent=%d CurrentTargetValid=%d AlternateTargetValid=%d TargetsDistinct=%d CurrentUAV=%d AlternateUAV=%d DirectPublish=%d ManualCaptureOnly=%d CaptureEveryFrame=%d CaptureOnMovement=%d TickCanEver=%d TickStartEnabled=%d TickEnabled=%d DistanceLODContractValid=%d AppliedMaxViewDistanceCm=%.2f AppliedLODDistanceFactor=%.3f AppliedFiniteFarPlane=%d ColorContractValid=%d ResourceOwner=CaptureManager AllocationFailures=%llu CpuMs=%.3f"),
			*GetNameSafe(World), *GetNameSafe(Portal),
			static_cast<unsigned long long>(ResourceEpoch),
			bRegistered ? 1 : 0, bRegisteredWorldMatches ? 1 : 0,
			bTextureTargetMatches ? 1 : 0,
			IsValid(CurrentTarget) ? 1 : 0, IsValid(AlternateTarget) ? 1 : 0,
			AlternateTarget != CurrentTarget ? 1 : 0,
			IsValid(CurrentTarget) && CurrentTarget->bSupportsUAV ? 1 : 0,
			IsValid(AlternateTarget) && AlternateTarget->bSupportsUAV ? 1 : 0,
			bDesiredDirectPublish ? 1 : 0,
			bManualCaptureOnly ? 1 : 0,
			bCaptureEveryFrame ? 1 : 0, bCaptureOnMovement ? 1 : 0,
			bTickCanEver ? 1 : 0, bTickStartEnabled ? 1 : 0,
			bTickEnabled ? 1 : 0,
			bDistanceLODContractValid ? 1 : 0,
			PostRegisterDistanceLOD.AppliedMaxViewDistanceCm,
			PostRegisterDistanceLOD.AppliedLODDistanceFactor,
			PostRegisterDistanceLOD.bAppliedFiniteFarPlane ? 1 : 0,
			bColorContractValid ? 1 : 0,
			static_cast<unsigned long long>(AllocationFailureCount),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][CubeLumenParity] Endpoint configured. Portal=%s Capture=%s PreRegisterMode=%d PostRegisterRequestedMode=%d PostRegisterAppliedMode=%d DynamicGI=%d Reflection=%d FinalGatherScreenTraces=%d ReflectionScreenTraces=%d RequestedSurfaceCacheResolution=%.2f SurfaceCacheResolution=%.2f RequestedFinalGatherQuality=%.2f FinalGatherQuality=%.2f RequestedSceneLightingQuality=%.2f SceneLightingQuality=%.2f PreRegisterApplyCpuUs=%.2f PostRegisterApplyCpuUs=%.2f HDRSceneColorContractPreserved=%d"),
		*GetNameSafe(Portal),
		*GetNameSafe(CaptureComponent),
		PreRegisterLumenParity.AppliedMode,
		PostRegisterLumenParity.RequestedMode,
		PostRegisterLumenParity.AppliedMode,
		static_cast<int32>(PostRegisterLumenParity.DynamicGlobalIlluminationMethod),
		static_cast<int32>(PostRegisterLumenParity.ReflectionMethod),
		PostRegisterLumenParity.bLumenFinalGatherScreenTraces ? 1 : 0,
		PostRegisterLumenParity.bLumenReflectionsScreenTraces ? 1 : 0,
		PostRegisterLumenParity.RequestedLumenSurfaceCacheResolution,
		PostRegisterLumenParity.LumenSurfaceCacheResolution,
		PostRegisterLumenParity.RequestedLumenFinalGatherQuality,
		PostRegisterLumenParity.LumenFinalGatherQuality,
		PostRegisterLumenParity.RequestedLumenSceneLightingQuality,
		PostRegisterLumenParity.LumenSceneLightingQuality,
		PreRegisterLumenParity.ApplyCpuUs,
		PostRegisterLumenParity.ApplyCpuUs,
		CaptureComponent->CaptureSource == SCS_SceneColorHDRNoAlpha ? 1 : 0);
#endif
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][DistanceLOD] Endpoint configured. Portal=%s Capture=%s RequestedMaxViewDistanceCm=%.2f AppliedMaxViewDistanceCm=%.2f MaxViewDistanceEnabled=%d RequestedLODDistanceFactor=%.3f AppliedLODDistanceFactor=%.3f RequestedFiniteFarPlane=%d AppliedFiniteFarPlane=%d PreRegisterApplyCpuUs=%.2f PostRegisterApplyCpuUs=%.2f ResourceRecreated=0 ContractValid=%d"),
		*GetNameSafe(Portal),
		*GetNameSafe(CaptureComponent),
		PostRegisterDistanceLOD.RequestedMaxViewDistanceCm,
		PostRegisterDistanceLOD.AppliedMaxViewDistanceCm,
		PostRegisterDistanceLOD.AppliedMaxViewDistanceCm > 0.0f ? 1 : 0,
		PostRegisterDistanceLOD.RequestedLODDistanceFactor,
		PostRegisterDistanceLOD.AppliedLODDistanceFactor,
		PostRegisterDistanceLOD.RequestedFiniteFarPlane,
		PostRegisterDistanceLOD.bAppliedFiniteFarPlane ? 1 : 0,
		PreRegisterDistanceLOD.ApplyCpuUs,
		PostRegisterDistanceLOD.ApplyCpuUs,
		bDistanceLODContractValid ? 1 : 0);
#endif
	CaptureComponent->ClearHiddenComponents();
	CaptureComponent->HideActorComponents(Portal, true);

	FEndpointRecord Record;
	Record.Portal = Portal;
	Record.CaptureComponent = CaptureComponent;
	Record.CaptureTarget = CurrentTarget;
	Record.RenderTarget = CurrentTarget;
	Record.AlternateRenderTarget = AlternateTarget;
	Record.ResourceEpoch = ResourceEpoch;
	Record.ResourceGeneration = ResourceGeneration;
	Record.CaptureGeneration = 0;
	Record.CaptureResolution = DesiredCaptureResolution;
	Record.bCubeAADirectPublish = bDesiredDirectPublish;
#if !UE_BUILD_SHIPPING
	Record.AllocationCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
#endif
	if (bCommitPreservesExistingForResolutionTransition)
	{
		FEndpointRecord ExistingRecord;
		if (!EndpointRecords.RemoveAndCopyValue(PortalKey, ExistingRecord)
			|| ExistingRecord.ResourceGeneration == 0
			|| RetiredResolutionEndpointRecords.Contains(PortalKey))
		{
			++AllocationFailureCount;
			// The replacement record already owns fully allocated managed objects. Account it as
			// an allocation before ReleaseRecord so lifetime telemetry remains balanced.
			++AllocationCount;
#if !UE_BUILD_SHIPPING
			TotalAllocationCpuMs += Record.AllocationCpuMs;
#endif
			ReleaseRecord(Record, TEXT("SeamlessCommitMissingOldGeneration"));
			PendingEndpointAllocations.Remove(PortalKey);
			WP_LOG(this, Error,
				TEXT("[CaptureManager][ResolutionTransition] Replacement commit rejected and new generation released. World=%s Portal=%s DesiredResolution=%u NewResourceGeneration=%u OldRecordRecovered=%d RetiredRecordAlreadyPresent=%d AllocationFailures=%llu EstimatedResidentColorMemoryMiB=%.2f CpuMs=%.4f"),
				*GetNameSafe(World), *GetNameSafe(Portal), DesiredCaptureResolution,
				ResourceGeneration, ExistingRecord.ResourceGeneration != 0 ? 1 : 0,
				RetiredResolutionEndpointRecords.Contains(PortalKey) ? 1 : 0,
				static_cast<unsigned long long>(AllocationFailureCount),
				GetEstimatedResidentColorMemoryMiB(),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			if (ExistingRecord.ResourceGeneration != 0)
			{
				EndpointRecords.Add(PortalKey, MoveTemp(ExistingRecord));
			}
			return false;
		}
#if !UE_BUILD_SHIPPING
		const uint32 OldResolution = ExistingRecord.CaptureResolution;
		const uint32 OldResourceGeneration = ExistingRecord.ResourceGeneration;
		const uint32 OldCaptureGeneration = ExistingRecord.CaptureGeneration;
#endif
		RetiredResolutionEndpointRecords.Add(
			PortalKey, MoveTemp(ExistingRecord));
		RetiredPublicationOverrides.Add(PortalKey);
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][ResolutionTransition] Old generation moved to frozen publication ownership. World=%s Portal=%s OldResolution=%u NewResolution=%u OldResourceGeneration=%u OldCaptureGeneration=%u NewResourceGeneration=%u RendererUsesOldGeneration=1 CaptureUsesNewGeneration=1 OldCaptureFrozen=1 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f CpuMs=%.4f"),
			*GetNameSafe(World), *GetNameSafe(Portal), OldResolution,
			DesiredCaptureResolution, OldResourceGeneration, OldCaptureGeneration,
			ResourceGeneration, GetEstimatedResidentColorMemoryMiB(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}
	EndpointRecords.Add(PortalKey, Record);
	PendingEndpointAllocations.Remove(PortalKey);
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
	SET_FLOAT_STAT(
		STAT_WP_CubeRenderTargetMiB,
		static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
	++AllocationCount;
#if !UE_BUILD_SHIPPING
	TotalAllocationCpuMs += Record.AllocationCpuMs;

	WP_LOG(this, Verbose,
		TEXT("[CaptureManager] Endpoint allocated. World=%s Portal=%s ResourceEpoch=%llu ResourceGeneration=%u CaptureGeneration=%u Resolution=%u ResolutionSource=ProjectSettingsDynamicPolicy Component=%s InitialCaptureTarget=%s StablePublishedReferenceOwner=%s AlternateTarget=%s ComponentOuter=%s CurrentTargetOuter=%s AlternateTargetOuter=%s Registered=%d ComponentWorldMatches=%d TextureTargetMatchesCurrent=%d InitialCaptureEqualsPublishedOwner=%d AlternateDistinct=%d CurrentUAV=%d AlternateUAV=%d CaptureEveryFrame=%d CaptureOnMovement=%d TickCanEver=%d TickStartEnabled=%d TickEnabled=%d Visible=%d DetailMode=%d ResourceOwner=CaptureManager TargetHistory=PriorAAResult PhysicalPingPong=%d StablePublishedObject=1 CrossEndpointReuse=0 AAMode=%s CopyBackEliminated=%d EndpointTargetCount=%d EndpointCount=%d StrongComponents=%d StrongRenderTargets=%d Allocations=%llu Releases=%llu EndpointColorMemoryMiB=%.2f EstimatedResidentColorMemoryMiB=%.2f AsyncAllocationWallMs=%.3f AllocationCpuMs=%.3f TotalAllocationCpuMs=%.3f"),
		*GetNameSafe(World), *GetNameSafe(Portal),
		static_cast<unsigned long long>(ResourceEpoch), Record.ResourceGeneration,
		Record.CaptureGeneration, Record.CaptureResolution,
		*GetNameSafe(CaptureComponent),
		*GetNameSafe(CurrentTarget), *GetNameSafe(CurrentTarget),
		*GetNameSafe(AlternateTarget),
		*GetNameSafe(CaptureComponent->GetOuter()),
		*GetNameSafe(CurrentTarget->GetOuter()),
		*GetNameSafe(IsValid(AlternateTarget) ? AlternateTarget->GetOuter() : nullptr),
		CaptureComponent->IsRegistered() ? 1 : 0,
		CaptureComponent->GetWorld() == World ? 1 : 0,
		CaptureComponent->TextureTarget == CurrentTarget ? 1 : 0,
		Record.CaptureTarget.Get() == Record.RenderTarget.Get() ? 1 : 0,
		!IsValid(AlternateTarget) || AlternateTarget != CurrentTarget ? 1 : 0,
		CurrentTarget->bSupportsUAV ? 1 : 0,
		IsValid(AlternateTarget) && AlternateTarget->bSupportsUAV ? 1 : 0,
		CaptureComponent->bCaptureEveryFrame ? 1 : 0,
		CaptureComponent->bCaptureOnMovement ? 1 : 0,
		CaptureComponent->PrimaryComponentTick.bCanEverTick ? 1 : 0,
		CaptureComponent->PrimaryComponentTick.bStartWithTickEnabled ? 1 : 0,
		CaptureComponent->IsComponentTickEnabled() ? 1 : 0,
		CaptureComponent->IsVisible() ? 1 : 0,
		static_cast<int32>(CaptureComponent->DetailMode),
		bDesiredDirectPublish ? 1 : 0,
		bDesiredDirectPublish ? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
		bDesiredDirectPublish ? 1 : 0,
		bDesiredDirectPublish ? 2 : 1,
		EndpointRecords.Num(), ManagedCaptureComponents.Num(), ManagedRenderTargets.Num(),
		static_cast<unsigned long long>(AllocationCount),
		static_cast<unsigned long long>(ReleaseCount),
		GetWPEndpointColorMemoryMiB(Record.CaptureResolution)
			* (bDesiredDirectPublish ? 2.0 : 1.0),
		GetEstimatedResidentColorMemoryMiB(), AsyncAllocationWallMs,
		Record.AllocationCpuMs, TotalAllocationCpuMs);
#endif
	return true;
}

void UWPCaptureManager::ReleasePendingAllocation(
	FPendingEndpointAllocation& Pending,
	const TCHAR* Reason)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	// Fence를 버리는 것은 대기가 아닙니다. ReleaseResource 명령은 같은 Render
	// command queue에서 기존 allocation 명령 뒤에 들어가므로 순서가 보장됩니다.
	Pending.AllocationFence.Reset();
	TSet<UTextureRenderTargetCube*> UniqueTargets;
	if (UTextureRenderTargetCube* Current = Pending.CurrentTarget.Get())
	{
		UniqueTargets.Add(Current);
	}
	if (UTextureRenderTargetCube* Alternate = Pending.AlternateTarget.Get())
	{
		UniqueTargets.Add(Alternate);
	}
	for (UTextureRenderTargetCube* Target : UniqueTargets)
	{
		Target->ReleaseResource();
		ManagedRenderTargets.RemoveSingleSwap(Target);
	}
	CubeTargetReleaseCount += static_cast<uint64>(UniqueTargets.Num());
#if !UE_BUILD_SHIPPING
	const double CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TotalCubeTargetReleaseCpuMs += CpuMs;
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][AsyncAllocation] Pending endpoint allocation released. World=%s Portal=%s ResourceEpoch=%llu ResourceGeneration=%u Resolution=%u Stage=%d ReleasedCubeTargets=%d StrongRenderTargets=%d Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Pending.Portal.Get()),
		static_cast<unsigned long long>(Pending.ResourceEpoch),
		Pending.ResourceGeneration, Pending.CaptureResolution,
		static_cast<int32>(Pending.Stage), UniqueTargets.Num(),
		ManagedRenderTargets.Num(), Reason ? Reason : TEXT("Unspecified"), CpuMs);
#endif
	Pending = FPendingEndpointAllocation();
}

void UWPCaptureManager::ReleaseRecord(FEndpointRecord& Record, const TCHAR* Reason)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	AWormholePortalActor* Portal = Record.Portal.Get();
	USceneCaptureComponentCube* CaptureComponent = Record.CaptureComponent.Get();
	UTextureRenderTargetCube* CaptureTarget = Record.CaptureTarget.Get();
	UTextureRenderTargetCube* RenderTarget = Record.RenderTarget.Get();
	UTextureRenderTargetCube* AlternateTarget = Record.AlternateRenderTarget.Get();
	if (CaptureComponent)
	{
		CaptureComponent->TextureTarget = nullptr;
		CaptureComponent->ClearHiddenComponents();
		CaptureComponent->DestroyComponent();
	}
#if !UE_BUILD_SHIPPING
	const double TargetReleaseStartSeconds = FPlatformTime::Seconds();
#endif
	TSet<UTextureRenderTargetCube*> UniqueTargets;
	if (CaptureTarget)
	{
		UniqueTargets.Add(CaptureTarget);
	}
	if (RenderTarget)
	{
		UniqueTargets.Add(RenderTarget);
	}
	if (AlternateTarget)
	{
		UniqueTargets.Add(AlternateTarget);
	}
	for (UTextureRenderTargetCube* Target : UniqueTargets)
	{
		Target->ReleaseResource();
		ManagedRenderTargets.RemoveSingleSwap(Target);
	}
	ManagedCaptureComponents.RemoveSingleSwap(CaptureComponent);
	CubeTargetReleaseCount += static_cast<uint64>(UniqueTargets.Num());
#if !UE_BUILD_SHIPPING
	const double TargetReleaseCpuMs =
		(FPlatformTime::Seconds() - TargetReleaseStartSeconds) * 1000.0;
	TotalCubeTargetReleaseCpuMs += TargetReleaseCpuMs;
#endif
	++ReleaseCount;
#if !UE_BUILD_SHIPPING
	const double ReleaseCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	TotalReleaseCpuMs += ReleaseCpuMs;

	WP_LOG(this, Verbose,
		TEXT("[CaptureManager] Endpoint released. World=%s Portal=%s ResourceEpoch=%llu ResourceGeneration=%u CaptureGeneration=%u Resolution=%u Component=%s CaptureTarget=%s PublishedTarget=%s AlternateTarget=%s CaptureEqualsPublished=%d ReleasedUniqueCubeTargets=%d CrossEndpointReuse=0 ResourceOwner=CaptureManager Allocations=%llu Releases=%llu Balance=%lld TargetAllocations=%llu TargetReleases=%llu TargetBalance=%lld RemainingEndpointCountBeforeErase=%d StrongComponents=%d StrongRenderTargets=%d EstimatedResidentColorMemoryMiBBeforeErase=%.2f Reason=%s TargetReleaseCpuMs=%.3f ReleaseCpuMs=%.3f TotalReleaseCpuMs=%.3f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
		static_cast<unsigned long long>(Record.ResourceEpoch), Record.ResourceGeneration,
		Record.CaptureGeneration, Record.CaptureResolution,
		*GetNameSafe(CaptureComponent),
		*GetNameSafe(CaptureTarget), *GetNameSafe(RenderTarget),
		*GetNameSafe(AlternateTarget),
		CaptureTarget == RenderTarget ? 1 : 0, UniqueTargets.Num(),
		static_cast<unsigned long long>(AllocationCount),
		static_cast<unsigned long long>(ReleaseCount),
		static_cast<long long>(AllocationCount) - static_cast<long long>(ReleaseCount),
		static_cast<unsigned long long>(CubeTargetAllocationCount),
		static_cast<unsigned long long>(CubeTargetReleaseCount),
		static_cast<long long>(CubeTargetAllocationCount)
			- static_cast<long long>(CubeTargetReleaseCount),
		EndpointRecords.Num(), ManagedCaptureComponents.Num(), ManagedRenderTargets.Num(),
		GetEstimatedResidentColorMemoryMiB(), Reason ? Reason : TEXT("Unspecified"),
		TargetReleaseCpuMs, ReleaseCpuMs, TotalReleaseCpuMs);
#endif
	Record = FEndpointRecord();
}

bool UWPCaptureManager::ReleaseEndpointResources(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	FEndpointRecord* Record = EndpointRecords.Find(PortalKey);
	FEndpointRecord* RetiredRecord =
		RetiredResolutionEndpointRecords.Find(PortalKey);
	FPendingEndpointAllocation* Pending =
		PendingEndpointAllocations.Find(PortalKey);
	if (!Record && !RetiredRecord && !Pending)
	{
		return false;
	}
	TArray<FGuid> ReferencingPairIds;
	for (const TPair<FGuid, FPairCaptureState>& Pair : PairCaptureStates)
	{
		if (Pair.Value.PortalA.Get() == Portal || Pair.Value.PortalB.Get() == Portal)
		{
			ReferencingPairIds.Add(Pair.Key);
		}
	}
	for (const FGuid& PairId : ReferencingPairIds)
	{
		RemovePairCaptureState(PairId, TEXT("EndpointResourceRelease"));
	}
	if (Record)
	{
		ReleaseRecord(*Record, Reason);
		EndpointRecords.Remove(PortalKey);
	}
	if (Pending)
	{
		ReleasePendingAllocation(*Pending, Reason);
		PendingEndpointAllocations.Remove(PortalKey);
	}
	if (RetiredRecord)
	{
		ReleaseRecord(*RetiredRecord, Reason);
		RetiredResolutionEndpointRecords.Remove(PortalKey);
	}
	RetiredPublicationOverrides.Remove(PortalKey);
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
	SET_FLOAT_STAT(
		STAT_WP_CubeRenderTargetMiB,
		static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
	return true;
}

bool UWPCaptureManager::ReleaseEndpointResourcesForResolutionChange(
	AWormholePortalActor* Portal,
	const TCHAR* Reason)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(Portal);
	bool bReleased = false;
	if (FEndpointRecord* Record = EndpointRecords.Find(PortalKey))
	{
		ReleaseRecord(*Record, Reason);
		EndpointRecords.Remove(PortalKey);
		bReleased = true;
	}
	if (FPendingEndpointAllocation* Pending =
		PendingEndpointAllocations.Find(PortalKey))
	{
		ReleasePendingAllocation(*Pending, Reason);
		PendingEndpointAllocations.Remove(PortalKey);
		bReleased = true;
	}
	if (FEndpointRecord* RetiredRecord =
		RetiredResolutionEndpointRecords.Find(PortalKey))
	{
		ReleaseRecord(*RetiredRecord, Reason);
		RetiredResolutionEndpointRecords.Remove(PortalKey);
		bReleased = true;
	}
	RetiredPublicationOverrides.Remove(PortalKey);
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
	SET_FLOAT_STAT(
		STAT_WP_CubeRenderTargetMiB,
		static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][ResolutionTransition] Endpoint release-first step completed. World=%s Portal=%s Released=%d PairAuthorityPreserved=1 TransitStatePreserved=1 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(Portal),
		bReleased ? 1 : 0, GetEstimatedResidentColorMemoryMiB(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return bReleased;
}

bool UWPCaptureManager::HasEndpointResources(const AWormholePortalActor* Portal) const
{
	return HasEndpointResources(Portal, 0);
}

bool UWPCaptureManager::HasEndpointResources(
	const AWormholePortalActor* Portal,
	const uint32 ExpectedResolution) const
{
	if (!Portal)
	{
		return false;
	}
	const FEndpointRecord* Record = EndpointRecords.Find(
		TWeakObjectPtr<AWormholePortalActor>(const_cast<AWormholePortalActor*>(Portal)));
	if (!Record || Record->ResourceEpoch == 0 || Record->ResourceGeneration == 0)
	{
		return false;
	}
	USceneCaptureComponentCube* CaptureComponent = Record->CaptureComponent.Get();
	UTextureRenderTargetCube* CaptureTarget = Record->CaptureTarget.Get();
	UTextureRenderTargetCube* RenderTarget = Record->RenderTarget.Get();
	UTextureRenderTargetCube* AlternateTarget =
		Record->AlternateRenderTarget.Get();
	const bool bAlternateContract = Record->bCubeAADirectPublish
		? IsValid(AlternateTarget)
			&& AlternateTarget != CaptureTarget
			&& AlternateTarget->bSupportsUAV
			&& HasRenderTargetColorContract(
				AlternateTarget, Record->CaptureResolution, true)
		: !IsValid(AlternateTarget);
	const bool bTargetTopologyValid = Record->bCubeAADirectPublish
		? CaptureTarget != AlternateTarget
			&& (RenderTarget == CaptureTarget || RenderTarget == AlternateTarget)
		: CaptureTarget == RenderTarget && !IsValid(AlternateTarget);
	return IsValid(CaptureComponent)
		&& IsValid(CaptureTarget)
		&& IsValid(RenderTarget)
		&& bAlternateContract
		&& bTargetTopologyValid
		&& (ExpectedResolution == 0
			|| Record->CaptureResolution
				== ResolveWPCaptureResolution(ExpectedResolution))
		&& Record->bCubeAADirectPublish == IsWPCubeAADirectPublishEnabled()
		&& RenderTarget->bSupportsUAV == Record->bCubeAADirectPublish
		&& CaptureComponent->IsRegistered()
		&& CaptureComponent->GetWorld() == ManagedWorld.Get()
		&& CaptureComponent->TextureTarget == CaptureTarget
		&& !CaptureComponent->bCaptureEveryFrame
		&& !CaptureComponent->bCaptureOnMovement
		&& !CaptureComponent->PrimaryComponentTick.bCanEverTick
		&& !CaptureComponent->PrimaryComponentTick.bStartWithTickEnabled
		&& !CaptureComponent->IsComponentTickEnabled()
		&& HasCaptureColorContract(
			CaptureComponent, CaptureTarget, Record->CaptureResolution)
		&& HasRenderTargetColorContract(
			CaptureTarget, Record->CaptureResolution,
			Record->bCubeAADirectPublish)
		&& HasRenderTargetColorContract(
			RenderTarget, Record->CaptureResolution,
			Record->bCubeAADirectPublish);
}

uint32 UWPCaptureManager::GetEndpointCaptureResolution(
	const AWormholePortalActor* Portal) const
{
	if (!Portal)
	{
		return 0;
	}
	const FEndpointRecord* Record = EndpointRecords.Find(
		TWeakObjectPtr<AWormholePortalActor>(
			const_cast<AWormholePortalActor*>(Portal)));
	return Record ? Record->CaptureResolution : 0;
}

double UWPCaptureManager::GetEndpointEstimatedColorMemoryMiB(
	const AWormholePortalActor* Portal) const
{
	if (!Portal)
	{
		return 0.0;
	}
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(
		const_cast<AWormholePortalActor*>(Portal));
	if (const FEndpointRecord* Record = EndpointRecords.Find(PortalKey))
	{
		return GetWPEndpointColorMemoryMiB(Record->CaptureResolution)
			* (Record->bCubeAADirectPublish ? 2.0 : 1.0);
	}
	if (const FPendingEndpointAllocation* Pending =
		PendingEndpointAllocations.Find(PortalKey))
	{
		const int32 TargetCount =
			(IsValid(Pending->CurrentTarget.Get()) ? 1 : 0)
			+ (IsValid(Pending->AlternateTarget.Get()) ? 1 : 0);
		return GetWPEndpointColorMemoryMiB(Pending->CaptureResolution)
			* static_cast<double>(TargetCount);
	}
	return 0.0;
}

double UWPCaptureManager::GetEstimatedPairColorMemoryMiB(
	const uint32 Resolution) const
{
	const uint32 SafeResolution = ResolveWPCaptureResolution(Resolution);
	const double TargetCountPerEndpoint =
		IsWPCubeAADirectPublishEnabled() ? 2.0 : 1.0;
	return GetWPEndpointColorMemoryMiB(SafeResolution)
		* TargetCountPerEndpoint * 2.0;
}

bool UWPCaptureManager::GetEndpointSnapshot(
	const AWormholePortalActor* Portal,
	FWPCaptureEndpointSnapshot& OutSnapshot) const
{
	OutSnapshot = FWPCaptureEndpointSnapshot();
	if (!Portal)
	{
		return false;
	}
	const FEndpointRecord* Record = EndpointRecords.Find(
		TWeakObjectPtr<AWormholePortalActor>(const_cast<AWormholePortalActor*>(Portal)));
	if (!Record)
	{
		return false;
	}
	return GetSnapshotFromRecord(Record, OutSnapshot);
}

bool UWPCaptureManager::GetPublishedEndpointSnapshot(
	const AWormholePortalActor* Portal,
	FWPCaptureEndpointSnapshot& OutSnapshot) const
{
	OutSnapshot = FWPCaptureEndpointSnapshot();
	if (!Portal)
	{
		return false;
	}
	const TWeakObjectPtr<AWormholePortalActor> PortalKey(
		const_cast<AWormholePortalActor*>(Portal));
	if (RetiredPublicationOverrides.Contains(PortalKey))
	{
		// Fail closed if the override invariant is ever broken. Falling through to the
		// replacement would expose only one new Endpoint before the atomic A/B switch.
		return GetSnapshotFromRecord(
			RetiredResolutionEndpointRecords.Find(PortalKey), OutSnapshot);
	}
	return GetSnapshotFromRecord(EndpointRecords.Find(PortalKey), OutSnapshot);
}

bool UWPCaptureManager::GetSnapshotFromRecord(
	const FEndpointRecord* Record,
	FWPCaptureEndpointSnapshot& OutSnapshot) const
{
	OutSnapshot = FWPCaptureEndpointSnapshot();
	if (!Record)
	{
		return false;
	}

	OutSnapshot.CaptureComponent = Record->CaptureComponent;
	OutSnapshot.CaptureTarget = Record->CaptureTarget;
	OutSnapshot.RenderTarget = Record->RenderTarget;
	OutSnapshot.ResourceEpoch = Record->ResourceEpoch;
	OutSnapshot.CaptureGeneration = Record->CaptureGeneration;
	OutSnapshot.bCubeAADirectPublish = Record->bCubeAADirectPublish;
	OutSnapshot.InitialValidFaceMask = Record->InitialValidFaceMask;
	OutSnapshot.CubeContract.CubeLayoutVersion = WPCaptureLayoutVersion;
	OutSnapshot.CubeContract.ResourceGeneration = Record->ResourceGeneration;
	OutSnapshot.CubeContract.ExpectedExtent = FIntPoint(
		Record->CaptureResolution, Record->CaptureResolution);
	OutSnapshot.CubeContract.ExpectedFormat = EWPCubeFormat::RGBA16Float;
	OutSnapshot.CubeContract.ExpectedMipCount = WPCaptureMipCount;
	OutSnapshot.CubeContract.ExpectedDimension = EWPCubeDimension::TextureCube;
	return OutSnapshot.IsReadyForSubmission(ManagedWorld.Get());
}

bool UWPCaptureManager::ActivatePairResolutionPublication(
	AWormholePortalActor* PortalA,
	AWormholePortalActor* PortalB,
	const TCHAR* Reason)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const TWeakObjectPtr<AWormholePortalActor> KeyA(PortalA);
	const TWeakObjectPtr<AWormholePortalActor> KeyB(PortalB);
	const FEndpointRecord* NewA = EndpointRecords.Find(KeyA);
	const FEndpointRecord* NewB = EndpointRecords.Find(KeyB);
	const FEndpointRecord* OldA = RetiredResolutionEndpointRecords.Find(KeyA);
	const FEndpointRecord* OldB = RetiredResolutionEndpointRecords.Find(KeyB);
	const bool bReady = NewA && NewB && OldA && OldB
		&& RetiredPublicationOverrides.Contains(KeyA)
		&& RetiredPublicationOverrides.Contains(KeyB)
		&& NewA->ResourceGeneration != 0 && NewB->ResourceGeneration != 0
		&& NewA->CaptureGeneration != 0 && NewB->CaptureGeneration != 0
		&& NewA->InitialValidFaceMask == 0x3f && NewB->InitialValidFaceMask == 0x3f
		&& NewA->CaptureResolution == NewB->CaptureResolution;
	if (!bReady)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][ResolutionTransition] Atomic publication activation rejected. World=%s PortalA=%s PortalB=%s NewA=%d NewB=%d OldA=%d OldB=%d OverrideA=%d OverrideB=%d NewGenerationA=%u NewGenerationB=%u NewCaptureGenerationA=%u NewCaptureGenerationB=%u NewResolutionA=%u NewResolutionB=%u RendererRemainsOnOld=1 Reason=%s CpuMs=%.4f"),
			*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(PortalA),
			*GetNameSafe(PortalB), NewA ? 1 : 0, NewB ? 1 : 0,
			OldA ? 1 : 0, OldB ? 1 : 0,
			RetiredPublicationOverrides.Contains(KeyA) ? 1 : 0,
			RetiredPublicationOverrides.Contains(KeyB) ? 1 : 0,
			NewA ? NewA->ResourceGeneration : 0,
			NewB ? NewB->ResourceGeneration : 0,
			NewA ? NewA->CaptureGeneration : 0,
			NewB ? NewB->CaptureGeneration : 0,
			NewA ? NewA->CaptureResolution : 0,
			NewB ? NewB->CaptureResolution : 0,
			Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	// 두 key를 같은 Game Thread 함수에서 제거하므로 다음 packet은 A/B 모두 새 세대를
	// 읽습니다. 이 시점에도 retired record는 Render fence 완료까지 강하게 유지됩니다.
	RetiredPublicationOverrides.Remove(KeyA);
	RetiredPublicationOverrides.Remove(KeyB);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][ResolutionTransition] Atomic A+B publication activated. World=%s PortalA=%s PortalB=%s OldResolution=%u NewResolution=%u OldResourceGenerationA=%u OldResourceGenerationB=%u NewResourceGenerationA=%u NewResourceGenerationB=%u NewCaptureGenerationA=%u NewCaptureGenerationB=%u RetiredResourcesHeldUntilFence=1 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(PortalA),
		*GetNameSafe(PortalB), OldA->CaptureResolution, NewA->CaptureResolution,
		OldA->ResourceGeneration, OldB->ResourceGeneration,
		NewA->ResourceGeneration, NewB->ResourceGeneration,
		NewA->CaptureGeneration, NewB->CaptureGeneration,
		GetEstimatedResidentColorMemoryMiB(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

bool UWPCaptureManager::ReleaseRetiredPairResolutionResources(
	AWormholePortalActor* PortalA,
	AWormholePortalActor* PortalB,
	const TCHAR* Reason)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 ReleasedEndpointCount = 0;
	for (AWormholePortalActor* Portal : { PortalA, PortalB })
	{
		const TWeakObjectPtr<AWormholePortalActor> Key(Portal);
		RetiredPublicationOverrides.Remove(Key);
		FEndpointRecord Retired;
		if (RetiredResolutionEndpointRecords.RemoveAndCopyValue(Key, Retired))
		{
			ReleaseRecord(Retired, Reason);
			++ReleasedEndpointCount;
		}
	}
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
	SET_FLOAT_STAT(
		STAT_WP_CubeRenderTargetMiB,
		static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][ResolutionTransition] Retired A+B generations released after Renderer fence. World=%s PortalA=%s PortalB=%s ReleasedEndpointCount=%d ExpectedEndpointCount=2 GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(PortalA),
		*GetNameSafe(PortalB), ReleasedEndpointCount,
		GetEstimatedResidentColorMemoryMiB(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return ReleasedEndpointCount == 2;
}

bool UWPCaptureManager::CancelPairResolutionTransition(
	AWormholePortalActor* PortalA,
	AWormholePortalActor* PortalB,
	const TCHAR* Reason)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 RestoredEndpointCount = 0;
	int32 ReleasedReplacementCount = 0;
	for (AWormholePortalActor* Portal : { PortalA, PortalB })
	{
		const TWeakObjectPtr<AWormholePortalActor> Key(Portal);
		if (FPendingEndpointAllocation* Pending =
			PendingEndpointAllocations.Find(Key))
		{
			ReleasePendingAllocation(*Pending, Reason);
			PendingEndpointAllocations.Remove(Key);
		}

		FEndpointRecord Retired;
		if (!RetiredResolutionEndpointRecords.RemoveAndCopyValue(Key, Retired))
		{
			RetiredPublicationOverrides.Remove(Key);
			continue;
		}
		if (FEndpointRecord* Replacement = EndpointRecords.Find(Key))
		{
			ReleaseRecord(*Replacement, Reason);
			EndpointRecords.Remove(Key);
			++ReleasedReplacementCount;
		}
		EndpointRecords.Add(Key, MoveTemp(Retired));
		RetiredPublicationOverrides.Remove(Key);
		++RestoredEndpointCount;
	}
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, ManagedRenderTargets.Num());
	SET_FLOAT_STAT(
		STAT_WP_CubeRenderTargetMiB,
		static_cast<float>(GetEstimatedResidentColorMemoryMiB()));
	WP_LOG(this, Warning,
		TEXT("[CaptureManager][ResolutionTransition] Seamless transition cancelled; frozen generations restored. World=%s PortalA=%s PortalB=%s RestoredEndpointCount=%d ReleasedReplacementCount=%d RendererOldGenerationPreserved=%d GameThreadWait=0 EstimatedResidentColorMemoryMiB=%.2f Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()), *GetNameSafe(PortalA),
		*GetNameSafe(PortalB), RestoredEndpointCount,
		ReleasedReplacementCount, RestoredEndpointCount > 0 ? 1 : 0,
		GetEstimatedResidentColorMemoryMiB(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	return RestoredEndpointCount > 0;
}

bool UWPCaptureManager::ResetPairCaptureCycleForResolutionTransition(
	const FGuid& PairId,
	const TCHAR* Reason)
{
	const double StartSeconds = FPlatformTime::Seconds();
	FPairCaptureState* PairState = PairCaptureStates.Find(PairId);
	if (!PairState)
	{
		return false;
	}
	const uint8 PreviousMask = PairState->StaggeredCompletionMask;
	PairState->StaggeredCompletionMask = 0;
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][ResolutionTransition] Pair capture cycle reset before replacement A+B warmup. World=%s PairId=%s PreviousStaggeredMask=%u NewStaggeredMask=0 PairCaptureEpoch=%llu Reason=%s CpuMs=%.4f"),
		*GetNameSafe(ManagedWorld.Get()),
		*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
		static_cast<uint32>(PreviousMask),
		static_cast<unsigned long long>(PairState->PairCaptureEpoch),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

UTextureRenderTargetCube* UWPCaptureManager::GetEndpointRenderTarget(
	const AWormholePortalActor* Portal) const
{
	FWPCaptureEndpointSnapshot Snapshot;
	return GetEndpointSnapshot(Portal, Snapshot) ? Snapshot.RenderTarget.Get() : nullptr;
}

uint32 UWPCaptureManager::GetEndpointCaptureGeneration(
	const AWormholePortalActor* Portal) const
{
	FWPCaptureEndpointSnapshot Snapshot;
	return GetEndpointSnapshot(Portal, Snapshot) ? Snapshot.CaptureGeneration : 0;
}

bool UWPCaptureManager::SetPairCaptureAuthority(
	const FGuid& PairId,
	AWormholePortalActor* PortalA,
	AWormholePortalActor* PortalB,
	const uint64 OwnershipEpoch,
	const bool bEnabled,
	const TCHAR* Reason)
{
	// 로그 전용: pair authority 갱신 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	UWorld* World = ManagedWorld.Get();
	const bool bInputValid = PairId.IsValid() && IsValid(PortalA) && IsValid(PortalB)
		&& PortalA != PortalB && PortalA->GetWorld() == World && PortalB->GetWorld() == World
		&& (!bEnabled || OwnershipEpoch != 0);
	if (!bInputValid)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][Authority] Pair authority rejected. World=%s PairId=%s PortalA=%s PortalB=%s Enabled=%d OwnershipEpoch=%llu Reason=InvalidInput RequestReason=%s CpuMs=%.4f"),
			*GetNameSafe(World), *PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*GetNameSafe(PortalA), *GetNameSafe(PortalB), bEnabled ? 1 : 0,
			static_cast<unsigned long long>(OwnershipEpoch),
			Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	FPairCaptureState& PairState = PairCaptureStates.FindOrAdd(PairId);
	const bool bTopologyChanged = PairState.PairId.IsValid()
		&& !((PairState.PortalA.Get() == PortalA && PairState.PortalB.Get() == PortalB)
			|| (PairState.PortalA.Get() == PortalB && PairState.PortalB.Get() == PortalA));
	if (bTopologyChanged)
	{
		ResetFirstPersonTransitState(PairState, TEXT("PairTopologyReplaced"));
		PairState.PairCaptureEpoch = 0;
		PairState.LastSubmissionFrame = MAX_uint64;
		PairState.LastSubmissionFrameA = MAX_uint64;
		PairState.LastSubmissionFrameB = MAX_uint64;
		PairState.StaggeredCompletionMask = 0;
	}

	const uint64 PreviousEpoch = PairState.OwnershipEpoch;
	const bool bPreviousEnabled = PairState.bAuthorityEnabled;
	PairState.PairId = PairId;
	PairState.PortalA = PortalA;
	PairState.PortalB = PortalB;
	PairState.OwnershipEpoch = bEnabled ? OwnershipEpoch : 0;
	PairState.bAuthorityEnabled = bEnabled;
	if (bPreviousEnabled != bEnabled || PreviousEpoch != PairState.OwnershipEpoch)
	{
		PairState.LastSubmissionFrame = MAX_uint64;
		PairState.StaggeredCompletionMask = 0;
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][Authority] Pair authority updated. World=%s PairId=%s PortalA=%s PortalB=%s PreviousEnabled=%d Enabled=%d PreviousEpoch=%llu OwnershipEpoch=%llu PairCaptureEpoch=%llu StaggeredCompletionMask=%u TopologyChanged=%d ActorTickAuthority=0 Reason=%s CpuMs=%.4f"),
		*GetNameSafe(World), *PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
		*GetNameSafe(PortalA), *GetNameSafe(PortalB), bPreviousEnabled ? 1 : 0,
		bEnabled ? 1 : 0, static_cast<unsigned long long>(PreviousEpoch),
		static_cast<unsigned long long>(PairState.OwnershipEpoch),
		static_cast<unsigned long long>(PairState.PairCaptureEpoch),
		static_cast<uint32>(PairState.StaggeredCompletionMask),
		bTopologyChanged ? 1 : 0,
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

void UWPCaptureManager::ResetFirstPersonTransitState(
	FPairCaptureState& PairState,
	const TCHAR* Reason,
	const bool bResetActiveSource)
{
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	const EFirstPersonTransitParallaxPhase PreviousPhase = PairState.FirstPersonPhase;
	const uint64 PreviousSequence = PairState.FirstPersonSequence;
#endif
	PairState.FirstPersonPhase = EFirstPersonTransitParallaxPhase::Idle;
	PairState.FirstPersonSequence = 0;
	PairState.FirstPersonTerminalEventFrame = 0;
	PairState.FirstPersonActor.Reset();
	PairState.FirstPersonSourcePortal.Reset();
	PairState.FirstPersonDestinationPortal.Reset();
	PairState.FirstPersonEntryPointWorld = FVector::ZeroVector;
	PairState.FirstPersonSelectedPlane = EWPTransitPlane::YZ;
	PairState.FirstPersonMapping = FWPTransform();
	PairState.bFirstPersonPathMappingValid = false;
	if (bResetActiveSource)
	{
		PairState.ActiveCameraRelativeSourcePortal.Reset();
	}
#if !UE_BUILD_SHIPPING
	if (PreviousPhase != EFirstPersonTransitParallaxPhase::Idle || PreviousSequence != 0)
	{
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][TransitParallax] State reset. PairId=%s PreviousPhase=%d PreviousSequence=%llu ResetActiveSource=%d Reason=%s CpuMs=%.4f"),
			*PairState.PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
			static_cast<int32>(PreviousPhase), static_cast<unsigned long long>(PreviousSequence),
			bResetActiveSource ? 1 : 0, Reason ? Reason : TEXT("Unspecified"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
#endif
}

bool UWPCaptureManager::ApplyPairTransitEvent(
	const FGuid& PairId,
	const FWPTransitEvent& Event,
	const EWPCaptureTransitPhase Phase)
{
	// 로그 전용: transit event 적용 CPU 시간을 측정합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	FPairCaptureState* PairState = PairCaptureStates.Find(PairId);
	AWormholePortalActor* SourcePortal = Event.SourcePortal.Get();
	AWormholePortalActor* DestinationPortal = Event.DestinationPortal.Get();
	AActor* TransitActor = Event.Actor.Get();
	const bool bPairMatches = PairState &&
		((PairState->PortalA.Get() == SourcePortal && PairState->PortalB.Get() == DestinationPortal)
			|| (PairState->PortalA.Get() == DestinationPortal && PairState->PortalB.Get() == SourcePortal));
	
	if (!bPairMatches || !IsValid(TransitActor) || Event.Sequence == 0)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][TransitParallax] Event rejected. PairId=%s Phase=%d Sequence=%llu Actor=%s Source=%s Destination=%s PairStateFound=%d PairMatches=%d Reason=InvalidLifecycleTopology CpuMs=%.4f"),
			*PairId.ToString(EGuidFormats::DigitsWithHyphensLower), static_cast<int32>(Phase),
			static_cast<unsigned long long>(Event.Sequence), *GetNameSafe(TransitActor),
			*GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal), PairState ? 1 : 0,
			bPairMatches ? 1 : 0, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	FVector CameraLocation = FVector::ZeroVector;
	const TCHAR* ViewReason = TEXT("Unknown");
	
	const bool bFirstPersonView = ResolveFirstPersonLocalView(
		ManagedWorld.Get(), TransitActor, CameraLocation, ViewReason);
	
	if (Phase == EWPCaptureTransitPhase::Started)
	{
		if (!bFirstPersonView)
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose,
				TEXT("[CaptureManager][TransitParallax] Started retained nearest-camera path. PairId=%s Sequence=%llu Actor=%s Source=%s Destination=%s ViewReason=%s CpuMs=%.4f"),
				*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
				static_cast<unsigned long long>(Event.Sequence), *GetNameSafe(TransitActor),
				*GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal), ViewReason,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			return true;
		}
		if (!Event.bPathMappingValid || !IsFiniteVector(Event.EntryPointWorld)
			|| !IsValidTransitPlane(Event.SelectedPlane) || Event.Mapping.SourceRotation.ContainsNaN()
			|| Event.Mapping.TransportRotation.ContainsNaN())
		{
			WP_LOG(this, Warning,
				TEXT("[CaptureManager][TransitParallax] Started rejected. PairId=%s Sequence=%llu PathMappingValid=%d EntryPoint=%s MoveDirection=%s Reason=InvalidPathSnapshot CpuMs=%.4f"),
				*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
				static_cast<unsigned long long>(Event.Sequence), Event.bPathMappingValid ? 1 : 0,
				*Event.EntryPointWorld.ToCompactString(), *Event.MoveDirectionWorld.ToCompactString(),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			return false;
		}
		if (PairState->FirstPersonPhase != EFirstPersonTransitParallaxPhase::Idle
			&& Event.Sequence <= PairState->FirstPersonSequence)
		{
			return false;
		}
		if (PairState->FirstPersonPhase != EFirstPersonTransitParallaxPhase::Idle)
		{
			ResetFirstPersonTransitState(*PairState, TEXT("SupersededByNewerStarted"));
		}
		PairState->FirstPersonPhase = EFirstPersonTransitParallaxPhase::Crossing;
		PairState->FirstPersonSequence = Event.Sequence;
		PairState->FirstPersonActor = TransitActor;
		PairState->FirstPersonSourcePortal = SourcePortal;
		PairState->FirstPersonDestinationPortal = DestinationPortal;
		PairState->FirstPersonEntryPointWorld = Event.EntryPointWorld;
		PairState->FirstPersonSelectedPlane = Event.SelectedPlane;
		PairState->FirstPersonMapping = Event.Mapping;
		PairState->bFirstPersonPathMappingValid = true;
		PairState->ActiveCameraRelativeSourcePortal = SourcePortal;
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][TransitParallax] Crossing locked. PairId=%s Sequence=%llu Actor=%s Source=%s Destination=%s EntryPoint=%s MoveDirection=%s CameraLocation=%s CpuMs=%.4f"),
			*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
			static_cast<unsigned long long>(Event.Sequence), *GetNameSafe(TransitActor),
			*GetNameSafe(SourcePortal), *GetNameSafe(DestinationPortal),
			*Event.EntryPointWorld.ToCompactString(), *Event.MoveDirectionWorld.ToCompactString(),
			*CameraLocation.ToCompactString(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return true;
	}

	const bool bMatchesActiveRun = PairState->FirstPersonPhase
			!= EFirstPersonTransitParallaxPhase::Idle
		&& Event.Sequence == PairState->FirstPersonSequence
		&& PairState->FirstPersonActor.Get() == TransitActor
		&& PairState->FirstPersonSourcePortal.Get() == SourcePortal
		&& PairState->FirstPersonDestinationPortal.Get() == DestinationPortal;
	if (!bMatchesActiveRun)
	{
		return false;
	}
	if (!bFirstPersonView)
	{
		ResetFirstPersonTransitState(*PairState, TEXT("TerminalRejectedByFirstPersonPolicy"), false);
		return true;
	}
	if (Event.bPathMappingValid && IsFiniteVector(Event.EntryPointWorld)
		&& IsFiniteVector(Event.MoveDirectionWorld) && !Event.MoveDirectionWorld.IsNearlyZero()
		&& !Event.Mapping.TransportRotation.ContainsNaN())
	{
		PairState->FirstPersonEntryPointWorld = Event.EntryPointWorld;
		PairState->FirstPersonSelectedPlane = Event.SelectedPlane;
		PairState->FirstPersonMapping = Event.Mapping;
		PairState->bFirstPersonPathMappingValid = true;
	}
	PairState->FirstPersonPhase = Phase == EWPCaptureTransitPhase::Committed
		? EFirstPersonTransitParallaxPhase::CommitPending
		: EFirstPersonTransitParallaxPhase::CancelPending;
	PairState->FirstPersonTerminalEventFrame = GFrameCounter;
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][TransitParallax] Terminal state queued. PairId=%s Phase=%d Sequence=%llu EventFrame=%llu WaitForLaterFrame=%d CameraLocation=%s PathMappingValid=%d CpuMs=%.4f"),
		*PairId.ToString(EGuidFormats::DigitsWithHyphensLower), static_cast<int32>(Phase),
		static_cast<unsigned long long>(Event.Sequence),
		static_cast<unsigned long long>(PairState->FirstPersonTerminalEventFrame),
		Phase == EWPCaptureTransitPhase::Committed ? 1 : 0,
		*CameraLocation.ToCompactString(), PairState->bFirstPersonPathMappingValid ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

bool UWPCaptureManager::IsPairCommitWaitingForFreshCamera(const FGuid& PairId) const
{
	const FPairCaptureState* PairState = PairCaptureStates.Find(PairId);
	return PairState
		&& PairState->FirstPersonPhase == EFirstPersonTransitParallaxPhase::CommitPending
		&& GFrameCounter <= PairState->FirstPersonTerminalEventFrame;
}

bool UWPCaptureManager::ValidateEndpointForSubmission(
	const AWormholePortalActor* Portal,
	FEndpointRecord*& OutRecord,
	FString& OutFailureReason)
{
	OutRecord = nullptr;
	OutFailureReason.Reset();
	if (!IsValid(Portal))
	{
		OutFailureReason = TEXT("PortalInvalid");
		return false;
	}
	OutRecord = EndpointRecords.Find(
		TWeakObjectPtr<AWormholePortalActor>(const_cast<AWormholePortalActor*>(Portal)));
	if (!OutRecord)
	{
		OutFailureReason = TEXT("EndpointRecordMissing");
		return false;
	}
	FWPCaptureEndpointSnapshot Snapshot;
	if (!GetEndpointSnapshot(Portal, Snapshot))
	{
		OutFailureReason = TEXT("EndpointSnapshotContractInvalid");
		return false;
	}
	return true;
}

bool UWPCaptureManager::SubmitEndpointCapture(
	FEndpointRecord& Record,
	AWormholePortalActor* Portal,
	AWormholePortalActor* OtherPortal,
	const FVector& CaptureLocation,
	const TCHAR* PositionMode,
	double& OutTransformCpuMs,
	double& OutSubmitCpuMs,
	const uint8 SelectedFaceMask)
{
	// 로그 전용: endpoint 전체/transform/submit CPU 시간을 구간별로 측정합니다.
	const double EndpointStartSeconds = FPlatformTime::Seconds();
	OutTransformCpuMs = 0.0;
	OutSubmitCpuMs = 0.0;
	const uint8 SanitizedFaceMask = SelectedFaceMask & 0x3f;
	const bool bFullCubeCapture = SanitizedFaceMask == 0x3f;
	USceneCaptureComponentCube* CaptureComponent = Record.CaptureComponent.Get();
	UTextureRenderTargetCube* InputTarget = Record.CaptureTarget.Get();
	UTextureRenderTargetCube* PublishedReferenceOwner = Record.RenderTarget.Get();
	UTextureRenderTargetCube* AlternateTarget =
		Record.AlternateRenderTarget.Get();
	UTextureRenderTargetCube* OutputTarget = Record.bCubeAADirectPublish
		? AlternateTarget
		: InputTarget;
	UWorld* World = ManagedWorld.Get();
	const bool bWorldSceneReady = IsValid(World) && World->Scene != nullptr;
	const bool bDirectTargetTopologyValid = Record.bCubeAADirectPublish
		&& IsValid(InputTarget)
		&& IsValid(PublishedReferenceOwner)
		&& IsValid(AlternateTarget)
		&& AlternateTarget != InputTarget
		&& (PublishedReferenceOwner == InputTarget
			|| PublishedReferenceOwner == AlternateTarget)
		&& InputTarget->bSupportsUAV
		&& PublishedReferenceOwner->bSupportsUAV
		&& AlternateTarget->bSupportsUAV;
	const bool bLegacyTargetTopologyValid = !Record.bCubeAADirectPublish
		&& IsValid(InputTarget)
		&& PublishedReferenceOwner == InputTarget
		&& !IsValid(AlternateTarget)
		&& !InputTarget->bSupportsUAV;
	const bool bTargetTopologyValid =
		bDirectTargetTopologyValid || bLegacyTargetTopologyValid;
	IWPRenderer* Renderer =
		IWPRenderer::Find();
	if (!IsFiniteVector(CaptureLocation) || SanitizedFaceMask == 0 || !IsValid(CaptureComponent)
		|| !IsValid(InputTarget)
		|| !IsValid(PublishedReferenceOwner)
		|| !IsValid(OutputTarget)
		|| !bTargetTopologyValid
		|| !bWorldSceneReady
		|| !CaptureComponent->IsRegistered()
		|| CaptureComponent->GetWorld() != World
		|| !CaptureComponent->IsVisible()
		|| !CaptureComponent->ShowFlags.Rendering
		|| static_cast<int32>(CaptureComponent->DetailMode) > World->GetDetailMode()
		|| Record.bCubeAADirectPublish != IsWPCubeAADirectPublishEnabled()
		|| OutputTarget->bSupportsUAV != Record.bCubeAADirectPublish
		|| CaptureComponent->TextureTarget != InputTarget
		|| Renderer == nullptr)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][Submit] Endpoint rejected before capture. Portal=%s OtherPortal=%s PositionMode=%s CaptureLocation=%s CaptureFinite=%d CaptureValid=%d InputTargetValid=%d StablePublishedReferenceOwnerValid=%d AlternateTargetValid=%d OutputTargetValid=%d DirectTopologyValid=%d LegacyTopologyValid=%d TargetTopologyValid=%d InputOutputDistinct=%d PublishedOwnerInPhysicalSet=%d WorldSceneReady=%d Registered=%d WorldMatches=%d Visible=%d ShowFlagRendering=%d DetailModeAllowed=%d TextureTargetMatchesInput=%d RendererAvailable=%d RecordDirectPublish=%d CurrentDirectPublish=%d InputUAV=%d PublishedOwnerUAV=%d OutputUAV=%d ResourceEpoch=%llu ResourceGeneration=%u CaptureGeneration=%u CaptureSubmitted=0 CpuMs=%.4f"),
			*GetNameSafe(Portal), *GetNameSafe(OtherPortal),
			PositionMode ? PositionMode : TEXT("Unknown"), *CaptureLocation.ToCompactString(),
			IsFiniteVector(CaptureLocation) ? 1 : 0,
			IsValid(CaptureComponent) ? 1 : 0,
			IsValid(InputTarget) ? 1 : 0,
			IsValid(PublishedReferenceOwner) ? 1 : 0,
			IsValid(AlternateTarget) ? 1 : 0,
			IsValid(OutputTarget) ? 1 : 0,
			bDirectTargetTopologyValid ? 1 : 0,
			bLegacyTargetTopologyValid ? 1 : 0,
			bTargetTopologyValid ? 1 : 0,
			InputTarget != OutputTarget ? 1 : 0,
			PublishedReferenceOwner == InputTarget
				|| PublishedReferenceOwner == AlternateTarget ? 1 : 0,
			bWorldSceneReady ? 1 : 0,
			IsValid(CaptureComponent) && CaptureComponent->IsRegistered() ? 1 : 0,
			IsValid(CaptureComponent) && CaptureComponent->GetWorld() == World ? 1 : 0,
			IsValid(CaptureComponent) && CaptureComponent->IsVisible() ? 1 : 0,
			IsValid(CaptureComponent) && CaptureComponent->ShowFlags.Rendering ? 1 : 0,
			IsValid(CaptureComponent) && IsValid(World)
				&& static_cast<int32>(CaptureComponent->DetailMode) <= World->GetDetailMode()
				? 1 : 0,
			IsValid(CaptureComponent) && CaptureComponent->TextureTarget == InputTarget ? 1 : 0,
			Renderer ? 1 : 0,
			Record.bCubeAADirectPublish ? 1 : 0,
			IsWPCubeAADirectPublishEnabled() ? 1 : 0,
			IsValid(InputTarget) && InputTarget->bSupportsUAV ? 1 : 0,
			IsValid(PublishedReferenceOwner)
				&& PublishedReferenceOwner->bSupportsUAV ? 1 : 0,
			IsValid(OutputTarget) && OutputTarget->bSupportsUAV ? 1 : 0,
			static_cast<unsigned long long>(Record.ResourceEpoch),
			Record.ResourceGeneration, Record.CaptureGeneration,
			(FPlatformTime::Seconds() - EndpointStartSeconds) * 1000.0);
		return false;
	}

	const double AAPreflightStartSeconds = FPlatformTime::Seconds();
	const bool bCubeAAPreflightReady = Renderer->CanEnqueueCubeAAPass(
		*InputTarget,
		*OutputTarget,
		*PublishedReferenceOwner,
		Record.bCubeAADirectPublish);
	const double AAPreflightCpuMs =
		(FPlatformTime::Seconds() - AAPreflightStartSeconds) * 1000.0;
	if (!bCubeAAPreflightReady)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][Submit] AA preflight rejected before capture. Portal=%s OtherPortal=%s PositionMode=%s InputTarget=%s OutputTarget=%s StablePublishedReferenceOwner=%s AAMode=%s ResourceEpoch=%llu ResourceGeneration=%u CaptureGeneration=%u CaptureSubmitted=0 PhysicalRoleSwapCommitted=0 PublishedObjectPointerStable=1 RawCaptureAvoided=1 AAPreflightCpuMs=%.4f EndpointCpuMs=%.4f"),
			*GetNameSafe(Portal), *GetNameSafe(OtherPortal),
			PositionMode ? PositionMode : TEXT("Unknown"),
			*GetNameSafe(InputTarget), *GetNameSafe(OutputTarget),
			*GetNameSafe(PublishedReferenceOwner),
			Record.bCubeAADirectPublish
				? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			static_cast<unsigned long long>(Record.ResourceEpoch),
			Record.ResourceGeneration, Record.CaptureGeneration,
			AAPreflightCpuMs,
			(FPlatformTime::Seconds() - EndpointStartSeconds) * 1000.0);
		return false;
	}

	// This is intentionally live-applied immediately before CaptureScene so an operator can
	// compare the none/Lumen-GI/Lumen-reflection modes without reallocating the cube or
	// changing its HDR SceneColor/PreExposure contract.
	(void)ApplyWPCubeLumenParity(*CaptureComponent);
	const FWPCaptureShowFlagState SubmitShowFlags =
		ApplyWPCaptureShowFlags(*CaptureComponent);
#if !UE_BUILD_SHIPPING
	if (SubmitShowFlags.bSettingsChanged)
	{
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][ShowFlags] Live setting change applied. Portal=%s Capture=%s CaptureGeneration=%u Scope=ManagedCubeSceneCaptureOnly PlayerGameViewChanged=0 LumenReflections=%d ScreenSpaceAO=%d CloudBundle=%d DynamicShadowsBundle=%d SkyLightingBundle=%d AtmosphereBundle=%d DeferredLightingBundle=%d LightingMaster=%d FogMaster=%d VolumetricFog=%d EffectiveScreenSpaceAO=%d EffectiveCloud=%d EffectiveSkyLighting=%d EffectiveDeferredLighting=%d EffectiveVolumetricFog=%d ResourceRecreated=0 ApplyCpuUs=%.2f"),
			*GetNameSafe(Portal),
			*GetNameSafe(CaptureComponent),
			Record.CaptureGeneration,
			SubmitShowFlags.bLumenReflections ? 1 : 0,
			SubmitShowFlags.bScreenSpaceAO ? 1 : 0,
			SubmitShowFlags.bCloud ? 1 : 0,
			SubmitShowFlags.bDynamicShadows ? 1 : 0,
			SubmitShowFlags.bSkyLighting ? 1 : 0,
			SubmitShowFlags.bAtmosphere ? 1 : 0,
			SubmitShowFlags.bDeferredLighting ? 1 : 0,
			SubmitShowFlags.bLighting ? 1 : 0,
			SubmitShowFlags.bFog ? 1 : 0,
			SubmitShowFlags.bVolumetricFog ? 1 : 0,
			SubmitShowFlags.bEffectiveScreenSpaceAO ? 1 : 0,
			SubmitShowFlags.bEffectiveCloud ? 1 : 0,
			SubmitShowFlags.bEffectiveSkyLighting ? 1 : 0,
			SubmitShowFlags.bEffectiveDeferredLighting ? 1 : 0,
			SubmitShowFlags.bEffectiveVolumetricFog ? 1 : 0,
			SubmitShowFlags.ApplyCpuUs);
	}
#endif
	const FWPCaptureDistanceLODState SubmitDistanceLOD =
		ApplyWPCaptureDistanceLOD(*CaptureComponent);
#if !UE_BUILD_SHIPPING
	if (SubmitDistanceLOD.bSettingsChanged)
	{
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][DistanceLOD] Live setting change applied. Portal=%s Capture=%s CaptureGeneration=%u RequestedMaxViewDistanceCm=%.2f AppliedMaxViewDistanceCm=%.2f RequestedLODDistanceFactor=%.3f AppliedLODDistanceFactor=%.3f RequestedFiniteFarPlane=%d AppliedFiniteFarPlane=%d ResourceRecreated=0 ApplyCpuUs=%.2f"),
			*GetNameSafe(Portal),
			*GetNameSafe(CaptureComponent),
			Record.CaptureGeneration,
			SubmitDistanceLOD.RequestedMaxViewDistanceCm,
			SubmitDistanceLOD.AppliedMaxViewDistanceCm,
			SubmitDistanceLOD.RequestedLODDistanceFactor,
			SubmitDistanceLOD.AppliedLODDistanceFactor,
			SubmitDistanceLOD.RequestedFiniteFarPlane,
			SubmitDistanceLOD.bAppliedFiniteFarPlane ? 1 : 0,
			SubmitDistanceLOD.ApplyCpuUs);
	}
#endif

	const double TransformStartSeconds = FPlatformTime::Seconds();
	CaptureComponent->SetWorldLocationAndRotation(CaptureLocation, FRotator::ZeroRotator);
	CaptureComponent->ClearHiddenComponents();
	CaptureComponent->HideActorComponents(Portal, true);
	if (OtherPortal)
	{
		CaptureComponent->HideActorComponents(OtherPortal, true);
	}
	OutTransformCpuMs = (FPlatformTime::Seconds() - TransformStartSeconds) * 1000.0;

	const double SubmitStartSeconds = FPlatformTime::Seconds();
	bool bCubeAAEnqueued = false;
	{
		SCOPE_CYCLE_COUNTER(STAT_WP_CubeCaptureSubmit);
		bool bCaptureSubmitted = false;
		double BuilderCreateCpuMs = 0.0;
		double BuilderExecuteCpuMs = 0.0;
		if (bFullCubeCapture)
		{
			CaptureComponent->CaptureScene();
			bCaptureSubmitted = true;
		}
		else
		{
			const double BuilderStartSeconds = FPlatformTime::Seconds();
			World->SendAllEndOfFrameUpdates();
			TUniquePtr<ISceneRenderBuilder> SceneRenderBuilder = ISceneRenderBuilder::Create(World->Scene);
			BuilderCreateCpuMs = (FPlatformTime::Seconds() - BuilderStartSeconds) * 1000.0;
			bCaptureSubmitted = SceneRenderBuilder
				&& Renderer->AddSelectedCubeCaptureRenderer(*CaptureComponent, SanitizedFaceMask, *SceneRenderBuilder);
			if (bCaptureSubmitted)
			{
				const double ExecuteStartSeconds = FPlatformTime::Seconds();
				SceneRenderBuilder->Execute();
				BuilderExecuteCpuMs = (FPlatformTime::Seconds() - ExecuteStartSeconds) * 1000.0;
			}
		}
		if (!bCaptureSubmitted)
		{
			OutSubmitCpuMs = (FPlatformTime::Seconds() - SubmitStartSeconds) * 1000.0;
			WP_LOG(this, Error,
				TEXT("[CaptureManager][SelectedCube] Submission failed. Portal=%s FaceMask=0x%02x FullCube=%d RendererAvailable=%d GameThreadWait=0 BuilderCreateCpuMs=%.4f SubmitCpuMs=%.4f"),
				*GetNameSafe(Portal), static_cast<uint32>(SanitizedFaceMask), bFullCubeCapture ? 1 : 0,
				Renderer ? 1 : 0, BuilderCreateCpuMs, OutSubmitCpuMs);
			return false;
		}
		INC_DWORD_STAT(STAT_WP_CubeCapturesSubmitted);
		int32 SelectedFaceCount = 0;
		for (uint32 FaceIndex = 0; FaceIndex < WPCaptureFaceCount; ++FaceIndex)
		{
			SelectedFaceCount += (SanitizedFaceMask & static_cast<uint8>(1u << FaceIndex)) != 0 ? 1 : 0;
		}
		INC_FLOAT_STAT_BY(
			STAT_WP_CubeCaptureMegapixels,
			static_cast<double>(Record.CaptureResolution) * Record.CaptureResolution
				* SelectedFaceCount / 1'000'000.0);
		bCubeAAEnqueued = Renderer->EnqueueCubeAAPass(
			*InputTarget,
			*OutputTarget,
			*PublishedReferenceOwner,
			Record.bCubeAADirectPublish,
			SanitizedFaceMask);
		ensureAlwaysMsgf(
			bCubeAAEnqueued,
			TEXT("CubeAA enqueue failed after successful same-submission preflight. ")
			TEXT("Portal=%s Input=%s Output=%s PublishedReferenceOwner=%s"),
			*GetNameSafe(Portal),
			*GetNameSafe(InputTarget),
			*GetNameSafe(OutputTarget),
			*GetNameSafe(PublishedReferenceOwner));
		if (bCubeAAEnqueued)
		{
			// The public/published UObject never changes. Only the two physical cube roles
			// alternate; the Render Thread retargets the stable texture reference after AA.
			if (Record.bCubeAADirectPublish)
			{
				Record.CaptureTarget = OutputTarget;
				Record.AlternateRenderTarget = InputTarget;
				CaptureComponent->TextureTarget = OutputTarget;
			}
			INC_DWORD_STAT(STAT_WP_CubeAAPassesSubmitted);
			if (Record.bCubeAADirectPublish)
			{
				++CubeAADirectPublishCount;
				CubeAACopyBackLogicalBytesAvoided +=
					GetWPCubeColorBytes(Record.CaptureResolution) * 2ull;
			}
			++Record.CaptureGeneration;
			Record.InitialValidFaceMask |= SanitizedFaceMask;
			WP_LOG(this, VeryVerbose,
				TEXT("[CaptureManager][HybridCube] Completed. Portal=%s Resolution=%u Route=%s FaceMask=0x%02x Faces=%d InitialValidMask=0x%02x CaptureGeneration=%u ViewFamilies=1 SceneRenderers=1 AAEnqueued=1 UnselectedSlicesPreserved=%d GameThreadWait=0 BuilderCreateCpuMs=%.4f BuilderExecuteCpuMs=%.4f"),
				*GetNameSafe(Portal), Record.CaptureResolution,
				bFullCubeCapture ? TEXT("NativeCubeSinglePass") : TEXT("SelectedCubeSinglePass"),
				static_cast<uint32>(SanitizedFaceMask), SelectedFaceCount,
				static_cast<uint32>(Record.InitialValidFaceMask), Record.CaptureGeneration,
				bFullCubeCapture ? 0 : 1, BuilderCreateCpuMs, BuilderExecuteCpuMs);
		}
	}
	OutSubmitCpuMs = (FPlatformTime::Seconds() - SubmitStartSeconds) * 1000.0;
	if (!bCubeAAEnqueued)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][Submit] Endpoint capture completed but AA enqueue failed after successful preflight. Portal=%s OtherPortal=%s PositionMode=%s InputTarget=%s OutputTarget=%s StablePublishedReferenceOwner=%s CurrentCaptureTarget=%s CurrentAlternateTarget=%s AAMode=%s ResourceEpoch=%llu ResourceGeneration=%u CaptureGenerationUnchanged=%u CaptureSubmitted=1 AAPreflightSucceeded=1 InvariantViolation=1 PhysicalRoleSwapCommitted=0 PublishedObjectPointerStable=1 PublishedContentSafetyUnknown=1 FilteredFallbackAvailable=0 PublishedContentAdvancedUnknown=1 CopyBackEliminated=0 RetryRequired=1 AAPreflightCpuMs=%.4f TransformCpuMs=%.4f SubmitCpuMs=%.4f EndpointCpuMs=%.4f"),
			*GetNameSafe(Portal), *GetNameSafe(OtherPortal),
			PositionMode ? PositionMode : TEXT("Unknown"),
			*GetNameSafe(InputTarget), *GetNameSafe(OutputTarget),
			*GetNameSafe(PublishedReferenceOwner),
			*GetNameSafe(Record.CaptureTarget.Get()),
			*GetNameSafe(Record.AlternateRenderTarget.Get()),
			Record.bCubeAADirectPublish
				? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			static_cast<unsigned long long>(Record.ResourceEpoch),
			Record.ResourceGeneration, Record.CaptureGeneration,
			AAPreflightCpuMs, OutTransformCpuMs, OutSubmitCpuMs,
			(FPlatformTime::Seconds() - EndpointStartSeconds) * 1000.0);
		return false;
	}

	return true;
}

bool UWPCaptureManager::SubmitPairCapture(
	const FGuid& PairId,
	AWormholePortalActor* PortalA,
	AWormholePortalActor* PortalB,
	const uint64 ExpectedOwnershipEpoch,
	const EWPManagedCaptureSubmissionMode SubmissionMode,
	FWPManagedPairCaptureResult& OutResult,
	const uint8 SelectedFaceMask)
{
	// 로그 전용: pair capture 전체 CPU 시간을 결과 telemetry에 기록합니다.
	const double PairStartSeconds = FPlatformTime::Seconds();
	OutResult = FWPManagedPairCaptureResult();
	OutResult.SubmissionMode = SubmissionMode;
	const uint8 SanitizedFaceMask = SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair
		? 0x3f : static_cast<uint8>(SelectedFaceMask & 0x3f);
	FPairCaptureState* PairState = PairCaptureStates.Find(PairId);
	const bool bTopologyMatches = PairState
		&& ((PairState->PortalA.Get() == PortalA && PairState->PortalB.Get() == PortalB)
			|| (PairState->PortalA.Get() == PortalB && PairState->PortalB.Get() == PortalA));
	const bool bStaggeredSubmission =
		SubmissionMode != EWPManagedCaptureSubmissionMode::AtomicPair;
	const uint8 SelectedEndpointMask =
		SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA
			? WPStaggeredEndpointAMask
			: (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB
				? WPStaggeredEndpointBMask
				: 0);
	const bool bRepeatedStaggeredSide = PairState && bStaggeredSubmission
		&& (PairState->StaggeredCompletionMask & SelectedEndpointMask) != 0;
	if (!PairState || !PairState->bAuthorityEnabled || ExpectedOwnershipEpoch == 0 || SanitizedFaceMask == 0
		|| PairState->OwnershipEpoch != ExpectedOwnershipEpoch || !bTopologyMatches
		|| PairState->LastSubmissionFrame == GFrameCounter
		|| bRepeatedStaggeredSide)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager][Submit] Pair rejected. PairId=%s PortalA=%s PortalB=%s SubmissionMode=%s PairStateFound=%d AuthorityEnabled=%d ExpectedEpoch=%llu EffectiveEpoch=%llu TopologyMatches=%d LastSubmissionFrame=%llu Frame=%llu StaggeredCompletionMask=%u RepeatedStaggeredSide=%d TransitForcesAtomic=0 TransitForcedSequence=0 Reason=AuthorityFrameOrModeGuard CpuMs=%.4f"),
			*PairId.ToString(EGuidFormats::DigitsWithHyphensLower), *GetNameSafe(PortalA),
			*GetNameSafe(PortalB), GetWPCaptureSubmissionModeName(SubmissionMode),
			PairState ? 1 : 0,
			PairState && PairState->bAuthorityEnabled ? 1 : 0,
			static_cast<unsigned long long>(ExpectedOwnershipEpoch),
			static_cast<unsigned long long>(PairState ? PairState->OwnershipEpoch : 0),
			bTopologyMatches ? 1 : 0,
			static_cast<unsigned long long>(PairState ? PairState->LastSubmissionFrame : 0),
			static_cast<unsigned long long>(GFrameCounter),
			static_cast<uint32>(PairState ? PairState->StaggeredCompletionMask : 0),
			bRepeatedStaggeredSide ? 1 : 0,
			(FPlatformTime::Seconds() - PairStartSeconds) * 1000.0);
#endif
		return false;
	}

	FEndpointRecord* RecordA = nullptr;
	FEndpointRecord* RecordB = nullptr;
	FString IgnoredFailureReason;
	const bool bEndpointAReady = ValidateEndpointForSubmission(
		PortalA, RecordA, IgnoredFailureReason);
	const bool bEndpointBReady = ValidateEndpointForSubmission(
		PortalB, RecordB, IgnoredFailureReason);
	if (!bEndpointAReady || !bEndpointBReady)
	{
		return false;
	}

	PairState->LastSubmissionFrame = GFrameCounter;
	OutResult.StaggeredCompletionMaskBefore = PairState->StaggeredCompletionMask;
	OutResult.PreviousSubmissionFrameA = PairState->LastSubmissionFrameA;
	OutResult.PreviousSubmissionFrameB = PairState->LastSubmissionFrameB;
	OutResult.CaptureGenerationABefore = RecordA->CaptureGeneration;
	OutResult.CaptureGenerationBBefore = RecordB->CaptureGeneration;
	OutResult.PairCaptureEpochBefore = PairState->PairCaptureEpoch;

	FVector CameraLocation = FVector::ZeroVector;
	OutResult.bCameraAvailable = GetPrimaryViewCameraLocation(ManagedWorld.Get(), CameraLocation);
	FVector CaptureLocationA = PortalA->GetActorLocation();
	FVector CaptureLocationB = PortalB->GetActorLocation();
	// 로그 전용: 각 capture 위치가 선택된 이유를 설명하는 label입니다.
	const TCHAR* PositionModeA = TEXT("FallbackPortalCenter");
	const TCHAR* PositionModeB = TEXT("FallbackPortalCenter");
	AWormholePortalActor* ActiveSourcePortal = nullptr;
	bool bMappingValid = false;
	bool bClearTransitStateAfterCapture = false;

	auto AssignLocations = [PortalA, PortalB, &CaptureLocationA, &CaptureLocationB,
		&PositionModeA, &PositionModeB](
		AWormholePortalActor* FirstPortal, const FVector& FirstLocation, const TCHAR* FirstMode,
		AWormholePortalActor* SecondPortal, const FVector& SecondLocation, const TCHAR* SecondMode)
	{
		if (FirstPortal == PortalA && SecondPortal == PortalB)
		{
			CaptureLocationA = FirstLocation;
			PositionModeA = FirstMode;
			CaptureLocationB = SecondLocation;
			PositionModeB = SecondMode;
			return true;
		}
		if (FirstPortal == PortalB && SecondPortal == PortalA)
		{
			CaptureLocationB = FirstLocation;
			PositionModeB = FirstMode;
			CaptureLocationA = SecondLocation;
			PositionModeA = SecondMode;
			return true;
		}
		return false;
	};

	if (PairState->FirstPersonPhase != EFirstPersonTransitParallaxPhase::Idle)
	{
		AWormholePortalActor* TransitSource = PairState->FirstPersonSourcePortal.Get();
		AWormholePortalActor* TransitDestination = PairState->FirstPersonDestinationPortal.Get();
		FVector FirstPersonCameraLocation = FVector::ZeroVector;
		const TCHAR* ViewReason = TEXT("Unknown");
		const bool bFirstPersonView = ResolveFirstPersonLocalView(
			ManagedWorld.Get(), PairState->FirstPersonActor.Get(), FirstPersonCameraLocation, ViewReason);
		const bool bStateMatchesPair = (TransitSource == PortalA && TransitDestination == PortalB)
			|| (TransitSource == PortalB && TransitDestination == PortalA);
		if (bStateMatchesPair && PairState->bFirstPersonPathMappingValid && bFirstPersonView)
		{
			CameraLocation = FirstPersonCameraLocation;
			OutResult.bCameraAvailable = true;
			if (PairState->FirstPersonPhase == EFirstPersonTransitParallaxPhase::CommitPending)
			{
				FVector VirtualSourceCameraLocation = FVector::ZeroVector;
				bMappingValid = UnmapCameraAlongTransitPath(
					CameraLocation, PairState->FirstPersonMapping,
					PairState->FirstPersonEntryPointWorld,
					PairState->FirstPersonSelectedPlane, VirtualSourceCameraLocation);
				if (bMappingValid)
				{
					bMappingValid = AssignLocations(
						TransitDestination, CameraLocation, TEXT("TransitCommitDestinationCamera"),
						TransitSource, VirtualSourceCameraLocation,
						TEXT("TransitCommitInverseSourceCamera"));
				}
				ActiveSourcePortal = TransitDestination;
				bClearTransitStateAfterCapture = bMappingValid;
			}
			else
			{
				FVector DestinationCameraLocation = FVector::ZeroVector;
				bMappingValid = MapCameraAlongTransitPath(
					CameraLocation, PairState->FirstPersonMapping,
					PairState->FirstPersonEntryPointWorld,
					PairState->FirstPersonSelectedPlane, DestinationCameraLocation);
				if (bMappingValid)
				{
					const bool bCancelled = PairState->FirstPersonPhase
						== EFirstPersonTransitParallaxPhase::CancelPending;
					bMappingValid = AssignLocations(
						TransitSource, CameraLocation,
						bCancelled ? TEXT("TransitCancelSourceCamera") : TEXT("TransitCrossingSourceCamera"),
						TransitDestination, DestinationCameraLocation,
						bCancelled ? TEXT("TransitCancelMappedDestinationCamera")
							: TEXT("TransitCrossingMappedDestinationCamera"));
					bClearTransitStateAfterCapture = bCancelled && bMappingValid;
				}
				ActiveSourcePortal = TransitSource;
			}
			OutResult.bUsedTransitSynchronizedFirstPerson = bMappingValid;
			if (bMappingValid)
			{
				PairState->ActiveCameraRelativeSourcePortal = ActiveSourcePortal;
			}
		}
		if (!OutResult.bUsedTransitSynchronizedFirstPerson)
		{
			WP_LOG(this, Warning,
				TEXT("[CaptureManager][TransitParallax] Active state abandoned before capture. PairId=%s Sequence=%llu Actor=%s Source=%s Destination=%s StateMatchesPair=%d PathMappingValid=%d FirstPersonView=%d ViewReason=%s MappingValid=%d CpuMs=%.4f"),
				*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
				static_cast<unsigned long long>(PairState->FirstPersonSequence),
				*GetNameSafe(PairState->FirstPersonActor.Get()), *GetNameSafe(TransitSource),
				*GetNameSafe(TransitDestination), bStateMatchesPair ? 1 : 0,
				PairState->bFirstPersonPathMappingValid ? 1 : 0, bFirstPersonView ? 1 : 0,
				ViewReason, bMappingValid ? 1 : 0,
				(FPlatformTime::Seconds() - PairStartSeconds) * 1000.0);
			ResetFirstPersonTransitState(*PairState, TEXT("RuntimeValidationFailed"), false);
		}
	}

	if (!OutResult.bUsedTransitSynchronizedFirstPerson && OutResult.bCameraAvailable)
	{
		const float DistanceA = FVector::Dist(CameraLocation, PortalA->GetActorLocation());
		const float DistanceB = FVector::Dist(CameraLocation, PortalB->GetActorLocation());
		AWormholePortalActor* NearestPortal = DistanceA <= DistanceB ? PortalA : PortalB;
		AWormholePortalActor* PreviousSource = PairState->ActiveCameraRelativeSourcePortal.Get();
		if ((PreviousSource == PortalA || PreviousSource == PortalB) && NearestPortal != PreviousSource)
		{
			const float PreviousDistance = FVector::Dist(CameraLocation, PreviousSource->GetActorLocation());
			const float ChallengerDistance = FVector::Dist(CameraLocation, NearestPortal->GetActorLocation());
			if (ChallengerDistance + WPSourceSwitchHysteresisCm >= PreviousDistance)
			{
				NearestPortal = PreviousSource;
			}
		}
		ActiveSourcePortal = NearestPortal;
		PairState->ActiveCameraRelativeSourcePortal = NearestPortal;
		AWormholePortalActor* RemotePortal = NearestPortal == PortalA ? PortalB : PortalA;

		const FVector NearestPortalCenter = NearestPortal->GetActorLocation();
		const double PortalRadiusCm = static_cast<double>(NearestPortal->GetPortalRadius());
		const double OuterRadiusCm = static_cast<double>(NearestPortal->GetTransitionRadius());
		const double SafeProxyRadiusCm = FMath::Max(
			OuterRadiusCm,
			PortalRadiusCm + WPCaptureProxySafetyShellCm);
		const FVector CenterToCamera = CameraLocation - NearestPortalCenter;
		const double CameraDistanceCm = CenterToCamera.Size();
		const bool bSafeProxyContractValid = IsFiniteVector(NearestPortalCenter)
			&& FMath::IsFinite(PortalRadiusCm)
			&& FMath::IsFinite(OuterRadiusCm)
			&& FMath::IsFinite(SafeProxyRadiusCm) && SafeProxyRadiusCm > 0.0
			&& FMath::IsFinite(CameraDistanceCm);

		FVector NearestCaptureLocation = CameraLocation;
		bool bNearestCaptureClamped = false;
		if (bSafeProxyContractValid
			&& CameraDistanceCm > SafeProxyRadiusCm
			&& !CenterToCamera.IsNearlyZero())
		{
			NearestCaptureLocation = NearestPortalCenter
				+ CenterToCamera.GetSafeNormal() * SafeProxyRadiusCm;
			bNearestCaptureClamped = true;
		}

		const FVector RemoteCaptureLocation = RemotePortal->GetActorLocation();
		bMappingValid = IsFiniteVector(NearestCaptureLocation)
			&& IsFiniteVector(RemoteCaptureLocation);
		const TCHAR* NearestPositionMode = !bSafeProxyContractValid
			? TEXT("NearestPlayerCameraInvalidSafeProxy")
			: (bNearestCaptureClamped
				? TEXT("NearestCameraClampedToSafeProxy")
				: TEXT("NearestPlayerCameraInsideSafeProxy"));

		AssignLocations(
			NearestPortal, NearestCaptureLocation, NearestPositionMode,
			RemotePortal, RemoteCaptureLocation,
			bMappingValid ? TEXT("RemotePortalCenter") : TEXT("InvalidRemotePortalCenter"));

		if (!bSafeProxyContractValid)
		{
			WP_LOG(this, Warning,
				TEXT("[CaptureManager][NearestCameraClamp] Invalid SafeProxy contract; preserving PlayerCamera location. PairId=%s NearestPortal=%s PortalRadiusCm=%.3f OuterRadiusCm=%.3f SafeProxyRadiusCm=%.3f CameraDistanceCm=%.3f"),
				*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
				*GetNameSafe(NearestPortal), PortalRadiusCm, OuterRadiusCm,
				SafeProxyRadiusCm, CameraDistanceCm);
		}
	}

	if (!IsFiniteVector(CaptureLocationA) || !IsFiniteVector(CaptureLocationB))
	{
		CaptureLocationA = PortalA->GetActorLocation();
		CaptureLocationB = PortalB->GetActorLocation();
		PositionModeA = TEXT("FallbackNonFinitePortalCenter");
		PositionModeB = TEXT("FallbackNonFinitePortalCenter");
		OutResult.bUsedTransitSynchronizedFirstPerson = false;
		bClearTransitStateAfterCapture = false;
	}

	double TransformCpuMsA = 0.0;
	double TransformCpuMsB = 0.0;
	double SubmitCpuMsA = 0.0;
	double SubmitCpuMsB = 0.0;
	switch (SubmissionMode)
	{
	case EWPManagedCaptureSubmissionMode::AtomicPair:
		OutResult.bSubmittedA = SubmitEndpointCapture(
			*RecordA, PortalA, PortalB, CaptureLocationA, PositionModeA,
			TransformCpuMsA, SubmitCpuMsA, 0x3f);
		OutResult.bSubmittedB = OutResult.bSubmittedA && SubmitEndpointCapture(
			*RecordB, PortalB, PortalA, CaptureLocationB, PositionModeB,
			TransformCpuMsB, SubmitCpuMsB, 0x3f);
		OutResult.SubmittedFaceMaskA = OutResult.bSubmittedA ? 0x3f : 0;
		OutResult.SubmittedFaceMaskB = OutResult.bSubmittedB ? 0x3f : 0;
		break;
	case EWPManagedCaptureSubmissionMode::EndpointA:
		OutResult.bSubmittedA = SubmitEndpointCapture(
			*RecordA, PortalA, PortalB, CaptureLocationA, PositionModeA,
			TransformCpuMsA, SubmitCpuMsA, SanitizedFaceMask);
		OutResult.SubmittedFaceMaskA = OutResult.bSubmittedA ? SanitizedFaceMask : 0;
		break;
	case EWPManagedCaptureSubmissionMode::EndpointB:
		OutResult.bSubmittedB = SubmitEndpointCapture(
			*RecordB, PortalB, PortalA, CaptureLocationB, PositionModeB,
			TransformCpuMsB, SubmitCpuMsB, SanitizedFaceMask);
		OutResult.SubmittedFaceMaskB = OutResult.bSubmittedB ? SanitizedFaceMask : 0;
		break;
	default:
		break;
	}
	OutResult.TransformCpuMs = TransformCpuMsA + TransformCpuMsB;
	OutResult.SubmitCpuMs = SubmitCpuMsA + SubmitCpuMsB;
	OutResult.CaptureGenerationAAfter = RecordA->CaptureGeneration;
	OutResult.CaptureGenerationBAfter = RecordB->CaptureGeneration;
	if (OutResult.bSubmittedA)
	{
		PairState->LastSubmissionFrameA = GFrameCounter;
		OutResult.SubmissionFrameA = GFrameCounter;
	}
	if (OutResult.bSubmittedB)
	{
		PairState->LastSubmissionFrameB = GFrameCounter;
		OutResult.SubmissionFrameB = GFrameCounter;
	}

	const bool bSelectedSubmissionSucceeded =
		(SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair
			&& OutResult.bSubmittedA && OutResult.bSubmittedB)
		|| (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointA
			&& OutResult.bSubmittedA)
		|| (SubmissionMode == EWPManagedCaptureSubmissionMode::EndpointB
			&& OutResult.bSubmittedB);
	if (bSelectedSubmissionSucceeded)
	{
		if (SubmissionMode == EWPManagedCaptureSubmissionMode::AtomicPair)
		{
			++PairState->PairCaptureEpoch;
			if (PairState->PairCaptureEpoch == 0)
			{
				++PairState->PairCaptureEpoch;
			}
			PairState->StaggeredCompletionMask = 0;
			OutResult.bPairCycleCompleted = true;
		}
		else
		{
			PairState->StaggeredCompletionMask |= SelectedEndpointMask;
			if (PairState->StaggeredCompletionMask == WPStaggeredCompleteMask)
			{
				++PairState->PairCaptureEpoch;
				if (PairState->PairCaptureEpoch == 0)
				{
					++PairState->PairCaptureEpoch;
				}
				PairState->StaggeredCompletionMask = 0;
				OutResult.bPairCycleCompleted = true;
			}
		}
		const uint64 ExpectedPairEpochAfter = OutResult.bPairCycleCompleted
			? (OutResult.PairCaptureEpochBefore == MAX_uint64
				? uint64(1)
				: OutResult.PairCaptureEpochBefore + 1)
			: OutResult.PairCaptureEpochBefore;
		OutResult.bPairEpochCoherent =
			PairState->PairCaptureEpoch == ExpectedPairEpochAfter;
		// Transit은 위치 계산에만 사용합니다. 일반 cadence의 staggered A/B도 한 cycle을
		// 완료하면 terminal 상태를 정리하며, atomic capture를 별도로 강제하지 않습니다.
		if (bClearTransitStateAfterCapture && OutResult.bPairCycleCompleted)
		{
			PairState->ActiveCameraRelativeSourcePortal = ActiveSourcePortal;
			ResetFirstPersonTransitState(*PairState, TEXT("TerminalPairCaptured"), false);
		}
	}
	OutResult.StaggeredCompletionMaskAfter = PairState->StaggeredCompletionMask;
	OutResult.PairCaptureEpochAfter = PairState->PairCaptureEpoch;
	OutResult.TotalCpuMs = (FPlatformTime::Seconds() - PairStartSeconds) * 1000.0;
	const bool bSuccess = OutResult.WasSuccessful();
	if (!bSuccess)
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager][Submit] Submission contract failed. PairId=%s SubmissionMode=%s GenerationA=%u->%u GenerationB=%u->%u PairEpoch=%llu->%llu PairCycleCompleted=%d StaggeredMask=%u->%u SubmittedA=%d SubmittedB=%d PairEpochCoherent=%d CpuMs=%.4f"),
			*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
			GetWPCaptureSubmissionModeName(SubmissionMode),
			OutResult.CaptureGenerationABefore, OutResult.CaptureGenerationAAfter,
			OutResult.CaptureGenerationBBefore, OutResult.CaptureGenerationBAfter,
			static_cast<unsigned long long>(OutResult.PairCaptureEpochBefore),
			static_cast<unsigned long long>(OutResult.PairCaptureEpochAfter),
			OutResult.bPairCycleCompleted ? 1 : 0,
			static_cast<uint32>(OutResult.StaggeredCompletionMaskBefore),
			static_cast<uint32>(OutResult.StaggeredCompletionMaskAfter),
			OutResult.bSubmittedA ? 1 : 0, OutResult.bSubmittedB ? 1 : 0,
			OutResult.bPairEpochCoherent ? 1 : 0, OutResult.TotalCpuMs);
	}
	return bSuccess;
}

bool UWPCaptureManager::RemovePairCaptureState(
	const FGuid& PairId,
	const TCHAR* Reason)
{
	FPairCaptureState* PairState = PairCaptureStates.Find(PairId);
	if (!PairState)
	{
		return false;
	}
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	const uint64 PreviousEpoch = PairState->OwnershipEpoch;
	const uint64 PreviousPairCaptureEpoch = PairState->PairCaptureEpoch;
	const uint64 PreviousTransitSequence = PairState->FirstPersonSequence;
#endif
	ResetFirstPersonTransitState(*PairState, TEXT("PairStateRemoved"));
	PairCaptureStates.Remove(PairId);
#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[CaptureManager][PairLifetime] Pair state removed. PairId=%s PreviousOwnershipEpoch=%llu PreviousPairCaptureEpoch=%llu PreviousTransitSequence=%llu RemainingPairStates=%d Reason=%s CpuMs=%.4f"),
		*PairId.ToString(EGuidFormats::DigitsWithHyphensLower),
		static_cast<unsigned long long>(PreviousEpoch),
		static_cast<unsigned long long>(PreviousPairCaptureEpoch),
		static_cast<unsigned long long>(PreviousTransitSequence), PairCaptureStates.Num(),
		Reason ? Reason : TEXT("Unspecified"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

double UWPCaptureManager::GetEstimatedResidentColorMemoryMiB() const
{
	double TotalMiB = 0.0;
	const auto AccumulateRecordMap = [&TotalMiB](
		const TMap<TWeakObjectPtr<AWormholePortalActor>, FEndpointRecord>& Records)
	{
		for (const TPair<TWeakObjectPtr<AWormholePortalActor>, FEndpointRecord>& Entry
			: Records)
		{
			const UTextureRenderTargetCube* RenderTarget =
				Entry.Value.RenderTarget.Get();
			const UTextureRenderTargetCube* CaptureTarget =
				Entry.Value.CaptureTarget.Get();
			const UTextureRenderTargetCube* AlternateTarget =
				Entry.Value.AlternateRenderTarget.Get();
			if (IsValid(RenderTarget))
			{
				TotalMiB += GetWPEndpointColorMemoryMiB(
					Entry.Value.CaptureResolution);
			}
			if (IsValid(AlternateTarget) && AlternateTarget != RenderTarget)
			{
				TotalMiB += GetWPEndpointColorMemoryMiB(
					Entry.Value.CaptureResolution);
			}
			// Corrupt/incomplete records are still reported accurately until repair.
			if (IsValid(CaptureTarget)
				&& CaptureTarget != RenderTarget
				&& CaptureTarget != AlternateTarget)
			{
				TotalMiB += GetWPEndpointColorMemoryMiB(
					Entry.Value.CaptureResolution);
			}
		}
	};
	AccumulateRecordMap(EndpointRecords);
	AccumulateRecordMap(RetiredResolutionEndpointRecords);
	for (const TPair<TWeakObjectPtr<AWormholePortalActor>,
		FPendingEndpointAllocation>& Entry : PendingEndpointAllocations)
	{
		if (IsValid(Entry.Value.CurrentTarget.Get()))
		{
			TotalMiB += GetWPEndpointColorMemoryMiB(
				Entry.Value.CaptureResolution);
		}
		if (IsValid(Entry.Value.AlternateTarget.Get()))
		{
			TotalMiB += GetWPEndpointColorMemoryMiB(
				Entry.Value.CaptureResolution);
		}
	}
	return TotalMiB;
}

void UWPCaptureManager::Shutdown(const TCHAR* Reason)
{
	// 로그 전용: shutdown 전 상태와 전체 CPU 시간을 최종 로그에 보존합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	const int32 PairStatesBeforeShutdown = PairCaptureStates.Num();
	PairCaptureStates.Reset();
	for (TPair<TWeakObjectPtr<AWormholePortalActor>,
		FPendingEndpointAllocation>& PendingEntry : PendingEndpointAllocations)
	{
		ReleasePendingAllocation(
			PendingEntry.Value, TEXT("ManagerShutdownPendingAllocation"));
	}
	PendingEndpointAllocations.Reset();
	RetiredPublicationOverrides.Reset();

	TArray<TWeakObjectPtr<AWormholePortalActor>> PortalKeys;
	EndpointRecords.GetKeys(PortalKeys);
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalKey : PortalKeys)
	{
		if (FEndpointRecord* Record = EndpointRecords.Find(PortalKey))
		{
			ReleaseRecord(*Record, Reason ? Reason : TEXT("ManagerShutdown"));
		}
		EndpointRecords.Remove(PortalKey);
	}
	TArray<TWeakObjectPtr<AWormholePortalActor>> RetiredPortalKeys;
	RetiredResolutionEndpointRecords.GetKeys(RetiredPortalKeys);
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalKey : RetiredPortalKeys)
	{
		if (FEndpointRecord* Record =
			RetiredResolutionEndpointRecords.Find(PortalKey))
		{
			ReleaseRecord(
				*Record,
				Reason ? Reason : TEXT("ManagerShutdownRetiredResolution"));
		}
		RetiredResolutionEndpointRecords.Remove(PortalKey);
	}
	SET_DWORD_STAT(STAT_WP_CubeCaptureTargets, 0);
	SET_FLOAT_STAT(STAT_WP_CubeRenderTargetMiB, 0.0f);
	ManagedCaptureComponents.Reset();
	ManagedRenderTargets.Reset();
	bInitialized = false;

	const TCHAR* EffectiveReason = Reason ? Reason : TEXT("Unspecified");
	const double ShutdownCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	if (AllocationCount == ReleaseCount
		&& CubeTargetAllocationCount == CubeTargetReleaseCount)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[CaptureManager] Shutdown complete. World=%s Endpoints=%d PairStatesBefore=%d PairStatesAfter=%d StrongComponents=%d StrongRenderTargets=%d Allocations=%llu Releases=%llu Balance=0 TargetAllocations=%llu TargetReleases=%llu TargetBalance=%lld AllocationFailures=%llu DirectPublishCount=%llu CopyBackLogicalTrafficGiBAvoided=%.3f EstimatedResidentColorMemoryMiB=%.2f MemoryAccounting=Cube6FacesPlusPersistent2DRenderSurface TotalAllocationCpuMs=%.3f TotalReleaseCpuMs=%.3f TotalTargetAllocationCpuMs=%.3f TotalTargetReleaseCpuMs=%.3f Reason=%s CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), EndpointRecords.Num(), PairStatesBeforeShutdown,
			PairCaptureStates.Num(), ManagedCaptureComponents.Num(), ManagedRenderTargets.Num(),
			static_cast<unsigned long long>(AllocationCount),
			static_cast<unsigned long long>(ReleaseCount),
			static_cast<unsigned long long>(CubeTargetAllocationCount),
			static_cast<unsigned long long>(CubeTargetReleaseCount),
			static_cast<long long>(CubeTargetAllocationCount)
				- static_cast<long long>(CubeTargetReleaseCount),
			static_cast<unsigned long long>(AllocationFailureCount),
			static_cast<unsigned long long>(CubeAADirectPublishCount),
			static_cast<double>(CubeAACopyBackLogicalBytesAvoided)
				/ (1024.0 * 1024.0 * 1024.0),
			GetEstimatedResidentColorMemoryMiB(), TotalAllocationCpuMs, TotalReleaseCpuMs,
			TotalCubeTargetAllocationCpuMs,
			TotalCubeTargetReleaseCpuMs,
			EffectiveReason, ShutdownCpuMs);
#endif
	}
	else
	{
		WP_LOG(this, Error,
			TEXT("[CaptureManager] Shutdown imbalance. World=%s Endpoints=%d PairStatesBefore=%d PairStatesAfter=%d StrongComponents=%d StrongRenderTargets=%d Allocations=%llu Releases=%llu Balance=%lld TargetAllocations=%llu TargetReleases=%llu TargetBalance=%lld AllocationFailures=%llu DirectPublishCount=%llu CopyBackLogicalTrafficGiBAvoided=%.3f EstimatedResidentColorMemoryMiB=%.2f MemoryAccounting=Cube6FacesPlusPersistent2DRenderSurface Reason=%s CpuMs=%.3f"),
			*GetNameSafe(ManagedWorld.Get()), EndpointRecords.Num(), PairStatesBeforeShutdown,
			PairCaptureStates.Num(), ManagedCaptureComponents.Num(), ManagedRenderTargets.Num(),
			static_cast<unsigned long long>(AllocationCount),
			static_cast<unsigned long long>(ReleaseCount),
			static_cast<long long>(AllocationCount) - static_cast<long long>(ReleaseCount),
			static_cast<unsigned long long>(CubeTargetAllocationCount),
			static_cast<unsigned long long>(CubeTargetReleaseCount),
			static_cast<long long>(CubeTargetAllocationCount)
				- static_cast<long long>(CubeTargetReleaseCount),
			static_cast<unsigned long long>(AllocationFailureCount),
			static_cast<unsigned long long>(CubeAADirectPublishCount),
			static_cast<double>(CubeAACopyBackLogicalBytesAvoided)
				/ (1024.0 * 1024.0 * 1024.0),
			GetEstimatedResidentColorMemoryMiB(), EffectiveReason, ShutdownCpuMs);
	}
	ManagedWorld.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPManagedPairCaptureAtomicityTest,
	"WormholePortal.Runtime.ManagedPairCaptureAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPManagedPairCaptureAtomicityTest::RunTest(const FString& Parameters)
{
	const double StartSeconds = FPlatformTime::Seconds();
	(void)Parameters;

	FWPManagedPairCaptureResult CoherentResult;
	CoherentResult.CaptureGenerationABefore = 41;
	CoherentResult.CaptureGenerationAAfter = 42;
	CoherentResult.CaptureGenerationBBefore = 89;
	CoherentResult.CaptureGenerationBAfter = 90;
	CoherentResult.bSubmittedA = true;
	CoherentResult.bSubmittedB = true;
	CoherentResult.bPairEpochCoherent = true;
	CoherentResult.bPairCycleCompleted = true;
	TestTrue(TEXT("Manager-owned successful pair submission requires exact +1/+1 generations"),
		CoherentResult.WasSuccessful());
	CoherentResult.CaptureGenerationBAfter = 91;
	TestFalse(TEXT("Generation skips fail the manager-owned pair atomicity contract"),
		CoherentResult.WasSuccessful());
	CoherentResult.CaptureGenerationBAfter = 90;
	CoherentResult.bPairEpochCoherent = false;
	TestFalse(TEXT("An incoherent pair epoch fails the manager-owned pair atomicity contract"),
		CoherentResult.WasSuccessful());

	FWPManagedPairCaptureResult EndpointAResult;
	EndpointAResult.SubmissionMode =
		EWPManagedCaptureSubmissionMode::EndpointA;
	EndpointAResult.CaptureGenerationABefore = 5;
	EndpointAResult.CaptureGenerationAAfter = 6;
	EndpointAResult.CaptureGenerationBBefore = 9;
	EndpointAResult.CaptureGenerationBAfter = 9;
	EndpointAResult.PairCaptureEpochBefore = 3;
	EndpointAResult.PairCaptureEpochAfter = 3;
	EndpointAResult.StaggeredCompletionMaskBefore = 0;
	EndpointAResult.StaggeredCompletionMaskAfter =
		WPStaggeredEndpointAMask;
	EndpointAResult.bSubmittedA = true;
	EndpointAResult.bPairEpochCoherent = true;
	TestTrue(TEXT("A first staggered endpoint advances only A and keeps the pair epoch"),
		EndpointAResult.WasSuccessful());
	EndpointAResult.StaggeredCompletionMaskAfter = 0;
	TestFalse(TEXT("A staggered endpoint rejects an unexpected post-submission mask"),
		EndpointAResult.WasSuccessful());
	EndpointAResult.StaggeredCompletionMaskAfter =
		WPStaggeredEndpointAMask;

	FWPManagedPairCaptureResult EndpointBResult;
	EndpointBResult.SubmissionMode =
		EWPManagedCaptureSubmissionMode::EndpointB;
	EndpointBResult.CaptureGenerationABefore = 6;
	EndpointBResult.CaptureGenerationAAfter = 6;
	EndpointBResult.CaptureGenerationBBefore = 9;
	EndpointBResult.CaptureGenerationBAfter = 10;
	EndpointBResult.PairCaptureEpochBefore = 3;
	EndpointBResult.PairCaptureEpochAfter = 4;
	EndpointBResult.StaggeredCompletionMaskBefore =
		WPStaggeredEndpointAMask;
	EndpointBResult.StaggeredCompletionMaskAfter = 0;
	EndpointBResult.bSubmittedB = true;
	EndpointBResult.bPairCycleCompleted = true;
	EndpointBResult.bPairEpochCoherent = true;
	TestTrue(TEXT("The second staggered endpoint completes one coherent pair cycle"),
		EndpointBResult.WasSuccessful());
	EndpointBResult.bPairCycleCompleted = false;
	TestFalse(TEXT("A complete stagger mask requires an exact pair-cycle completion"),
		EndpointBResult.WasSuccessful());

	AddInfo(FString::Printf(
		TEXT("Managed capture contracts verified. AtomicDelta=+1/+1 StaggeredDelta=+1/+0,+0/+1 PairEpochOnCycleOnly=1 CpuMs=%.4f"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWPCaptureDistanceLODContractTest,
	"WormholePortal.Runtime.CaptureDistanceLODContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWPCaptureDistanceLODContractTest::RunTest(const FString& Parameters)
{
	const double StartSeconds = FPlatformTime::Seconds();
	(void)Parameters;

	TestEqual(
		TEXT("A non-positive max view distance preserves the unlimited baseline"),
		ResolveWPCaptureMaxViewDistanceCm(0.0f),
		-1.0f);
	TestEqual(
		TEXT("A small positive max view distance is clamped to the quality guard"),
		ResolveWPCaptureMaxViewDistanceCm(100.0f),
		WPMinimumCaptureMaxViewDistanceCm);
	TestEqual(
		TEXT("The optimized 100 m capture distance is preserved exactly"),
		ResolveWPCaptureMaxViewDistanceCm(10'000.0f),
		10'000.0f);
	TestEqual(
		TEXT("An excessive max view distance is clamped"),
		ResolveWPCaptureMaxViewDistanceCm(20'000'000.0f),
		WPMaximumCaptureMaxViewDistanceCm);

	TestEqual(
		TEXT("LOD factors below one cannot increase capture detail"),
		ResolveWPCaptureLODDistanceFactor(0.25f),
		WPMinimumCaptureLODDistanceFactor);
	TestEqual(
		TEXT("The optimized 2x LOD distance factor is preserved exactly"),
		ResolveWPCaptureLODDistanceFactor(2.0f),
		2.0f);
	TestEqual(
		TEXT("An excessive LOD distance factor is clamped"),
		ResolveWPCaptureLODDistanceFactor(50.0f),
		WPMaximumCaptureLODDistanceFactor);
	AddInfo(FString::Printf(
		TEXT("Capture distance/LOD contracts verified. UnlimitedSentinel=-1 OptimizedMaxViewDistanceCm=10000 OptimizedLODDistanceFactor=2.0 FiniteFarPlaneSeparatelyGated=1 CpuMs=%.4f"),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0));
	return true;
}
#endif
