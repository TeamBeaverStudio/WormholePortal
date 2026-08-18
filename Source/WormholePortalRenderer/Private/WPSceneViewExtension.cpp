// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPSceneViewExtension.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RHITextureReference.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "Passes/WPCompositePass.h"
#include "Rendering/WormholeLayer/WPWormholeLayerRenderPass.h"
#include "Math/WPPortalMaskMath.h"
#include "WPLog.h"
#include "WormholePortalStats.h"
#include "Math/WPPortalVisibilityMath.h"

namespace
{
	TAutoConsoleVariable<int32> CVarWPSceneViewExtensionEnabled(
		TEXT("wp.SceneViewExtensionEnabled"),
		1,
		TEXT("Canonical master switch for Warmup/Production rendering.\n")
		TEXT("0: disabled, 1: enabled (default)."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarWPSimulateViewEnabled(
		TEXT("wp.SimulateViewEnabled"),
		1,
		TEXT("Allows the PIE Simulate-In-Editor level viewport to run the production ownership pass.\n")
		TEXT("0: disabled, 1: enabled (default). Cube capture camera selection remains unchanged."),
		ECVF_RenderThreadSafe);

	// 로그 전용: aggregate 로그의 출력 주기만 제어하며 render pass 선택에는 관여하지 않는다.
	TAutoConsoleVariable<float> CVarWPViewSummaryInterval(
		TEXT("wp.ViewSummaryInterval"),
		5.0f,
		TEXT("Seconds between aggregate SceneViewExtension Game/Render Thread logs."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarWPOwnershipForceProductionFailureCallbacks(
		TEXT("wp.OwnershipForceProductionFailureCallbacks"),
		0,
		TEXT("Production ownership rollback validation hook. It never mutates a texture/resource. ")
		TEXT("0: normal, -1: fail every Production pair/callback, positive N: fail the next N ")
		TEXT("Production pair attempts after same-epoch warmup commit, then recover. Toggle through 0 to rearm."),
		ECVF_RenderThreadSafe | ECVF_Cheat);

	int32 GetWPOwnershipForceProductionFailureCallbacks_RenderThread()
	{
#if UE_BUILD_SHIPPING
		return 0;
#else
		return CVarWPOwnershipForceProductionFailureCallbacks.GetValueOnRenderThread();
#endif
	}

	uint64 HashWPReason(const TCHAR* Reason)
	{
		return Reason ? static_cast<uint64>(FCrc::StrCrc32(Reason)) : 0ull;
	}

	enum class EWPOwnershipViewKind : uint8
	{
		Unsupported,
		PrimaryPlayer,
		OfflineCinematic,
		SimulateInEditor
	};

	struct FWPOwnershipViewPolicyInput
	{
		bool bHasFamilyAndScene = false;
		bool bSceneCapture = false;
		bool bReflectionCapture = false;
		bool bPlanarReflection = false;
		bool bGameView = false;
		bool bPerspective = false;
		bool bFeatureLevelSupported = false;
		bool bPIEWorld = false;
		bool bSimulateViewEnabled = true;
		bool bHasViewElementDrawer = false;
		bool bVirtualTexture = false;
		bool bOfflineRender = false;
		bool bCustomRenderPass = false;
		uint32 ViewActorId = 0;
		int32 PlayerIndex = INDEX_NONE;
		int32 StereoViewIndex = INDEX_NONE;
		EStereoscopicPass StereoPass = EStereoscopicPass::eSSP_FULL;
		int32 FamilyViewCount = 0;
	};

	struct FWPOwnershipViewPolicyResult
	{
		EWPOwnershipViewKind Kind = EWPOwnershipViewKind::Unsupported;
		const TCHAR* Reason = TEXT("Unknown");

		bool IsAccepted() const
		{
			return Kind != EWPOwnershipViewKind::Unsupported;
		}
	};

	const TCHAR* GetWPOwnershipViewKindName(const EWPOwnershipViewKind Kind)
	{
		switch (Kind)
		{
		case EWPOwnershipViewKind::PrimaryPlayer: return TEXT("PrimaryPlayer");
		case EWPOwnershipViewKind::OfflineCinematic: return TEXT("OfflineCinematic");
		case EWPOwnershipViewKind::SimulateInEditor: return TEXT("SimulateInEditor");
		default: return TEXT("Unsupported");
		}
	}

	FWPOwnershipViewPolicyResult EvaluateWPOwnershipViewPolicy(
		const FWPOwnershipViewPolicyInput& Input)
	{
		if (!Input.bHasFamilyAndScene)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("MissingFamilyOrScene")};
		}
		if (Input.bSceneCapture || Input.bReflectionCapture || Input.bPlanarReflection)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("SceneCaptureOrReflection")};
		}
		if (!Input.bGameView || !Input.bPerspective || !Input.bFeatureLevelSupported)
		{
			return {EWPOwnershipViewKind::Unsupported,
				TEXT("UnsupportedGameProjectionOrFeatureLevel")};
		}
		if (Input.StereoViewIndex != INDEX_NONE
			|| Input.StereoPass != EStereoscopicPass::eSSP_FULL)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("StereoView")};
		}
		if (Input.FamilyViewCount != 1)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("NotExactlyOneFamilyView")};
		}
		if (Input.PlayerIndex == 0)
		{
			return {EWPOwnershipViewKind::PrimaryPlayer, TEXT("PrimaryPlayer")};
		}
		if (Input.PlayerIndex != INDEX_NONE)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("NonPrimaryPlayerView")};
		}
		// MRQ's main deferred beauty view is an actor-owned, mono offline game view
		// with no PlayerIndex. Accept only that narrow signature; scene captures,
		// virtual textures, additional custom passes, stereo and multi-view families
		// remain rejected by the surrounding fail-closed gates.
		if (Input.bOfflineRender
			&& !Input.bVirtualTexture
			&& !Input.bCustomRenderPass
			&& Input.ViewActorId != 0)
		{
			return {EWPOwnershipViewKind::OfflineCinematic,
				TEXT("OfflineCinematic")};
		}
		if (!Input.bSimulateViewEnabled)
		{
			return {EWPOwnershipViewKind::Unsupported, TEXT("SimulateViewDisabled")};
		}
		if (Input.bVirtualTexture || Input.bOfflineRender || Input.bCustomRenderPass)
		{
			return {EWPOwnershipViewKind::Unsupported,
				TEXT("UnsupportedSimulateSpecialRenderPass")};
		}
		if (!Input.bPIEWorld || !Input.bHasViewElementDrawer || Input.ViewActorId != 0)
		{
			return {EWPOwnershipViewKind::Unsupported,
				TEXT("UnsupportedSimulateViewIdentity")};
		}
		return {EWPOwnershipViewKind::SimulateInEditor, TEXT("SimulateInEditor")};
	}

	FWPOwnershipViewPolicyInput MakeWPOwnershipViewPolicyInput(
		const FSceneView& View,
		const bool bPIEWorld,
		const bool bSimulateViewEnabled)
	{
		FWPOwnershipViewPolicyInput Input;
		Input.bHasFamilyAndScene = View.Family && View.Family->Scene;
		Input.bSceneCapture = View.bIsSceneCapture;
		Input.bReflectionCapture = View.bIsReflectionCapture;
		Input.bPlanarReflection = View.bIsPlanarReflection;
		Input.bGameView = View.bIsGameView;
		Input.bPerspective = View.IsPerspectiveProjection();
		Input.bFeatureLevelSupported = View.GetFeatureLevel() >= ERHIFeatureLevel::SM5;
		Input.bPIEWorld = bPIEWorld;
		Input.bSimulateViewEnabled = bSimulateViewEnabled;
		Input.bHasViewElementDrawer = View.Drawer != nullptr;
		Input.bVirtualTexture = View.bIsVirtualTexture;
		Input.bOfflineRender = View.bIsOfflineRender;
		Input.bCustomRenderPass = View.CustomRenderPass != nullptr;
		Input.ViewActorId = View.ViewActor.ActorUniqueId;
		Input.PlayerIndex = View.PlayerIndex;
		Input.StereoViewIndex = View.StereoViewIndex;
		Input.StereoPass = View.StereoPass;
		Input.FamilyViewCount = View.Family ? View.Family->Views.Num() : 0;
		return Input;
	}

	bool ShouldRecordWPVisibilityFeedback(
		const EWPOwnershipViewKind ViewKind,
		const uint32 ViewActorId,
		const uint32 ReferenceViewActorId)
	{
		// Composite rendering keeps the existing accepted-view policy. Capture
		// pause feedback is narrower: only the exact primary Player0 reference
		// actor may influence whether A+B capture stops. SIE or actor mismatch
		// deliberately produces no new sample so the GT freshness gate fails open.
		return ViewKind == EWPOwnershipViewKind::PrimaryPlayer
			&& ViewActorId != 0
			&& ReferenceViewActorId != 0
			&& ViewActorId == ReferenceViewActorId;
	}

	bool ShouldUseWPOverrideOutput(const int32 EndpointIndex, const int32 EndpointCount)
	{
		return EndpointCount > 0 && EndpointIndex == EndpointCount - 1;
	}

	struct FWPProductionEndpointOrderKey
	{
		double NearSurfaceDistanceCm = TNumericLimits<double>::Max();
		FName StableSelector = NAME_None;
		FGuid PairId;
		uint64 HandleValue = 0;
		EWPSide EndpointSide = EWPSide::None;
	};

	bool IsWPProductionEndpointBefore(
		const FWPProductionEndpointOrderKey& Left,
		const FWPProductionEndpointOrderKey& Right)
	{
		// Every active pair participates in one deterministic far-to-near work list.
		// Selector, PairId, handle and side make exact-distance ties independent of TMap order.
		if (Left.NearSurfaceDistanceCm != Right.NearSurfaceDistanceCm)
		{
			return Left.NearSurfaceDistanceCm > Right.NearSurfaceDistanceCm;
		}
		if (Left.StableSelector != Right.StableSelector)
		{
			return Left.StableSelector.LexicalLess(Right.StableSelector);
		}
		if (Left.PairId != Right.PairId)
		{
			return Left.PairId < Right.PairId;
		}
		if (Left.HandleValue != Right.HandleValue)
		{
			return Left.HandleValue < Right.HandleValue;
		}
		return static_cast<uint8>(Left.EndpointSide)
			< static_cast<uint8>(Right.EndpointSide);
	}

	uint64 MakeWPPairViewStorageKey(
		const uint64 ViewKey,
		const uint64 HandleValue)
	{
		uint64 Result = 1469598103934665603ull;
		Result ^= ViewKey;
		Result *= 1099511628211ull;
		Result ^= HandleValue;
		Result *= 1099511628211ull;
		return Result;
	}

	bool IsFiniteSceneViewVector(const FVector3d& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsFiniteSceneViewVector(const FVector3f& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool BuildWPProductionViewCenters(
		const FWPRenderThreadPacket& Packet,
		const FVector3d& PreViewTranslation,
		FVector3f& OutCenterA,
		FVector3f& OutCenterB)
	{
		if (!IsFiniteSceneViewVector(Packet.PortalACenterWorld)
			|| !IsFiniteSceneViewVector(Packet.PortalBCenterWorld)
			|| !IsFiniteSceneViewVector(PreViewTranslation))
		{
			return false;
		}
		OutCenterA = FVector3f(Packet.PortalACenterWorld + PreViewTranslation);
		OutCenterB = FVector3f(Packet.PortalBCenterWorld + PreViewTranslation);
		return IsFiniteSceneViewVector(OutCenterA)
			&& IsFiniteSceneViewVector(OutCenterB);
	}

	bool IsFiniteInvertibleSceneViewMatrix(const FMatrix44d& Matrix)
	{
		const double Determinant = Matrix.Determinant();
		return !Matrix.ContainsNaN()
			&& FMath::IsFinite(Determinant)
			&& FMath::Abs(Determinant) > UE_DOUBLE_SMALL_NUMBER;
	}

	FRHITexture* ResolveTextureReference(const FTextureReferenceRHIRef& TextureReference)
	{
		if (!TextureReference.IsValid())
		{
			return nullptr;
		}

		FRHITexture* ReferencedTexture = TextureReference->GetReferencedTexture();
		return ReferencedTexture != FRHITextureReference::GetDefaultTexture()
			? ReferencedTexture
			: nullptr;
	}

	FRHITexture* GetTextureReferenceProxy(
		const FTextureReferenceRHIRef& TextureReference)
	{
		// Do not call GetReferencedTexture() for capture cubes. D3D12 applies
		// UpdateTextureReference on the RHI Thread, so an RT-side unwrap can observe the
		// previous physical cube. Passing this proxy to the draw preserves command order.
		return TextureReference.IsValid()
			? TextureReference.GetReference()
			: nullptr;
	}

	bool HasPositiveTextureExtent(const FRHITexture* Texture)
	{
		return Texture
			&& Texture->GetDesc().Extent.X > 0
			&& Texture->GetDesc().Extent.Y > 0
			&& Texture->GetDesc().Depth > 0
			&& Texture->GetDesc().ArraySize > 0;
	}

	uint64 MakeViewKey(const FSceneView& View)
	{
		uint64 Result = 1469598103934665603ull;
		const auto Mix = [&Result](const uint64 Value)
		{
			Result ^= Value;
			Result *= 1099511628211ull;
		};

		Mix(View.GetViewKey());
		Mix(View.ViewActor.ActorUniqueId);
		Mix(static_cast<uint32>(View.PlayerIndex));
		Mix(static_cast<uint32>(View.StereoViewIndex));
		Mix(static_cast<uint32>(View.StereoPass));
		return Result;
	}

	bool IsWPSceneViewExtensionMasterEnabled(const int32 Enabled)
	{
		return Enabled != 0;
	}

	bool ShouldActivateWPSceneViewExtension(
		const int32 ActiveOwnershipPairCount,
		const int32 ActiveVisibilityObservationPairCount)
	{
		return ActiveOwnershipPairCount > 0
			|| ActiveVisibilityObservationPairCount > 0;
	}

	bool IsWPOwnershipSnapshotReadyForRendering(
		const FWPPairOwnershipSnapshot& Ownership,
		const uint64 PacketSequence)
	{
		return Ownership.IsReadyForRendering(PacketSequence);
	}

	bool IsWPOwnershipPacketReadyForRendering(
		const FWPRenderThreadPacket& Packet)
	{
		return Packet.IsOwnershipContractReady();
	}

	bool IsWPVisibilityObservationPacketReady(
		const FWPRenderThreadPacket& Packet)
	{
		return Packet.IsVisibilityObservationContractReady();
	}

	bool IsWPProductionEndpointAllowedByOcclusion(
		const FWPRenderThreadPacket& Packet,
		const EWPSide Side)
	{
		if (!Packet.bCaptureOcclusionValid)
		{
			return true;
		}
		const uint8 EndpointMask = Side == EWPSide::SideA ? 0x1u : 0x2u;
		return (Packet.CaptureOcclusionVisibleEndpointMask & EndpointMask) != 0;
	}

	uint32 AdvanceWPWarmupConsecutiveFrameCount(
		const uint32 PreviousFrameNumber,
		const uint32 CurrentFrameNumber,
		const uint32 PreviousCount)
	{
		if (PreviousFrameNumber == CurrentFrameNumber)
		{
			return PreviousCount;
		}
		return PreviousFrameNumber != MAX_uint32
			&& PreviousFrameNumber + 1u == CurrentFrameNumber
				? PreviousCount + 1u
				: 1u;
	}

	bool ShouldPreserveWPWarmupProgressAcrossPacketUpdate(
		const uint64 TrackerEpoch,
		const uint64 PacketEpoch,
		const bool bFailureLatched)
	{
		// PacketSequence also advances for a 1 Hz heartbeat. That transport-only
		// update must not make a low-FPS warmup permanently miss its second frame.
		// Epoch/resource invalidation still resets progress, and a failed packet
		// starts fresh after the next sequence arrives.
		return TrackerEpoch != 0
			&& TrackerEpoch == PacketEpoch
			&& !bFailureLatched;
	}

	EPixelFormat GetWPRHIPixelFormat(const EWPRayLUTFormat Format)
	{
		switch (Format)
		{
		case EWPRayLUTFormat::RGBA32Float: return PF_A32B32G32R32F;
		default: return PF_Unknown;
		}
	}

	void PopulateWPRayLUTContractParameters(
		FWPCompositePassParameters& OutParameters,
		const FWPRayLUTContract& Contract,
		const float RayLUTZ,
		const uint32 EndpointRevision)
	{
		OutParameters.RayLUTZ = RayLUTZ;
		OutParameters.ExpectedLUTExtent = Contract.ExpectedExtent;
		OutParameters.ExpectedLUTFormat = GetWPRHIPixelFormat(Contract.ExpectedFormat);
		OutParameters.ExpectedLUTMipCount = Contract.ExpectedMipCount;
		OutParameters.ExpectedLUTDimension = Contract.ExpectedDimension;
		OutParameters.LUTLayoutVersion = Contract.LayoutVersion;
		OutParameters.LUTGeneration = Contract.Generation;
		// The immutable contract and the separately copied endpoint identity must
		// agree; a torn/stale packet deliberately produces an invalid revision.
		OutParameters.LUTRevision = EndpointRevision == Contract.Revision
			? EndpointRevision : 0;
	}

	EPixelFormat GetWPCubeRHIPixelFormat(const EWPCubeFormat Format)
	{
		switch (Format)
		{
		case EWPCubeFormat::RGBA16Float: return PF_FloatRGBA;
		default: return PF_Unknown;
		}
	}

	FVector3f GetWPUnitAxis(const FMatrix44d& Matrix, const EAxis::Type Axis)
	{
		return FVector3f(Matrix.GetUnitAxis(Axis));
	}

	uint64 MixWPHash(uint64 Hash, const uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
		return Hash;
	}

#if WITH_DEV_AUTOMATION_TESTS
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FWPOwnershipViewClassificationTest,
		"WormholePortal.Renderer.OwnershipViewClassification",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FWPOwnershipViewClassificationTest::RunTest(const FString& Parameters)
	{
		(void)Parameters;
		FWPOwnershipViewPolicyInput Input;
		Input.bHasFamilyAndScene = true;
		Input.bGameView = true;
		Input.bPerspective = true;
		Input.bFeatureLevelSupported = true;
		Input.bPIEWorld = true;
		Input.bSimulateViewEnabled = true;
		Input.FamilyViewCount = 1;
		Input.PlayerIndex = 0;

		auto EvaluateKind = [&Input]()
		{
			return EvaluateWPOwnershipViewPolicy(Input).Kind;
		};
		auto TestKind = [this, &EvaluateKind](
			const TCHAR* What,
			const EWPOwnershipViewKind Expected)
		{
			TestEqual(What,
				static_cast<uint8>(EvaluateKind()),
				static_cast<uint8>(Expected));
		};

		TestKind(TEXT("The existing mono primary-player view remains accepted."),
			EWPOwnershipViewKind::PrimaryPlayer);

		Input.PlayerIndex = INDEX_NONE;
		Input.bHasViewElementDrawer = true;
		Input.ViewActorId = 0;
		TestKind(TEXT("The exact PIE level-viewport signature is accepted as SIE."),
			EWPOwnershipViewKind::SimulateInEditor);

		Input.bSimulateViewEnabled = false;
		TestKind(TEXT("The SIE compatibility switch fails closed."),
			EWPOwnershipViewKind::Unsupported);
		Input.bSimulateViewEnabled = true;
		Input.bPIEWorld = false;
		TestKind(TEXT("A non-PIE editor viewport is never mistaken for SIE."),
			EWPOwnershipViewKind::Unsupported);
		Input.bPIEWorld = true;
		Input.bHasViewElementDrawer = false;
		TestKind(TEXT("An INDEX_NONE runtime view without an editor drawer remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.bHasViewElementDrawer = true;
		Input.ViewActorId = 17;
		TestKind(TEXT("An actor-owned INDEX_NONE view remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.ViewActorId = 0;
		Input.PlayerIndex = 1;
		TestKind(TEXT("A secondary player view remains rejected."),
			EWPOwnershipViewKind::Unsupported);

		Input.PlayerIndex = INDEX_NONE;
		Input.bSceneCapture = true;
		TestKind(TEXT("Scene captures remain rejected before SIE classification."),
			EWPOwnershipViewKind::Unsupported);
		Input.bSceneCapture = false;
		Input.bReflectionCapture = true;
		TestKind(TEXT("Reflection captures remain rejected before SIE classification."),
			EWPOwnershipViewKind::Unsupported);
		Input.bReflectionCapture = false;
		Input.bPlanarReflection = true;
		TestKind(TEXT("Planar reflections remain rejected before SIE classification."),
			EWPOwnershipViewKind::Unsupported);
		Input.bPlanarReflection = false;
		Input.bVirtualTexture = true;
		TestKind(TEXT("A virtual-texture view is never mistaken for SIE."),
			EWPOwnershipViewKind::Unsupported);
		Input.bVirtualTexture = false;
		Input.bOfflineRender = true;
		TestKind(TEXT("An offline-render view is never mistaken for SIE."),
			EWPOwnershipViewKind::Unsupported);
		Input.bPIEWorld = false;
		Input.bHasViewElementDrawer = false;
		Input.ViewActorId = 17;
		TestKind(TEXT("The actor-owned MRQ beauty signature is accepted."),
			EWPOwnershipViewKind::OfflineCinematic);
		Input.bOfflineRender = false;
		Input.bCustomRenderPass = true;
		TestKind(TEXT("A custom render pass is never mistaken for SIE."),
			EWPOwnershipViewKind::Unsupported);
		Input.bCustomRenderPass = false;

		Input.StereoViewIndex = 0;
		TestKind(TEXT("A stereo sub-view remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.StereoViewIndex = INDEX_NONE;
		Input.StereoPass = EStereoscopicPass::eSSP_PRIMARY;
		TestKind(TEXT("A non-full stereo pass remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.StereoPass = EStereoscopicPass::eSSP_FULL;
		Input.FamilyViewCount = 2;
		TestKind(TEXT("A multi-view family remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.FamilyViewCount = 1;
		Input.bGameView = false;
		TestKind(TEXT("A non-game editor view remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.bGameView = true;
		Input.bPerspective = false;
		TestKind(TEXT("An orthographic view remains rejected."),
			EWPOwnershipViewKind::Unsupported);
		Input.bPerspective = true;
		Input.bFeatureLevelSupported = false;
		TestKind(TEXT("A pre-SM5 view remains rejected."),
			EWPOwnershipViewKind::Unsupported);

		TestTrue(TEXT("The exact primary reference actor may publish capture-pause visibility."),
			ShouldRecordWPVisibilityFeedback(
				EWPOwnershipViewKind::PrimaryPlayer, 17, 17));
		TestFalse(TEXT("SIE may composite but never controls capture-pause visibility."),
			ShouldRecordWPVisibilityFeedback(
				EWPOwnershipViewKind::SimulateInEditor, 0, 17));
		TestFalse(TEXT("MRQ may composite but never controls capture-pause visibility."),
			ShouldRecordWPVisibilityFeedback(
				EWPOwnershipViewKind::OfflineCinematic, 17, 17));
		TestFalse(TEXT("A primary actor mismatch fails open by withholding visibility."),
			ShouldRecordWPVisibilityFeedback(
				EWPOwnershipViewKind::PrimaryPlayer, 18, 17));
		TestFalse(TEXT("A missing reference actor cannot pause pair capture."),
			ShouldRecordWPVisibilityFeedback(
				EWPOwnershipViewKind::PrimaryPlayer, 17, 0));

		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FWPProductionOwnershipPolicyTest,
		"WormholePortal.Renderer.ProductionOwnershipPolicy",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FWPProductionOwnershipPolicyTest::RunTest(const FString& Parameters)
	{
		(void)Parameters;
		TestTrue(TEXT("The master switch enables rendering."),
			IsWPSceneViewExtensionMasterEnabled(1));
		TestFalse(TEXT("The master switch disables rendering."),
			IsWPSceneViewExtensionMasterEnabled(0));
		TestFalse(TEXT("No ownership or observation work preserves the zero-extension path."),
			ShouldActivateWPSceneViewExtension(0, 0));
		TestTrue(TEXT("A ready warmup/production pair activates the extension."),
			ShouldActivateWPSceneViewExtension(1, 0));
		TestTrue(TEXT("A visibility-only Pair activates the extension without ownership resources."),
			ShouldActivateWPSceneViewExtension(0, 1));

		FWPPairOwnershipSnapshot Ownership;
		Ownership.PairId = FGuid(1, 2, 3, 4);
		Ownership.RequestedOwnership = EWPPairOwnershipMode::Production;
		Ownership.EffectiveOwnership = EWPPairOwnershipMode::Production;
		Ownership.OwnershipEpoch = 7;
		Ownership.bEndpointAReady = true;
		Ownership.bEndpointBReady = true;
		Ownership.bOwnershipInputsReady = true;
		TestTrue(TEXT("A fully ready production snapshot enters the production work list."),
			IsWPOwnershipSnapshotReadyForRendering(Ownership, 3));
		Ownership.EffectiveOwnership = EWPPairOwnershipMode::Warmup;
		TestTrue(TEXT("A ready warmup participates in the GT/RT multi-pair work list."),
			IsWPOwnershipSnapshotReadyForRendering(Ownership, 3));
		Ownership.bEndpointBReady = false;
		TestFalse(TEXT("Either endpoint losing readiness prevents production rendering."),
			IsWPOwnershipSnapshotReadyForRendering(Ownership, 3));

		FWPRenderThreadPacket RenderPacket;
		RenderPacket.PairId = FGuid(1, 2, 3, 4);
		RenderPacket.RequestedOwnership = EWPPairOwnershipMode::Production;
		RenderPacket.EffectiveOwnership = EWPPairOwnershipMode::Warmup;
		RenderPacket.OwnershipEpoch = 9;
		RenderPacket.PacketSequence = 5;
		RenderPacket.bOwnershipEndpointAReady = true;
		RenderPacket.bOwnershipEndpointBReady = true;
		RenderPacket.bOwnershipInputsReady = true;
		TestTrue(TEXT("Only a fully ready warmup packet enters the RT ownership path."),
			IsWPOwnershipPacketReadyForRendering(RenderPacket));
		RenderPacket.bOwnershipInputsReady = false;
		TestFalse(TEXT("An unready warmup packet remains on the zero-callback disabled path."),
			IsWPOwnershipPacketReadyForRendering(RenderPacket));
		RenderPacket.bEnabled = true;
		RenderPacket.bCaptureVisibilityFeedbackEnabled = true;
		RenderPacket.bHasReferenceView = true;
		RenderPacket.ReferenceViewActorId = 17;
		RenderPacket.OwnershipFeedbackState =
			MakeShared<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe>();
		RenderPacket.PortalACenterWorld = FVector3d(10.0, 20.0, 30.0);
		RenderPacket.PortalBCenterWorld = FVector3d(40.0, 50.0, 60.0);
		RenderPacket.MetricA.PortalRadiusCm = 100.0f;
		RenderPacket.MetricA.MouthRadiusCm = 100.0f;
		RenderPacket.MetricA.MetricOuterRadiusCm = 100.0f;
		RenderPacket.MetricA.OuterRadiusCm = 100.0f;
		RenderPacket.MetricA.Revision = 1;
		RenderPacket.MetricB = RenderPacket.MetricA;
		TestTrue(TEXT("Visibility observation remains ready without ownership inputs or captures."),
			IsWPVisibilityObservationPacketReady(RenderPacket));

		TestEqual(TEXT("First warmup frame starts at one."),
			AdvanceWPWarmupConsecutiveFrameCount(MAX_uint32, 100, 0), 1u);
		TestEqual(TEXT("The immediately following frame reaches the two-frame ACK threshold."),
			AdvanceWPWarmupConsecutiveFrameCount(100, 101, 1), 2u);
		TestEqual(TEXT("A duplicate callback in the same frame does not advance warmup."),
			AdvanceWPWarmupConsecutiveFrameCount(101, 101, 2), 2u);
		TestEqual(TEXT("A frame gap resets the consecutive warmup count."),
			AdvanceWPWarmupConsecutiveFrameCount(101, 104, 2), 1u);
		TestTrue(TEXT("A heartbeat packet update in the same epoch preserves warmup progress."),
			ShouldPreserveWPWarmupProgressAcrossPacketUpdate(9, 9, false));
		TestFalse(TEXT("An ownership epoch change resets warmup progress."),
			ShouldPreserveWPWarmupProgressAcrossPacketUpdate(9, 10, false));
		TestFalse(TEXT("A failed warmup packet cannot carry progress into its retry."),
			ShouldPreserveWPWarmupProgressAcrossPacketUpdate(9, 9, true));
		const uint32 PreservedHeartbeatCount =
			ShouldPreserveWPWarmupProgressAcrossPacketUpdate(9, 9, false)
				? 1u : 0u;
		TestEqual(TEXT("A same-epoch heartbeat can still reach the two-frame ACK threshold."),
			AdvanceWPWarmupConsecutiveFrameCount(
				100, 101, PreservedHeartbeatCount), 2u);

		FWPRayLUTContract VolumeLUTContract;
		VolumeLUTContract.LayoutVersion = 1;
		VolumeLUTContract.Generation = 2;
		VolumeLUTContract.Revision = 3;
		VolumeLUTContract.ExpectedExtent = FIntVector(512, 48, 24);
		VolumeLUTContract.ExpectedFormat = EWPRayLUTFormat::RGBA32Float;
		VolumeLUTContract.ExpectedMipCount = 1;
		VolumeLUTContract.ExpectedDimension = EWPRayLUTDimension::Texture3D;
		FWPCompositePassParameters VolumeLUTParameters;
		PopulateWPRayLUTContractParameters(
			VolumeLUTParameters, VolumeLUTContract, 0.25f, 3);
		TestEqual(TEXT("Production mapping preserves the volume LUT X extent."),
			VolumeLUTParameters.ExpectedLUTExtent.X, 512);
		TestEqual(TEXT("Production mapping preserves the volume LUT depth."),
			VolumeLUTParameters.ExpectedLUTExtent.Z, 24);
		TestEqual(TEXT("Production mapping preserves the volume LUT mip count."),
			VolumeLUTParameters.ExpectedLUTMipCount, 1u);
		TestEqual(TEXT("Production mapping preserves the Texture3D contract."),
			static_cast<uint8>(VolumeLUTParameters.ExpectedLUTDimension),
			static_cast<uint8>(EWPRayLUTDimension::Texture3D));
		TestEqual(TEXT("Production mapping preserves the endpoint volume coordinate."),
			VolumeLUTParameters.RayLUTZ, 0.25f);
		TestEqual(TEXT("A matching endpoint revision remains valid."),
			VolumeLUTParameters.LUTRevision, 3u);
		PopulateWPRayLUTContractParameters(
			VolumeLUTParameters, VolumeLUTContract, 0.25f, 4);
		TestEqual(TEXT("A mismatched endpoint revision fails the pass contract closed."),
			VolumeLUTParameters.LUTRevision, 0u);

		FWPPairOwnershipFeedbackState FeedbackState;
		FeedbackState.RecordWarmupPassForTest(9, 42, true);
		FWPPairOwnershipFeedback Feedback = FeedbackState.ReadGameThread();
		TestEqual(TEXT("Warmup ACK feedback preserves its exact epoch."),
			Feedback.WarmupSucceededEpoch, 9ull);
		TestEqual(TEXT("Warmup ACK feedback preserves its exact packet sequence."),
			Feedback.WarmupSucceededPacketSequence, 42ull);
		TestEqual(TEXT("Warmup ACK feedback has no failure epoch."),
			Feedback.ProductionFailedEpoch, 0ull);
		TestEqual(TEXT("Warmup ACK feedback has no failure packet sequence."),
			Feedback.ProductionFailedPacketSequence, 0ull);
		TestEqual(TEXT("The warmup pass counter is part of the coherent ACK tuple."),
			Feedback.WarmupPassCount, 1ull);
		TestEqual(TEXT("The ACK tuple has no production failure count."),
			Feedback.ProductionFailureCount, 0ull);
		TestTrue(TEXT("A published feedback snapshot has an even seqlock version."),
			(Feedback.SnapshotVersion & 1ull) == 0);
		TestEqual(TEXT("An uncontended feedback snapshot needs no retry."),
			Feedback.SnapshotReadRetryCount, 0u);
		TestTrue(TEXT("An uncontended feedback snapshot is coherent."),
			Feedback.bSnapshotCoherent);
		TestFalse(TEXT("An odd in-progress writer version is never a stable snapshot."),
			FWPPairOwnershipFeedbackState::IsStableFeedbackSnapshotVersion(3, 3));
		TestFalse(TEXT("A version change across field reads rejects the mixed payload."),
			FWPPairOwnershipFeedbackState::IsStableFeedbackSnapshotVersion(2, 4));
		TestTrue(TEXT("The same even version around field reads is stable."),
			FWPPairOwnershipFeedbackState::IsStableFeedbackSnapshotVersion(4, 4));

		FeedbackState.RecordVisibilitySampleForTest(9, 42, 0x2);
		const FWPPairOwnershipFeedback OwnershipOnlyFeedback =
			FeedbackState.ReadGameThread(false);
		TestTrue(TEXT("CVar-off ownership feedback remains coherent without visibility loads."),
			OwnershipOnlyFeedback.bSnapshotCoherent);
		TestEqual(TEXT("CVar-off feedback preserves the ownership ACK."),
			OwnershipOnlyFeedback.WarmupSucceededEpoch, 9ull);
		TestEqual(TEXT("CVar-off feedback skips the visibility epoch atomic."),
			OwnershipOnlyFeedback.VisibilityOwnershipEpoch, 0ull);
		TestEqual(TEXT("CVar-off feedback skips the visibility packet atomic."),
			OwnershipOnlyFeedback.VisibilityPacketSequence, 0ull);
		TestEqual(TEXT("CVar-off feedback skips the visibility sample-sequence atomic."),
			OwnershipOnlyFeedback.VisibilitySampleSequence, 0ull);
		TestEqual(TEXT("CVar-off feedback skips the visible-endpoint-mask atomic."),
			OwnershipOnlyFeedback.VisibleEndpointMask, static_cast<uint8>(0));
		Feedback = FeedbackState.ReadGameThread(true);
		TestEqual(TEXT("CVar-on feedback reads the exact visibility epoch."),
			Feedback.VisibilityOwnershipEpoch, 9ull);
		TestEqual(TEXT("CVar-on feedback reads the exact visibility packet."),
			Feedback.VisibilityPacketSequence, 42ull);
		TestEqual(TEXT("CVar-on feedback reads one visibility sample."),
			Feedback.VisibilitySampleSequence, 1ull);
		TestEqual(TEXT("CVar-on feedback preserves the exact endpoint identity."),
			Feedback.VisibleEndpointMask, static_cast<uint8>(0x2));
		TestEqual(TEXT("Endpoint count remains derivable from the exact mask."),
			Feedback.GetVisibleEndpointCount(), 1u);

		FWPPairOwnershipFeedbackState FrameUnionFeedbackState;
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 100, 0x1);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("The first family's A-only mask remains pending until its render frame completes."),
			Feedback.VisibilitySampleSequence, 0ull);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 100, 0x2);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("A second family's B-only mask cannot expose a partial render-frame union."),
			Feedback.VisibilitySampleSequence, 0ull);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 101, 0);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("The next frame publishes the completed prior frame exactly once."),
			Feedback.VisibilitySampleSequence, 1ull);
		TestEqual(TEXT("Accepted view families preserve the union of exact endpoint identities."),
			Feedback.VisibleEndpointMask, static_cast<uint8>(0x3));
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 101, 0);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("Another family in the current frame does not republish it."),
			Feedback.VisibilitySampleSequence, 1ull);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 102, 0);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("A completed all-zero frame publishes one zero mask."),
			Feedback.VisibleEndpointMask, static_cast<uint8>(0));
		TestEqual(TEXT("The all-zero frame advances the mailbox once."),
			Feedback.VisibilitySampleSequence, 2ull);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 42, 102, 0);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 43, 102, 0);
		FrameUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 43, 103, 0);
		Feedback = FrameUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("Mixed packet revisions in one render frame fail open to both endpoints."),
			Feedback.VisibleEndpointMask, static_cast<uint8>(0x3));
		TestEqual(TEXT("The mixed-packet frame publishes its conservative latest packet."),
			Feedback.VisibilityPacketSequence, 43ull);

		FWPPairOwnershipFeedbackState EpochUnionFeedbackState;
		EpochUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			9, 50, 200, 0);
		EpochUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			10, 51, 201, 0);
		Feedback = EpochUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("An epoch change discards the old pending visibility frame."),
			Feedback.VisibilitySampleSequence, 0ull);
		EpochUnionFeedbackState.AccumulateVisibilitySampleForRenderFrameForTest(
			10, 51, 202, 0);
		Feedback = EpochUnionFeedbackState.ReadGameThread(true);
		TestEqual(TEXT("The first completed frame in the new epoch is published."),
			Feedback.VisibilitySampleSequence, 1ull);
		TestEqual(TEXT("The published visibility belongs to the new ownership epoch."),
			Feedback.VisibilityOwnershipEpoch, 10ull);

		const auto InjectFailureBetweenFieldReads =
			[&FeedbackState](const uint32 RetryCount)
			{
				if (RetryCount == 0)
				{
					FeedbackState.ReportProductionFailureForTest(9, 43);
				}
			};
		Feedback = FeedbackState.ReadGameThreadWithInterleaveForTest(
			InjectFailureBetweenFieldReads);
		TestEqual(TEXT("A torn old-success/new-failure read is discarded."),
			Feedback.WarmupSucceededEpoch, 0ull);
		TestEqual(TEXT("Failure clears the complete warmup ACK tuple."),
			Feedback.WarmupSucceededPacketSequence, 0ull);
		TestEqual(TEXT("Interleaved failure preserves its exact epoch."),
			Feedback.ProductionFailedEpoch, 9ull);
		TestEqual(TEXT("Production failure feedback preserves its exact packet sequence."),
			Feedback.ProductionFailedPacketSequence, 43ull);
		TestEqual(TEXT("The stable post-interleave tuple preserves the warmup count."),
			Feedback.WarmupPassCount, 1ull);
		TestEqual(TEXT("The stable post-interleave tuple preserves the failure count."),
			Feedback.ProductionFailureCount, 1ull);
		TestEqual(TEXT("A producer interleave forces exactly one snapshot retry."),
			Feedback.SnapshotReadRetryCount, 1u);
		TestTrue(TEXT("The retried feedback snapshot also ends on an even version."),
			(Feedback.SnapshotVersion & 1ull) == 0);
		TestTrue(TEXT("The stable post-interleave snapshot is coherent."),
			Feedback.bSnapshotCoherent);

		FWPPairOwnershipFeedbackState PartialWriteFeedbackState;
		PartialWriteFeedbackState.RecordWarmupPassForTest(12, 100, true);
		PartialWriteFeedbackState.BeginProductionFailurePartialWriteForTest();
		Feedback = PartialWriteFeedbackState.ReadGameThread();
		TestFalse(TEXT("The exact odd partial-failure payload is never exposed to the GT."),
			Feedback.bSnapshotCoherent);
		TestEqual(TEXT("The partial writer leaves the observable seqlock version odd."),
			Feedback.SnapshotVersion, 3ull);
		TestEqual(TEXT("The exact partial-write test stops at the bounded retry ceiling."),
			Feedback.SnapshotReadRetryCount,
			FWPPairOwnershipFeedbackState::GetMaxFeedbackSnapshotReadRetriesForTest());
		TestEqual(TEXT("An incoherent partial payload cannot expose the cleared ACK as valid feedback."),
			Feedback.WarmupSucceededEpoch, 0ull);
		TestEqual(TEXT("An incoherent partial payload cannot expose the not-yet-published failure epoch."),
			Feedback.ProductionFailedEpoch, 0ull);
		PartialWriteFeedbackState.CompleteProductionFailurePartialWriteForTest(12, 101);
		Feedback = PartialWriteFeedbackState.ReadGameThread();
		TestTrue(TEXT("Completing the partial writer publishes one coherent failure tuple."),
			Feedback.bSnapshotCoherent);
		TestEqual(TEXT("The completed partial writer keeps the ACK invalidated."),
			Feedback.WarmupSucceededEpoch, 0ull);
		TestEqual(TEXT("The completed partial writer publishes the failure epoch."),
			Feedback.ProductionFailedEpoch, 12ull);
		TestEqual(TEXT("The completed partial writer publishes the failure packet sequence."),
			Feedback.ProductionFailedPacketSequence, 101ull);
		TestEqual(TEXT("The completed partial writer increments the failure count once."),
			Feedback.ProductionFailureCount, 1ull);
		TestEqual(TEXT("The completed partial writer ends on the next even version."),
			Feedback.SnapshotVersion, 4ull);

		FeedbackState.SetFeedbackSnapshotVersionForTest(5);
		Feedback = FeedbackState.ReadGameThread();
		TestFalse(TEXT("A permanently odd writer version exhausts bounded retries fail-closed."),
			Feedback.bSnapshotCoherent);
		TestEqual(TEXT("The bounded odd-version read reports its observed version."),
			Feedback.SnapshotVersion, 5ull);
		TestEqual(TEXT("The bounded odd-version read reports the retry ceiling."),
			Feedback.SnapshotReadRetryCount,
			FWPPairOwnershipFeedbackState::GetMaxFeedbackSnapshotReadRetriesForTest());
		FeedbackState.SetFeedbackSnapshotVersionForTest(6);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FWPMultiPairOwnershipPolicyTest,
		"WormholePortal.Renderer.MultiPairOwnershipPolicy",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FWPMultiPairOwnershipPolicyTest::RunTest(const FString& Parameters)
	{
		(void)Parameters;

		TArray<FWPProductionEndpointOrderKey> Endpoints;
		const FGuid PairAId(1, 0, 0, 0);
		const FGuid PairBId(2, 0, 0, 0);
		Endpoints.Add({25.0, FName(TEXT("PairB")), PairBId, 20, EWPSide::SideB});
		Endpoints.Add({100.0, FName(TEXT("PairB")), PairBId, 20, EWPSide::SideA});
		Endpoints.Add({100.0, FName(TEXT("PairA")), PairAId, 30, EWPSide::SideB});
		Endpoints.Add({100.0, FName(TEXT("PairA")), PairAId, 10, EWPSide::SideA});
		Endpoints.Sort([](
			const FWPProductionEndpointOrderKey& Left,
			const FWPProductionEndpointOrderKey& Right)
		{
			return IsWPProductionEndpointBefore(Left, Right);
		});
		TestEqual(TEXT("All-pair ordering is far-to-near."),
			Endpoints[0].NearSurfaceDistanceCm, 100.0);
		TestEqual(TEXT("Stable selector breaks an exact-distance tie before map/handle order."),
			Endpoints[0].StableSelector, FName(TEXT("PairA")));
		TestEqual(TEXT("Handle breaks an exact selector/distance tie deterministically."),
			Endpoints[0].HandleValue, 10ull);
		TestEqual(TEXT("Side is the final deterministic endpoint tie breaker."),
			Endpoints[0].EndpointSide, EWPSide::SideA);
		TestEqual(TEXT("The near endpoint is submitted last."),
			Endpoints.Last().NearSurfaceDistanceCm, 25.0);
		TArray<FWPProductionEndpointOrderKey> PairIdTie;
		PairIdTie.Add({50.0, FName(TEXT("SharedSelector")), PairBId, 1,
			EWPSide::SideA});
		PairIdTie.Add({50.0, FName(TEXT("SharedSelector")), PairAId, 99,
			EWPSide::SideB});
		PairIdTie.Sort([](
			const FWPProductionEndpointOrderKey& Left,
			const FWPProductionEndpointOrderKey& Right)
		{
			return IsWPProductionEndpointBefore(Left, Right);
		});
		TestEqual(TEXT("PairId breaks a shared selector/distance tie before transient handle order."),
			PairIdTie[0].PairId, PairAId);
		TestFalse(TEXT("An intermediate endpoint cannot consume OverrideOutput."),
			ShouldUseWPOverrideOutput(0, 2));
		TestTrue(TEXT("Only the final endpoint consumes OverrideOutput."),
			ShouldUseWPOverrideOutput(1, 2));
		TestFalse(TEXT("An empty endpoint chain cannot consume OverrideOutput."),
			ShouldUseWPOverrideOutput(0, 0));

		const uint64 ViewKey = 1234;
		TestNotEqual(TEXT("Two pairs in one view retain independent transition fingerprints."),
			MakeWPPairViewStorageKey(ViewKey, 10),
			MakeWPPairViewStorageKey(ViewKey, 20));

		FWPPairOwnershipFeedbackState PairAFeedback;
		FWPPairOwnershipFeedbackState PairBFeedback;
		PairAFeedback.RecordWarmupPassForTest(7, 70, true);
		PairBFeedback.ReportProductionFailureForTest(8, 80);
		const FWPPairOwnershipFeedback PairA = PairAFeedback.ReadGameThread();
		const FWPPairOwnershipFeedback PairB = PairBFeedback.ReadGameThread();
		TestEqual(TEXT("A second pair's failure cannot clear the first pair's warmup ACK."),
			PairA.WarmupSucceededEpoch, 7ull);
		TestEqual(TEXT("The successful pair retains its exact ACK sequence."),
			PairA.WarmupSucceededPacketSequence, 70ull);
		TestEqual(TEXT("The failed pair publishes only its own failure epoch."),
			PairB.ProductionFailedEpoch, 8ull);
		TestEqual(TEXT("The failed pair publishes only its own failure sequence."),
			PairB.ProductionFailedPacketSequence, 80ull);
		TestEqual(TEXT("The failed pair never fabricates a warmup ACK."),
			PairB.WarmupSucceededEpoch, 0ull);
		return true;
	}

