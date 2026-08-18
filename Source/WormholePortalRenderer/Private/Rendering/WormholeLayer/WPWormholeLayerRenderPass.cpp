// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/WormholeLayer/WPWormholeLayerRenderPass.h"

#include "Rendering/WormholeLayer/WPWormholeDepthProxyComponent.h"
#include "Rendering/WormholeLayer/WPWormholeDepthProxyRenderData.h"

#include "DepthRendering.h"
#include "CustomRenderPassSceneCapture.h"
#include "Components/MeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/WPPortalVisibilityMath.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Passes/WPCompositePass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "Rendering/CustomRenderPass.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneInterface.h"
#include "ScenePrivate.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"
#include "Subsystem/WPRegistrySubsystem.h"
#include "SystemTextures.h"
#include "Rendering/WormholeLayer/WPWormholeBeforeDOFComposite.h"
#include "Rendering/WormholeLayer/WPWormholeSeparateTranslucencyMask.h"
#include "WormholePortalActor.h"
#include "WPRendererService.h"
#include "WPRenderState.h"
#include "WPLog.h"

#include <atomic>

namespace
{
	TAutoConsoleVariable<int32> CVarWPCompositePath(TEXT("wp.CompositePath"), 1, TEXT("Wormhole composite implementation. 0: legacy direct production composite, 1: prepared HDR layer with depth-tested translucency (default). The selected path never falls back to the other path."), ECVF_RenderThreadSafe);
	const FName WPWormholeDepthProxyComponentTag(TEXT("WP.WormholeDepthProxy"));
	std::atomic<bool> GLoggedMissingDepthPrepass{false};

	uint64 MakeWPWormholeLayerViewKey(const FSceneView& View)
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

	struct FWPPreparedViewState
	{
		TRefCountPtr<IPooledRenderTarget> CleanSceneColorHistory;
		TRefCountPtr<IPooledRenderTarget> CleanSceneColorWithSeparateHistory;
		TRefCountPtr<IPooledRenderTarget> CleanSceneDepthHistory;
		FRDGTextureRef CleanBeforeDOFSceneColorRDG = nullptr;
		FRDGTextureRef PreparedWormholeLayerRDG = nullptr;
		FRDGTextureRef WormholeCoverageMaskRDG = nullptr;
		FRDGTextureRef WormholeVelocityOverrideRDG = nullptr;
		FIntRect CleanSceneColorViewRect;
		FIntRect CleanSceneColorWithSeparateViewRect;
		FIntRect CleanSceneDepthViewRect;
		FIntRect CleanBeforeDOFSceneColorViewRect;
		FIntRect PreparedLayerViewRect;
		FIntRect WormholeCoverageMaskViewRect;
		FIntRect WormholeVelocityOverrideViewRect;
		FMatrix44f PreviousWorldToClip = FMatrix44f::Identity;
		FMatrix44f PreviousScreenToWorld = FMatrix44f::Identity;
		float PreviousPreExposure = 1.0f;
		float PreparedPreExposure = 1.0f;
		uint32 CleanHistoryFrameNumber = MAX_uint32;
		uint32 CleanWithSeparateHistoryFrameNumber = MAX_uint32;
		uint32 CleanDepthHistoryFrameNumber = MAX_uint32;
		uint32 CleanBeforeDOFRDGFrameNumber = MAX_uint32;
		uint32 PreparedEvaluationFrameNumber = MAX_uint32;
		uint32 PreparedRDGFrameNumber = MAX_uint32;
		int32 CandidateEndpointCount = 0;
		int32 PreparedEndpointCount = 0;
		int32 CoverageEndpointCount = 0;
		int32 VelocityEndpointCount = 0;
		bool bPreparedSourceHistoryCompatible = false;
		bool bHasReadyPreparedPairs = false;
		bool bProductionBypassClaimed = false;
		uint32 LastNotReadyErrorLogFrame = MAX_uint32;
		uint32 LastVelocityPatchErrorLogFrame = MAX_uint32;
	};

	class FWPPreparedLayerSharedState final : public TSharedFromThis<FWPPreparedLayerSharedState, ESPMode::ThreadSafe>
	{
	public:
		TSharedRef<FWPPreparedViewState, ESPMode::ThreadSafe> FindOrAddView_RenderThread(const uint64 ViewKey)
		{
			check(IsInRenderingThread());
			TSharedPtr<FWPPreparedViewState, ESPMode::ThreadSafe>& State = ViewsRenderThread.FindOrAdd(ViewKey);
			if (!State.IsValid()) State = MakeShared<FWPPreparedViewState, ESPMode::ThreadSafe>();
			return State.ToSharedRef();
		}

		TSharedPtr<FWPPreparedViewState, ESPMode::ThreadSafe> FindView_RenderThread(const uint64 ViewKey) const
		{
			check(IsInRenderingThread());
			return ViewsRenderThread.FindRef(ViewKey);
		}

		void Reset_RenderThread()
		{
			check(IsInRenderingThread());
			ViewsRenderThread.Reset();
		}

	private:
		TMap<uint64, TSharedPtr<FWPPreparedViewState, ESPMode::ThreadSafe>> ViewsRenderThread;
	};

	struct FWPPreparedEndpoint
	{
		const FWPRenderThreadPacket* Packet = nullptr;
		uint64 HandleValue = 0;
		EWPSide Side = EWPSide::None;
		double NearSurfaceDistanceCm = 0.0;
		FName StableSelector = NAME_None;
	};

	bool IsWPPreparedEndpointBefore(const FWPPreparedEndpoint& Left, const FWPPreparedEndpoint& Right)
	{
		if (Left.NearSurfaceDistanceCm != Right.NearSurfaceDistanceCm) return Left.NearSurfaceDistanceCm > Right.NearSurfaceDistanceCm;
		if (Left.StableSelector != Right.StableSelector) return Left.StableSelector.LexicalLess(Right.StableSelector);
		if (Left.Packet && Right.Packet && Left.Packet->PairId != Right.Packet->PairId) return Left.Packet->PairId < Right.Packet->PairId;
		if (Left.HandleValue != Right.HandleValue) return Left.HandleValue < Right.HandleValue;
		return static_cast<uint8>(Left.Side) < static_cast<uint8>(Right.Side);
	}

	class FWPWormholeLayerRenderPass final : public FCustomRenderPassBase
	{
	public:
		FWPWormholeLayerRenderPass(const FIntPoint& InRenderTargetSize, TArray<FPrimitiveComponentId>&& InDepthProxyPrimitiveIds, const uint64 InSourceViewKey, TSharedRef<FWPPreparedLayerSharedState, ESPMode::ThreadSafe> InPreparedState, TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> InRenderState)
			: FCustomRenderPassBase(TEXT("WPWormholeLayerRenderPass"), ERenderMode::DepthAndBasePass, ERenderOutput::SceneColorAndAlpha, InRenderTargetSize), PreparedState(MoveTemp(InPreparedState)), RenderState(MoveTemp(InRenderState)), DepthProxyCount(InDepthProxyPrimitiveIds.Num()), SourceViewKey(InSourceViewKey)
		{
			bSceneColorWithTranslucent = true;
			SetUserData(MakeUnique<FWPWormholeDepthProxyPassUserData>(MoveTemp(InDepthProxyPrimitiveIds)));
			TUniquePtr<FSceneCaptureCustomRenderPassUserData> RoutingData = MakeUnique<FSceneCaptureCustomRenderPassUserData>();
			RoutingData->bMainViewFamily = true;
#if !UE_BUILD_SHIPPING
			RoutingData->CaptureActorName = TEXT("WPWormholeLayerMainViewRouting");
#endif
			SetUserData(MoveTemp(RoutingData));
		}

