// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Passes/WPCubeAAPass.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"

DECLARE_GPU_STAT_NAMED(WPCubeAA, TEXT("WP.CubeAA"));
DECLARE_GPU_STAT_NAMED(WPCubeAAFilter, TEXT("WP.CubeAA.Filter"));
DECLARE_GPU_STAT_NAMED(WPCubeAACopyBack, TEXT("WP.CubeAA.CopyBack"));

namespace
{
	constexpr int32 WPCubeAAThreadGroupSize = 8;
	constexpr int32 WPCubeFaceCount = 6;
}

bool FWPCubeAACS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(
		Parameters.Platform,
		ERHIFeatureLevel::SM5);
}

IMPLEMENT_GLOBAL_SHADER(
	FWPCubeAACS,
	"/Plugin/WormholePortal/WPCubeAA.usf",
	"MainCS",
	SF_Compute);

bool AddWPCubeAAPass(
	FRDGBuilder& GraphBuilder,
	FRHITexture* InputCubeTexture,
	FRHITexture* OutputCubeTexture,
	const bool bDirectPublish,
	const uint8 FaceMask)
{
	const uint8 SanitizedFaceMask = FaceMask & 0x3f;
	if (!InputCubeTexture || !OutputCubeTexture
		|| SanitizedFaceMask == 0 || (bDirectPublish && InputCubeTexture == OutputCubeTexture))
	{
		return false;
	}

	const FRHITextureDesc& InputDesc = InputCubeTexture->GetDesc();
	const FRHITextureDesc& OutputDesc = OutputCubeTexture->GetDesc();
	const bool bInputValid =
		InputDesc.Dimension == ETextureDimension::TextureCube
		&& InputDesc.Extent.X > 1
		&& InputDesc.Extent.X == InputDesc.Extent.Y
		&& InputDesc.ArraySize == 1
		&& InputDesc.NumMips == 1
		&& InputDesc.NumSamples == 1
		&& InputDesc.Format == PF_FloatRGBA;
	const bool bOutputValid =
		OutputDesc.Dimension == ETextureDimension::TextureCube
		&& OutputDesc.Extent == InputDesc.Extent
		&& OutputDesc.ArraySize == InputDesc.ArraySize
		&& OutputDesc.NumMips == InputDesc.NumMips
		&& OutputDesc.NumSamples == InputDesc.NumSamples
		&& OutputDesc.Format == InputDesc.Format
		&& (!bDirectPublish
			|| EnumHasAnyFlags(OutputDesc.Flags, ETextureCreateFlags::UAV));
	if (!bInputValid || !bOutputValid)
	{
		return false;
	}

	FGlobalShaderMap* ShaderMap =
		GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!ShaderMap
		|| !ShaderMap->HasShader(
			&FWPCubeAACS::GetStaticType(),
			0))
	{
		return false;
	}

	FRDGTextureRef InputCube = RegisterExternalTexture(
		GraphBuilder,
		InputCubeTexture,
		TEXT("WP.CubeAA.Input"));
	if (!InputCube)
	{
		return false;
	}

	FRDGTextureRef OutputCube = InputCubeTexture == OutputCubeTexture
		? InputCube
		: RegisterExternalTexture(
			GraphBuilder,
			OutputCubeTexture,
			TEXT("WP.CubeAA.Published"));
	if (!OutputCube)
	{
		return false;
	}

	FRDGTextureRef FilterOutputCube = OutputCube;
	if (!bDirectPublish)
	{
		FRDGTextureDesc FilteredDesc = FRDGTextureDesc::CreateCube(
			InputDesc.Extent.X,
			InputDesc.Format,
			FClearValueBinding::None,
			ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::UAV);
		FilterOutputCube = GraphBuilder.CreateTexture(
			FilteredDesc,
			TEXT("WP.CubeAA.LegacyFiltered"));
	}

	FRDGTextureSRVDesc InputSRVDesc(InputCube);
	InputSRVDesc.MipLevel = 0;
	InputSRVDesc.NumMipLevels = 1;
	InputSRVDesc.FirstArraySlice = 0;
	InputSRVDesc.NumArraySlices = WPCubeFaceCount;
	InputSRVDesc.DimensionOverride =
		ETextureDimension::Texture2DArray;

	FWPCubeAACS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FWPCubeAACS::FParameters>();
	PassParameters->CubeExtent = InputDesc.Extent;
	PassParameters->CubeInvExtent = FVector2f(
		1.0f / static_cast<float>(InputDesc.Extent.X),
		1.0f / static_cast<float>(InputDesc.Extent.Y));
	PassParameters->EdgeThreshold = 0.125f;
	PassParameters->EdgeThresholdMin = 0.0312f;
	PassParameters->SpanMax = 8.0f;
	PassParameters->ReduceMul = 0.125f;
	PassParameters->ReduceMin = 1.0f / 128.0f;
	PassParameters->FaceMask = SanitizedFaceMask;
	PassParameters->InputCubeFaces =
		GraphBuilder.CreateSRV(InputSRVDesc);
	PassParameters->InputCubeSampler =
		TStaticSamplerState<
			SF_Bilinear,
			AM_Clamp,
			AM_Clamp,
			AM_Clamp>::GetRHI();
	PassParameters->OutputCubeFaces =
		GraphBuilder.CreateUAV(
			FRDGTextureUAVDesc(
				FilterOutputCube,
				0,
				PF_Unknown));

	TShaderMapRef<FWPCubeAACS> ComputeShader(ShaderMap);
	const FIntVector GroupCount(
		FMath::DivideAndRoundUp(
			InputDesc.Extent.X,
			WPCubeAAThreadGroupSize),
		FMath::DivideAndRoundUp(
			InputDesc.Extent.Y,
			WPCubeAAThreadGroupSize),
		WPCubeFaceCount);

	RDG_EVENT_SCOPE_STAT(
		GraphBuilder,
		WPCubeAA,
		"WP.CubeAA %dx%d FaceMask=0x%02x HDR=Linear Source=RawScratch Output=%s CopyBack=%s",
		InputDesc.Extent.X,
		InputDesc.Extent.Y,
		static_cast<uint32>(SanitizedFaceMask),
		bDirectPublish ? "DirectPublished" : "TransientFiltered",
		bDirectPublish ? "Eliminated" : "ValidationFallback");
	{
		RDG_EVENT_SCOPE_STAT(
			GraphBuilder,
			WPCubeAAFilter,
			"WP.CubeAA.Filter.%s",
			bDirectPublish ? "DirectPublish" : "LegacyCopyValidation");
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME(
				"WP.CubeAA.Filter.%s",
				bDirectPublish ? "DirectPublish" : "LegacyCopyValidation"),
			ComputeShader,
			PassParameters,
			GroupCount);
	}

	if (!bDirectPublish)
	{
		{
			RDG_EVENT_SCOPE_STAT(
				GraphBuilder,
				WPCubeAACopyBack,
				"WP.CubeAA.CopyBack.LegacyCopyValidation");
			FRHICopyTextureInfo CopyInfo;
			CopyInfo.NumSlices = WPCubeFaceCount;
			CopyInfo.NumMips = 1;
			AddCopyTexturePass(
				GraphBuilder,
				FilterOutputCube,
				OutputCube,
				CopyInfo);
		}
	}

	GraphBuilder.SetTextureAccessFinal(
		InputCube,
		ERHIAccess::SRVMask);
	GraphBuilder.SetTextureAccessFinal(
		OutputCube,
		ERHIAccess::SRVMask);
	return true;
}