#endif
}

FWPSceneViewExtension::FWPSceneViewExtension(
	const FAutoRegister& AutoRegister,
	UWorld* World,
	TSharedRef<FWPRenderState, ESPMode::ThreadSafe> InRenderState)
	: FWorldSceneViewExtension(AutoRegister, World)
	, RenderState(MoveTemp(InRenderState))
{
}

bool FWPSceneViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	const int32 RegisteredPairCount = RenderState->RegisteredPairCount.Load();
	return IsWPSceneViewExtensionMasterEnabled(
			CVarWPSceneViewExtensionEnabled.GetValueOnAnyThread())
		&& ShouldActivateWPSceneViewExtension(
			RenderState->ActiveOwnershipPairCount.Load(),
			RenderState->ActiveVisibilityObservationPairCount.Load())
		&& !RenderState->bShuttingDown.Load()
		&& RegisteredPairCount > 0
		&& FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context);
}

void FWPSceneViewExtension::PreRenderView_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView)
{
	(void)GraphBuilder;
	RecordVisibilityObservations_RenderThread(InView);
}

void FWPSceneViewExtension::RecordVisibilityObservations_RenderThread(
	const FSceneView& View)
{
	check(IsInRenderingThread());
	SCOPE_CYCLE_COUNTER(STAT_WP_VisibilityObservation);
#if !UE_BUILD_SHIPPING
	const double ObservationStartSeconds = FPlatformTime::Seconds();
	++VisibilityObservationViewInvocationCount;
#endif

	if (RenderState->ActiveVisibilityObservationPairCount.Load() <= 0)
	{
#if !UE_BUILD_SHIPPING
		VisibilityObservationAccumulatedCpuMs +=
			(FPlatformTime::Seconds() - ObservationStartSeconds) * 1000.0;
		MaybeLogVisibilityObservationSummary_RenderThread();
#endif
		return;
	}

	const FWPOwnershipViewPolicyResult ViewPolicy =
		EvaluateWPOwnershipViewPolicy(MakeWPOwnershipViewPolicyInput(
			View, RenderState->bPIEWorld,
			CVarWPSimulateViewEnabled.GetValueOnRenderThread() != 0));
	if (ViewPolicy.Kind != EWPOwnershipViewKind::PrimaryPlayer)
	{
#if !UE_BUILD_SHIPPING
		++VisibilityObservationRejectedViewCount;
		const double ObservationCpuMs =
			(FPlatformTime::Seconds() - ObservationStartSeconds) * 1000.0;
		VisibilityObservationAccumulatedCpuMs += ObservationCpuMs;
		VisibilityObservationMaxCpuMs = FMath::Max(
			VisibilityObservationMaxCpuMs, ObservationCpuMs);
		MaybeLogVisibilityObservationSummary_RenderThread();
#endif
		return;
	}

	const FVector3d ViewOrigin(View.ViewMatrices.GetViewOrigin());
	for (const TPair<uint64, FWPRenderThreadPacket>& Pair :
		RenderState->PairsRenderThread)
	{
#if !UE_BUILD_SHIPPING
		const uint64 HandleValue = Pair.Key;
#endif
		const FWPRenderThreadPacket& Packet = Pair.Value;
		if (!IsWPVisibilityObservationPacketReady(Packet)
			|| !ShouldRecordWPVisibilityFeedback(
				ViewPolicy.Kind,
				View.ViewActor.ActorUniqueId,
				Packet.ReferenceViewActorId))
		{
			continue;
		}

#if !UE_BUILD_SHIPPING
		const double PairObservationStartSeconds = FPlatformTime::Seconds();
		++VisibilityObservationPairEvaluationCount;
#endif
		const float SafeProxyRadiusA = static_cast<float>(
			WPPortalVisibilityMath::GetSafeProxyRadiusCm(
				Packet.MetricA.PortalRadiusCm, Packet.MetricA.OuterRadiusCm));
		const float SafeProxyRadiusB = static_cast<float>(
			WPPortalVisibilityMath::GetSafeProxyRadiusCm(
				Packet.MetricB.PortalRadiusCm, Packet.MetricB.OuterRadiusCm));
		const auto IsEndpointVisible = [&View, &ViewOrigin](
			const FVector3d& Center,
			const float SafeProxyRadius)
		{
			return FVector3d::DistSquared(ViewOrigin, Center)
					<= FMath::Square(static_cast<double>(SafeProxyRadius))
				|| View.GetCullingFrustum().IntersectSphere(
					FVector(Center), SafeProxyRadius);
		};
		const bool bEndpointAVisible = IsEndpointVisible(
			Packet.PortalACenterWorld, SafeProxyRadiusA);
		const bool bEndpointBVisible = IsEndpointVisible(
			Packet.PortalBCenterWorld, SafeProxyRadiusB);
		const uint8 VisibleEndpointMask =
			(bEndpointAVisible ? 0x1u : 0u)
			| (bEndpointBVisible ? 0x2u : 0u);

		Packet.RecordVisibilitySample_RenderThread(
			GFrameNumberRenderThread, VisibleEndpointMask);

#if !UE_BUILD_SHIPPING
		const int32 VisibleEndpointCount =
			(bEndpointAVisible ? 1 : 0) + (bEndpointBVisible ? 1 : 0);
		++VisibilityObservationSampleWriteCount;
		uint64 Fingerprint = 1469598103934665603ull;
		Fingerprint = MixWPHash(Fingerprint, VisibleEndpointMask);
		Fingerprint = MixWPHash(Fingerprint, Packet.ReferenceViewActorId);
		Fingerprint = MixWPHash(Fingerprint,
			IsWPOwnershipPacketReadyForRendering(Packet) ? 1u : 0u);
		const uint64* PreviousFingerprint =
			LastVisibilityObservationFingerprintByHandleRenderThread.Find(HandleValue);
		if (!PreviousFingerprint || *PreviousFingerprint != Fingerprint)
		{
			LastVisibilityObservationFingerprintByHandleRenderThread.Add(
				HandleValue, Fingerprint);
			WP_LOG(nullptr, VeryVerbose,
				TEXT("[RenderThread][VisibilityObservation] State changed. World=%s PairId=%s Handle=%llu OwnershipEpoch=%llu PacketSequence=%llu ViewKey=%u ViewActorId=%u ReferenceViewActorId=%u VisibleEndpointMask=0x%02x VisibleEndpointCount=%d EndpointAVisible=%d EndpointBVisible=%d SafeProxyRadiusA=%.3f SafeProxyRadiusB=%.3f OwnershipRenderingReady=%d VisibilityObservationReady=1 CaptureGenerationA=%u CaptureGenerationB=%u CubemapRequired=0 LUTRequired=0 CaptureGenerationRequired=0 CompositeAttempted=0 RDGPassSubmitted=0 FeedbackMailboxPending=1 PairCpuMs=%.4f"),
				*RenderState->WorldName, *Packet.PairId.ToString(), HandleValue,
				Packet.OwnershipEpoch, Packet.PacketSequence,
				View.GetViewKey(), View.ViewActor.ActorUniqueId,
				Packet.ReferenceViewActorId,
				static_cast<uint32>(VisibleEndpointMask), VisibleEndpointCount,
				bEndpointAVisible ? 1 : 0, bEndpointBVisible ? 1 : 0,
				SafeProxyRadiusA, SafeProxyRadiusB,
				IsWPOwnershipPacketReadyForRendering(Packet) ? 1 : 0,
				Packet.CaptureGenerationA, Packet.CaptureGenerationB,
				(FPlatformTime::Seconds() - PairObservationStartSeconds) * 1000.0);
		}
#endif
	}

#if !UE_BUILD_SHIPPING
	const double ObservationCpuMs =
		(FPlatformTime::Seconds() - ObservationStartSeconds) * 1000.0;
	VisibilityObservationAccumulatedCpuMs += ObservationCpuMs;
	VisibilityObservationMaxCpuMs = FMath::Max(
		VisibilityObservationMaxCpuMs, ObservationCpuMs);
	MaybeLogVisibilityObservationSummary_RenderThread();
#endif
}

