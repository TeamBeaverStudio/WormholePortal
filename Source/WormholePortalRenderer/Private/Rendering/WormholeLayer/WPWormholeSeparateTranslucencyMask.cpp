// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/WormholeLayer/WPWormholeSeparateTranslucencyMask.h"

#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

namespace
{
	DECLARE_GPU_STAT_NAMED(WPWormholeSeparateTranslucencyMask, TEXT("WP.WormholeSeparateTranslucencyMask"));
	DECLARE_GPU_STAT_NAMED(WPCleanSceneColorWithSeparateTranslucency, TEXT("WP.CleanSceneColorWithSeparateTranslucency"));

	class FWPWormholeSeparateTranslucencyMaskPS final : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FWPWormholeSeparateTranslucencyMaskPS);
		SHADER_USE_PARAMETER_STRUCT(FWPWormholeSeparateTranslucencyMaskPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Separate)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, SeparateSampler)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Coverage)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, CoverageSampler)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FWPCleanSceneColorWithSeparateTranslucencyPS final : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FWPCleanSceneColorWithSeparateTranslucencyPS);
		SHADER_USE_PARAMETER_STRUCT(FWPCleanSceneColorWithSeparateTranslucencyPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Clean)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CleanTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, CleanSampler)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Separate)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, SeparateSampler)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FWPWormholeSeparateTranslucencyMaskPS, "/Plugin/WormholePortal/WPWormholeSeparateTranslucencyMask.usf", "MainPS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FWPCleanSceneColorWithSeparateTranslucencyPS, "/Plugin/WormholePortal/WPWormholeSeparateTranslucencyMask.usf", "ComposeCleanHistoryPS", SF_Pixel);
}

FScreenPassTexture AddWPWormholeSeparateTranslucencyMaskPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& SeparateTranslucency,
	const FScreenPassTexture& WormholeCoverageMask,
	const FScreenPassRenderTarget& OverrideOutput)
{
	if (!SeparateTranslucency.IsValid() || !WormholeCoverageMask.IsValid()) return FScreenPassTexture();
	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	if (!GlobalShaderMap || !GlobalShaderMap->HasShader(&FWPWormholeSeparateTranslucencyMaskPS::GetStaticType(), 0)) return FScreenPassTexture();
	const TShaderMapRef<FWPWormholeSeparateTranslucencyMaskPS> PixelShader(GlobalShaderMap);
	FScreenPassRenderTarget Output = OverrideOutput;
	if (!Output.IsValid()) Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SeparateTranslucency, View.GetOverwriteLoadAction(), TEXT("WP.WormholeSeparateTranslucencyMask.Output"));
	if (!Output.IsValid()) return FScreenPassTexture();

	const FScreenPassTextureViewport SeparateViewport(SeparateTranslucency);
	const FScreenPassTextureViewport CoverageViewport(WormholeCoverageMask);
	const FScreenPassTextureViewport OutputViewport(Output);
	FWPWormholeSeparateTranslucencyMaskPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWPWormholeSeparateTranslucencyMaskPS::FParameters>();
	PassParameters->Separate = GetScreenPassTextureViewportParameters(SeparateViewport);
	PassParameters->SeparateTexture = SeparateTranslucency.Texture;
	PassParameters->SeparateSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->Coverage = GetScreenPassTextureViewportParameters(CoverageViewport);
	PassParameters->CoverageTexture = WormholeCoverageMask.Texture;
	PassParameters->CoverageSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	RDG_EVENT_SCOPE_STAT(GraphBuilder, WPWormholeSeparateTranslucencyMask, "WP.WormholeSeparateTranslucencyMask Separate=%dx%d Mask=%dx%d", SeparateViewport.Rect.Width(), SeparateViewport.Rect.Height(), CoverageViewport.Rect.Width(), CoverageViewport.Rect.Height());
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, RDG_EVENT_NAME("WP.WormholeSeparateTranslucencyMask"), PixelShader, PassParameters, Output.ViewRect);
	return Output;
}

FScreenPassTexture AddWPCleanSceneColorWithSeparateTranslucencyPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& CleanSceneColor,
	const FScreenPassTexture& SeparateTranslucency)
{
	if (!CleanSceneColor.IsValid() || !SeparateTranslucency.IsValid()) return FScreenPassTexture();
	const FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	if (!GlobalShaderMap || !GlobalShaderMap->HasShader(&FWPCleanSceneColorWithSeparateTranslucencyPS::GetStaticType(), 0)) return FScreenPassTexture();
	const TShaderMapRef<FWPCleanSceneColorWithSeparateTranslucencyPS> PixelShader(GlobalShaderMap);
	const FScreenPassRenderTarget Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, CleanSceneColor, View.GetOverwriteLoadAction(), TEXT("WP.CleanSceneColorWithSeparateTranslucency.Output"));
	if (!Output.IsValid()) return FScreenPassTexture();

	const FScreenPassTextureViewport CleanViewport(CleanSceneColor);
	const FScreenPassTextureViewport SeparateViewport(SeparateTranslucency);
	const FScreenPassTextureViewport OutputViewport(Output);
	FWPCleanSceneColorWithSeparateTranslucencyPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWPCleanSceneColorWithSeparateTranslucencyPS::FParameters>();
	PassParameters->Clean = GetScreenPassTextureViewportParameters(CleanViewport);
	PassParameters->CleanTexture = CleanSceneColor.Texture;
	PassParameters->CleanSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->Separate = GetScreenPassTextureViewportParameters(SeparateViewport);
	PassParameters->SeparateTexture = SeparateTranslucency.Texture;
	PassParameters->SeparateSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	RDG_EVENT_SCOPE_STAT(GraphBuilder, WPCleanSceneColorWithSeparateTranslucency, "WP.CleanSceneColorWithSeparateTranslucency Clean=%dx%d Separate=%dx%d", CleanViewport.Rect.Width(), CleanViewport.Rect.Height(), SeparateViewport.Rect.Width(), SeparateViewport.Rect.Height());
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, RDG_EVENT_NAME("WP.CleanSceneColorWithSeparateTranslucency"), PixelShader, PassParameters, Output.ViewRect);
	return Output;
}
