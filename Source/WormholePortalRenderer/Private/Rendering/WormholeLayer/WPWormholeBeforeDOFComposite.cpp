// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/WormholeLayer/WPWormholeBeforeDOFComposite.h"

#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

DECLARE_GPU_STAT_NAMED(WPWormholeBeforeDOFComposite, TEXT("WP.WormholeBeforeDOFComposite"));
DECLARE_GPU_STAT_NAMED(WPWormholeVelocityPatch, TEXT("WP.WormholeVelocityPatch"));

namespace
{
	class FWPWormholeBeforeDOFCompositePS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FWPWormholeBeforeDOFCompositePS);
		SHADER_USE_PARAMETER_STRUCT(FWPWormholeBeforeDOFCompositePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, CleanMain)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Prepared)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Coverage)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CleanMainTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, CleanMainSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PreparedTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, PreparedSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, CoverageSampler)
			SHADER_PARAMETER(float, PreparedToMainPreExposureScale)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FWPWormholeVelocityPatchPS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FWPWormholeVelocityPatchPS);
		SHADER_USE_PARAMETER_STRUCT(FWPWormholeVelocityPatchPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Coverage)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, VelocityOverride)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, CoverageSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityOverrideTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, VelocityOverrideSampler)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FWPWormholeBeforeDOFCompositePS, "/Plugin/WormholePortal/WPWormholeBeforeDOFComposite.usf", "MainPS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FWPWormholeVelocityPatchPS, "/Plugin/WormholePortal/WPWormholeBeforeDOFComposite.usf", "VelocityPatchPS", SF_Pixel);
}

FScreenPassTexture AddWPWormholeBeforeDOFCompositePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& CleanMainSceneColor,
	const FScreenPassTexture& PreparedWormholeLayer,
	const FScreenPassTexture& WormholeCoverageMask,
	const float PreparedToMainPreExposureScale,
	const FScreenPassRenderTarget& OverrideOutput)
{
	if (!CleanMainSceneColor.IsValid() || !PreparedWormholeLayer.IsValid() || !WormholeCoverageMask.IsValid() || !FMath::IsFinite(PreparedToMainPreExposureScale) || PreparedToMainPreExposureScale <= 0.0f) return FScreenPassTexture();
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	if (!GlobalShaderMap || !GlobalShaderMap->HasShader(&FWPWormholeBeforeDOFCompositePS::GetStaticType(), 0)) return FScreenPassTexture();
	const TShaderMapRef<FWPWormholeBeforeDOFCompositePS> PixelShader(GlobalShaderMap);
	if (!PixelShader.IsValid()) return FScreenPassTexture();

	FScreenPassRenderTarget Output = OverrideOutput;
	if (!Output.IsValid()) Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, CleanMainSceneColor, View.GetOverwriteLoadAction(), TEXT("WP.WormholeBeforeDOFComposite.Output"));
	if (!Output.IsValid()) return FScreenPassTexture();

	const FScreenPassTextureViewport CleanViewport(CleanMainSceneColor);
	const FScreenPassTextureViewport PreparedViewport(PreparedWormholeLayer);
	const FScreenPassTextureViewport CoverageViewport(WormholeCoverageMask);
	const FScreenPassTextureViewport OutputViewport(Output);
	FWPWormholeBeforeDOFCompositePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWPWormholeBeforeDOFCompositePS::FParameters>();
	PassParameters->CleanMain = GetScreenPassTextureViewportParameters(CleanViewport);
	PassParameters->Prepared = GetScreenPassTextureViewportParameters(PreparedViewport);
	PassParameters->Coverage = GetScreenPassTextureViewportParameters(CoverageViewport);
	PassParameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->CleanMainTexture = CleanMainSceneColor.Texture;
	PassParameters->CleanMainSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PreparedTexture = PreparedWormholeLayer.Texture;
	PassParameters->PreparedSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->CoverageTexture = WormholeCoverageMask.Texture;
	PassParameters->CoverageSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PreparedToMainPreExposureScale = PreparedToMainPreExposureScale;
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	ClearUnusedGraphResources(PixelShader, PassParameters);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, WPWormholeBeforeDOFComposite, "WP.WormholeBeforeDOFComposite Clean=%dx%d Prepared=%dx%d Mask=%dx%d Output=%dx%d PreExposureScale=%.5f", CleanViewport.Rect.Width(), CleanViewport.Rect.Height(), PreparedViewport.Rect.Width(), PreparedViewport.Rect.Height(), CoverageViewport.Rect.Width(), CoverageViewport.Rect.Height(), OutputViewport.Rect.Width(), OutputViewport.Rect.Height(), PreparedToMainPreExposureScale);
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, RDG_EVENT_NAME("WP.WormholeBeforeDOFComposite"), PixelShader, PassParameters, Output.ViewRect);
	return MoveTemp(Output);
}

bool AddWPWormholeVelocityPatchPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& WormholeVelocityOverride,
	const FScreenPassTexture& WormholeCoverageMask,
	FRDGTextureRef SceneVelocityTexture,
	const FIntRect& SceneVelocityViewRect)
{
	if (!WormholeVelocityOverride.IsValid() || !WormholeCoverageMask.IsValid() || !SceneVelocityTexture || SceneVelocityTexture->Desc.Dimension != ETextureDimension::Texture2D || SceneVelocityTexture->Desc.NumSamples != 1 || !EnumHasAnyFlags(SceneVelocityTexture->Desc.Flags, ETextureCreateFlags::RenderTargetable)) return false;
	if (WormholeVelocityOverride.Texture->Desc.Format != SceneVelocityTexture->Desc.Format || SceneVelocityViewRect.Min.X < 0 || SceneVelocityViewRect.Min.Y < 0 || SceneVelocityViewRect.Max.X > SceneVelocityTexture->Desc.Extent.X || SceneVelocityViewRect.Max.Y > SceneVelocityTexture->Desc.Extent.Y) return false;

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	if (!GlobalShaderMap || !GlobalShaderMap->HasShader(&FWPWormholeVelocityPatchPS::GetStaticType(), 0)) return false;
	const TShaderMapRef<FWPWormholeVelocityPatchPS> PixelShader(GlobalShaderMap);
	if (!PixelShader.IsValid()) return false;

	const FScreenPassTextureViewport CoverageViewport(WormholeCoverageMask);
	const FScreenPassTextureViewport VelocityViewport(WormholeVelocityOverride);
	const FScreenPassRenderTarget Output(SceneVelocityTexture, SceneVelocityViewRect, ERenderTargetLoadAction::ELoad);
	const FScreenPassTextureViewport OutputViewport(Output);
	FWPWormholeVelocityPatchPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWPWormholeVelocityPatchPS::FParameters>();
	PassParameters->Coverage = GetScreenPassTextureViewportParameters(CoverageViewport);
	PassParameters->VelocityOverride = GetScreenPassTextureViewportParameters(VelocityViewport);
	PassParameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->CoverageTexture = WormholeCoverageMask.Texture;
	PassParameters->CoverageSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->VelocityOverrideTexture = WormholeVelocityOverride.Texture;
	PassParameters->VelocityOverrideSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	ClearUnusedGraphResources(PixelShader, PassParameters);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, WPWormholeVelocityPatch, "WP.WormholeVelocityPatch Velocity=%dx%d Mask=%dx%d Output=%dx%d", VelocityViewport.Rect.Width(), VelocityViewport.Rect.Height(), CoverageViewport.Rect.Width(), CoverageViewport.Rect.Height(), OutputViewport.Rect.Width(), OutputViewport.Rect.Height());
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, RDG_EVENT_NAME("WP.WormholeVelocityPatch"), PixelShader, PassParameters, Output.ViewRect);
	return true;
}