void FWPSceneViewExtension::SubscribeToPostProcessingPass(
	const EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	const bool bIsPassEnabled)
{
	(void)bIsPassEnabled;
	if (Pass != EPostProcessingPass::MotionBlur)
	{
		return;
	}
	const int32 ActiveOwnershipPairCount = RenderState->ActiveOwnershipPairCount.Load();
	if (ActiveOwnershipPairCount <= 0)
	{
		// No-active-pair must be a true zero-pass fast path. Runtime state changes are
		// observed on the next frame, so this does not prevent subsequent activation.
		return;
	}
	SCOPE_CYCLE_COUNTER(STAT_WP_ViewFilter);

	// 로그 전용: rejection reason 문자열이며 실제 accept/reject는 bAccepted가 보존한다.
	const TCHAR* SkipReason = TEXT("None");
	const bool bAccepted = ShouldAcceptView_RenderThread(InView, SkipReason);

	if (!bAccepted)
	{
		if (FCString::Stricmp(SkipReason, TEXT("SceneCapture")) == 0)
		{
			INC_DWORD_STAT(STAT_WP_SceneCaptureViewsSkipped);
		}
		if (ActiveOwnershipPairCount > 0)
		{
#if !UE_BUILD_SHIPPING
			++OwnershipUnsupportedViewCount;
#endif
			MaybeLogOwnershipSummary_RenderThread();
		}
		return;
	}
	if (TryClaimWPWormholeLayerOwnership_RenderThread(InView))
	{
		return;
	}

	INC_DWORD_STAT(STAT_WP_AcceptedViews);
	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
		this, &FWPSceneViewExtension::PostProcessPassAfterMotionBlur_RenderThread));
}

bool FWPSceneViewExtension::ShouldAcceptView_RenderThread(
	const FSceneView& View, const TCHAR*& OutSkipReason) const
{
	if (!View.Family || !View.Family->Scene)
	{
		OutSkipReason = TEXT("MissingFamilyOrScene");
		return false;
	}

	// 같은 World의 Cube Capture에도 Extension이 호출되므로 self-feedback을 가장 먼저 차단합니다.
	if (View.bIsSceneCapture || View.bIsReflectionCapture || View.bIsPlanarReflection)
	{
		OutSkipReason = TEXT("SceneCapture");
		return false;
	}

	if (!View.bIsGameView)
	{
		OutSkipReason = TEXT("NonGameView");
		return false;
	}

	if (!View.IsPerspectiveProjection() || View.GetFeatureLevel() < ERHIFeatureLevel::SM5)
	{
		OutSkipReason = TEXT("UnsupportedProjectionOrFeatureLevel");
		return false;
	}

	if (RenderState->PairsRenderThread.IsEmpty())
	{
		OutSkipReason = TEXT("NoRenderThreadPackets");
		return false;
	}

	return true;
}