		IMPLEMENT_CUSTOM_RENDER_PASS(FWPWormholeLayerRenderPass);

	protected:
		virtual void OnPreRender(FRDGBuilder& GraphBuilder) override
		{
			const double StartSeconds = FPlatformTime::Seconds();
			for (FViewInfo* CustomView : Views)
			{
				if (CustomView) CustomView->bSceneHasSkyMaterial = false;
			}
			const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(GetRenderTargetSize(), PF_FloatRGBA, FClearValueBinding::Transparent, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable);
			RenderTargetTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("WP.WormholeLayer.Output"));

			const FScene* RenderScene = Views.Num() > 0 && Views[0] && Views[0]->Family && Views[0]->Family->Scene ? Views[0]->Family->Scene->GetRenderScene() : nullptr;
			const int32 EarlyZPassMode = RenderScene ? static_cast<int32>(RenderScene->EarlyZPassMode) : static_cast<int32>(DDM_None);
			const bool bEarlyZPassMovable = RenderScene && RenderScene->bEarlyZPassMovable;
			const bool bDepthPrepassExpected = RenderScene && (RenderScene->EarlyZPassMode != DDM_None || RenderScene->bEarlyZPassMovable);
			if (!bDepthPrepassExpected)
			{
				bool bExpected = false;
				if (GLoggedMissingDepthPrepass.compare_exchange_strong(bExpected, true, std::memory_order_relaxed))
				{
					WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeDepthProxy] Depth prepass is disabled. EarlyZPassMode=%d EarlyZPassMovable=%d DepthProxyIds=%d CpuMs=%.4f"), EarlyZPassMode, bEarlyZPassMovable ? 1 : 0, DepthProxyCount, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				}
			}
			else
			{
				GLoggedMissingDepthPrepass.store(false, std::memory_order_relaxed);
			}
		}

		virtual void OnPostRender(FRDGBuilder& GraphBuilder) override
		{
			BuildPreparedLayer(GraphBuilder);
		}

	private:
		void BuildPreparedLayer(FRDGBuilder& GraphBuilder)
		{
			const double StartSeconds = FPlatformTime::Seconds();
			if (!RenderTargetTexture || Views.IsEmpty() || !Views[0] || !RenderState.IsValid() || RenderState->bShuttingDown.Load())
			{
				WP_LOG(nullptr, Warning, TEXT("[RenderThread][WPWormholeLayer] Cannot prepare layer. ViewKey=%llu OutputValid=%d ViewValid=%d RenderStateValid=%d RenderStateShuttingDown=%d CpuMs=%.4f"), SourceViewKey, RenderTargetTexture ? 1 : 0, !Views.IsEmpty() && Views[0] ? 1 : 0, RenderState.IsValid() ? 1 : 0, RenderState.IsValid() && RenderState->bShuttingDown.Load() ? 1 : 0, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return;
			}

			const FViewInfo& View = *static_cast<const FViewInfo*>(Views[0]);
			const FSceneTextures& SceneTextures = View.GetSceneTextures();
			FRDGTextureRef SceneDepthTexture = SceneTextures.Depth.Resolve ? SceneTextures.Depth.Resolve : SceneTextures.Depth.Target;
			if (!SceneDepthTexture)
			{
				WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeLayer] Custom SceneDepth is unavailable. ViewKey=%llu CpuMs=%.4f"), SourceViewKey, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return;
			}

			const TSharedRef<FWPPreparedViewState, ESPMode::ThreadSafe> PreparedViewState = PreparedState->FindOrAddView_RenderThread(SourceViewKey);
			PreparedViewState->PreparedWormholeLayerRDG = nullptr;
			PreparedViewState->WormholeCoverageMaskRDG = nullptr;
			PreparedViewState->WormholeVelocityOverrideRDG = nullptr;
			PreparedViewState->PreparedEvaluationFrameNumber = GFrameNumberRenderThread;
			PreparedViewState->PreparedRDGFrameNumber = MAX_uint32;
			PreparedViewState->CandidateEndpointCount = 0;
			PreparedViewState->PreparedEndpointCount = 0;
			PreparedViewState->CoverageEndpointCount = 0;
			PreparedViewState->VelocityEndpointCount = 0;
			PreparedViewState->bPreparedSourceHistoryCompatible = false;
			PreparedViewState->bHasReadyPreparedPairs = false;
			PreparedViewState->bProductionBypassClaimed = false;
			const FIntPoint TargetSize = GetRenderTargetSize();
			const auto IsHistoryFrameFresh = [](const uint32 FrameNumber)
			{
				return FrameNumber != MAX_uint32 && FrameNumber <= GFrameNumberRenderThread && GFrameNumberRenderThread - FrameNumber <= 1;
			};
			const bool bBaseHistoryTextureValid = PreparedViewState->CleanSceneColorHistory.IsValid() && PreparedViewState->CleanSceneColorViewRect.Width() > 0 && PreparedViewState->CleanSceneColorViewRect.Height() > 0;
			const bool bSeparateHistoryTextureValid = PreparedViewState->CleanSceneColorWithSeparateHistory.IsValid() && PreparedViewState->CleanSceneColorWithSeparateViewRect.Width() > 0 && PreparedViewState->CleanSceneColorWithSeparateViewRect.Height() > 0;
			const bool bSeparateHistoryFrameFresh = IsHistoryFrameFresh(PreparedViewState->CleanWithSeparateHistoryFrameNumber);
			const bool bUseSeparateInclusiveHistory = bSeparateHistoryTextureValid && bSeparateHistoryFrameFresh;
			const bool bHistoryTextureValid = bUseSeparateInclusiveHistory || bBaseHistoryTextureValid;
			const TRefCountPtr<IPooledRenderTarget>& SelectedHistory = bUseSeparateInclusiveHistory ? PreparedViewState->CleanSceneColorWithSeparateHistory : PreparedViewState->CleanSceneColorHistory;
			const uint32 HistoryFrameNumber = bUseSeparateInclusiveHistory ? PreparedViewState->CleanWithSeparateHistoryFrameNumber : PreparedViewState->CleanHistoryFrameNumber;
			FRDGTextureRef HistoryTexture = bHistoryTextureValid ? GraphBuilder.RegisterExternalTexture(SelectedHistory, bUseSeparateInclusiveHistory ? TEXT("WP.CleanSceneColorWithSeparateHistory") : TEXT("WP.CleanSceneColorHistory")) : GSystemTextures.GetBlackDummy(GraphBuilder);
			const FIntRect HistoryViewRect = bHistoryTextureValid ? (bUseSeparateInclusiveHistory ? PreparedViewState->CleanSceneColorWithSeparateViewRect : PreparedViewState->CleanSceneColorViewRect) : FIntRect(FIntPoint::ZeroValue, HistoryTexture->Desc.Extent);
			const bool bHistoryExtentCompatible = bHistoryTextureValid && HistoryViewRect.Min.X >= 0 && HistoryViewRect.Min.Y >= 0 && HistoryViewRect.Max.X <= HistoryTexture->Desc.Extent.X && HistoryViewRect.Max.Y <= HistoryTexture->Desc.Extent.Y;
			const bool bHistoryFrameFresh = IsHistoryFrameFresh(HistoryFrameNumber);
			const bool bDepthHistoryTextureValid = PreparedViewState->CleanSceneDepthHistory.IsValid() && PreparedViewState->CleanSceneDepthViewRect.Width() > 0 && PreparedViewState->CleanSceneDepthViewRect.Height() > 0;
			const bool bDepthHistoryFrameFresh = IsHistoryFrameFresh(PreparedViewState->CleanDepthHistoryFrameNumber);
			FRDGTextureRef PreviousDepthTexture = bDepthHistoryTextureValid ? GraphBuilder.RegisterExternalTexture(PreparedViewState->CleanSceneDepthHistory, TEXT("WP.CleanSceneDepthHistory")) : nullptr;
			const bool bDepthHistoryExtentCompatible = PreviousDepthTexture && PreparedViewState->CleanSceneDepthViewRect.Min.X >= 0 && PreparedViewState->CleanSceneDepthViewRect.Min.Y >= 0 && PreparedViewState->CleanSceneDepthViewRect.Max.X <= PreviousDepthTexture->Desc.Extent.X && PreparedViewState->CleanSceneDepthViewRect.Max.Y <= PreviousDepthTexture->Desc.Extent.Y;
			const bool bPreparedSourceHistoryCompatible = bHistoryExtentCompatible && bHistoryFrameFresh && bDepthHistoryExtentCompatible && bDepthHistoryFrameFresh;
			const float CurrentPreExposure = FMath::Max(View.PreExposure, UE_KINDA_SMALL_NUMBER);
			const float PreviousPreExposure = FMath::Max(PreparedViewState->PreviousPreExposure, UE_KINDA_SMALL_NUMBER);
			const float PreviousPreExposureScale = CurrentPreExposure / PreviousPreExposure;
			const FScreenPassTexture BaseSceneColor(HistoryTexture, HistoryViewRect);
			FScreenPassTexture CompositeInput(RenderTargetTexture, FIntRect(FIntPoint::ZeroValue, TargetSize));

			TArray<FWPPreparedEndpoint, TInlineAllocator<8>> Endpoints;
			int32 ReadyPairCount = 0;
			const FVector3d ViewOrigin(View.ViewMatrices.GetViewOrigin());
			for (const TPair<uint64, FWPRenderThreadPacket>& PairEntry : RenderState->PairsRenderThread)
			{
				const FWPRenderThreadPacket& Packet = PairEntry.Value;
				if (!Packet.IsOwnershipContractReady()) continue;
				++ReadyPairCount;
				const auto AddEndpoint = [&Endpoints, &View, &ViewOrigin, &Packet, HandleValue = PairEntry.Key](const EWPSide Side)
				{
					const bool bEndpointA = Side == EWPSide::SideA;
					const FWPMetricSettings& Metric = bEndpointA ? Packet.MetricA : Packet.MetricB;
					const FWPPortalVisualSettings& Visual = bEndpointA ? Packet.VisualA : Packet.VisualB;
					const float VisualScale = Visual.IsFiniteAndValid() ? Visual.UniformScale : 1.0f;
					const float ProxyRadiusCm = static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(Metric.PortalRadiusCm, Metric.OuterRadiusCm)) * VisualScale;
					const FVector3d Center = bEndpointA ? Packet.PortalACenterWorld : Packet.PortalBCenterWorld;
					const double DistanceSquared = FVector3d::DistSquared(ViewOrigin, Center);
					const bool bInsideSafeProxy = DistanceSquared <= FMath::Square(static_cast<double>(ProxyRadiusCm));
					if (!bInsideSafeProxy && !View.GetCullingFrustum().IntersectSphere(FVector(Center), ProxyRadiusCm)) return;
					const uint8 EndpointMask = Side == EWPSide::SideA ? 0x1u : 0x2u;
					if (!bInsideSafeProxy && Packet.bCaptureOcclusionValid && (Packet.CaptureOcclusionVisibleEndpointMask & EndpointMask) == 0)
					{
						return;
					}
					FWPPreparedEndpoint& Endpoint = Endpoints.AddDefaulted_GetRef();
					Endpoint.Packet = &Packet;
					Endpoint.HandleValue = HandleValue;
					Endpoint.Side = Side;
					Endpoint.NearSurfaceDistanceCm = FMath::Max(FMath::Sqrt(DistanceSquared) - static_cast<double>(ProxyRadiusCm), 0.0);
					Endpoint.StableSelector = bEndpointA ? Packet.StableSelectorNameA : Packet.StableSelectorNameB;
				};
				AddEndpoint(EWPSide::SideA);
				AddEndpoint(EWPSide::SideB);
			}
			Endpoints.Sort([](const FWPPreparedEndpoint& Left, const FWPPreparedEndpoint& Right) { return IsWPPreparedEndpointBefore(Left, Right); });
			PreparedViewState->CandidateEndpointCount = Endpoints.Num();

