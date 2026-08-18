// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FRDGBuilder;
class FRHITexture;

class FWPCubeAACS final : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FWPCubeAACS);
	SHADER_USE_PARAMETER_STRUCT(FWPCubeAACS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, CubeExtent)
		SHADER_PARAMETER(FVector2f, CubeInvExtent)
		SHADER_PARAMETER(float, EdgeThreshold)
		SHADER_PARAMETER(float, EdgeThresholdMin)
		SHADER_PARAMETER(float, SpanMax)
		SHADER_PARAMETER(float, ReduceMul)
		SHADER_PARAMETER(float, ReduceMin)
		SHADER_PARAMETER(uint32, FaceMask)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, InputCubeFaces)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputCubeSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, OutputCubeFaces)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(
		const FGlobalShaderPermutationParameters& Parameters);
};

/**
 * Reads a captured cube and resolves linear-HDR spatial AA. Direct mode requires a distinct
 * physical output; validation mode accepts the same input/output and performs transient copy-back.
 */
bool AddWPCubeAAPass(
	FRDGBuilder& GraphBuilder,
	FRHITexture* InputCubeTexture,
	FRHITexture* OutputCubeTexture,
	bool bDirectPublish,
	uint8 FaceMask);