bool FWPSceneViewExtension::ShouldAcceptOwnershipView_RenderThread(
	const FSceneView& View,
	const TCHAR*& OutSkipReason) const
{
	const FWPOwnershipViewPolicyResult Policy = EvaluateWPOwnershipViewPolicy(
		MakeWPOwnershipViewPolicyInput(
			View,
			RenderState->bPIEWorld,
			CVarWPSimulateViewEnabled.GetValueOnRenderThread() != 0));
	OutSkipReason = Policy.Reason;
	return Policy.IsAccepted();
}

EWPEligibilityReason FWPSceneViewExtension::EvaluateEligibility_RenderThread(
	const FWPRenderThreadPacket& Packet) const
{
	if (!Packet.bEnabled)
	{
		return EWPEligibilityReason::Disabled;
	}

	if (!Packet.PairId.IsValid()
		|| !IsFiniteSceneViewVector(Packet.PortalACenterWorld)
		|| !IsFiniteSceneViewVector(Packet.PortalBCenterWorld)
		|| !IsFiniteInvertibleSceneViewMatrix(Packet.PortalAToWorld)
		|| !IsFiniteInvertibleSceneViewMatrix(Packet.WorldToPortalA)
		|| !IsFiniteInvertibleSceneViewMatrix(Packet.PortalBToWorld)
		|| !IsFiniteInvertibleSceneViewMatrix(Packet.WorldToPortalB))
	{
		return EWPEligibilityReason::InvalidPairOrTransform;
	}

	if (!Packet.MetricA.IsFiniteAndValid() || !Packet.MetricB.IsFiniteAndValid())
	{
		return EWPEligibilityReason::MetricInvalid;
	}

	if (!Packet.bMetricCompatible || !Packet.MetricA.IsCompatibleWith(Packet.MetricB))
	{
		return EWPEligibilityReason::MetricMismatch;
	}

	if (!Packet.bScaleSupported)
	{
		return EWPEligibilityReason::UnsupportedScale;
	}

	if (!Packet.bCaptureReady || Packet.CaptureGenerationA == 0 || Packet.CaptureGenerationB == 0)
	{
		return EWPEligibilityReason::CaptureNotSubmitted;
	}

	if (!Packet.CubeA.IsValid() || !Packet.CubeB.IsValid()
		|| (!Packet.bAnalyticNoTransitionA && !Packet.RayLUTA.IsValid())
		|| (!Packet.bAnalyticNoTransitionB && !Packet.RayLUTB.IsValid()))
	{
		return EWPEligibilityReason::MissingTextureReference;
	}

	FRHITexture* CubeReferenceProxyA =
		GetTextureReferenceProxy(Packet.CubeA);
	FRHITexture* CubeReferenceProxyB =
		GetTextureReferenceProxy(Packet.CubeB);
	const FRHITexture* RayLUTA = ResolveTextureReference(Packet.RayLUTA);
	const FRHITexture* RayLUTB = ResolveTextureReference(Packet.RayLUTB);
	if (!CubeReferenceProxyA || !CubeReferenceProxyB
		|| CubeReferenceProxyA->GetTextureReference() != CubeReferenceProxyA
		|| CubeReferenceProxyB->GetTextureReference() != CubeReferenceProxyB
		|| (!Packet.bAnalyticNoTransitionA && !RayLUTA)
		|| (!Packet.bAnalyticNoTransitionB && !RayLUTB))
	{
		return EWPEligibilityReason::UnresolvedReferencedTexture;
	}

	if (!Packet.CubeContractA.IsValid()
		|| !Packet.CubeContractB.IsValid()
		|| (!Packet.bAnalyticNoTransitionA
			&& RayLUTA->GetDesc().Dimension != ETextureDimension::Texture3D)
		|| (!Packet.bAnalyticNoTransitionB
			&& RayLUTB->GetDesc().Dimension != ETextureDimension::Texture3D)
		|| (!Packet.bAnalyticNoTransitionA && !HasPositiveTextureExtent(RayLUTA))
		|| (!Packet.bAnalyticNoTransitionB && !HasPositiveTextureExtent(RayLUTB)))
	{
		return EWPEligibilityReason::WrongTextureDimension;
	}

	return EWPEligibilityReason::Eligible;
}

void FWPSceneViewExtension::RecordEligibilityTransition_RenderThread(
	const uint64 HandleValue,
	const FWPRenderThreadPacket& Packet,
	const EWPEligibilityReason Reason,
	const double CpuMs)
{
	// 로그 전용: Current/Previous와 cache는 동일 production eligibility 전환 로그의
	// 반복 출력을 억제하며 eligibility 판정에는 영향을 주지 않는다.
	const FEligibilityFingerprint Current{Reason};
	const FEligibilityFingerprint* Previous = LastEligibilityByHandle.Find(HandleValue);
	if (Previous && Previous->Equals(Current))
	{
		return;
	}

	const EWPEligibilityReason PreviousReason = Previous
		? Previous->Reason
		: EWPEligibilityReason::Count;
	const bool bRegressed = Previous
		&& PreviousReason == EWPEligibilityReason::Eligible
		&& Reason != EWPEligibilityReason::Eligible;

	if (bRegressed)
	{
		const FRHITexture* CubeReferenceProxyA =
			GetTextureReferenceProxy(Packet.CubeA);
		const FRHITexture* CubeReferenceProxyB =
			GetTextureReferenceProxy(Packet.CubeB);
		const FRHITexture* RayLUTA = ResolveTextureReference(Packet.RayLUTA);
		const FRHITexture* RayLUTB = ResolveTextureReference(Packet.RayLUTB);
		const FString PairId = Packet.PairId.ToString();
		const TCHAR* PreviousName = Previous
			? GetWPEligibilityReasonName(PreviousReason)
			: TEXT("Unobserved");
		WP_LOG(nullptr, Warning,
			TEXT("[RenderThread][Production][Eligibility] Regressed. World=%s PairId=%s Handle=%llu Previous=%s Current=%s Sequence=%llu CaptureA=%u CaptureB=%u CaptureSubmitted=%d MetricCompatible=%d ScaleSupported=%d RefCubeA=%d RefCubeB=%d RefLUTA=%d RefLUTB=%d CubeReferenceProxyA=%d CubeReferenceProxyB=%d RHILUTA=%d RHILUTB=%d RequestedOwnership=%s EffectiveOwnership=%s OwnershipEpoch=%llu OwnershipInputsReady=%d CpuMs=%.4f"),
			*RenderState->WorldName, *PairId, HandleValue, PreviousName,
			GetWPEligibilityReasonName(Reason), Packet.PacketSequence,
			Packet.CaptureGenerationA, Packet.CaptureGenerationB,
			Packet.bCaptureReady ? 1 : 0, Packet.bMetricCompatible ? 1 : 0,
			Packet.bScaleSupported ? 1 : 0, Packet.CubeA.IsValid() ? 1 : 0,
			Packet.CubeB.IsValid() ? 1 : 0, Packet.RayLUTA.IsValid() ? 1 : 0,
			Packet.RayLUTB.IsValid() ? 1 : 0,
			CubeReferenceProxyA ? 1 : 0, CubeReferenceProxyB ? 1 : 0,
			RayLUTA ? 1 : 0, RayLUTB ? 1 : 0,
			GetWPPairOwnershipModeName(Packet.RequestedOwnership),
			GetWPPairOwnershipModeName(Packet.EffectiveOwnership),
			Packet.OwnershipEpoch, Packet.bOwnershipInputsReady ? 1 : 0, CpuMs);
	}
#if !UE_BUILD_SHIPPING
	else
	{
		const FRHITexture* CubeReferenceProxyA =
			GetTextureReferenceProxy(Packet.CubeA);
		const FRHITexture* CubeReferenceProxyB =
			GetTextureReferenceProxy(Packet.CubeB);
		const FRHITexture* RayLUTA = ResolveTextureReference(Packet.RayLUTA);
		const FRHITexture* RayLUTB = ResolveTextureReference(Packet.RayLUTB);
		const FString PairId = Packet.PairId.ToString();
		const TCHAR* PreviousName = Previous
			? GetWPEligibilityReasonName(PreviousReason)
			: TEXT("Unobserved");
		WP_LOG(nullptr, Verbose,
			TEXT("[RenderThread][Production][Eligibility] Changed. World=%s PairId=%s Handle=%llu Previous=%s Current=%s Sequence=%llu CaptureA=%u CaptureB=%u CaptureSubmitted=%d MetricCompatible=%d ScaleSupported=%d RefCubeA=%d RefCubeB=%d RefLUTA=%d RefLUTB=%d CubeReferenceProxyA=%d CubeReferenceProxyB=%d RHILUTA=%d RHILUTB=%d RequestedOwnership=%s EffectiveOwnership=%s OwnershipEpoch=%llu OwnershipInputsReady=%d CpuMs=%.4f"),
			*RenderState->WorldName, *PairId, HandleValue, PreviousName,
			GetWPEligibilityReasonName(Reason), Packet.PacketSequence,
			Packet.CaptureGenerationA, Packet.CaptureGenerationB,
			Packet.bCaptureReady ? 1 : 0, Packet.bMetricCompatible ? 1 : 0,
			Packet.bScaleSupported ? 1 : 0, Packet.CubeA.IsValid() ? 1 : 0,
			Packet.CubeB.IsValid() ? 1 : 0, Packet.RayLUTA.IsValid() ? 1 : 0,
			Packet.RayLUTB.IsValid() ? 1 : 0,
			CubeReferenceProxyA ? 1 : 0, CubeReferenceProxyB ? 1 : 0,
			RayLUTA ? 1 : 0, RayLUTB ? 1 : 0,
			GetWPPairOwnershipModeName(Packet.RequestedOwnership),
			GetWPPairOwnershipModeName(Packet.EffectiveOwnership),
			Packet.OwnershipEpoch, Packet.bOwnershipInputsReady ? 1 : 0, CpuMs);
	}
#endif

	LastEligibilityByHandle.Add(HandleValue, Current);
}