			int32 SubmittedEndpointCount = 0;
			TArray<FWPPreparedEndpoint, TInlineAllocator<8>> SubmittedEndpoints;
			EWPCompositeFailureReason LastFailureReason = EWPCompositeFailureReason::None;
			for (const FWPPreparedEndpoint& Endpoint : Endpoints)
			{
				FWPCompositePassParameters Parameters;
				if (!Endpoint.Packet || !BuildWPCompositePassParametersForEndpoint(*Endpoint.Packet, Endpoint.Side, FVector3d(View.ViewMatrices.GetPreViewTranslation()), Parameters))
				{
					LastFailureReason = EWPCompositeFailureReason::InvalidParameters;
					continue;
				}
				Parameters.SceneMode = EWPCompositeSceneMode::PreparedWormhole;
				Parameters.PreviousWorldToClip = PreparedViewState->PreviousWorldToClip;
				Parameters.PreviousScreenToWorld = PreparedViewState->PreviousScreenToWorld;
				Parameters.PreviousSceneColorPreExposureScale = PreviousPreExposureScale;
				Parameters.bPreviousSceneColorValid = bHistoryExtentCompatible && bDepthHistoryExtentCompatible;
				Parameters.PreviousSceneDepthTexture = PreviousDepthTexture;
				Parameters.PreviousSceneDepthViewRect = PreparedViewState->CleanSceneDepthViewRect;
				Parameters.PreparedForegroundTexture = RenderTargetTexture;
				Parameters.PreparedForegroundViewRect = FIntRect(FIntPoint::ZeroValue, TargetSize);
				bool bShaderAvailable = false;
				bool bSceneDepthArray = false;
				FScreenPassTexture EndpointOutput = AddWPCompositePass(GraphBuilder, View, CompositeInput, BaseSceneColor, FScreenPassRenderTarget(), SceneDepthTexture, 0, Parameters, Endpoint.HandleValue, bShaderAvailable, bSceneDepthArray, LastFailureReason);
				if (!EndpointOutput.IsValid()) continue;
				CompositeInput = EndpointOutput;
				SubmittedEndpoints.Add(Endpoint);
				++SubmittedEndpointCount;
			}