bool FWPSceneViewExtension::TryPostProcessMultiPairOwnershipPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs,
	FScreenPassTexture& OutOutput)
{
	// 로그 전용: StartSeconds/CallbackCpuMs는 callback 경과 시간과 ownership summary만 측정한다.
	const double StartSeconds = FPlatformTime::Seconds();
	const uint64 ViewStorageKey = MakeViewKey(View);

	struct FActivePair
	{
		uint64 HandleValue = 0;
		const FWPRenderThreadPacket* Packet = nullptr;
	};
	TArray<FActivePair, TInlineAllocator<8>> ActivePairs;
	for (const TPair<uint64, FWPRenderThreadPacket>& Pair : RenderState->PairsRenderThread)
	{
		if (IsWPOwnershipPacketReadyForRendering(Pair.Value))
		{
			ActivePairs.Add({Pair.Key, &Pair.Value});
		}
	}
	if (ActivePairs.Num() <= 1)
	{
		return false;
	}
	ActivePairs.Sort([](const FActivePair& Left, const FActivePair& Right)
	{
		return Left.HandleValue < Right.HandleValue;
	});

#if !UE_BUILD_SHIPPING
	++OwnershipCallbackInvocationCount;
	OwnershipPairAttemptCount += ActivePairs.Num();
#endif
	const auto FinishCallback = [this, StartSeconds](const bool bHandled)
	{
#if !UE_BUILD_SHIPPING
		const double CallbackCpuMs =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
		OwnershipMaxCallbackCpuMs = FMath::Max(
			OwnershipMaxCallbackCpuMs, CallbackCpuMs);
#endif
		MaybeLogOwnershipSummary_RenderThread();
		return bHandled;
	};

	// 로그 전용: result fingerprint/cache와 이 lambda의 반환값은 동일 결과 로그만 억제하며
	// pair의 warmup/production 성공 여부나 출력에는 관여하지 않는다.
	const auto RecordOwnershipResult = [this, &View, ViewStorageKey,
		StartSeconds, ActivePairCount = ActivePairs.Num()](
		const uint64 HandleValue,
		const FWPRenderThreadPacket* Packet,
		const TCHAR* Reason,
		const int32 VisibleEndpoints,
		const bool bPassSucceeded,
		const double SetupCpuMs) -> bool
	{
		uint64 Fingerprint = 1469598103934665603ull;
		Fingerprint = MixWPHash(Fingerprint, HandleValue);
		Fingerprint = MixWPHash(
			Fingerprint, Packet ? Packet->OwnershipEpoch : 0);
		if (!bPassSucceeded)
		{
			// A fresh packet is meaningful when a failure is retried, but transport-only
			// heartbeat sequence changes must not emit successful state logs every second.
			Fingerprint = MixWPHash(
				Fingerprint, Packet ? Packet->PacketSequence : 0);
		}
		Fingerprint = MixWPHash(Fingerprint,
			Packet ? static_cast<uint64>(Packet->EffectiveOwnership) : 0);
		Fingerprint = MixWPHash(
			Fingerprint, HashWPReason(Reason));
		Fingerprint = MixWPHash(
			Fingerprint, static_cast<uint64>(VisibleEndpoints));
		Fingerprint = MixWPHash(Fingerprint, bPassSucceeded ? 1u : 0u);
		const uint64 StorageKey = MakeWPPairViewStorageKey(
			ViewStorageKey, HandleValue);
		const uint64* Previous = LastOwnershipResultFingerprintByViewRenderThread.Find(
			StorageKey);
		if (Previous && *Previous == Fingerprint)
		{
			return false;
		}
		if (LastOwnershipResultFingerprintByViewRenderThread.Num() >= 1024
			&& !LastOwnershipResultFingerprintByViewRenderThread.Contains(StorageKey))
		{
			LastOwnershipResultFingerprintByViewRenderThread.Reset();
		}
		LastOwnershipResultFingerprintByViewRenderThread.Add(StorageKey, Fingerprint);

#if !UE_BUILD_SHIPPING
		// 로그 전용: 이미 결정된 결과 로그에 view 종류를 기록하기 위한 snapshot이다.
		const FWPOwnershipViewPolicyResult ViewPolicy =
			EvaluateWPOwnershipViewPolicy(MakeWPOwnershipViewPolicyInput(
				View, RenderState->bPIEWorld,
				CVarWPSimulateViewEnabled.GetValueOnRenderThread() != 0));
		WP_LOG(nullptr, VeryVerbose,
			TEXT("[RenderThread][Production][MultiPair] Pair view result changed. World=%s ActivePairCount=%d PairId=%s Handle=%llu SelectorA=%s SelectorB=%s RequestedMode=%s EffectiveMode=%s OwnershipEpoch=%llu PacketSequence=%llu ViewStorageKey=%llu ViewKey=%u ViewActorId=%u PlayerIndex=%d OwnershipViewKind=%s PIEWorld=%d HasViewElementDrawer=%d VirtualTexture=%d OfflineRender=%d CustomRenderPass=%d VisibleEndpoints=%d OcclusionValid=%d OcclusionVisibleMask=0x%02x PassSucceeded=%d Reason=%s QueueLatencyMs=%.4f SetupCpuMs=%.4f CallbackCpuMs=%.4f ProductionGpuStat=WP.ProductionComposite FailClosed=PortalAbsentOnUntouchedSceneColor GlobalEndpointOrder=FarToNearSurfaceStableSelectorPairIdHandleSide"),
			*RenderState->WorldName, ActivePairCount,
			Packet && Packet->PairId.IsValid() ? *Packet->PairId.ToString() : TEXT("None"),
			HandleValue,
			Packet ? *Packet->StableSelectorNameA.ToString() : TEXT("None"),
			Packet ? *Packet->StableSelectorNameB.ToString() : TEXT("None"),
			Packet ? GetWPPairOwnershipModeName(Packet->RequestedOwnership)
				: TEXT("Disabled"),
			Packet ? GetWPPairOwnershipModeName(Packet->EffectiveOwnership)
				: TEXT("Disabled"),
			Packet ? Packet->OwnershipEpoch : 0,
			Packet ? Packet->PacketSequence : 0,
			ViewStorageKey, View.GetViewKey(), View.ViewActor.ActorUniqueId,
			View.PlayerIndex, GetWPOwnershipViewKindName(ViewPolicy.Kind),
			RenderState->bPIEWorld ? 1 : 0, View.Drawer ? 1 : 0,
			View.bIsVirtualTexture ? 1 : 0, View.bIsOfflineRender ? 1 : 0,
			View.CustomRenderPass ? 1 : 0, VisibleEndpoints,
			Packet && Packet->bCaptureOcclusionValid ? 1 : 0,
			Packet
				? static_cast<uint32>(Packet->CaptureOcclusionVisibleEndpointMask)
				: 0x3u,
			bPassSucceeded ? 1 : 0, Reason,
			Packet ? Packet->QueueLatencyMs : 0.0, SetupCpuMs,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return true;
	};

	const int32 RawOwnershipForceFailureCallbacks = FMath::Max(
		GetWPOwnershipForceProductionFailureCallbacks_RenderThread(), -1);
	if (RawOwnershipForceFailureCallbacks != LastObservedOwnershipForceFailureCallbacks)
	{
#if !UE_BUILD_SHIPPING
		// 로그 전용: 설정 변경 메시지의 PreviousConfigured 필드용 이전 값 snapshot이다.
		const int32 PreviousConfigured = LastObservedOwnershipForceFailureCallbacks;
#endif
		LastObservedOwnershipForceFailureCallbacks = RawOwnershipForceFailureCallbacks;
		RemainingOwnershipForceFailureCallbacks = RawOwnershipForceFailureCallbacks > 0
			? RawOwnershipForceFailureCallbacks : 0;
#if !UE_BUILD_SHIPPING
		WP_LOG(nullptr, Verbose,
			TEXT("[RenderThread][Ownership][MultiPair] Production failure hook changed. World=%s PreviousConfigured=%d Configured=%d Remaining=%d PersistentFailure=%d ActivePairCount=%d ProductionResourcesModified=0 CpuMs=%.4f"),
			*RenderState->WorldName, PreviousConfigured,
			RawOwnershipForceFailureCallbacks,
			RemainingOwnershipForceFailureCallbacks,
			RawOwnershipForceFailureCallbacks == -1 ? 1 : 0,
			ActivePairs.Num(),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}

	struct FPairWork
	{
		uint64 HandleValue = 0;
		const FWPRenderThreadPacket* Packet = nullptr;
		FVector3f PortalCenterTranslatedA = FVector3f::ZeroVector;
		FVector3f PortalCenterTranslatedB = FVector3f::ZeroVector;
		int32 VisibleEndpointCount = 0;
		int32 ExpectedEndpointCount = 0;
		int32 SubmittedEndpointCount = 0;
		// 로그 전용: pair별 setup 비용의 누적/최댓값이며 pass 성공 판정에는 쓰지 않는다.
		double TotalSetupCpuMs = 0.0;
		double MaxSetupAttemptCpuMs = 0.0;
		bool bWarmup = false;
		bool bProduction = false;
		bool bFailed = false;
		bool bLatchedSkip = false;
		bool bNoVisibleEndpoint = false;
		bool bFullyOccluded = false;
	};
	struct FEndpointWork
	{
		int32 PairIndex = INDEX_NONE;
		FWPProductionEndpointOrderKey OrderKey;
		FWPCompositePassParameters Parameters;
	};

	TArray<FPairWork, TInlineAllocator<8>> PairWorks;
	PairWorks.Reserve(ActivePairs.Num());
	for (const FActivePair& ActivePair : ActivePairs)
	{
		FPairWork& Pair = PairWorks.AddDefaulted_GetRef();
		Pair.HandleValue = ActivePair.HandleValue;
		Pair.Packet = ActivePair.Packet;
		Pair.bWarmup = ActivePair.Packet->EffectiveOwnership
			== EWPPairOwnershipMode::Warmup;
		Pair.bProduction = ActivePair.Packet->EffectiveOwnership
			== EWPPairOwnershipMode::Production;
	}

	// 로그 전용: ownership view rejection 메시지/fingerprint에 넣을 reason 문자열이다.
	const TCHAR* OwnershipViewSkipReason = TEXT("None");
	if (!ShouldAcceptOwnershipView_RenderThread(View, OwnershipViewSkipReason))
	{
		++OwnershipUnsupportedViewCount;
		for (FPairWork& Pair : PairWorks)
		{
			RecordOwnershipResult(Pair.HandleValue, Pair.Packet,
				OwnershipViewSkipReason, 0, false, 0.0);
		}
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		return FinishCallback(true);
	}

	const FScreenPassTextureSlice SceneColorSlice =
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	const FScreenPassTexture BaseSceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder, SceneColorSlice);
	FRDGTextureRef SceneDepthTexture = Inputs.SceneTextures.SceneTextures
		? Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture
		: nullptr;
	const FRDGTextureRef SceneColorSourceTexture = SceneColorSlice.TextureSRV
		? SceneColorSlice.TextureSRV->Desc.Texture : nullptr;
	const bool bSceneColorUsesArray = SceneColorSourceTexture
		&& SceneColorSourceTexture->Desc.Dimension == ETextureDimension::Texture2DArray;
	const int32 SceneColorArraySlice = bSceneColorUsesArray
		? static_cast<int32>(SceneColorSlice.TextureSRV->Desc.FirstArraySlice)
		: INDEX_NONE;
	const bool bSceneDepthArray = SceneDepthTexture
		&& SceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2DArray;
	const int32 SceneDepthArraySlice = bSceneDepthArray
		? WPPortalMaskMath::ResolveSceneDepthArraySlice(
			bSceneColorUsesArray,
			SceneColorArraySlice,
			View.StereoViewIndex,
			static_cast<int32>(SceneDepthTexture->Desc.ArraySize))
		: 0;

	const auto FailPair = [this, &RecordOwnershipResult, &PairWorks, StartSeconds](
		FPairWork& Pair,
		const TCHAR* Reason,
		const TCHAR* FailureStage)
	{
		if (Pair.bFailed)
		{
			return;
		}
		Pair.bFailed = true;
		Pair.Packet->ReportProductionFailure_RenderThread();
		if (Pair.bWarmup)
		{
			FOwnershipWarmupTracker& FailureTracker =
				OwnershipWarmupByHandleRenderThread.FindOrAdd(Pair.HandleValue);
			FailureTracker = FOwnershipWarmupTracker();
			FailureTracker.Epoch = Pair.Packet->OwnershipEpoch;
			FailureTracker.PacketSequence = Pair.Packet->PacketSequence;
			FailureTracker.bFailureLatched = true;
#if !UE_BUILD_SHIPPING
			++OwnershipWarmupFailureCount;
#endif
		}
		else
		{
#if !UE_BUILD_SHIPPING
			++OwnershipProductionFailureCount;
#endif
		}
		const bool bPairFailureOccurredDuringPreflight = PairWorks.Num() > 1
			&& (FCString::Strcmp(FailureStage, TEXT("PairPreflight")) == 0
				|| FCString::Strcmp(FailureStage, TEXT("EndpointPreflight")) == 0);
		if (bPairFailureOccurredDuringPreflight)
		{
#if !UE_BUILD_SHIPPING
			++OwnershipPreflightPairFailureCount;
#endif
		}
		const bool bNewFailureFingerprint = RecordOwnershipResult(
			Pair.HandleValue, Pair.Packet, Reason, Pair.VisibleEndpointCount,
			false, Pair.TotalSetupCpuMs);
		if (bNewFailureFingerprint)
		{
			WP_LOG(nullptr, Warning,
				TEXT("[RenderThread][ProductionFailure][MultiPair] Pair failed closed. World=%s WorldType=%s ActivePairCount=%d PairId=%s Handle=%llu RequestedMode=%s EffectiveMode=%s OwnershipEpoch=%llu PacketSequence=%llu Reason=%s FailureStage=%s Warmup=%d Production=%d ExpectedEndpoints=%d SubmittedEndpoints=%d Recovery=%s PairSetupCpuMs=%.4f PairMaxSetupAttemptCpuMs=%.4f CallbackCpuMs=%.4f PairFailureOccurredDuringPreflight=%d ViewFailurePolicy=AtomicRollback UntouchedSceneColor=1 PortalAbsentFailClosed=1 ProductionResourcesModified=0"),
				*RenderState->WorldName, *RenderState->WorldType, PairWorks.Num(),
				*Pair.Packet->PairId.ToString(), Pair.HandleValue,
				GetWPPairOwnershipModeName(Pair.Packet->RequestedOwnership),
				GetWPPairOwnershipModeName(Pair.Packet->EffectiveOwnership),
				Pair.Packet->OwnershipEpoch, Pair.Packet->PacketSequence,
				Reason, FailureStage, Pair.bWarmup ? 1 : 0, Pair.bProduction ? 1 : 0,
				Pair.ExpectedEndpointCount, Pair.SubmittedEndpointCount,
				Pair.bWarmup ? TEXT("FreshPacketRetry") : TEXT("NextOwnershipEpochWarmup"),
				Pair.TotalSetupCpuMs, Pair.MaxSetupAttemptCpuMs,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
				bPairFailureOccurredDuringPreflight ? 1 : 0);
		}
	};

	if (!BaseSceneColor.IsValid())
	{
		for (FPairWork& Pair : PairWorks)
		{
			FailPair(Pair, TEXT("InvalidBaseSceneColor"), TEXT("SharedInput"));
		}
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		return FinishCallback(true);
	}

	TArray<FEndpointWork, TInlineAllocator<16>> GlobalEndpoints;
	const FVector3d ViewOrigin(View.ViewMatrices.GetViewOrigin());
	if (!IsFiniteSceneViewVector(ViewOrigin))
	{
		for (FPairWork& Pair : PairWorks)
		{
			FailPair(Pair, TEXT("InvalidViewOrigin"), TEXT("SharedInput"));
		}
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		return FinishCallback(true);
	}
	for (int32 PairIndex = 0; PairIndex < PairWorks.Num(); ++PairIndex)
	{
		FPairWork& Pair = PairWorks[PairIndex];
		const FWPRenderThreadPacket& Packet = *Pair.Packet;

		if (!Packet.IsOwnershipContractReady())
		{
			FailPair(Pair, TEXT("OwnershipPacketContractInvalid"), TEXT("PairPreflight"));
			continue;
		}
		if (Pair.bWarmup)
		{
			const FOwnershipWarmupTracker* FailureTracker =
				OwnershipWarmupByHandleRenderThread.Find(Pair.HandleValue);
			if (FailureTracker
				&& FailureTracker->Epoch == Packet.OwnershipEpoch
				&& FailureTracker->PacketSequence == Packet.PacketSequence
				&& FailureTracker->bFailureLatched)
			{
				Pair.bLatchedSkip = true;
				++OwnershipWarmupLatchedSkipCount;
				RecordOwnershipResult(Pair.HandleValue, Pair.Packet,
					TEXT("WarmupFailureLatchedUntilFreshPacket"), 0,
					false, 0.0);
				continue;
			}
		}

		// 로그 전용: ownership eligibility 평가 비용을 전환/요약 로그에 기록한다.
		const double EligibilityStartSeconds = FPlatformTime::Seconds();
		const EWPEligibilityReason Eligibility = EvaluateEligibility_RenderThread(Packet);
		const double EligibilityCpuMs =
			(FPlatformTime::Seconds() - EligibilityStartSeconds) * 1000.0;
		++OwnershipEligibilityEvaluationCount;
		OwnershipAccumulatedEligibilityCpuMs += EligibilityCpuMs;
		OwnershipMaxEligibilityCpuMs = FMath::Max(
			OwnershipMaxEligibilityCpuMs, EligibilityCpuMs);
		RecordEligibilityTransition_RenderThread(
			Pair.HandleValue, Packet, Eligibility, EligibilityCpuMs);
		if (Eligibility != EWPEligibilityReason::Eligible)
		{
			FailPair(Pair, GetWPEligibilityReasonName(Eligibility),
				TEXT("PairPreflight"));
			continue;
		}

		if (!BuildWPProductionViewCenters(
			Packet,
			FVector3d(View.ViewMatrices.GetPreViewTranslation()),
			Pair.PortalCenterTranslatedA,
			Pair.PortalCenterTranslatedB))
		{
			FailPair(Pair, TEXT("PerViewStateBuildFailed"), TEXT("PairPreflight"));
			continue;
		}

		TArray<FEndpointWork, TInlineAllocator<2>> PairEndpoints;
		const auto AddEndpoint = [&View, &ViewOrigin, &PairEndpoints, &Pair,
			PairIndex, &Packet](
			const EWPSide Side,
			const FVector3d& Center,
			const float ProxyRadiusCm,
			const FName StableSelector)
		{
			const double DistanceSquared = FVector3d::DistSquared(ViewOrigin, Center);
			const bool bVisible = DistanceSquared
				<= FMath::Square(static_cast<double>(ProxyRadiusCm))
				|| View.GetCullingFrustum().IntersectSphere(FVector(Center), ProxyRadiusCm);
			Pair.VisibleEndpointCount += bVisible ? 1 : 0;
			const bool bCompositeVisible = bVisible
				&& IsWPProductionEndpointAllowedByOcclusion(Packet, Side);
			if (!Pair.bWarmup && !bCompositeVisible)
			{
				return;
			}

			const bool bEndpointA = Side == EWPSide::SideA;
			const FWPMetricSettings& Metric = bEndpointA
				? Packet.MetricA : Packet.MetricB;
			const FWPPortalVisualSettings& Visual = bEndpointA
				? Packet.VisualA : Packet.VisualB;
			const float VisualScale = Visual.IsFiniteAndValid()
				? Visual.UniformScale : 1.0f;
			const FWPRayLUTContract& LUTContract = bEndpointA
				? Packet.RayLUTContractA : Packet.RayLUTContractB;
			const FWPCubeContract& LocalCubeContract = bEndpointA
				? Packet.CubeContractA : Packet.CubeContractB;
			const FWPCubeContract& LinkedCubeContract = bEndpointA
				? Packet.CubeContractB : Packet.CubeContractA;
			const FMatrix44d& SelfToWorld = bEndpointA
				? Packet.PortalAToWorld : Packet.PortalBToWorld;
			const FMatrix44d& LinkedToWorld = bEndpointA
				? Packet.PortalBToWorld : Packet.PortalAToWorld;

			FEndpointWork& Endpoint = PairEndpoints.AddDefaulted_GetRef();
			Endpoint.PairIndex = PairIndex;
			Endpoint.OrderKey.NearSurfaceDistanceCm = FMath::Max(
				FMath::Sqrt(DistanceSquared) - static_cast<double>(ProxyRadiusCm),
				0.0);
			Endpoint.OrderKey.StableSelector = StableSelector;
			Endpoint.OrderKey.PairId = Packet.PairId;
			Endpoint.OrderKey.HandleValue = Pair.HandleValue;
			Endpoint.OrderKey.EndpointSide = Side;
			FWPCompositePassParameters& Pass = Endpoint.Parameters;
			Pass.PortalCenterTranslated = bEndpointA
				? Pair.PortalCenterTranslatedA
				: Pair.PortalCenterTranslatedB;
			Pass.PortalRadiusCm = Metric.PortalRadiusCm * VisualScale;
			Pass.ThroatLengthCm = Metric.ThroatHalfLengthCm * 2.0f * VisualScale;
			Pass.ProxyRadiusCm = ProxyRadiusCm * VisualScale;
			Pass.MetricOuterRadiusCm = Metric.MetricOuterRadiusCm * VisualScale;
			Pass.DepthBiasCm = 1.0f;
			Pass.SelfX = GetWPUnitAxis(SelfToWorld, EAxis::X);
			Pass.SelfY = GetWPUnitAxis(SelfToWorld, EAxis::Y);
			Pass.SelfZ = GetWPUnitAxis(SelfToWorld, EAxis::Z);
			Pass.LinkedX = GetWPUnitAxis(LinkedToWorld, EAxis::X);
			Pass.LinkedY = GetWPUnitAxis(LinkedToWorld, EAxis::Y);
			Pass.LinkedZ = GetWPUnitAxis(LinkedToWorld, EAxis::Z);
			const FTextureReferenceRHIRef& LUTReference = bEndpointA
				? Packet.RayLUTA : Packet.RayLUTB;
			Pass.RayLUTTexture = ResolveTextureReference(LUTReference);
			Pass.bAnalyticNoTransition = bEndpointA
				? Packet.bAnalyticNoTransitionA : Packet.bAnalyticNoTransitionB;
			PopulateWPRayLUTContractParameters(
				Pass,
				LUTContract,
				bEndpointA ? Packet.RayLUTZA : Packet.RayLUTZB,
				bEndpointA ? Packet.RayLUTRevisionA : Packet.RayLUTRevisionB);
			Pass.LocalCubeTextureReference = GetTextureReferenceProxy(
				bEndpointA ? Packet.CubeA : Packet.CubeB);
			Pass.ExpectedLocalCubeExtent = LocalCubeContract.ExpectedExtent;
			Pass.ExpectedLocalCubeFormat = GetWPCubeRHIPixelFormat(
				LocalCubeContract.ExpectedFormat);
			Pass.ExpectedLocalCubeMipCount = LocalCubeContract.ExpectedMipCount;
			Pass.LocalCubeLayoutVersion = LocalCubeContract.CubeLayoutVersion;
			Pass.LocalCubeResourceGeneration = LocalCubeContract.ResourceGeneration;
			Pass.LocalCubeCaptureGeneration = bEndpointA
				? Packet.CaptureGenerationA : Packet.CaptureGenerationB;
			Pass.bLocalCubeContractValid = LocalCubeContract.IsValid();
			Pass.LinkedCubeTextureReference = GetTextureReferenceProxy(
				bEndpointA ? Packet.CubeB : Packet.CubeA);
			Pass.ExpectedLinkedCubeExtent = LinkedCubeContract.ExpectedExtent;
			Pass.ExpectedLinkedCubeFormat = GetWPCubeRHIPixelFormat(
				LinkedCubeContract.ExpectedFormat);
			Pass.ExpectedLinkedCubeMipCount = LinkedCubeContract.ExpectedMipCount;
			Pass.LinkedCubeLayoutVersion = LinkedCubeContract.CubeLayoutVersion;
			Pass.LinkedCubeResourceGeneration = LinkedCubeContract.ResourceGeneration;
			Pass.LinkedCubeCaptureGeneration = bEndpointA
				? Packet.CaptureGenerationB : Packet.CaptureGenerationA;
			Pass.bLinkedCubeContractValid = LinkedCubeContract.IsValid();
		};

		AddEndpoint(EWPSide::SideA, Packet.PortalACenterWorld,
			static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(
				Packet.MetricA.PortalRadiusCm, Packet.MetricA.OuterRadiusCm)),
			Packet.StableSelectorNameA);
		AddEndpoint(EWPSide::SideB, Packet.PortalBCenterWorld,
			static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(
				Packet.MetricB.PortalRadiusCm, Packet.MetricB.OuterRadiusCm)),
			Packet.StableSelectorNameB);
		Pair.ExpectedEndpointCount = PairEndpoints.Num();
		if (Pair.bProduction && PairEndpoints.IsEmpty())
		{
			Pair.bNoVisibleEndpoint = true;
			Pair.bFullyOccluded = Pair.VisibleEndpointCount > 0
				&& Packet.bCaptureOcclusionValid
				&& (Packet.CaptureOcclusionVisibleEndpointMask & 0x3u) == 0;
			continue;
		}

		if (Pair.bProduction && !PairEndpoints.IsEmpty()
			&& (RawOwnershipForceFailureCallbacks == -1
				|| RemainingOwnershipForceFailureCallbacks > 0))
		{
			// Fail the pair once during preflight, before any endpoint from it enters
			// the global composite. Positive N therefore counts failed pairs, not pixels.
			PairEndpoints[0].Parameters.bForceResourceFailure = true;
		}

		bool bPairPreflightSucceeded = true;
		for (FEndpointWork& Endpoint : PairEndpoints)
		{
			bool bShaderAvailable = false;
			bool bBoundSceneDepthArray = false;
			EWPCompositeFailureReason FailureReason =
				EWPCompositeFailureReason::None;
			++OwnershipPreflightAttemptCount;
			// 로그 전용: preflight 실행 시간만 측정하며 validation 결과에는 관여하지 않는다.
			const double PreflightStartSeconds = FPlatformTime::Seconds();
			const bool bPreflightSucceeded = ValidateWPCompositePass(
				View,
				BaseSceneColor,
				BaseSceneColor,
				SceneDepthTexture,
				SceneDepthArraySlice,
				Endpoint.Parameters,
				bShaderAvailable,
				bBoundSceneDepthArray,
				FailureReason);
			const double PreflightCpuMs =
				(FPlatformTime::Seconds() - PreflightStartSeconds) * 1000.0;
			OwnershipAccumulatedPreflightCpuMs += PreflightCpuMs;
			OwnershipMaxPreflightCpuMs = FMath::Max(
				OwnershipMaxPreflightCpuMs, PreflightCpuMs);
			if (!bPreflightSucceeded)
			{
				++OwnershipPreflightFailureCount;
				if (FailureReason == EWPCompositeFailureReason::ForcedResourceFailure
					&& RawOwnershipForceFailureCallbacks > 0
					&& RemainingOwnershipForceFailureCallbacks > 0)
				{
					--RemainingOwnershipForceFailureCallbacks;
#if !UE_BUILD_SHIPPING
					WP_LOG(nullptr, Verbose,
						TEXT("[RenderThread][Ownership][MultiPair] Forced pair preflight failure consumed. World=%s PairId=%s Handle=%llu OwnershipEpoch=%llu PacketSequence=%llu Remaining=%d FailureReason=%s PreflightCpuMs=%.4f PairEnteredComposite=0 ProductionResourcesModified=0"),
						*RenderState->WorldName, *Packet.PairId.ToString(),
						Pair.HandleValue, Packet.OwnershipEpoch, Packet.PacketSequence,
						RemainingOwnershipForceFailureCallbacks,
						GetWPCompositeFailureReasonName(FailureReason),
						PreflightCpuMs);
#endif
				}
				FailPair(Pair, GetWPCompositeFailureReasonName(FailureReason),
					TEXT("EndpointPreflight"));
				bPairPreflightSucceeded = false;
				break;
			}
		}
		if (!bPairPreflightSucceeded)
		{
			continue;
		}
		for (FEndpointWork& Endpoint : PairEndpoints)
		{
			GlobalEndpoints.Add(MoveTemp(Endpoint));
		}
	}
	const bool bAnyPairFailedClosed = PairWorks.ContainsByPredicate(
		[](const FPairWork& Pair)
		{
			return Pair.bFailed || Pair.bLatchedSkip;
		});
	if (bAnyPairFailedClosed)
	{
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		return FinishCallback(true);
	}

	GlobalEndpoints.Sort([](const FEndpointWork& Left, const FEndpointWork& Right)
	{
		return IsWPProductionEndpointBefore(Left.OrderKey, Right.OrderKey);
	});
	FScreenPassTexture CompositeInput = BaseSceneColor;
	for (int32 EndpointIndex = 0; EndpointIndex < GlobalEndpoints.Num(); ++EndpointIndex)
	{
		FEndpointWork& Endpoint = GlobalEndpoints[EndpointIndex];
		FPairWork& Pair = PairWorks[Endpoint.PairIndex];
		if (Pair.bFailed || Pair.bLatchedSkip)
		{
			continue;
		}

		bool bShaderAvailable = false;
		bool bBoundSceneDepthArray = false;
		EWPCompositeFailureReason FailureReason =
			EWPCompositeFailureReason::None;
		++OwnershipRDGSetupAttemptCount;
		// 로그 전용: endpoint RDG setup 비용만 측정하며 pass 제출 결과에는 관여하지 않는다.
		const double SetupStartSeconds = FPlatformTime::Seconds();
		const FScreenPassRenderTarget EndpointOverride =
			ShouldUseWPOverrideOutput(EndpointIndex, GlobalEndpoints.Num())
				? Inputs.OverrideOutput
				: FScreenPassRenderTarget();
		FScreenPassTexture EndpointOutput = AddWPCompositePass(
			GraphBuilder,
			View,
			CompositeInput,
			BaseSceneColor,
			EndpointOverride,
			SceneDepthTexture,
			SceneDepthArraySlice,
			Endpoint.Parameters,
			Pair.HandleValue,
			bShaderAvailable,
			bBoundSceneDepthArray,
			FailureReason);
		const double SetupCpuMs =
			(FPlatformTime::Seconds() - SetupStartSeconds) * 1000.0;
		Pair.TotalSetupCpuMs += SetupCpuMs;
		Pair.MaxSetupAttemptCpuMs = FMath::Max(
			Pair.MaxSetupAttemptCpuMs, SetupCpuMs);
		OwnershipAccumulatedSetupCpuMs += SetupCpuMs;
		OwnershipMaxSetupCpuMs = FMath::Max(OwnershipMaxSetupCpuMs, SetupCpuMs);
		if (!EndpointOutput.IsValid())
		{
			++OwnershipUnexpectedSubmissionFailureCount;
			FailPair(Pair,
				GetWPCompositeFailureReasonName(FailureReason),
				TEXT("UnexpectedSubmissionAfterSuccessfulPreflight"));
			for (FPairWork& RollbackPair : PairWorks)
			{
				if (!RollbackPair.bFailed && !RollbackPair.bLatchedSkip)
				{
					FailPair(RollbackPair,
						TEXT("ViewAtomicRollbackAfterUnexpectedSubmissionFailure"),
						TEXT("UnexpectedSubmissionViewAtomicRollback"));
				}
			}
			WP_LOG(nullptr, Error,
				TEXT("[RenderThread][OwnershipFailure][MultiPair] Unexpected post-preflight failure discarded the complete View composite. World=%s FailedPairId=%s FailedHandle=%llu EndpointIndex=%d EndpointCount=%d FailureReason=%s OverrideOutputSelected=%d ActivePairCount=%d ViewAtomicRollback=1 PartialPairOutputCommitted=0 UntouchedSceneColor=1 PortalAbsentFailClosed=1 Recovery=FreshOwnershipEpochWarmup CallbackCpuMs=%.4f"),
				*RenderState->WorldName, *Pair.Packet->PairId.ToString(),
				Pair.HandleValue, EndpointIndex, GlobalEndpoints.Num(),
				GetWPCompositeFailureReasonName(FailureReason),
				EndpointOverride.IsValid() ? 1 : 0, PairWorks.Num(),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			return FinishCallback(true);
		}
		CompositeInput = MoveTemp(EndpointOutput);
		++Pair.SubmittedEndpointCount;
		if (Pair.bWarmup)
		{
			++OwnershipWarmupEndpointPassCount;
		}
		else
		{
			++OwnershipProductionEndpointPassCount;
		}
	}

	const uint32 FrameNumber = View.Family ? View.Family->FrameNumber : MAX_uint32;
	for (FPairWork& Pair : PairWorks)
	{
		if (!Pair.bFailed && !Pair.bLatchedSkip
			&& Pair.SubmittedEndpointCount != Pair.ExpectedEndpointCount)
		{
			FailPair(Pair, TEXT("PairEndpointSubmissionIncomplete"),
				TEXT("PostSubmissionInvariant"));
		}
	}
	const bool bPostSubmissionInvariantFailed = PairWorks.ContainsByPredicate(
		[](const FPairWork& Pair)
		{
			return Pair.bFailed || Pair.bLatchedSkip;
		});
	if (bPostSubmissionInvariantFailed)
	{
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		WP_LOG(nullptr, Error,
			TEXT("[RenderThread][ProductionFailure][MultiPair] Post-submission invariant failure discarded the complete View composite. World=%s WorldType=%s ActivePairCount=%d EndpointCount=%d ViewKey=%u ViewActorId=%u PlayerIndex=%d StereoViewIndex=%d UntouchedSceneColor=1 PortalAbsentFailClosed=1 ViewAtomicRollback=1 PartialPairOutputCommitted=0 CpuMs=%.4f"),
			*RenderState->WorldName, *RenderState->WorldType, PairWorks.Num(),
			GlobalEndpoints.Num(), View.GetViewKey(), View.ViewActor.ActorUniqueId,
			View.PlayerIndex, View.StereoViewIndex,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return FinishCallback(true);
	}
	for (FPairWork& Pair : PairWorks)
	{
		if (Pair.bFailed || Pair.bLatchedSkip)
		{
			continue;
		}

		if (Pair.bWarmup)
		{
			++OwnershipWarmupCallbackCount;
			FOwnershipWarmupTracker& Tracker =
				OwnershipWarmupByHandleRenderThread.FindOrAdd(Pair.HandleValue);
			if (Tracker.Epoch != Pair.Packet->OwnershipEpoch)
			{
				Tracker = FOwnershipWarmupTracker();
				Tracker.Epoch = Pair.Packet->OwnershipEpoch;
				Tracker.PacketSequence = Pair.Packet->PacketSequence;
			}
			else if (Tracker.PacketSequence != Pair.Packet->PacketSequence)
			{
#if !UE_BUILD_SHIPPING
				// 로그 전용: warmup progress 보존 메시지의 이전 sequence snapshot이다.
				const uint64 PreviousPacketSequence = Tracker.PacketSequence;
#endif
				const bool bPreserveProgress =
					ShouldPreserveWPWarmupProgressAcrossPacketUpdate(
						Tracker.Epoch, Pair.Packet->OwnershipEpoch,
						Tracker.bFailureLatched);
				if (bPreserveProgress)
				{
					Tracker.PacketSequence = Pair.Packet->PacketSequence;
					Tracker.bFailureLatched = false;
					if (Tracker.ConsecutiveSuccessCount > 0 || Tracker.bAcknowledged)
					{
#if !UE_BUILD_SHIPPING
						++OwnershipWarmupProgressPreservedCount;
						WP_LOG(nullptr, VeryVerbose,
							TEXT("[RenderThread][Ownership][MultiPair] Warmup progress preserved across fresh same-epoch packet. World=%s PairId=%s Handle=%llu OwnershipEpoch=%llu PreviousPacketSequence=%llu PacketSequence=%llu ConsecutiveSuccessCount=%u LastFrameNumber=%u Acknowledged=%d ActivePairCount=%d CpuMs=%.4f"),
							*RenderState->WorldName, *Pair.Packet->PairId.ToString(),
							Pair.HandleValue, Pair.Packet->OwnershipEpoch,
							PreviousPacketSequence, Pair.Packet->PacketSequence,
							Tracker.ConsecutiveSuccessCount, Tracker.LastFrameNumber,
							Tracker.bAcknowledged ? 1 : 0, PairWorks.Num(),
							(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
					}
				}
				else
				{
					Tracker = FOwnershipWarmupTracker();
					Tracker.Epoch = Pair.Packet->OwnershipEpoch;
					Tracker.PacketSequence = Pair.Packet->PacketSequence;
				}
			}
			if (Tracker.LastFrameNumber != FrameNumber)
			{
				Tracker.ConsecutiveSuccessCount =
					AdvanceWPWarmupConsecutiveFrameCount(
						Tracker.LastFrameNumber,
						FrameNumber,
						Tracker.ConsecutiveSuccessCount);
				Tracker.LastFrameNumber = FrameNumber;
			}
			const bool bAcknowledge = Tracker.ConsecutiveSuccessCount >= 2
				&& !Tracker.bAcknowledged;
			Pair.Packet->RecordWarmupPass_RenderThread(bAcknowledge);
			if (bAcknowledge)
			{
				Tracker.bAcknowledged = true;
				++OwnershipWarmupAckCount;
			}
			RecordOwnershipResult(Pair.HandleValue, Pair.Packet,
				bAcknowledge ? TEXT("WarmupAcknowledged") : TEXT("WarmupValidated"),
				Pair.VisibleEndpointCount, true, Pair.TotalSetupCpuMs);
		}
		else
		{
			++OwnershipProductionViewCount;
			if (Pair.bNoVisibleEndpoint)
			{
				++OwnershipInvisibleViewCount;
			}
			RecordOwnershipResult(Pair.HandleValue, Pair.Packet,
				Pair.bNoVisibleEndpoint
					? (Pair.bFullyOccluded
						? TEXT("ProductionFullyOccluded")
						: TEXT("ProductionNoVisibleEndpoint"))
					: TEXT("ProductionSubmitted"),
				Pair.VisibleEndpointCount, true, Pair.TotalSetupCpuMs);
		}
	}

	OutOutput = GlobalEndpoints.IsEmpty()
		? Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder)
		: CompositeInput;
	return FinishCallback(true);
}

bool FWPSceneViewExtension::TryPostProcessOwnershipPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs,
	FScreenPassTexture& OutOutput)
{
	// 로그 전용: StartSeconds/CallbackCpuMs는 callback 경과 시간과 ownership summary만 측정한다.
	const double StartSeconds = FPlatformTime::Seconds();
	const uint64 ViewStorageKey = MakeViewKey(View);
	TArray<TPair<uint64, const FWPRenderThreadPacket*>, TInlineAllocator<2>> ActivePairs;
	for (const TPair<uint64, FWPRenderThreadPacket>& Pair : RenderState->PairsRenderThread)
	{
		if (IsWPOwnershipPacketReadyForRendering(Pair.Value))
		{
			ActivePairs.Emplace(Pair.Key, &Pair.Value);
		}
	}
	if (ActivePairs.IsEmpty())
	{
		const int32 ActiveWarmupOrProductionPairs =
			RenderState->ActiveOwnershipPairCount.Load();
		if (ActiveWarmupOrProductionPairs > 0)
		{
			++OwnershipCallbackInvocationCount;
			++OwnershipMissingReadyPacketViewCount;
			OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			const double CallbackCpuMs =
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
			OwnershipMaxCallbackCpuMs = FMath::Max(
				OwnershipMaxCallbackCpuMs, CallbackCpuMs);
			MaybeLogOwnershipSummary_RenderThread();
			return true;
		}
		MaybeLogOwnershipSummary_RenderThread();
		return false;
	}
	if (ActivePairs.Num() > 1)
	{
		return TryPostProcessMultiPairOwnershipPass_RenderThread(
			GraphBuilder, View, Inputs, OutOutput);
	}
	++OwnershipCallbackInvocationCount;
	++OwnershipPairAttemptCount;

	// 로그 전용: result fingerprint/cache와 반환값은 동일 결과 로그를 억제할 뿐
	// single-pair ownership pass의 성공/실패 및 출력에는 관여하지 않는다.
	const auto RecordOwnershipResult = [this, &View, ViewStorageKey, StartSeconds](
		const uint64 HandleValue,
		const FWPRenderThreadPacket* Packet,
		const TCHAR* Reason,
		const int32 VisibleEndpoints,
		const bool bPassSucceeded,
		const double SetupCpuMs) -> bool
	{
		uint64 Fingerprint = 1469598103934665603ull;
		Fingerprint = MixWPHash(Fingerprint, HandleValue);
		Fingerprint = MixWPHash(Fingerprint, Packet ? Packet->OwnershipEpoch : 0);
		if (!bPassSucceeded)
		{
			Fingerprint = MixWPHash(
				Fingerprint, Packet ? Packet->PacketSequence : 0);
		}
		Fingerprint = MixWPHash(
			Fingerprint, Packet ? static_cast<uint64>(Packet->EffectiveOwnership) : 0);
		Fingerprint = MixWPHash(Fingerprint, HashWPReason(Reason));
		Fingerprint = MixWPHash(Fingerprint, static_cast<uint64>(VisibleEndpoints));
		Fingerprint = MixWPHash(Fingerprint, bPassSucceeded ? 1u : 0u);
		const uint64 StorageKey = MakeWPPairViewStorageKey(
			ViewStorageKey, HandleValue);
		const uint64* Previous = LastOwnershipResultFingerprintByViewRenderThread.Find(
			StorageKey);
		if (Previous && *Previous == Fingerprint)
		{
			return false;
		}
		if (LastOwnershipResultFingerprintByViewRenderThread.Num() >= 1024
			&& !LastOwnershipResultFingerprintByViewRenderThread.Contains(StorageKey))
		{
			LastOwnershipResultFingerprintByViewRenderThread.Reset();
		}
		LastOwnershipResultFingerprintByViewRenderThread.Add(StorageKey, Fingerprint);

#if !UE_BUILD_SHIPPING
		// 로그 전용: 이미 결정된 ownership 결과 메시지의 view-kind snapshot이다.
		const FWPOwnershipViewPolicyResult ViewPolicy =
			EvaluateWPOwnershipViewPolicy(MakeWPOwnershipViewPolicyInput(
				View, RenderState->bPIEWorld,
				CVarWPSimulateViewEnabled.GetValueOnRenderThread() != 0));
		WP_LOG(nullptr, VeryVerbose,
			TEXT("[RenderThread][Production] View result changed. World=%s PairId=%s Handle=%llu SelectorA=%s SelectorB=%s RequestedMode=%s EffectiveMode=%s OwnershipEpoch=%llu PacketSequence=%llu ViewStorageKey=%llu ViewKey=%u ViewActorId=%u PlayerIndex=%d OwnershipViewKind=%s PIEWorld=%d HasViewElementDrawer=%d VirtualTexture=%d OfflineRender=%d CustomRenderPass=%d StereoViewIndex=%d StereoPass=%d VisibleEndpoints=%d OcclusionValid=%d OcclusionVisibleMask=0x%02x PassSucceeded=%d Reason=%s QueueLatencyMs=%.4f SetupCpuMs=%.4f CallbackCpuMs=%.4f ProductionGpuStat=WP.ProductionComposite FailClosed=PortalAbsentOnUntouchedSceneColor"),
			*RenderState->WorldName,
			Packet && Packet->PairId.IsValid() ? *Packet->PairId.ToString() : TEXT("None"),
			HandleValue,
			Packet ? *Packet->StableSelectorNameA.ToString() : TEXT("None"),
			Packet ? *Packet->StableSelectorNameB.ToString() : TEXT("None"),
			Packet ? GetWPPairOwnershipModeName(Packet->RequestedOwnership) : TEXT("Disabled"),
			Packet ? GetWPPairOwnershipModeName(Packet->EffectiveOwnership) : TEXT("Disabled"),
			Packet ? Packet->OwnershipEpoch : 0,
			Packet ? Packet->PacketSequence : 0,
			ViewStorageKey, View.GetViewKey(), View.ViewActor.ActorUniqueId,
			View.PlayerIndex, GetWPOwnershipViewKindName(ViewPolicy.Kind),
			RenderState->bPIEWorld ? 1 : 0, View.Drawer ? 1 : 0,
			View.bIsVirtualTexture ? 1 : 0, View.bIsOfflineRender ? 1 : 0,
			View.CustomRenderPass ? 1 : 0,
			View.StereoViewIndex, static_cast<int32>(View.StereoPass),
			VisibleEndpoints,
			Packet && Packet->bCaptureOcclusionValid ? 1 : 0,
			Packet
				? static_cast<uint32>(Packet->CaptureOcclusionVisibleEndpointMask)
				: 0x3u,
			bPassSucceeded ? 1 : 0, Reason,
			Packet ? Packet->QueueLatencyMs : 0.0, SetupCpuMs,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return true;
	};

	check(ActivePairs.Num() == 1);

	const uint64 HandleValue = ActivePairs[0].Key;
	const FWPRenderThreadPacket& Packet = *ActivePairs[0].Value;
	const bool bWarmup = Packet.EffectiveOwnership == EWPPairOwnershipMode::Warmup;
	const bool bProduction = Packet.EffectiveOwnership == EWPPairOwnershipMode::Production;
	const int32 RawOwnershipForceFailureCallbacks = FMath::Max(
		GetWPOwnershipForceProductionFailureCallbacks_RenderThread(), -1);
	if (RawOwnershipForceFailureCallbacks != LastObservedOwnershipForceFailureCallbacks)
	{
#if !UE_BUILD_SHIPPING
		// 로그 전용: 설정 변경 메시지의 PreviousConfigured 필드용 이전 값 snapshot이다.
		const int32 PreviousConfigured = LastObservedOwnershipForceFailureCallbacks;
#endif
		LastObservedOwnershipForceFailureCallbacks = RawOwnershipForceFailureCallbacks;
		RemainingOwnershipForceFailureCallbacks = RawOwnershipForceFailureCallbacks > 0
			? RawOwnershipForceFailureCallbacks
			: 0;
#if !UE_BUILD_SHIPPING
		WP_LOG(nullptr, Verbose,
			TEXT("[RenderThread][Ownership] Production failure hook changed. World=%s PreviousConfigured=%d Configured=%d Remaining=%d PersistentFailure=%d ProductionResourcesModified=0 CpuMs=%.4f"),
			*RenderState->WorldName, PreviousConfigured,
			RawOwnershipForceFailureCallbacks,
			RemainingOwnershipForceFailureCallbacks,
			RawOwnershipForceFailureCallbacks == -1 ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}

	// 로그 전용: ownership view rejection 메시지/fingerprint에 넣을 reason 문자열이다.
	const TCHAR* OwnershipViewSkipReason = TEXT("None");
	if (!ShouldAcceptOwnershipView_RenderThread(View, OwnershipViewSkipReason))
	{
		++OwnershipUnsupportedViewCount;
		RecordOwnershipResult(HandleValue, &Packet, OwnershipViewSkipReason,
			0, false, 0.0);
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		const double CallbackCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
		OwnershipMaxCallbackCpuMs = FMath::Max(OwnershipMaxCallbackCpuMs, CallbackCpuMs);
		MaybeLogOwnershipSummary_RenderThread();
		return true;
	}

	// 로그 전용: TotalSetupCpuMs/MaxSetupAttemptCpuMs 인자는 실패 로그 및 summary에만
	// 전달되며 failure 처리와 fail-closed 출력 자체에는 관여하지 않는다.
	const auto FailOwnershipPass = [this, &GraphBuilder, &Inputs, &Packet,
		&OutOutput, &RecordOwnershipResult, HandleValue, bWarmup, bProduction,
		StartSeconds](
			const TCHAR* Reason,
			const double TotalSetupCpuMs,
			const double MaxSetupAttemptCpuMs,
			const int32 FailedVisibleEndpointCount)
	{
		Packet.ReportProductionFailure_RenderThread();
		if (bWarmup)
		{
			FOwnershipWarmupTracker& FailureTracker =
				OwnershipWarmupByHandleRenderThread.FindOrAdd(HandleValue);
			FailureTracker = FOwnershipWarmupTracker();
			FailureTracker.Epoch = Packet.OwnershipEpoch;
			FailureTracker.PacketSequence = Packet.PacketSequence;
			FailureTracker.bFailureLatched = true;
			++OwnershipWarmupFailureCount;
		}
		else
		{
			++OwnershipProductionFailureCount;
		}
		OwnershipAccumulatedSetupCpuMs += TotalSetupCpuMs;
		OwnershipMaxSetupCpuMs = FMath::Max(
			OwnershipMaxSetupCpuMs, MaxSetupAttemptCpuMs);
		const bool bNewFailureFingerprint = RecordOwnershipResult(
			HandleValue, &Packet, Reason, FailedVisibleEndpointCount,
			false, TotalSetupCpuMs);
		if (bNewFailureFingerprint)
		{
			WP_LOG(nullptr, Warning,
				TEXT("[RenderThread][ProductionFailure] Pass failed; portal omitted on untouched SceneColor. World=%s WorldType=%s PairId=%s Handle=%llu RequestedMode=%s EffectiveMode=%s OwnershipEpoch=%llu PacketSequence=%llu Reason=%s Warmup=%d Production=%d Recovery=%s TotalSetupCpuMs=%.4f MaxSetupAttemptCpuMs=%.4f CallbackCpuMs=%.4f UntouchedSceneColor=1 PortalAbsentFailClosed=1 ProductionResourcesModified=0"),
				*RenderState->WorldName, *RenderState->WorldType, *Packet.PairId.ToString(),
				HandleValue, GetWPPairOwnershipModeName(Packet.RequestedOwnership),
				GetWPPairOwnershipModeName(Packet.EffectiveOwnership),
				Packet.OwnershipEpoch, Packet.PacketSequence, Reason,
				bWarmup ? 1 : 0, bProduction ? 1 : 0,
				bWarmup ? TEXT("FreshPacketRetry") : TEXT("NextOwnershipEpochWarmup"),
				TotalSetupCpuMs, MaxSetupAttemptCpuMs,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		}
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		const double CallbackCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
		OwnershipMaxCallbackCpuMs = FMath::Max(OwnershipMaxCallbackCpuMs, CallbackCpuMs);
		MaybeLogOwnershipSummary_RenderThread();
	};

	if (bWarmup)
	{
		const FOwnershipWarmupTracker* FailureTracker =
			OwnershipWarmupByHandleRenderThread.Find(HandleValue);
		if (FailureTracker
			&& FailureTracker->Epoch == Packet.OwnershipEpoch
			&& FailureTracker->PacketSequence == Packet.PacketSequence
			&& FailureTracker->bFailureLatched)
		{
			++OwnershipWarmupLatchedSkipCount;
			OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			RecordOwnershipResult(
				HandleValue, &Packet, TEXT("WarmupFailureLatchedUntilFreshPacket"),
				0, false, 0.0);
			const double CallbackCpuMs =
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
			OwnershipMaxCallbackCpuMs = FMath::Max(
				OwnershipMaxCallbackCpuMs, CallbackCpuMs);
			MaybeLogOwnershipSummary_RenderThread();
			return true;
		}
	}

	if (!Packet.IsOwnershipContractReady())
	{
		FailOwnershipPass(TEXT("OwnershipPacketContractInvalid"), 0.0, 0.0, 0);
		return true;
	}
	// 로그 전용: ownership eligibility 평가 비용을 전환/요약 로그에 기록한다.
	const double EligibilityStartSeconds = FPlatformTime::Seconds();
	const EWPEligibilityReason Eligibility = EvaluateEligibility_RenderThread(Packet);
	const double EligibilityCpuMs =
		(FPlatformTime::Seconds() - EligibilityStartSeconds) * 1000.0;
	++OwnershipEligibilityEvaluationCount;
	OwnershipAccumulatedEligibilityCpuMs += EligibilityCpuMs;
	OwnershipMaxEligibilityCpuMs = FMath::Max(
		OwnershipMaxEligibilityCpuMs, EligibilityCpuMs);
	RecordEligibilityTransition_RenderThread(
		HandleValue, Packet, Eligibility, EligibilityCpuMs);
	if (Eligibility != EWPEligibilityReason::Eligible)
	{
		FailOwnershipPass(
			GetWPEligibilityReasonName(Eligibility), 0.0, 0.0, 0);
		return true;
	}

	FVector3f PortalCenterTranslatedA = FVector3f::ZeroVector;
	FVector3f PortalCenterTranslatedB = FVector3f::ZeroVector;
	if (!BuildWPProductionViewCenters(
		Packet,
		FVector3d(View.ViewMatrices.GetPreViewTranslation()),
		PortalCenterTranslatedA,
		PortalCenterTranslatedB))
	{
		FailOwnershipPass(TEXT("PerViewStateBuildFailed"), 0.0, 0.0, 0);
		return true;
	}

	struct FOwnedEndpoint
	{
		EWPSide Side = EWPSide::None;
		double DistanceSquared = 0.0;
		double NearSurfaceDistanceCm = 0.0;
		FName StableSelector = NAME_None;
		bool bVisible = false;
		bool bCompositeVisible = false;
	};
	TArray<FOwnedEndpoint, TInlineAllocator<2>> Endpoints;
	const FVector3d ViewOrigin(View.ViewMatrices.GetViewOrigin());
	if (!IsFiniteSceneViewVector(ViewOrigin))
	{
		FailOwnershipPass(TEXT("InvalidViewOrigin"), 0.0, 0.0, 0);
		return true;
	}
	const auto AddOwnedEndpoint = [&View, &ViewOrigin, &Endpoints, &Packet](
		const EWPSide Side,
		const FVector3d& Center,
		const float ProxyRadiusCm,
		const FName StableSelector)
	{
		FOwnedEndpoint& Endpoint = Endpoints.AddDefaulted_GetRef();
		Endpoint.Side = Side;
		Endpoint.DistanceSquared = FVector3d::DistSquared(ViewOrigin, Center);
		Endpoint.NearSurfaceDistanceCm = FMath::Max(
			FMath::Sqrt(Endpoint.DistanceSquared) - static_cast<double>(ProxyRadiusCm),
			0.0);
		Endpoint.StableSelector = StableSelector;
		Endpoint.bVisible = Endpoint.DistanceSquared
			<= FMath::Square(static_cast<double>(ProxyRadiusCm))
			|| View.GetCullingFrustum().IntersectSphere(FVector(Center), ProxyRadiusCm);
		Endpoint.bCompositeVisible = Endpoint.bVisible
			&& IsWPProductionEndpointAllowedByOcclusion(Packet, Side);
	};
	AddOwnedEndpoint(
		EWPSide::SideA,
		Packet.PortalACenterWorld,
		static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(
			Packet.MetricA.PortalRadiusCm, Packet.MetricA.OuterRadiusCm)),
		Packet.StableSelectorNameA);
	AddOwnedEndpoint(
		EWPSide::SideB,
		Packet.PortalBCenterWorld,
		static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(
			Packet.MetricB.PortalRadiusCm, Packet.MetricB.OuterRadiusCm)),
		Packet.StableSelectorNameB);
	Endpoints.Sort([](const FOwnedEndpoint& Left, const FOwnedEndpoint& Right)
	{
		// Far-to-near makes the nearest endpoint authoritative where projected
		// analytic coverage overlaps. Stable selector and side break exact ties.
		if (Left.NearSurfaceDistanceCm != Right.NearSurfaceDistanceCm)
		{
			return Left.NearSurfaceDistanceCm > Right.NearSurfaceDistanceCm;
		}
		if (Left.StableSelector != Right.StableSelector)
		{
			return Left.StableSelector.LexicalLess(Right.StableSelector);
		}
		return static_cast<uint8>(Left.Side) < static_cast<uint8>(Right.Side);
	});

	TArray<FOwnedEndpoint, TInlineAllocator<2>> PassEndpoints;
	int32 VisibleEndpointCount = 0;
	for (const FOwnedEndpoint& Endpoint : Endpoints)
	{
		VisibleEndpointCount += Endpoint.bVisible ? 1 : 0;
		if (bWarmup || Endpoint.bCompositeVisible)
		{
			PassEndpoints.Add(Endpoint);
		}
	}
	if (bProduction && PassEndpoints.IsEmpty())
	{
		const bool bFullyOccluded = VisibleEndpointCount > 0
			&& Packet.bCaptureOcclusionValid
			&& (Packet.CaptureOcclusionVisibleEndpointMask & 0x3u) == 0;
		++OwnershipInvisibleViewCount;
		++OwnershipProductionViewCount;
		OutOutput = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		RecordOwnershipResult(HandleValue, &Packet,
			bFullyOccluded
				? TEXT("ProductionFullyOccluded")
				: TEXT("ProductionNoVisibleEndpoint"),
			VisibleEndpointCount, true, 0.0);
		const double CallbackCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
		OwnershipMaxCallbackCpuMs = FMath::Max(OwnershipMaxCallbackCpuMs, CallbackCpuMs);
		MaybeLogOwnershipSummary_RenderThread();
		return true;
	}

	const FScreenPassTextureSlice SceneColorSlice =
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	const FScreenPassTexture BaseSceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder, SceneColorSlice);
	if (!BaseSceneColor.IsValid())
	{
		FailOwnershipPass(
			TEXT("InvalidBaseSceneColor"), 0.0, 0.0, VisibleEndpointCount);
		return true;
	}
	FRDGTextureRef SceneDepthTexture = Inputs.SceneTextures.SceneTextures
		? Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture
		: nullptr;
	const FRDGTextureRef SceneColorSourceTexture = SceneColorSlice.TextureSRV
		? SceneColorSlice.TextureSRV->Desc.Texture
		: nullptr;
	const bool bSceneColorUsesArray = SceneColorSourceTexture
		&& SceneColorSourceTexture->Desc.Dimension == ETextureDimension::Texture2DArray;
	const int32 SceneColorArraySlice = bSceneColorUsesArray
		? static_cast<int32>(SceneColorSlice.TextureSRV->Desc.FirstArraySlice)
		: INDEX_NONE;
	const bool bSceneDepthArray = SceneDepthTexture
		&& SceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2DArray;
	const int32 SceneDepthArraySlice = bSceneDepthArray
		? WPPortalMaskMath::ResolveSceneDepthArraySlice(
			bSceneColorUsesArray,
			SceneColorArraySlice,
			View.StereoViewIndex,
			static_cast<int32>(SceneDepthTexture->Desc.ArraySize))
		: 0;

	FScreenPassTexture CompositeInput = BaseSceneColor;
	// 로그 전용: endpoint setup 비용의 callback 내 누적값이다.
	double TotalSetupCpuMs = 0.0;
	for (int32 EndpointIndex = 0; EndpointIndex < PassEndpoints.Num(); ++EndpointIndex)
	{
		const FOwnedEndpoint& Endpoint = PassEndpoints[EndpointIndex];
		const bool bEndpointA = Endpoint.Side == EWPSide::SideA;
		const FWPMetricSettings& Metric = bEndpointA ? Packet.MetricA : Packet.MetricB;
		const FWPPortalVisualSettings& Visual = bEndpointA ? Packet.VisualA : Packet.VisualB;
		const float VisualScale = Visual.IsFiniteAndValid() ? Visual.UniformScale : 1.0f;
		const FWPRayLUTContract& LUTContract = bEndpointA
			? Packet.RayLUTContractA : Packet.RayLUTContractB;
		const float RayLUTZ = bEndpointA ? Packet.RayLUTZA : Packet.RayLUTZB;
		const uint32 RayLUTRevision = bEndpointA
			? Packet.RayLUTRevisionA : Packet.RayLUTRevisionB;
		const FTextureReferenceRHIRef& LUTReference = bEndpointA
			? Packet.RayLUTA : Packet.RayLUTB;
		const FWPCubeContract& LocalCubeContract = bEndpointA
			? Packet.CubeContractA : Packet.CubeContractB;
		const FWPCubeContract& LinkedCubeContract = bEndpointA
			? Packet.CubeContractB : Packet.CubeContractA;
		const FTextureReferenceRHIRef& LocalCubeReference = bEndpointA
			? Packet.CubeA : Packet.CubeB;
		const FTextureReferenceRHIRef& LinkedCubeReference = bEndpointA
			? Packet.CubeB : Packet.CubeA;
		const FMatrix44d& SelfToWorld = bEndpointA
			? Packet.PortalAToWorld : Packet.PortalBToWorld;
		const FMatrix44d& LinkedToWorld = bEndpointA
			? Packet.PortalBToWorld : Packet.PortalAToWorld;

		FWPCompositePassParameters Parameters;
		Parameters.PortalCenterTranslated = bEndpointA
			? PortalCenterTranslatedA : PortalCenterTranslatedB;
		Parameters.PortalRadiusCm = Metric.PortalRadiusCm * VisualScale;
		Parameters.ThroatLengthCm = Metric.ThroatHalfLengthCm * 2.0f * VisualScale;
		Parameters.ProxyRadiusCm = static_cast<float>(
			WPPortalVisibilityMath::GetSafeProxyRadiusCm(
				Metric.PortalRadiusCm, Metric.OuterRadiusCm)) * VisualScale;
		Parameters.MetricOuterRadiusCm = Metric.MetricOuterRadiusCm * VisualScale;
		Parameters.DepthBiasCm = 1.0f;
		Parameters.SelfX = GetWPUnitAxis(SelfToWorld, EAxis::X);
		Parameters.SelfY = GetWPUnitAxis(SelfToWorld, EAxis::Y);
		Parameters.SelfZ = GetWPUnitAxis(SelfToWorld, EAxis::Z);
		Parameters.LinkedX = GetWPUnitAxis(LinkedToWorld, EAxis::X);
		Parameters.LinkedY = GetWPUnitAxis(LinkedToWorld, EAxis::Y);
		Parameters.LinkedZ = GetWPUnitAxis(LinkedToWorld, EAxis::Z);
		Parameters.RayLUTTexture = ResolveTextureReference(LUTReference);
		Parameters.bAnalyticNoTransition = bEndpointA
			? Packet.bAnalyticNoTransitionA : Packet.bAnalyticNoTransitionB;
		PopulateWPRayLUTContractParameters(
			Parameters, LUTContract, RayLUTZ, RayLUTRevision);
		Parameters.LocalCubeTextureReference =
			GetTextureReferenceProxy(LocalCubeReference);
		Parameters.ExpectedLocalCubeExtent = LocalCubeContract.ExpectedExtent;
		Parameters.ExpectedLocalCubeFormat = GetWPCubeRHIPixelFormat(
			LocalCubeContract.ExpectedFormat);
		Parameters.ExpectedLocalCubeMipCount = LocalCubeContract.ExpectedMipCount;
		Parameters.LocalCubeLayoutVersion = LocalCubeContract.CubeLayoutVersion;
		Parameters.LocalCubeResourceGeneration = LocalCubeContract.ResourceGeneration;
		Parameters.LocalCubeCaptureGeneration = bEndpointA
			? Packet.CaptureGenerationA : Packet.CaptureGenerationB;
		Parameters.bLocalCubeContractValid = LocalCubeContract.IsValid();
		Parameters.LinkedCubeTextureReference =
			GetTextureReferenceProxy(LinkedCubeReference);
		Parameters.ExpectedLinkedCubeExtent = LinkedCubeContract.ExpectedExtent;
		Parameters.ExpectedLinkedCubeFormat = GetWPCubeRHIPixelFormat(
			LinkedCubeContract.ExpectedFormat);
		Parameters.ExpectedLinkedCubeMipCount = LinkedCubeContract.ExpectedMipCount;
		Parameters.LinkedCubeLayoutVersion = LinkedCubeContract.CubeLayoutVersion;
		Parameters.LinkedCubeResourceGeneration = LinkedCubeContract.ResourceGeneration;
		Parameters.LinkedCubeCaptureGeneration = bEndpointA
			? Packet.CaptureGenerationB : Packet.CaptureGenerationA;
		Parameters.bLinkedCubeContractValid = LinkedCubeContract.IsValid();
		Parameters.bForceResourceFailure = bProduction
			&& (RawOwnershipForceFailureCallbacks == -1
				|| RemainingOwnershipForceFailureCallbacks > 0);

		const bool bLastEndpoint = EndpointIndex == PassEndpoints.Num() - 1;
		const FScreenPassRenderTarget EndpointOverride = bLastEndpoint
			? Inputs.OverrideOutput
			: FScreenPassRenderTarget();
		bool bShaderAvailable = false;
		bool bBoundSceneDepthArray = false;
		EWPCompositeFailureReason FailureReason =
			EWPCompositeFailureReason::None;
		++OwnershipRDGSetupAttemptCount;
		// 로그 전용: endpoint RDG setup 비용만 측정하며 pass 제출 결과에는 관여하지 않는다.
		const double SetupStartSeconds = FPlatformTime::Seconds();
		FScreenPassTexture EndpointOutput = AddWPCompositePass(
			GraphBuilder,
			View,
			CompositeInput,
			BaseSceneColor,
			EndpointOverride,
			SceneDepthTexture,
			SceneDepthArraySlice,
			Parameters,
			HandleValue,
			bShaderAvailable,
			bBoundSceneDepthArray,
			FailureReason);
		const double SetupCpuMs =
			(FPlatformTime::Seconds() - SetupStartSeconds) * 1000.0;
		TotalSetupCpuMs += SetupCpuMs;
		OwnershipMaxSetupCpuMs = FMath::Max(OwnershipMaxSetupCpuMs, SetupCpuMs);
		if (FailureReason == EWPCompositeFailureReason::ForcedResourceFailure
			&& RawOwnershipForceFailureCallbacks > 0
			&& RemainingOwnershipForceFailureCallbacks > 0)
		{
			--RemainingOwnershipForceFailureCallbacks;
#if !UE_BUILD_SHIPPING
			WP_LOG(nullptr, Verbose,
				TEXT("[RenderThread][Ownership] Forced production failure consumed. World=%s PairId=%s Handle=%llu OwnershipEpoch=%llu PacketSequence=%llu Remaining=%d FailureReason=%s SetupCpuMs=%.4f ProductionResourcesModified=0"),
				*RenderState->WorldName, *Packet.PairId.ToString(), HandleValue,
				Packet.OwnershipEpoch, Packet.PacketSequence,
				RemainingOwnershipForceFailureCallbacks,
				GetWPCompositeFailureReasonName(FailureReason), SetupCpuMs);
#endif
		}
		if (!EndpointOutput.IsValid())
		{
			FailOwnershipPass(
				GetWPCompositeFailureReasonName(FailureReason),
				TotalSetupCpuMs,
				SetupCpuMs,
				VisibleEndpointCount);
			return true;
		}
		CompositeInput = MoveTemp(EndpointOutput);
		if (bWarmup)
		{
			++OwnershipWarmupEndpointPassCount;
		}
		else
		{
			++OwnershipProductionEndpointPassCount;
		}
	}

	OwnershipAccumulatedSetupCpuMs += TotalSetupCpuMs;
	if (bWarmup)
	{
		++OwnershipWarmupCallbackCount;
		FOwnershipWarmupTracker& Tracker = OwnershipWarmupByHandleRenderThread.FindOrAdd(
			HandleValue);
		const uint32 FrameNumber = View.Family ? View.Family->FrameNumber : MAX_uint32;
		if (Tracker.Epoch != Packet.OwnershipEpoch)
		{
			Tracker = FOwnershipWarmupTracker();
			Tracker.Epoch = Packet.OwnershipEpoch;
			Tracker.PacketSequence = Packet.PacketSequence;
		}
		else if (Tracker.PacketSequence != Packet.PacketSequence)
		{
#if !UE_BUILD_SHIPPING
			// 로그 전용: warmup progress 보존 메시지의 이전 sequence snapshot이다.
			const uint64 PreviousPacketSequence = Tracker.PacketSequence;
#endif
			const bool bPreserveProgress =
				ShouldPreserveWPWarmupProgressAcrossPacketUpdate(
					Tracker.Epoch, Packet.OwnershipEpoch, Tracker.bFailureLatched);
			if (bPreserveProgress)
			{
				Tracker.PacketSequence = Packet.PacketSequence;
				Tracker.bFailureLatched = false;
				if (Tracker.ConsecutiveSuccessCount > 0 || Tracker.bAcknowledged)
				{
#if !UE_BUILD_SHIPPING
					++OwnershipWarmupProgressPreservedCount;
					WP_LOG(nullptr, VeryVerbose,
						TEXT("[RenderThread][Ownership] Warmup progress preserved across fresh same-epoch packet. World=%s PairId=%s Handle=%llu OwnershipEpoch=%llu PreviousPacketSequence=%llu PacketSequence=%llu ConsecutiveSuccessCount=%u LastFrameNumber=%u Acknowledged=%d FailureLatched=0 PreserveReason=TransportHeartbeatOrContentUpdate CpuMs=%.4f"),
						*RenderState->WorldName, *Packet.PairId.ToString(), HandleValue,
						Packet.OwnershipEpoch, PreviousPacketSequence, Packet.PacketSequence,
						Tracker.ConsecutiveSuccessCount, Tracker.LastFrameNumber,
						Tracker.bAcknowledged ? 1 : 0,
						(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
				}
			}
			else
			{
				Tracker = FOwnershipWarmupTracker();
				Tracker.Epoch = Packet.OwnershipEpoch;
				Tracker.PacketSequence = Packet.PacketSequence;
			}
		}
		if (Tracker.LastFrameNumber != FrameNumber)
		{
			Tracker.ConsecutiveSuccessCount =
				AdvanceWPWarmupConsecutiveFrameCount(
					Tracker.LastFrameNumber,
					FrameNumber,
					Tracker.ConsecutiveSuccessCount);
			Tracker.LastFrameNumber = FrameNumber;
		}
		const bool bAcknowledge = Tracker.ConsecutiveSuccessCount >= 2
			&& !Tracker.bAcknowledged;
		Packet.RecordWarmupPass_RenderThread(bAcknowledge);
		if (bAcknowledge)
		{
			Tracker.bAcknowledged = true;
			++OwnershipWarmupAckCount;
		}
		RecordOwnershipResult(
			HandleValue,
			&Packet,
			bAcknowledge ? TEXT("WarmupAcknowledged") : TEXT("WarmupValidated"),
			VisibleEndpointCount,
			true,
			TotalSetupCpuMs);
	}
	else
	{
		++OwnershipProductionViewCount;
		RecordOwnershipResult(
			HandleValue, &Packet, TEXT("ProductionSubmitted"),
			VisibleEndpointCount, true, TotalSetupCpuMs);
	}

	OutOutput = CompositeInput;
	const double CallbackCpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	OwnershipAccumulatedCallbackCpuMs += CallbackCpuMs;
	OwnershipMaxCallbackCpuMs = FMath::Max(OwnershipMaxCallbackCpuMs, CallbackCpuMs);
	MaybeLogOwnershipSummary_RenderThread();
	return true;
}

FScreenPassTexture FWPSceneViewExtension::PostProcessPassAfterMotionBlur_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_PostProcessCallback);
	FScreenPassTexture OwnershipOutput;
	if (TryPostProcessOwnershipPass_RenderThread(
		GraphBuilder, View, Inputs, OwnershipOutput))
	{
		return OwnershipOutput.IsValid()
			? OwnershipOutput
			: Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

void FWPSceneViewExtension::MaybeLogVisibilityObservationSummary_RenderThread()
{
#if !UE_BUILD_SHIPPING
	const double NowSeconds = FPlatformTime::Seconds();
	if (VisibilityObservationSummaryStartSeconds <= 0.0)
	{
		VisibilityObservationSummaryStartSeconds = NowSeconds;
		return;
	}
	const double IntervalSeconds = FMath::Max(
		static_cast<double>(CVarWPViewSummaryInterval.GetValueOnRenderThread()), 1.0);
	if (NowSeconds - VisibilityObservationSummaryStartSeconds < IntervalSeconds)
	{
		return;
	}

	const double AverageViewCpuMs = VisibilityObservationViewInvocationCount > 0
		? VisibilityObservationAccumulatedCpuMs
			/ static_cast<double>(VisibilityObservationViewInvocationCount)
		: 0.0;
	const int32 FingerprintCountBefore =
		LastVisibilityObservationFingerprintByHandleRenderThread.Num();
	for (auto Iterator =
		LastVisibilityObservationFingerprintByHandleRenderThread.CreateIterator();
		Iterator;
		++Iterator)
	{
		if (!RenderState->PairsRenderThread.Contains(Iterator.Key()))
		{
			Iterator.RemoveCurrent();
		}
	}
	const int32 FingerprintsPruned = FingerprintCountBefore
		- LastVisibilityObservationFingerprintByHandleRenderThread.Num();
	WP_LOG(nullptr, Verbose,
		TEXT("[RenderThread][VisibilityObservationSummary] World=%s WorldType=%s IntervalSeconds=%.3f ActiveVisibilityObservationPairs=%d ActiveOwnershipPairs=%d RenderThreadPackets=%d ViewInvocations=%llu PairEvaluations=%llu FeedbackSampleWrites=%llu RejectedViews=%llu TotalCpuMs=%.4f AverageViewCpuMs=%.4f MaxViewCpuMs=%.4f FingerprintEntries=%d FingerprintsPruned=%d CubemapRequired=0 LUTRequired=0 CaptureGenerationRequired=0 CompositeAttempted=0 RDGPassSubmitted=0 GPUWorkSubmitted=0"),
		*RenderState->WorldName, *RenderState->WorldType,
		NowSeconds - VisibilityObservationSummaryStartSeconds,
		RenderState->ActiveVisibilityObservationPairCount.Load(),
		RenderState->ActiveOwnershipPairCount.Load(),
		RenderState->PairsRenderThread.Num(),
		VisibilityObservationViewInvocationCount,
		VisibilityObservationPairEvaluationCount,
		VisibilityObservationSampleWriteCount,
		VisibilityObservationRejectedViewCount,
		VisibilityObservationAccumulatedCpuMs,
		AverageViewCpuMs,
		VisibilityObservationMaxCpuMs,
		LastVisibilityObservationFingerprintByHandleRenderThread.Num(),
		FingerprintsPruned);

	VisibilityObservationSummaryStartSeconds = NowSeconds;
	VisibilityObservationAccumulatedCpuMs = 0.0;
	VisibilityObservationMaxCpuMs = 0.0;
	VisibilityObservationViewInvocationCount = 0;
	VisibilityObservationPairEvaluationCount = 0;
	VisibilityObservationSampleWriteCount = 0;
	VisibilityObservationRejectedViewCount = 0;
#endif
}

void FWPSceneViewExtension::MaybeLogOwnershipSummary_RenderThread()
{
	// 로그 전용: NowSeconds/IntervalSeconds/OwnershipSummaryStartSeconds는 summary 출력 주기만
	// 제어하며 ownership pass 실행이나 warmup 상태 전이에는 관여하지 않는다.
	const double NowSeconds = FPlatformTime::Seconds();
	if (OwnershipSummaryStartSeconds <= 0.0)
	{
		OwnershipSummaryStartSeconds = NowSeconds;
		return;
	}
	const double IntervalSeconds = FMath::Max(
		static_cast<double>(CVarWPViewSummaryInterval.GetValueOnRenderThread()), 1.0);
	if (NowSeconds - OwnershipSummaryStartSeconds < IntervalSeconds)
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	// 로그 전용: prune 전후 개수와 경과 시간은 summary의 cache 진단 필드에만 사용한다.
	const double TrackerPruneStartSeconds = FPlatformTime::Seconds();
	const int32 WarmupTrackerCountBefore = OwnershipWarmupByHandleRenderThread.Num();
#endif
	for (auto Iterator = OwnershipWarmupByHandleRenderThread.CreateIterator(); Iterator; ++Iterator)
	{
		if (!RenderState->PairsRenderThread.Contains(Iterator.Key()))
		{
			Iterator.RemoveCurrent();
		}
	}
#if !UE_BUILD_SHIPPING
	const int32 WarmupTrackerPruned = WarmupTrackerCountBefore
		- OwnershipWarmupByHandleRenderThread.Num();
	const int32 EligibilityEntryCountBefore = LastEligibilityByHandle.Num();
#endif
	for (auto Iterator = LastEligibilityByHandle.CreateIterator(); Iterator; ++Iterator)
	{
		if (!RenderState->PairsRenderThread.Contains(Iterator.Key()))
		{
			Iterator.RemoveCurrent();
		}
	}
#if !UE_BUILD_SHIPPING
	const int32 EligibilityEntriesPruned = EligibilityEntryCountBefore
		- LastEligibilityByHandle.Num();
	const double TrackerPruneCpuMs =
		(FPlatformTime::Seconds() - TrackerPruneStartSeconds) * 1000.0;

	// 로그 전용: 아래 count별 평균 CPU 값과 interval counter/accumulator는
	// ProductionSummary 메시지에만 사용되며 render 결과를 제어하지 않는다.
	// Pair result counters scale with active pair count. Callback CPU is sampled
	// once per view callback, so its denominator must remain callback invocations.
	const uint64 OwnershipCallbackCount = OwnershipCallbackInvocationCount;
	const double AverageCallbackCpuMs = OwnershipCallbackCount > 0
		? OwnershipAccumulatedCallbackCpuMs / static_cast<double>(OwnershipCallbackCount)
		: 0.0;
	const double AverageSetupCpuMs = OwnershipRDGSetupAttemptCount > 0
		? OwnershipAccumulatedSetupCpuMs
			/ static_cast<double>(OwnershipRDGSetupAttemptCount)
		: 0.0;
	const double AveragePreflightCpuMs = OwnershipPreflightAttemptCount > 0
		? OwnershipAccumulatedPreflightCpuMs
			/ static_cast<double>(OwnershipPreflightAttemptCount)
		: 0.0;
	const double AverageEligibilityCpuMs = OwnershipEligibilityEvaluationCount > 0
		? OwnershipAccumulatedEligibilityCpuMs
			/ static_cast<double>(OwnershipEligibilityEvaluationCount)
		: 0.0;
	WP_LOG(nullptr, Verbose,
		TEXT("[RenderThread][ProductionSummary] World=%s WorldType=%s IntervalSeconds=%.3f ActiveWarmupOrProductionPairs=%d CallbackInvocations=%llu PairAttempts=%llu WarmupPairViews=%llu WarmupEndpointPasses=%llu WarmupAckPublished=%llu WarmupFailures=%llu WarmupLatchedSkips=%llu WarmupProgressPreservedAcrossPacketUpdates=%llu ProductionPairViews=%llu ProductionEndpointPasses=%llu ProductionFailures=%llu UnsupportedViews=%llu MissingReadyPacketViews=%llu InvisibleProductionPairViews=%llu PreflightPairFailures=%llu UnexpectedSubmissionFailures=%llu ForcedProductionFailureConfigured=%d ForcedProductionFailureRemaining=%d TotalCallbackCpuMs=%.4f AverageCallbackCpuMs=%.4f MaxCallbackCpuMs=%.4f EligibilityEvaluations=%llu TotalEligibilityCpuMs=%.4f AverageEligibilityCpuMs=%.4f MaxEligibilityCpuMs=%.4f PreflightAttempts=%llu PreflightFailures=%llu TotalPreflightCpuMs=%.4f AveragePreflightCpuMs=%.4f MaxPreflightCpuMs=%.4f RDGSetupAttempts=%llu TotalRDGSetupCpuMs=%.4f AverageRDGSetupCpuMs=%.4f MaxRDGSetupCpuMs=%.4f WarmupTrackerEntries=%d WarmupTrackersPruned=%d EligibilityEntries=%d EligibilityEntriesPruned=%d CachePruneCpuMs=%.4f GpuStat=WP.ProductionComposite WarmupUsesProductionOutput=1 WarmupConsecutiveFramesRequired=2 FailurePolicy=UntouchedSceneColorPortalAbsentFailClosed BaseSceneColorImmutable=1 EndpointOrder=GlobalFarToNearSurfaceStableSelectorPairIdHandleSide MultiPairPreflightFailure=ViewAtomicRollback UnexpectedSubmissionFailure=ViewAtomicRollback MasterEnabled=%d"),
		*RenderState->WorldName, *RenderState->WorldType,
		NowSeconds - OwnershipSummaryStartSeconds,
		RenderState->ActiveOwnershipPairCount.Load(),
		OwnershipCallbackInvocationCount, OwnershipPairAttemptCount,
		OwnershipWarmupCallbackCount,
		OwnershipWarmupEndpointPassCount, OwnershipWarmupAckCount,
		OwnershipWarmupFailureCount, OwnershipWarmupLatchedSkipCount,
		OwnershipWarmupProgressPreservedCount,
		OwnershipProductionViewCount, OwnershipProductionEndpointPassCount,
		OwnershipProductionFailureCount, OwnershipUnsupportedViewCount,
		OwnershipMissingReadyPacketViewCount, OwnershipInvisibleViewCount,
		OwnershipPreflightPairFailureCount,
		OwnershipUnexpectedSubmissionFailureCount,
		LastObservedOwnershipForceFailureCallbacks,
		RemainingOwnershipForceFailureCallbacks,
		OwnershipAccumulatedCallbackCpuMs, AverageCallbackCpuMs,
		OwnershipMaxCallbackCpuMs, OwnershipEligibilityEvaluationCount,
		OwnershipAccumulatedEligibilityCpuMs, AverageEligibilityCpuMs,
		OwnershipMaxEligibilityCpuMs,
		OwnershipPreflightAttemptCount, OwnershipPreflightFailureCount,
		OwnershipAccumulatedPreflightCpuMs, AveragePreflightCpuMs,
		OwnershipMaxPreflightCpuMs,
		OwnershipRDGSetupAttemptCount,
		OwnershipAccumulatedSetupCpuMs,
		AverageSetupCpuMs, OwnershipMaxSetupCpuMs,
		OwnershipWarmupByHandleRenderThread.Num(), WarmupTrackerPruned,
		LastEligibilityByHandle.Num(), EligibilityEntriesPruned, TrackerPruneCpuMs,
		CVarWPSceneViewExtensionEnabled.GetValueOnRenderThread() != 0 ? 1 : 0);
#endif

	// 로그 전용: 방금 출력한 ownership interval의 telemetry counter/timing accumulator만 초기화한다.
	OwnershipSummaryStartSeconds = NowSeconds;
	OwnershipAccumulatedCallbackCpuMs = 0.0;
	OwnershipMaxCallbackCpuMs = 0.0;
	OwnershipAccumulatedSetupCpuMs = 0.0;
	OwnershipMaxSetupCpuMs = 0.0;
	OwnershipRDGSetupAttemptCount = 0;
	OwnershipAccumulatedPreflightCpuMs = 0.0;
	OwnershipMaxPreflightCpuMs = 0.0;
	OwnershipAccumulatedEligibilityCpuMs = 0.0;
	OwnershipMaxEligibilityCpuMs = 0.0;
	OwnershipEligibilityEvaluationCount = 0;
	OwnershipWarmupCallbackCount = 0;
	OwnershipWarmupEndpointPassCount = 0;
	OwnershipWarmupAckCount = 0;
	OwnershipWarmupFailureCount = 0;
	OwnershipWarmupLatchedSkipCount = 0;
	OwnershipWarmupProgressPreservedCount = 0;
	OwnershipProductionViewCount = 0;
	OwnershipProductionEndpointPassCount = 0;
	OwnershipProductionFailureCount = 0;
	OwnershipUnsupportedViewCount = 0;
	OwnershipMissingReadyPacketViewCount = 0;
	OwnershipInvisibleViewCount = 0;
	OwnershipCallbackInvocationCount = 0;
	OwnershipPairAttemptCount = 0;
	OwnershipPreflightAttemptCount = 0;
	OwnershipPreflightFailureCount = 0;
	OwnershipPreflightPairFailureCount = 0;
	OwnershipUnexpectedSubmissionFailureCount = 0;
}