			const FRDGTextureDesc CoverageMaskDesc = FRDGTextureDesc::Create2D(TargetSize, PF_R8, FClearValueBinding::Black, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable);
			FRDGTextureRef CoverageMaskTexture = GraphBuilder.CreateTexture(CoverageMaskDesc, TEXT("WP.WormholeCoverageMask"));
			AddClearRenderTargetPass(GraphBuilder, CoverageMaskTexture, FLinearColor::Black);
			FScreenPassTexture CoverageInput(CoverageMaskTexture, FIntRect(FIntPoint::ZeroValue, TargetSize));
			const FScreenPassTexture PreparedVisibility(RenderTargetTexture, FIntRect(FIntPoint::ZeroValue, TargetSize));
			int32 CoverageEndpointCount = 0;
			for (const FWPPreparedEndpoint& Endpoint : SubmittedEndpoints)
			{
				FWPCompositePassParameters Parameters;
				if (!Endpoint.Packet || !BuildWPCompositePassParametersForEndpoint(*Endpoint.Packet, Endpoint.Side, FVector3d(View.ViewMatrices.GetPreViewTranslation()), Parameters))
				{
					LastFailureReason = EWPCompositeFailureReason::InvalidParameters;
					break;
				}
				Parameters.SceneMode = EWPCompositeSceneMode::PreparedCoverageMask;
				bool bShaderAvailable = false;
				bool bSceneDepthArray = false;
				FScreenPassTexture EndpointOutput = AddWPCompositePass(GraphBuilder, View, CoverageInput, PreparedVisibility, FScreenPassRenderTarget(), SceneDepthTexture, 0, Parameters, Endpoint.HandleValue, bShaderAvailable, bSceneDepthArray, LastFailureReason);
				if (!EndpointOutput.IsValid()) break;
				CoverageInput = EndpointOutput;
				++CoverageEndpointCount;
			}

			if (!SceneTextures.Velocity || SceneTextures.Velocity->Desc.Format == PF_Unknown)
			{
				WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeVelocityOverride] Custom SceneVelocity is unavailable. ViewKey=%llu RenderFrame=%u CpuMs=%.4f"), SourceViewKey, GFrameNumberRenderThread, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return;
			}
			const FRDGTextureDesc VelocityOverrideDesc = FRDGTextureDesc::Create2D(TargetSize, SceneTextures.Velocity->Desc.Format, FClearValueBinding::Transparent, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable);
			FRDGTextureRef VelocityOverrideTexture = GraphBuilder.CreateTexture(VelocityOverrideDesc, TEXT("WP.WormholeVelocityOverride"));
			AddClearRenderTargetPass(GraphBuilder, VelocityOverrideTexture, FLinearColor::Black);
			FScreenPassTexture VelocityInput(VelocityOverrideTexture, FIntRect(FIntPoint::ZeroValue, TargetSize));
			int32 VelocityEndpointCount = 0;
			for (const FWPPreparedEndpoint& Endpoint : SubmittedEndpoints)
			{
				FWPCompositePassParameters Parameters;
				if (!Endpoint.Packet || !BuildWPCompositePassParametersForEndpoint(*Endpoint.Packet, Endpoint.Side, FVector3d(View.ViewMatrices.GetPreViewTranslation()), Parameters))
				{
					LastFailureReason = EWPCompositeFailureReason::InvalidParameters;
					break;
				}
				Parameters.SceneMode = EWPCompositeSceneMode::PreparedVelocityOverride;
				Parameters.PreviousWorldToClip = PreparedViewState->PreviousWorldToClip;
				Parameters.PreviousScreenToWorld = PreparedViewState->PreviousScreenToWorld;
				Parameters.PreviousSceneColorPreExposureScale = PreviousPreExposureScale;
				Parameters.bPreviousSceneColorValid = bHistoryExtentCompatible && bDepthHistoryExtentCompatible;
				Parameters.PreviousSceneDepthTexture = PreviousDepthTexture;
				Parameters.PreviousSceneDepthViewRect = PreparedViewState->CleanSceneDepthViewRect;
				bool bShaderAvailable = false;
				bool bSceneDepthArray = false;
				FScreenPassTexture EndpointOutput = AddWPCompositePass(GraphBuilder, View, VelocityInput, BaseSceneColor, FScreenPassRenderTarget(), SceneDepthTexture, 0, Parameters, Endpoint.HandleValue, bShaderAvailable, bSceneDepthArray, LastFailureReason);
				if (!EndpointOutput.IsValid()) break;
				VelocityInput = EndpointOutput;
				++VelocityEndpointCount;
			}

			PreparedViewState->PreparedLayerViewRect = CompositeInput.ViewRect;
			PreparedViewState->WormholeCoverageMaskViewRect = CoverageInput.ViewRect;
			PreparedViewState->WormholeVelocityOverrideViewRect = VelocityInput.ViewRect;
			PreparedViewState->PreparedPreExposure = CurrentPreExposure;
			PreparedViewState->PreparedEndpointCount = SubmittedEndpointCount;
			PreparedViewState->CoverageEndpointCount = CoverageEndpointCount;
			PreparedViewState->VelocityEndpointCount = VelocityEndpointCount;
			PreparedViewState->bPreparedSourceHistoryCompatible = bPreparedSourceHistoryCompatible;
			PreparedViewState->bHasReadyPreparedPairs = ReadyPairCount > 0;
			const bool bPreparedPairComplete = SubmittedEndpointCount > 0 && CoverageEndpointCount == SubmittedEndpointCount && VelocityEndpointCount == SubmittedEndpointCount && bPreparedSourceHistoryCompatible;
			if (bPreparedPairComplete)
			{
				PreparedViewState->PreparedWormholeLayerRDG = CompositeInput.Texture;
				PreparedViewState->WormholeCoverageMaskRDG = CoverageInput.Texture;
				PreparedViewState->WormholeVelocityOverrideRDG = VelocityInput.Texture;
				PreparedViewState->PreparedRDGFrameNumber = GFrameNumberRenderThread;
			}
		}

		TSharedRef<FWPPreparedLayerSharedState, ESPMode::ThreadSafe> PreparedState;
		TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState;
		int32 DepthProxyCount = 0;
		uint64 SourceViewKey = 0;
	};

	class FWPWormholeLayerSceneViewExtension final : public FSceneViewExtensionBase
	{
		struct FPairState
		{
			TWeakObjectPtr<AWormholePortalActor> PortalA;
			TWeakObjectPtr<AWormholePortalActor> PortalB;
			TWeakObjectPtr<UWPWormholeDepthProxyComponent> ProxyA;
			TWeakObjectPtr<UWPWormholeDepthProxyComponent> ProxyB;
			float RadiusACm = 0.0f;
			float RadiusBCm = 0.0f;
		};

		struct FWorldState
		{
			TWeakObjectPtr<UWorld> World;
			TMap<FGuid, FPairState> Pairs;
			TSet<FPrimitiveComponentId> HiddenSkyPrimitiveIds;
			FDelegateHandle ActorSpawnedHandle;
			uint32 LastReconcileFrame = MAX_uint32;
		};

	public:
		explicit FWPWormholeLayerSceneViewExtension(const FAutoRegister& AutoRegister, FWPRendererService& InRendererService) : FSceneViewExtensionBase(AutoRegister), RendererService(InRendererService), PreparedState(MakeShared<FWPPreparedLayerSharedState, ESPMode::ThreadSafe>())
		{
			WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FWPWormholeLayerSceneViewExtension::HandleWorldCleanup);
		}

		virtual ~FWPWormholeLayerSceneViewExtension() override
		{
			if (IsInGameThread()) Shutdown_GameThread();
		}

		void Shutdown_GameThread()
		{
			check(IsInGameThread());
			if (bShutdown) return;
			bShutdown = true;
			if (WorldCleanupHandle.IsValid())
			{
				FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
				WorldCleanupHandle.Reset();
			}
			DestroyAllWorldStates();
			QueuePreparedStateReset_GameThread();
		}

		bool TryClaimWormholeLayerOwnership_RenderThread(const FSceneView& View)
		{
			check(IsInRenderingThread());
			const double StartSeconds = FPlatformTime::Seconds();
			if (CVarWPCompositePath.GetValueOnRenderThread() != 1) return false;
			const uint64 ViewKey = MakeWPWormholeLayerViewKey(View);
			const TSharedPtr<FWPPreparedViewState, ESPMode::ThreadSafe> ViewState = PreparedState->FindView_RenderThread(ViewKey);
			const bool bEvaluatedThisFrame = ViewState.IsValid() && ViewState->PreparedEvaluationFrameNumber == GFrameNumberRenderThread;
			const bool bCurrentFrame = ViewState.IsValid() && ViewState->PreparedRDGFrameNumber == GFrameNumberRenderThread;
			const bool bComplete = ViewState.IsValid() && ViewState->PreparedEndpointCount > 0 && ViewState->PreparedEndpointCount == ViewState->CoverageEndpointCount && ViewState->PreparedEndpointCount == ViewState->VelocityEndpointCount;
			const bool bCameraHistoryValid = !View.bCameraCut;
			const bool bReady = ViewState.IsValid() && bCurrentFrame && bComplete && ViewState->bPreparedSourceHistoryCompatible && bCameraHistoryValid && ViewState->bHasReadyPreparedPairs && ViewState->PreparedWormholeLayerRDG && ViewState->WormholeCoverageMaskRDG && ViewState->WormholeVelocityOverrideRDG;
			const bool bNoPreparedEndpointExpected = bEvaluatedThisFrame && ViewState->CandidateEndpointCount == 0;
			if (bReady || bNoPreparedEndpointExpected)
			{
				ViewState->bProductionBypassClaimed = true;
			}
			else
			{
				const bool bLogError = !ViewState.IsValid() || ViewState->LastNotReadyErrorLogFrame == MAX_uint32 || GFrameNumberRenderThread - ViewState->LastNotReadyErrorLogFrame >= 300;
				if (ViewState.IsValid() && bLogError) ViewState->LastNotReadyErrorLogFrame = GFrameNumberRenderThread;
				if (bLogError) WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeLayer] Prepared layer is not ready. ViewKey=%llu RenderFrame=%u StateValid=%d EvaluationFrame=%u EvaluatedThisFrame=%d PreparedFrame=%u CurrentFrame=%d CandidateEndpoints=%d PreparedEndpoints=%d CoverageEndpoints=%d VelocityEndpoints=%d Complete=%d HistoryCompatible=%d CameraCut=%d ReadyPairs=%d LayerValid=%d MaskValid=%d VelocityValid=%d CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, ViewState.IsValid() ? 1 : 0, ViewState.IsValid() ? ViewState->PreparedEvaluationFrameNumber : MAX_uint32, bEvaluatedThisFrame ? 1 : 0, ViewState.IsValid() ? ViewState->PreparedRDGFrameNumber : MAX_uint32, bCurrentFrame ? 1 : 0, ViewState.IsValid() ? ViewState->CandidateEndpointCount : 0, ViewState.IsValid() ? ViewState->PreparedEndpointCount : 0, ViewState.IsValid() ? ViewState->CoverageEndpointCount : 0, ViewState.IsValid() ? ViewState->VelocityEndpointCount : 0, bComplete ? 1 : 0, ViewState.IsValid() && ViewState->bPreparedSourceHistoryCompatible ? 1 : 0, View.bCameraCut ? 1 : 0, ViewState.IsValid() && ViewState->bHasReadyPreparedPairs ? 1 : 0, ViewState.IsValid() && ViewState->PreparedWormholeLayerRDG ? 1 : 0, ViewState.IsValid() && ViewState->WormholeCoverageMaskRDG ? 1 : 0, ViewState.IsValid() && ViewState->WormholeVelocityOverrideRDG ? 1 : 0, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			}
			return true;
		}

		virtual int32 GetPriority() const override { return 100; }

		virtual void SubscribeToPostProcessingPass(const EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, const bool bIsPassEnabled) override
		{
			(void)bIsPassEnabled;
			if (CVarWPCompositePath.GetValueOnRenderThread() != 1 || InView.CustomRenderPass || InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection) return;
			if (Pass == EPostProcessingPass::BeforeDOF)
			{
				InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FWPWormholeLayerSceneViewExtension::BeforeDOFComposite_RenderThread));
			}
			else if (Pass == EPostProcessingPass::TranslucencyAfterDOF)
			{
				InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FWPWormholeLayerSceneViewExtension::MaskSeparateTranslucency_RenderThread));
			}
		}

		virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override
		{
			check(IsInGameThread());
			if (bShutdown) return;
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bEnabled = CVarWPCompositePath.GetValueOnGameThread() == 1;
			if (bEnabled != bLastEnabled)
			{
				if (!bEnabled)
				{
					DestroyAllWorldStates();
					QueuePreparedStateReset_GameThread();
				}
				bLastEnabled = bEnabled;
			}
			if (!bEnabled) return;

			FSceneInterface* Scene = InViewFamily.Scene;
			UWorld* World = Scene ? Scene->GetWorld() : nullptr;
			const FIntPoint RenderTargetSize = InView.UnscaledViewRect.Size();
			const uint32 FrameNumber = static_cast<uint32>(GFrameCounter);
			if (!Scene || !IsValid(World) || !World->IsGameWorld() || !InViewFamily.EngineShowFlags.Game || InView.bIsSceneCapture || InView.CustomRenderPass || RenderTargetSize.X <= 0 || RenderTargetSize.Y <= 0) return;

			TArray<FPrimitiveComponentId> DepthProxyPrimitiveIds;
			ReconcileWorld(World, FrameNumber, DepthProxyPrimitiveIds);

			FSceneInterface::FCustomRenderPassRendererInput PassInput;
			PassInput.ViewLocation = InView.ViewMatrices.GetViewOrigin();
			PassInput.ViewRotationMatrix = InView.ViewMatrices.GetWorldToView().RemoveTranslation();
			PassInput.ProjectionMatrix = InView.ViewMatrices.GetViewToClip();
			PassInput.HiddenPrimitives = InView.HiddenPrimitives;
			if (const FWorldState* CurrentWorldState = WorldStates.Find(TWeakObjectPtr<UWorld>(World)))
			{
				for (const FPrimitiveComponentId PrimitiveId : CurrentWorldState->HiddenSkyPrimitiveIds) PassInput.HiddenPrimitives.Add(PrimitiveId);
			}

			PassInput.ShowOnlyPrimitives = InView.ShowOnlyPrimitives;
			PassInput.EngineShowFlags = InViewFamily.EngineShowFlags;
			PassInput.EngineShowFlags.SetSeparateTranslucency(false);
			PassInput.ViewActor = InView.ViewActor;
			PassInput.bIsSceneCapture = false;
			PassInput.bUseMainViewFamilyShowFlags = false;
			const uint64 SourceViewKey = MakeWPWormholeLayerViewKey(InView);
			TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState = RendererService.FindRenderStateForWorld(World);
			PassInput.CustomRenderPass = new FWPWormholeLayerRenderPass(RenderTargetSize, MoveTemp(DepthProxyPrimitiveIds), SourceViewKey, PreparedState, MoveTemp(RenderState));

			if (!Scene->AddCustomRenderPass(&InViewFamily, PassInput))
			{
				delete PassInput.CustomRenderPass;
				PassInput.CustomRenderPass = nullptr;
				WP_LOG(nullptr, Error, TEXT("[GameThread][WPWormholeLayer][Queue] Result=Failed World=%s Frame=%u ViewKey=%llu Width=%d Height=%d CpuMs=%.4f"), *GetNameSafe(World), FrameNumber, SourceViewKey, RenderTargetSize.X, RenderTargetSize.Y, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			}
		}

	private:
		FScreenPassTexture BeforeDOFComposite_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
		{
			const double StartSeconds = FPlatformTime::Seconds();
			const uint64 ViewKey = MakeWPWormholeLayerViewKey(View);
			const FScreenPassTexture CleanMainSceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
			if (!CleanMainSceneColor.IsValid())
			{
				WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeBeforeDOFComposite] Main SceneColor is invalid. ViewKey=%llu RenderFrame=%u CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
			}

			const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
			const TSharedRef<FWPPreparedViewState, ESPMode::ThreadSafe> ViewState = PreparedState->FindOrAddView_RenderThread(ViewKey);
			FRDGTextureRef CurrentSceneDepth = Inputs.SceneTextures.SceneTextures ? Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture : nullptr;
			FRDGTextureRef CurrentSceneVelocity = ViewInfo.GetSceneTextures().Velocity;
			ViewState->CleanSceneColorViewRect = CleanMainSceneColor.ViewRect;
			ViewState->CleanBeforeDOFSceneColorRDG = CleanMainSceneColor.Texture;
			ViewState->CleanBeforeDOFSceneColorViewRect = CleanMainSceneColor.ViewRect;
			ViewState->CleanBeforeDOFRDGFrameNumber = GFrameNumberRenderThread;
			ViewState->PreviousWorldToClip = FMatrix44f(View.ViewMatrices.GetWorldToClip());
			ViewState->PreviousScreenToWorld = FMatrix44f(View.ViewMatrices.GetScreenToClipMatrix() * View.ViewMatrices.GetClipToWorld());
			ViewState->PreviousPreExposure = FMath::Max(ViewInfo.PreExposure, UE_KINDA_SMALL_NUMBER);
			ViewState->CleanHistoryFrameNumber = GFrameNumberRenderThread;
			GraphBuilder.QueueTextureExtraction(CleanMainSceneColor.Texture, &ViewState->CleanSceneColorHistory);
			if (CurrentSceneDepth && CurrentSceneDepth->Desc.Dimension == ETextureDimension::Texture2D && CurrentSceneDepth->Desc.NumSamples == 1)
			{
				ViewState->CleanSceneDepthViewRect = CleanMainSceneColor.ViewRect;
				ViewState->CleanDepthHistoryFrameNumber = GFrameNumberRenderThread;
				GraphBuilder.QueueTextureExtraction(CurrentSceneDepth, &ViewState->CleanSceneDepthHistory);
			}
			else
			{
				ViewState->CleanSceneDepthHistory.SafeRelease();
				ViewState->CleanSceneDepthViewRect = FIntRect();
				ViewState->CleanDepthHistoryFrameNumber = MAX_uint32;
				WP_LOG(nullptr, Warning, TEXT("[RenderThread][WPWormholeLayer] SceneDepth history is unavailable. ViewKey=%llu RenderFrame=%u SceneTexturesValid=%d DepthValid=%d DepthDimension=%d DepthSamples=%u CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, Inputs.SceneTextures.SceneTextures ? 1 : 0, CurrentSceneDepth ? 1 : 0, CurrentSceneDepth ? static_cast<int32>(CurrentSceneDepth->Desc.Dimension) : -1, CurrentSceneDepth ? CurrentSceneDepth->Desc.NumSamples : 0u, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			}

			if (ViewState->PreparedRDGFrameNumber != GFrameNumberRenderThread || !ViewState->bProductionBypassClaimed || !ViewState->PreparedWormholeLayerRDG || !ViewState->WormholeCoverageMaskRDG || !ViewState->WormholeVelocityOverrideRDG)
			{
				return CleanMainSceneColor;
			}

			const FScreenPassTexture PreparedWormholeLayer(ViewState->PreparedWormholeLayerRDG, ViewState->PreparedLayerViewRect);
			const FScreenPassTexture WormholeCoverageMask(ViewState->WormholeCoverageMaskRDG, ViewState->WormholeCoverageMaskViewRect);
			const FScreenPassTexture WormholeVelocityOverride(ViewState->WormholeVelocityOverrideRDG, ViewState->WormholeVelocityOverrideViewRect);
			const double VelocityPatchStartSeconds = FPlatformTime::Seconds();
			if (!AddWPWormholeVelocityPatchPass(GraphBuilder, View, WormholeVelocityOverride, WormholeCoverageMask, CurrentSceneVelocity, CleanMainSceneColor.ViewRect))
			{
				const bool bLogError = ViewState->LastVelocityPatchErrorLogFrame == MAX_uint32 || GFrameNumberRenderThread - ViewState->LastVelocityPatchErrorLogFrame >= 300;
				if (bLogError)
				{
					ViewState->LastVelocityPatchErrorLogFrame = GFrameNumberRenderThread;
					WP_LOG(nullptr, Warning, TEXT("[RenderThread][WPWormholeVelocityPatch] Patch skipped. ViewKey=%llu RenderFrame=%u SceneVelocityValid=%d Dimension=%d Format=%d Samples=%u Extent=%dx%d OverrideFormat=%d CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, CurrentSceneVelocity ? 1 : 0, CurrentSceneVelocity ? static_cast<int32>(CurrentSceneVelocity->Desc.Dimension) : -1, CurrentSceneVelocity ? static_cast<int32>(CurrentSceneVelocity->Desc.Format) : -1, CurrentSceneVelocity ? CurrentSceneVelocity->Desc.NumSamples : 0u, CurrentSceneVelocity ? CurrentSceneVelocity->Desc.Extent.X : 0, CurrentSceneVelocity ? CurrentSceneVelocity->Desc.Extent.Y : 0, static_cast<int32>(WormholeVelocityOverride.Texture->Desc.Format), (FPlatformTime::Seconds() - VelocityPatchStartSeconds) * 1000.0);
				}
			}
			const float PreparedToMainPreExposureScale = ViewState->PreviousPreExposure / FMath::Max(ViewState->PreparedPreExposure, UE_KINDA_SMALL_NUMBER);
			const FScreenPassTexture CompositeOutput = AddWPWormholeBeforeDOFCompositePass(GraphBuilder, View, CleanMainSceneColor, PreparedWormholeLayer, WormholeCoverageMask, PreparedToMainPreExposureScale, Inputs.OverrideOutput);
			if (!CompositeOutput.IsValid())
			{
				WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeBeforeDOFComposite] Composite pass failed. ViewKey=%llu RenderFrame=%u PreparedPreExposure=%.6f MainPreExposure=%.6f PreExposureScale=%.6f CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, ViewState->PreparedPreExposure, ViewState->PreviousPreExposure, PreparedToMainPreExposureScale, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return CleanMainSceneColor;
			}
			return CompositeOutput;
		}

		FScreenPassTexture MaskSeparateTranslucency_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
		{
			const double StartSeconds = FPlatformTime::Seconds();
			const uint64 ViewKey = MakeWPWormholeLayerViewKey(View);
			const FScreenPassTexture SeparateTranslucency = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SeparateTranslucency));
			if (!SeparateTranslucency.IsValid())
			{
				WP_LOG(nullptr, Warning, TEXT("[RenderThread][WPWormholeSeparateTranslucencyMask] Separate translucency is invalid. ViewKey=%llu RenderFrame=%u CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return SeparateTranslucency;
			}

			const TSharedPtr<FWPPreparedViewState, ESPMode::ThreadSafe> ViewState = PreparedState->FindView_RenderThread(ViewKey);
			if (ViewState.IsValid() && ViewState->CleanBeforeDOFSceneColorRDG && ViewState->CleanBeforeDOFRDGFrameNumber == GFrameNumberRenderThread)
			{
				const FScreenPassTexture CleanBeforeDOFSceneColor(ViewState->CleanBeforeDOFSceneColorRDG, ViewState->CleanBeforeDOFSceneColorViewRect);
				const FScreenPassTexture CleanWithSeparate = AddWPCleanSceneColorWithSeparateTranslucencyPass(GraphBuilder, View, CleanBeforeDOFSceneColor, SeparateTranslucency);
				if (CleanWithSeparate.IsValid())
				{
					ViewState->CleanSceneColorWithSeparateViewRect = CleanWithSeparate.ViewRect;
					ViewState->CleanWithSeparateHistoryFrameNumber = GFrameNumberRenderThread;
					GraphBuilder.QueueTextureExtraction(CleanWithSeparate.Texture, &ViewState->CleanSceneColorWithSeparateHistory);
				}
				ViewState->CleanBeforeDOFSceneColorRDG = nullptr;
				ViewState->CleanBeforeDOFRDGFrameNumber = MAX_uint32;
			}
			if (!ViewState.IsValid() || ViewState->PreparedRDGFrameNumber != GFrameNumberRenderThread || !ViewState->bProductionBypassClaimed || !ViewState->WormholeCoverageMaskRDG)
			{
				return SeparateTranslucency;
			}

			const FScreenPassTexture WormholeCoverageMask(ViewState->WormholeCoverageMaskRDG, ViewState->WormholeCoverageMaskViewRect);
			const FScreenPassTexture Output = AddWPWormholeSeparateTranslucencyMaskPass(GraphBuilder, View, SeparateTranslucency, WormholeCoverageMask, Inputs.OverrideOutput);
			if (!Output.IsValid())
			{
				WP_LOG(nullptr, Error, TEXT("[RenderThread][WPWormholeSeparateTranslucencyMask] Mask pass failed. ViewKey=%llu RenderFrame=%u SeparateWidth=%d SeparateHeight=%d MaskWidth=%d MaskHeight=%d CpuMs=%.4f"), ViewKey, GFrameNumberRenderThread, SeparateTranslucency.ViewRect.Width(), SeparateTranslucency.ViewRect.Height(), WormholeCoverageMask.ViewRect.Width(), WormholeCoverageMask.ViewRect.Height(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return SeparateTranslucency;
			}

			ViewState->PreparedWormholeLayerRDG = nullptr;
			ViewState->WormholeCoverageMaskRDG = nullptr;
			ViewState->WormholeVelocityOverrideRDG = nullptr;
			return Output;
		}

		void QueuePreparedStateReset_GameThread()
		{
			check(IsInGameThread());
			const TSharedRef<FWPPreparedLayerSharedState, ESPMode::ThreadSafe> State = PreparedState;
			ENQUEUE_RENDER_COMMAND(WPResetWormholePreparedLayerState)([State](FRHICommandListImmediate& RHICmdList)
			{
				(void)RHICmdList;
				State->Reset_RenderThread();
			});
		}

		void CacheSkyPrimitivesForActor(FWorldState& State, AActor* Actor)
		{
			if (!IsValid(Actor) || Actor->GetWorld() != State.World.Get()) return;
			TArray<UMeshComponent*> MeshComponents;
			Actor->GetComponents(MeshComponents);
			for (UMeshComponent* MeshComponent : MeshComponents)
			{
				if (!IsValid(MeshComponent) || !MeshComponent->IsRegistered()) continue;
				bool bSky = false;
				for (UMaterialInterface* MaterialInterface : MeshComponent->GetMaterials())
				{
					const UMaterial* Material = IsValid(MaterialInterface) ? MaterialInterface->GetMaterial() : nullptr;
					if (Material && Material->bIsSky)
					{
						bSky = true;
						break;
					}
				}
				const FPrimitiveComponentId PrimitiveId = MeshComponent->GetPrimitiveSceneId();
				if (bSky && PrimitiveId.IsValid()) State.HiddenSkyPrimitiveIds.Add(PrimitiveId);
			}
		}

		void InitializeSkyCache(UWorld* World, FWorldState& State)
		{
			if (!IsValid(World) || State.ActorSpawnedHandle.IsValid()) return;
			for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt) CacheSkyPrimitivesForActor(State, *ActorIt);
			State.ActorSpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateRaw(this, &FWPWormholeLayerSceneViewExtension::HandleActorSpawned));
		}

		void HandleActorSpawned(AActor* Actor)
		{
			check(IsInGameThread());
			if (!IsValid(Actor)) return;
			if (FWorldState* State = WorldStates.Find(TWeakObjectPtr<UWorld>(Actor->GetWorld()))) CacheSkyPrimitivesForActor(*State, Actor);
		}

		void EnsureEndpointProxy(const FGuid& PairId, const TCHAR* Endpoint, AWormholePortalActor* Portal, TWeakObjectPtr<UWPWormholeDepthProxyComponent>& InOutProxy, float& InOutRadiusCm)
		{
			const double StartSeconds = FPlatformTime::Seconds();
			if (!IsValid(Portal) || !Portal->GetRootComponent()) return;
			const double PortalRadiusCm = static_cast<double>(Portal->GetPortalRadius());
			const double OuterRadiusCm = static_cast<double>(Portal->GetTransitionRadius());
			const double VisualScale = static_cast<double>(Portal->GetPortalVisualScale());
			const double SafeProxyRadiusCm = WPPortalVisibilityMath::GetSafeProxyRadiusCm(PortalRadiusCm, OuterRadiusCm) * VisualScale;
			if (!FMath::IsFinite(SafeProxyRadiusCm) || SafeProxyRadiusCm <= static_cast<double>(KINDA_SMALL_NUMBER))
			{
				DestroyProxy(InOutProxy);
				InOutRadiusCm = 0.0f;
				WP_LOG(nullptr, Warning, TEXT("[GameThread][WPWormholeDepthProxy][InvalidRadius] World=%s PairId=%s Endpoint=%s Portal=%s PortalRadiusCm=%.3f OuterRadiusCm=%.3f VisualScale=%.6f SafeProxyRadiusCm=%.3f CpuMs=%.4f"), *GetNameSafe(Portal->GetWorld()), *PairId.ToString(), Endpoint, *GetNameSafe(Portal), PortalRadiusCm, OuterRadiusCm, VisualScale, SafeProxyRadiusCm, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				return;
			}

			UWPWormholeDepthProxyComponent* Proxy = InOutProxy.Get();
			if (!IsValid(Proxy) || Proxy->GetOwner() != Portal)
			{
				DestroyProxy(InOutProxy);
				const FName ProxyName = MakeUniqueObjectName(Portal, UWPWormholeDepthProxyComponent::StaticClass(), FName(*FString::Printf(TEXT("WPWormholeDepthProxy_%s_%s"), Endpoint, *PairId.ToString(EGuidFormats::Digits))));
				Proxy = NewObject<UWPWormholeDepthProxyComponent>(Portal, UWPWormholeDepthProxyComponent::StaticClass(), ProxyName, RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
				if (!Proxy) return;
				Portal->AddInstanceComponent(Proxy);
				Proxy->OnComponentCreated();
				Proxy->ComponentTags.AddUnique(WPWormholeDepthProxyComponentTag);
				Proxy->SetupAttachment(Portal->GetRootComponent());
				Proxy->SetAbsolute(false, false, true);
				Proxy->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
				Proxy->SetProxyRadiusCm(SafeProxyRadiusCm);
				Proxy->SetVisibility(true, true);
				Proxy->SetHiddenInGame(false, true);
				Proxy->RegisterComponent();
				InOutProxy = Proxy;
				InOutRadiusCm = static_cast<float>(SafeProxyRadiusCm);
				return;
			}

			if (!FMath::IsNearlyEqual(static_cast<double>(InOutRadiusCm), SafeProxyRadiusCm, 0.01))
			{
				Proxy->SetProxyRadiusCm(SafeProxyRadiusCm);
				InOutRadiusCm = static_cast<float>(SafeProxyRadiusCm);
			}
		}

		void ReconcileWorld(UWorld* World, const uint32 FrameNumber, TArray<FPrimitiveComponentId>& OutPrimitiveIds)
		{
			OutPrimitiveIds.Reset();
			if (!IsValid(World)) return;

			const TWeakObjectPtr<UWorld> WorldKey(World);
			FWorldState& State = WorldStates.FindOrAdd(WorldKey);
			State.World = World;
			InitializeSkyCache(World, State);
			if (State.LastReconcileFrame != FrameNumber)
			{
				State.LastReconcileFrame = FrameNumber;
				TArray<FWPPortalPairSnapshot> PairSnapshots;
				if (const UWPRegistrySubsystem* Registry = World->GetSubsystem<UWPRegistrySubsystem>()) Registry->GetRegisteredPortalPairs(PairSnapshots);
				TSet<FGuid> ActivePairIds;
				ActivePairIds.Reserve(PairSnapshots.Num());
				for (const FWPPortalPairSnapshot& PairSnapshot : PairSnapshots)
				{
					if (!PairSnapshot.IsStructurallyValid()) continue;
					AWormholePortalActor* PortalA = PairSnapshot.PortalA.Get();
					AWormholePortalActor* PortalB = PairSnapshot.PortalB.Get();
					if (!IsValid(PortalA) || !IsValid(PortalB) || PortalA->GetWorld() != World || PortalB->GetWorld() != World) continue;
					ActivePairIds.Add(PairSnapshot.PairId);
					FPairState& PairState = State.Pairs.FindOrAdd(PairSnapshot.PairId);
					if (PairState.PortalA.Get() != PortalA || PairState.PortalB.Get() != PortalB)
					{
						const bool bHadExistingPairState = PairState.PortalA.IsValid() || PairState.PortalB.IsValid() || PairState.ProxyA.IsValid() || PairState.ProxyB.IsValid();
						if (bHadExistingPairState) DestroyPairState(PairState);
						PairState = FPairState{};
						PairState.PortalA = PortalA;
						PairState.PortalB = PortalB;
					}
					EnsureEndpointProxy(PairSnapshot.PairId, TEXT("A"), PortalA, PairState.ProxyA, PairState.RadiusACm);
					EnsureEndpointProxy(PairSnapshot.PairId, TEXT("B"), PortalB, PairState.ProxyB, PairState.RadiusBCm);
				}

				TArray<FGuid> RemovedPairIds;
				for (const TPair<FGuid, FPairState>& PairEntry : State.Pairs) if (!ActivePairIds.Contains(PairEntry.Key)) RemovedPairIds.Add(PairEntry.Key);
				for (const FGuid& PairId : RemovedPairIds)
				{
					if (FPairState* PairState = State.Pairs.Find(PairId)) DestroyPairState(*PairState);
					State.Pairs.Remove(PairId);
				}
			}

			OutPrimitiveIds.Reserve(State.Pairs.Num() * 2);
			for (const TPair<FGuid, FPairState>& PairEntry : State.Pairs)
			{
				for (const TWeakObjectPtr<UWPWormholeDepthProxyComponent>& WeakProxy : {PairEntry.Value.ProxyA, PairEntry.Value.ProxyB})
				{
					const UWPWormholeDepthProxyComponent* Proxy = WeakProxy.Get();
					if (!IsValid(Proxy) || !Proxy->IsRegistered() || !Proxy->GetPrimitiveSceneId().IsValid()) continue;
					OutPrimitiveIds.AddUnique(Proxy->GetPrimitiveSceneId());
				}
			}
		}

		void DestroyProxy(TWeakObjectPtr<UWPWormholeDepthProxyComponent>& InOutProxy)
		{
			if (UWPWormholeDepthProxyComponent* Proxy = InOutProxy.Get()) Proxy->DestroyComponent();
			InOutProxy.Reset();
		}

		void DestroyPairState(FPairState& State)
		{
			DestroyProxy(State.ProxyA);
			DestroyProxy(State.ProxyB);
			State.PortalA.Reset();
			State.PortalB.Reset();
			State.RadiusACm = 0.0f;
			State.RadiusBCm = 0.0f;
		}

		void DestroyWorldState(FWorldState& State)
		{
			if (UWorld* World = State.World.Get(); World && State.ActorSpawnedHandle.IsValid()) World->RemoveOnActorSpawnedHandler(State.ActorSpawnedHandle);
			State.ActorSpawnedHandle.Reset();
			for (TPair<FGuid, FPairState>& PairEntry : State.Pairs) DestroyPairState(PairEntry.Value);
			State.Pairs.Reset();
			State.HiddenSkyPrimitiveIds.Reset();
			State.World.Reset();
		}

		void DestroyAllWorldStates()
		{
			for (TPair<TWeakObjectPtr<UWorld>, FWorldState>& WorldEntry : WorldStates) DestroyWorldState(WorldEntry.Value);
			WorldStates.Reset();
		}

		void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
		{
			check(IsInGameThread());
			(void)bSessionEnded;
			(void)bCleanupResources;
			const TWeakObjectPtr<UWorld> WorldKey(World);
			if (FWorldState* State = WorldStates.Find(WorldKey))
			{
				DestroyWorldState(*State);
				WorldStates.Remove(WorldKey);
			}
			QueuePreparedStateReset_GameThread();
		}

	private:
		FWPRendererService& RendererService;
		TSharedRef<FWPPreparedLayerSharedState, ESPMode::ThreadSafe> PreparedState;
		TMap<TWeakObjectPtr<UWorld>, FWorldState> WorldStates;
		FDelegateHandle WorldCleanupHandle;
		bool bLastEnabled = false;
		bool bShutdown = false;
	};

	TWeakPtr<FWPWormholeLayerSceneViewExtension, ESPMode::ThreadSafe> GWPWormholeLayerViewExtension;
}

TSharedRef<FSceneViewExtensionBase, ESPMode::ThreadSafe> CreateWPWormholeLayerRenderPassViewExtension(FWPRendererService& RendererService)
{
	const TSharedRef<FWPWormholeLayerSceneViewExtension, ESPMode::ThreadSafe> Extension = FSceneViewExtensions::NewExtension<FWPWormholeLayerSceneViewExtension>(RendererService);
	GWPWormholeLayerViewExtension = Extension;
	return Extension;
}

void ShutdownWPWormholeLayerRenderPassViewExtension_GameThread()
{
	check(IsInGameThread());
	if (const TSharedPtr<FWPWormholeLayerSceneViewExtension, ESPMode::ThreadSafe> Extension = GWPWormholeLayerViewExtension.Pin()) Extension->Shutdown_GameThread();
	GWPWormholeLayerViewExtension.Reset();
}

bool TryClaimWPWormholeLayerOwnership_RenderThread(const FSceneView& View)
{
	check(IsInRenderingThread());
	const TSharedPtr<FWPWormholeLayerSceneViewExtension, ESPMode::ThreadSafe> Extension = GWPWormholeLayerViewExtension.Pin();
	if (Extension.IsValid()) return Extension->TryClaimWormholeLayerOwnership_RenderThread(View);
	return CVarWPCompositePath.GetValueOnRenderThread() == 1;
}
