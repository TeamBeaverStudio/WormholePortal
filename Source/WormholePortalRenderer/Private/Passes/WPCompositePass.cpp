// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Passes/WPCompositePass.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "HAL/IConsoleManager.h"
#include "GlobalShader.h"
#include "GlobalRenderResources.h"
#include "Math/WPPortalVisibilityMath.h"
#include "PixelShaderUtils.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphEvent.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "RHITextureReference.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"
#include "WPRenderState.h"
#include "WormholePortalStats.h"

DECLARE_GPU_STAT_NAMED(WPProductionComposite, TEXT("WP.ProductionComposite"));

namespace
{
	TAutoConsoleVariable<int32> CVarWPDNEGFilterDebug(
		TEXT("wp.DNEG.FilterDebug"),
		0,
		TEXT("Visualizes DNEG phase filtering and optical branch coverage.\n")
		TEXT(" 0: Off\n")
		TEXT(" 1: Yellow=4-tap, Orange=8-tap, Cyan=C2 branch, ")
		TEXT("Red=C2 critical"),
		ECVF_RenderThreadSafe);

	class FWPCompositePS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FWPCompositePS);
		SHADER_USE_PARAMETER_STRUCT(FWPCompositePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Input)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Base)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Foreground)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseSceneColorTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, BaseSceneColorSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ForegroundSceneColorTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, ForegroundSceneColorSampler)
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthSampler)
			SHADER_PARAMETER(uint32, SceneMode)
			SHADER_PARAMETER(FVector3f, CurrentPreViewTranslation)
			SHADER_PARAMETER(FMatrix44f, PreviousWorldToClip)
			SHADER_PARAMETER(FMatrix44f, PreviousScreenToWorld)
			SHADER_PARAMETER(float, PreviousSceneColorPreExposureScale)
			SHADER_PARAMETER(float, PreviousSceneColorValid)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, PreviousDepth)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PreviousSceneDepthTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, PreviousSceneDepthSampler)
			SHADER_PARAMETER_RDG_TEXTURE(Texture3D, RayLUTTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, RayLUTSampler)
			SHADER_PARAMETER(float, RayLUTZ)
			SHADER_PARAMETER(float, RayLUTEnabled)
			SHADER_PARAMETER_TEXTURE(TextureCube, LocalCubeTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, LocalCubeSampler)
			SHADER_PARAMETER_TEXTURE(TextureCube, LinkedCubeTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, LinkedCubeSampler)
			SHADER_PARAMETER(FVector3f, PortalCenterTranslated)
			SHADER_PARAMETER(float, PortalRadiusCm)
			SHADER_PARAMETER(float, ThroatLengthCm)
			SHADER_PARAMETER(float, ProxyRadiusCm)
			SHADER_PARAMETER(float, MetricOuterRadiusCm)
			SHADER_PARAMETER(float, DepthBiasCm)
			SHADER_PARAMETER(int32, FilterDebugMode)
			SHADER_PARAMETER(FVector3f, SelfX)
			SHADER_PARAMETER(FVector3f, SelfY)
			SHADER_PARAMETER(FVector3f, SelfZ)
			SHADER_PARAMETER(FVector3f, LinkedX)
			SHADER_PARAMETER(FVector3f, LinkedY)
			SHADER_PARAMETER(FVector3f, LinkedZ)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(
		FWPCompositePS,
		"/Plugin/WormholePortal/WPComposite.usf",
		"MainPS",
		SF_Pixel);

	bool IsFiniteCompositeVector(const FVector3f& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	bool IsValidCompositeBasisVector(const FVector3f& Value)
	{
		return IsFiniteCompositeVector(Value) && Value.SizeSquared() > UE_SMALL_NUMBER;
	}

	bool AreParametersValid(const FWPCompositePassParameters& Parameters)
	{
		return static_cast<uint8>(Parameters.SceneMode) <= static_cast<uint8>(EWPCompositeSceneMode::PreparedVelocityOverride)
			&& IsFiniteCompositeVector(Parameters.CurrentPreViewTranslation)
			&& FMath::IsFinite(Parameters.PreviousSceneColorPreExposureScale)
			&& Parameters.PreviousSceneColorPreExposureScale > 0.0f
			&& IsFiniteCompositeVector(Parameters.PortalCenterTranslated)
			&& FMath::IsFinite(Parameters.PortalRadiusCm)
			&& Parameters.PortalRadiusCm > KINDA_SMALL_NUMBER
			&& FMath::IsFinite(Parameters.ThroatLengthCm)
			&& Parameters.ThroatLengthCm >= 0.0f
			&& FMath::IsFinite(Parameters.ProxyRadiusCm)
			&& Parameters.ProxyRadiusCm >= Parameters.PortalRadiusCm
			&& FMath::IsFinite(Parameters.MetricOuterRadiusCm)
			&& Parameters.MetricOuterRadiusCm > KINDA_SMALL_NUMBER
			&& FMath::IsFinite(Parameters.DepthBiasCm)
			&& Parameters.DepthBiasCm >= 0.0f
			&& FMath::IsFinite(Parameters.RayLUTZ)
			&& Parameters.RayLUTZ >= 0.0f
			&& Parameters.RayLUTZ <= 1.0f
			&& IsValidCompositeBasisVector(Parameters.SelfX)
			&& IsValidCompositeBasisVector(Parameters.SelfY)
			&& IsValidCompositeBasisVector(Parameters.SelfZ)
			&& IsValidCompositeBasisVector(Parameters.LinkedX)
			&& IsValidCompositeBasisVector(Parameters.LinkedY)
			&& IsValidCompositeBasisVector(Parameters.LinkedZ);
	}

	bool IsValidCubeContract(
		const FIntPoint ExpectedExtent,
		const EPixelFormat ExpectedFormat,
		const uint32 ExpectedMipCount,
		const uint32 LayoutVersion,
		const uint32 ResourceGeneration,
		const bool bContractValid)
	{
		return bContractValid
			&& ExpectedExtent.X > 0
			&& ExpectedExtent.Y > 0
			&& ExpectedExtent.X == ExpectedExtent.Y
			&& ExpectedFormat != PF_Unknown
			&& ExpectedMipCount > 0
			&& LayoutVersion > 0
			&& ResourceGeneration > 0;
	}

	bool ValidateRayLUTParameters(
		const FWPCompositePassParameters& Parameters,
		EWPCompositeFailureReason& OutFailureReason)
	{
		if (Parameters.bAnalyticNoTransition)
		{
			// T==0 is the exact constant-radius solution. A missing texture/contract is
			// intentional and the pixel shader receives RayLUTEnabled=0.
			return true;
		}
		if (!Parameters.RayLUTTexture)
		{
			OutFailureReason = EWPCompositeFailureReason::LUTUnavailable;
			return false;
		}
		if (Parameters.ExpectedLUTExtent.X <= 0
			|| Parameters.ExpectedLUTExtent.Y <= 0
			|| Parameters.ExpectedLUTExtent.Z <= 0
			|| Parameters.ExpectedLUTFormat == PF_Unknown
			|| Parameters.ExpectedLUTMipCount == 0
			|| Parameters.ExpectedLUTDimension != EWPRayLUTDimension::Texture3D
			|| Parameters.LUTLayoutVersion == 0
			|| Parameters.LUTGeneration == 0
			|| Parameters.LUTRevision == 0)
		{
			OutFailureReason = EWPCompositeFailureReason::InvalidLUTContract;
			return false;
		}

		const FRHITextureDesc& LUTDesc = Parameters.RayLUTTexture->GetDesc();
		if (LUTDesc.Dimension != ETextureDimension::Texture3D
			|| LUTDesc.Depth == 0
			|| LUTDesc.NumSamples != 1)
		{
			OutFailureReason = EWPCompositeFailureReason::InvalidLUTDimension;
			return false;
		}
		const FIntVector ActualLUTExtent(
			LUTDesc.Extent.X,
			LUTDesc.Extent.Y,
			static_cast<int32>(LUTDesc.Depth));
		if (ActualLUTExtent != Parameters.ExpectedLUTExtent)
		{
			OutFailureReason = EWPCompositeFailureReason::InvalidLUTExtent;
			return false;
		}
		if (LUTDesc.NumMips != Parameters.ExpectedLUTMipCount)
		{
			OutFailureReason = EWPCompositeFailureReason::InvalidLUTMipCount;
			return false;
		}
		if (LUTDesc.Format != Parameters.ExpectedLUTFormat)
		{
			OutFailureReason = EWPCompositeFailureReason::InvalidLUTFormat;
			return false;
		}
		return true;
	}

	bool ValidateCubeParameters(
		FRHITexture* CubeTextureReference,
		const FIntPoint ExpectedExtent,
		const EPixelFormat ExpectedFormat,
		const uint32 ExpectedMipCount,
		const uint32 LayoutVersion,
		const uint32 ResourceGeneration,
		const bool bContractValid,
		const uint32 CaptureGeneration,
		const bool bLocalCube,
		EWPCompositeFailureReason& OutFailureReason)
	{
		if (!CubeTextureReference)
		{
			OutFailureReason = bLocalCube
				? EWPCompositeFailureReason::LocalCubeUnavailable
				: EWPCompositeFailureReason::LinkedCubeUnavailable;
			return false;
		}
		// The proxy itself must reach the RHI command. Calling GetReferencedTexture/GetDesc on
		// the Render Thread races D3D12's queued RHI-thread texture-reference switch.
		if (CubeTextureReference->GetTextureReference() != CubeTextureReference)
		{
			OutFailureReason = bLocalCube
				? EWPCompositeFailureReason::InvalidLocalCubeReferenceProxy
				: EWPCompositeFailureReason::InvalidLinkedCubeReferenceProxy;
			return false;
		}
		if (!IsValidCubeContract(
			ExpectedExtent,
			ExpectedFormat,
			ExpectedMipCount,
			LayoutVersion,
			ResourceGeneration,
			bContractValid))
		{
			OutFailureReason = bLocalCube
				? EWPCompositeFailureReason::InvalidLocalCubeContract
				: EWPCompositeFailureReason::InvalidLinkedCubeContract;
			return false;
		}

		if (CaptureGeneration == 0)
		{
			OutFailureReason = bLocalCube
				? EWPCompositeFailureReason::LocalCubeContentNotReady
				: EWPCompositeFailureReason::LinkedCubeContentNotReady;
			return false;
		}
		return true;
	}

	bool IsSupportedCompositeSceneDepthTexture(const FRDGTextureRef SceneDepthTexture)
	{
		return SceneDepthTexture
			&& (SceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2D
				|| SceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2DArray)
			&& (!SceneDepthTexture->Desc.IsTextureArray()
				|| SceneDepthTexture->Desc.ArraySize > 0)
			&& SceneDepthTexture->Desc.NumSamples == 1;
	}

	bool IsCompositeShaderAvailable(const FSceneView& View, FGlobalShaderMap*& OutGlobalShaderMap)
	{
		OutGlobalShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
		if (!OutGlobalShaderMap
			|| !OutGlobalShaderMap->HasShader(&FWPCompositePS::GetStaticType(), 0))
		{
			return false;
		}

		const TShaderMapRef<FWPCompositePS> PixelShader(OutGlobalShaderMap);
		return PixelShader.IsValid();
	}

	FRHITexture* ResolveWPTextureReference(const FTextureReferenceRHIRef& TextureReference)
	{
		if (!TextureReference.IsValid()) return nullptr;
		FRHITexture* ReferencedTexture = TextureReference->GetReferencedTexture();
		return ReferencedTexture != FRHITextureReference::GetDefaultTexture() ? ReferencedTexture : nullptr;
	}

	FRHITexture* GetWPTextureReferenceProxy(const FTextureReferenceRHIRef& TextureReference)
	{
		return TextureReference.IsValid() ? TextureReference.GetReference() : nullptr;
	}

	EPixelFormat GetWPRayLUTPixelFormat(const EWPRayLUTFormat Format)
	{
		return Format == EWPRayLUTFormat::RGBA32Float ? PF_A32B32G32R32F : PF_Unknown;
	}

	EPixelFormat GetWPCubePixelFormat(const EWPCubeFormat Format)
	{
		return Format == EWPCubeFormat::RGBA16Float ? PF_FloatRGBA : PF_Unknown;
	}

	void PopulateWPRayLUTContract(FWPCompositePassParameters& OutParameters, const FWPRayLUTContract& Contract, const float RayLUTZ, const uint32 EndpointRevision)
	{
		OutParameters.RayLUTZ = RayLUTZ;
		OutParameters.ExpectedLUTExtent = Contract.ExpectedExtent;
		OutParameters.ExpectedLUTFormat = GetWPRayLUTPixelFormat(Contract.ExpectedFormat);
		OutParameters.ExpectedLUTMipCount = Contract.ExpectedMipCount;
		OutParameters.ExpectedLUTDimension = Contract.ExpectedDimension;
		OutParameters.LUTLayoutVersion = Contract.LayoutVersion;
		OutParameters.LUTGeneration = Contract.Generation;
		OutParameters.LUTRevision = EndpointRevision == Contract.Revision ? EndpointRevision : 0;
	}
}

bool BuildWPCompositePassParametersForEndpoint(
	const FWPRenderThreadPacket& Packet,
	const EWPSide Side,
	const FVector3d& PreViewTranslation,
	FWPCompositePassParameters& OutParameters)
{
	if ((Side != EWPSide::SideA && Side != EWPSide::SideB) || PreViewTranslation.ContainsNaN()) return false;
	const bool bEndpointA = Side == EWPSide::SideA;
	const FVector3d CenterWorld = bEndpointA ? Packet.PortalACenterWorld : Packet.PortalBCenterWorld;
	if (CenterWorld.ContainsNaN()) return false;
	const FWPMetricSettings& Metric = bEndpointA ? Packet.MetricA : Packet.MetricB;
	const FWPPortalVisualSettings& Visual = bEndpointA ? Packet.VisualA : Packet.VisualB;
	const FWPRayLUTContract& LUTContract = bEndpointA ? Packet.RayLUTContractA : Packet.RayLUTContractB;
	const FWPCubeContract& LocalCubeContract = bEndpointA ? Packet.CubeContractA : Packet.CubeContractB;
	const FWPCubeContract& LinkedCubeContract = bEndpointA ? Packet.CubeContractB : Packet.CubeContractA;
	const FMatrix44d& SelfToWorld = bEndpointA ? Packet.PortalAToWorld : Packet.PortalBToWorld;
	const FMatrix44d& LinkedToWorld = bEndpointA ? Packet.PortalBToWorld : Packet.PortalAToWorld;
	const float VisualScale = Visual.IsFiniteAndValid() ? Visual.UniformScale : 1.0f;
	OutParameters = FWPCompositePassParameters{};
	OutParameters.CurrentPreViewTranslation = FVector3f(PreViewTranslation);
	OutParameters.PortalCenterTranslated = FVector3f(CenterWorld + PreViewTranslation);
	OutParameters.PortalRadiusCm = Metric.PortalRadiusCm * VisualScale;
	OutParameters.ThroatLengthCm = Metric.ThroatHalfLengthCm * 2.0f * VisualScale;
	OutParameters.ProxyRadiusCm = static_cast<float>(WPPortalVisibilityMath::GetSafeProxyRadiusCm(Metric.PortalRadiusCm, Metric.OuterRadiusCm)) * VisualScale;
	OutParameters.MetricOuterRadiusCm = Metric.MetricOuterRadiusCm * VisualScale;
	OutParameters.DepthBiasCm = 1.0f;
	OutParameters.SelfX = FVector3f(SelfToWorld.GetUnitAxis(EAxis::X));
	OutParameters.SelfY = FVector3f(SelfToWorld.GetUnitAxis(EAxis::Y));
	OutParameters.SelfZ = FVector3f(SelfToWorld.GetUnitAxis(EAxis::Z));
	OutParameters.LinkedX = FVector3f(LinkedToWorld.GetUnitAxis(EAxis::X));
	OutParameters.LinkedY = FVector3f(LinkedToWorld.GetUnitAxis(EAxis::Y));
	OutParameters.LinkedZ = FVector3f(LinkedToWorld.GetUnitAxis(EAxis::Z));
	OutParameters.RayLUTTexture = ResolveWPTextureReference(bEndpointA ? Packet.RayLUTA : Packet.RayLUTB);
	OutParameters.bAnalyticNoTransition = bEndpointA ? Packet.bAnalyticNoTransitionA : Packet.bAnalyticNoTransitionB;
	PopulateWPRayLUTContract(OutParameters, LUTContract, bEndpointA ? Packet.RayLUTZA : Packet.RayLUTZB, bEndpointA ? Packet.RayLUTRevisionA : Packet.RayLUTRevisionB);
	OutParameters.LocalCubeTextureReference = GetWPTextureReferenceProxy(bEndpointA ? Packet.CubeA : Packet.CubeB);
	OutParameters.ExpectedLocalCubeExtent = LocalCubeContract.ExpectedExtent;
	OutParameters.ExpectedLocalCubeFormat = GetWPCubePixelFormat(LocalCubeContract.ExpectedFormat);
	OutParameters.ExpectedLocalCubeMipCount = LocalCubeContract.ExpectedMipCount;
	OutParameters.LocalCubeLayoutVersion = LocalCubeContract.CubeLayoutVersion;
	OutParameters.LocalCubeResourceGeneration = LocalCubeContract.ResourceGeneration;
	OutParameters.LocalCubeCaptureGeneration = bEndpointA ? Packet.CaptureGenerationA : Packet.CaptureGenerationB;
	OutParameters.bLocalCubeContractValid = LocalCubeContract.IsValid();
	OutParameters.LinkedCubeTextureReference = GetWPTextureReferenceProxy(bEndpointA ? Packet.CubeB : Packet.CubeA);
	OutParameters.ExpectedLinkedCubeExtent = LinkedCubeContract.ExpectedExtent;
	OutParameters.ExpectedLinkedCubeFormat = GetWPCubePixelFormat(LinkedCubeContract.ExpectedFormat);
	OutParameters.ExpectedLinkedCubeMipCount = LinkedCubeContract.ExpectedMipCount;
	OutParameters.LinkedCubeLayoutVersion = LinkedCubeContract.CubeLayoutVersion;
	OutParameters.LinkedCubeResourceGeneration = LinkedCubeContract.ResourceGeneration;
	OutParameters.LinkedCubeCaptureGeneration = bEndpointA ? Packet.CaptureGenerationB : Packet.CaptureGenerationA;
	OutParameters.bLinkedCubeContractValid = LinkedCubeContract.IsValid();
	return AreParametersValid(OutParameters);
}

const TCHAR* GetWPCompositeFailureReasonName(const EWPCompositeFailureReason Reason)
{
	switch (Reason)
	{
	case EWPCompositeFailureReason::None: return TEXT("None");
	case EWPCompositeFailureReason::InvalidSceneColor: return TEXT("InvalidSceneColor");
	case EWPCompositeFailureReason::InvalidParameters: return TEXT("InvalidParameters");
	case EWPCompositeFailureReason::SceneDepthUnavailable: return TEXT("SceneDepthUnavailable");
	case EWPCompositeFailureReason::InvalidDepthSlice: return TEXT("InvalidDepthSlice");
	case EWPCompositeFailureReason::InvalidDepthBinding: return TEXT("InvalidDepthBinding");
	case EWPCompositeFailureReason::LUTUnavailable: return TEXT("LUTUnavailable");
	case EWPCompositeFailureReason::InvalidLUTContract: return TEXT("InvalidLUTContract");
	case EWPCompositeFailureReason::InvalidLUTDimension: return TEXT("InvalidLUTDimension");
	case EWPCompositeFailureReason::InvalidLUTExtent: return TEXT("InvalidLUTExtent");
	case EWPCompositeFailureReason::InvalidLUTMipCount: return TEXT("InvalidLUTMipCount");
	case EWPCompositeFailureReason::InvalidLUTFormat: return TEXT("InvalidLUTFormat");
	case EWPCompositeFailureReason::LUTRegistrationFailed: return TEXT("LUTRegistrationFailed");
	case EWPCompositeFailureReason::ForcedResourceFailure: return TEXT("ForcedResourceFailure");
	case EWPCompositeFailureReason::LocalCubeUnavailable: return TEXT("LocalCubeUnavailable");
	case EWPCompositeFailureReason::InvalidLocalCubeReferenceProxy:
		return TEXT("InvalidLocalCubeReferenceProxy");
	case EWPCompositeFailureReason::InvalidLocalCubeContract: return TEXT("InvalidLocalCubeContract");
	case EWPCompositeFailureReason::InvalidLocalCubeDimension: return TEXT("InvalidLocalCubeDimension");
	case EWPCompositeFailureReason::InvalidLocalCubeExtent: return TEXT("InvalidLocalCubeExtent");
	case EWPCompositeFailureReason::InvalidLocalCubeFormat: return TEXT("InvalidLocalCubeFormat");
	case EWPCompositeFailureReason::InvalidLocalCubeMipCount: return TEXT("InvalidLocalCubeMipCount");
	case EWPCompositeFailureReason::LocalCubeContentNotReady: return TEXT("LocalCubeContentNotReady");
	case EWPCompositeFailureReason::LocalCubeRegistrationFailed: return TEXT("LocalCubeRegistrationFailed");
	case EWPCompositeFailureReason::LinkedCubeUnavailable: return TEXT("LinkedCubeUnavailable");
	case EWPCompositeFailureReason::InvalidLinkedCubeReferenceProxy:
		return TEXT("InvalidLinkedCubeReferenceProxy");
	case EWPCompositeFailureReason::InvalidLinkedCubeContract: return TEXT("InvalidLinkedCubeContract");
	case EWPCompositeFailureReason::InvalidLinkedCubeDimension: return TEXT("InvalidLinkedCubeDimension");
	case EWPCompositeFailureReason::InvalidLinkedCubeExtent: return TEXT("InvalidLinkedCubeExtent");
	case EWPCompositeFailureReason::InvalidLinkedCubeFormat: return TEXT("InvalidLinkedCubeFormat");
	case EWPCompositeFailureReason::InvalidLinkedCubeMipCount: return TEXT("InvalidLinkedCubeMipCount");
	case EWPCompositeFailureReason::LinkedCubeContentNotReady: return TEXT("LinkedCubeContentNotReady");
	case EWPCompositeFailureReason::LinkedCubeRegistrationFailed: return TEXT("LinkedCubeRegistrationFailed");
	case EWPCompositeFailureReason::ShaderUnavailable: return TEXT("ShaderUnavailable");
	case EWPCompositeFailureReason::OutputUnavailable: return TEXT("OutputUnavailable");
	default: return TEXT("Unknown");
	}
}

bool ValidateWPCompositePass(
	const FSceneView& View,
	const FScreenPassTexture& SceneColor,
	const FScreenPassTexture& BaseSceneColor,
	const FRDGTextureRef SceneDepthTexture,
	const int32 SceneDepthArraySlice,
	const FWPCompositePassParameters& Parameters,
	bool& bOutShaderAvailable,
	bool& bOutSceneDepthArray,
	EWPCompositeFailureReason& OutFailureReason)
{
	bOutShaderAvailable = false;
	bOutSceneDepthArray = SceneDepthTexture
		&& SceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2DArray;
	OutFailureReason = EWPCompositeFailureReason::None;

	const bool bSceneColorCompatible = SceneColor.IsValid()
		&& SceneColor.Texture->Desc.Dimension == ETextureDimension::Texture2D
		&& SceneColor.Texture->Desc.NumSamples == 1
		&& BaseSceneColor.IsValid()
		&& BaseSceneColor.Texture->Desc.Dimension == ETextureDimension::Texture2D
		&& BaseSceneColor.Texture->Desc.NumSamples == 1;
	if (!bSceneColorCompatible)
	{
		OutFailureReason = EWPCompositeFailureReason::InvalidSceneColor;
		return false;
	}
	if (!AreParametersValid(Parameters))
	{
		OutFailureReason = EWPCompositeFailureReason::InvalidParameters;
		return false;
	}
	if (!SceneDepthTexture)
	{
		OutFailureReason = EWPCompositeFailureReason::SceneDepthUnavailable;
		return false;
	}
	if (!IsSupportedCompositeSceneDepthTexture(SceneDepthTexture))
	{
		OutFailureReason = EWPCompositeFailureReason::InvalidDepthBinding;
		return false;
	}
	if (bOutSceneDepthArray
		&& (SceneDepthArraySlice < 0 || SceneDepthArraySlice >= SceneDepthTexture->Desc.ArraySize))
	{
		OutFailureReason = EWPCompositeFailureReason::InvalidDepthSlice;
		return false;
	}
	if (Parameters.bForceResourceFailure)
	{
		OutFailureReason = EWPCompositeFailureReason::ForcedResourceFailure;
		return false;
	}
	if (!ValidateRayLUTParameters(Parameters, OutFailureReason))
	{
		return false;
	}
	if (!ValidateCubeParameters(
		Parameters.LocalCubeTextureReference,
		Parameters.ExpectedLocalCubeExtent,
		Parameters.ExpectedLocalCubeFormat,
		Parameters.ExpectedLocalCubeMipCount,
		Parameters.LocalCubeLayoutVersion,
		Parameters.LocalCubeResourceGeneration,
		Parameters.bLocalCubeContractValid,
		Parameters.LocalCubeCaptureGeneration,
		true,
		OutFailureReason))
	{
		return false;
	}
	if (!ValidateCubeParameters(
		Parameters.LinkedCubeTextureReference,
		Parameters.ExpectedLinkedCubeExtent,
		Parameters.ExpectedLinkedCubeFormat,
		Parameters.ExpectedLinkedCubeMipCount,
		Parameters.LinkedCubeLayoutVersion,
		Parameters.LinkedCubeResourceGeneration,
		Parameters.bLinkedCubeContractValid,
		Parameters.LinkedCubeCaptureGeneration,
		false,
		OutFailureReason))
	{
		return false;
	}

	FGlobalShaderMap* GlobalShaderMap = nullptr;
	if (!IsCompositeShaderAvailable(View, GlobalShaderMap))
	{
		OutFailureReason = EWPCompositeFailureReason::ShaderUnavailable;
		return false;
	}
	bOutShaderAvailable = true;
	return true;
}

FScreenPassTexture AddWPCompositePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& SceneColor,
	const FScreenPassTexture& BaseSceneColor,
	const FScreenPassRenderTarget& OverrideOutput,
	const FRDGTextureRef SceneDepthTexture,
	const int32 SceneDepthArraySlice,
	const FWPCompositePassParameters& Parameters,
	const uint64 PairHandle,
	bool& bOutShaderAvailable,
	bool& bOutSceneDepthArray,
	EWPCompositeFailureReason& OutFailureReason)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_RDGPassSetup);
	if (!ValidateWPCompositePass(
		View,
		SceneColor,
		BaseSceneColor,
		SceneDepthTexture,
		SceneDepthArraySlice,
		Parameters,
		bOutShaderAvailable,
		bOutSceneDepthArray,
		OutFailureReason))
	{
		return FScreenPassTexture();
	}
	// Add-pass availability reports preserve the original submission contract:
	// resource-registration failures occur before the shader is reported ready.
	bOutShaderAvailable = false;

	FRDGTextureSRVRef SceneDepthSRV = bOutSceneDepthArray
		? GraphBuilder.CreateSRV(
			FRDGTextureSRVDesc::CreateForSlice(SceneDepthTexture, SceneDepthArraySlice))
		: GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));
	if (!SceneDepthSRV)
	{
		OutFailureReason = EWPCompositeFailureReason::InvalidDepthBinding;
		return FScreenPassTexture();
	}

	FRHITexture* RayLUTBindingTexture = Parameters.bAnalyticNoTransition
		&& GBlackVolumeTexture
		? GBlackVolumeTexture->TextureRHI.GetReference()
		: Parameters.RayLUTTexture;
	if (!RayLUTBindingTexture)
	{
		OutFailureReason = EWPCompositeFailureReason::LUTUnavailable;
		return FScreenPassTexture();
	}
	const FRHITextureDesc& LUTDesc = RayLUTBindingTexture->GetDesc();
	FRDGTextureRef RayLUTRDG = RegisterExternalTexture(
		GraphBuilder,
		RayLUTBindingTexture,
		TEXT("WP.ProductionComposite.RayLUT"));
	if (!RayLUTRDG)
	{
		OutFailureReason = EWPCompositeFailureReason::LUTRegistrationFailed;
		return FScreenPassTexture();
	}

	FGlobalShaderMap* GlobalShaderMap = nullptr;
	if (!IsCompositeShaderAvailable(View, GlobalShaderMap))
	{
		bOutShaderAvailable = false;
		OutFailureReason = EWPCompositeFailureReason::ShaderUnavailable;
		return FScreenPassTexture();
	}
	const TShaderMapRef<FWPCompositePS> PixelShader(GlobalShaderMap);
	bOutShaderAvailable = PixelShader.IsValid();
	if (!bOutShaderAvailable)
	{
		OutFailureReason = EWPCompositeFailureReason::ShaderUnavailable;
		return FScreenPassTexture();
	}

	FScreenPassRenderTarget Output = OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(
			GraphBuilder,
			SceneColor,
			View.GetOverwriteLoadAction(),
			TEXT("WP.ProductionComposite.Output"));
	}
	if (!Output.IsValid())
	{
		OutFailureReason = EWPCompositeFailureReason::OutputUnavailable;
		return FScreenPassTexture();
	}

	const FScreenPassTextureViewport InputViewport(SceneColor);
	const FScreenPassTextureViewport BaseViewport(BaseSceneColor);
	const bool bPreparedForegroundValid = Parameters.SceneMode == EWPCompositeSceneMode::PreparedWormhole
		&& Parameters.PreparedForegroundTexture
		&& Parameters.PreparedForegroundTexture->Desc.Dimension == ETextureDimension::Texture2D
		&& Parameters.PreparedForegroundViewRect.Width() > 0
		&& Parameters.PreparedForegroundViewRect.Height() > 0
		&& Parameters.PreparedForegroundViewRect.Min.X >= 0
		&& Parameters.PreparedForegroundViewRect.Min.Y >= 0
		&& Parameters.PreparedForegroundViewRect.Max.X <= Parameters.PreparedForegroundTexture->Desc.Extent.X
		&& Parameters.PreparedForegroundViewRect.Max.Y <= Parameters.PreparedForegroundTexture->Desc.Extent.Y;
	const FScreenPassTexture ForegroundSceneColor = bPreparedForegroundValid
		? FScreenPassTexture(Parameters.PreparedForegroundTexture, Parameters.PreparedForegroundViewRect)
		: SceneColor;
	const FScreenPassTextureViewport ForegroundViewport(ForegroundSceneColor);
	const FScreenPassTextureViewport OutputViewport(Output);
	const bool bPreviousDepthValid = Parameters.SceneMode == EWPCompositeSceneMode::PreparedWormhole
		&& Parameters.PreviousSceneDepthTexture
		&& Parameters.PreviousSceneDepthTexture->Desc.Dimension == ETextureDimension::Texture2D
		&& Parameters.PreviousSceneDepthTexture->Desc.NumSamples == 1
		&& Parameters.PreviousSceneDepthViewRect.Width() > 0
		&& Parameters.PreviousSceneDepthViewRect.Height() > 0
		&& Parameters.PreviousSceneDepthViewRect.Min.X >= 0
		&& Parameters.PreviousSceneDepthViewRect.Min.Y >= 0
		&& Parameters.PreviousSceneDepthViewRect.Max.X <= Parameters.PreviousSceneDepthTexture->Desc.Extent.X
		&& Parameters.PreviousSceneDepthViewRect.Max.Y <= Parameters.PreviousSceneDepthTexture->Desc.Extent.Y;
	const FScreenPassTexture PreviousDepthTexture = bPreviousDepthValid
		? FScreenPassTexture(Parameters.PreviousSceneDepthTexture, Parameters.PreviousSceneDepthViewRect)
		: FScreenPassTexture(GSystemTextures.GetBlackDummy(GraphBuilder), FIntRect(FIntPoint::ZeroValue, FIntPoint(1, 1)));
	const FScreenPassTextureViewport PreviousDepthViewport(PreviousDepthTexture);
	FWPCompositePS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FWPCompositePS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->Input = GetScreenPassTextureViewportParameters(InputViewport);
	PassParameters->Base = GetScreenPassTextureViewportParameters(BaseViewport);
	PassParameters->Foreground = GetScreenPassTextureViewportParameters(ForegroundViewport);
	PassParameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->InputTexture = SceneColor.Texture;
	PassParameters->InputSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->BaseSceneColorTexture = BaseSceneColor.Texture;
	PassParameters->BaseSceneColorSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->ForegroundSceneColorTexture = ForegroundSceneColor.Texture;
	PassParameters->ForegroundSceneColorSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->SceneDepthTexture = SceneDepthSRV;
	PassParameters->SceneDepthSampler =
		TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->SceneMode = static_cast<uint32>(Parameters.SceneMode);
	PassParameters->CurrentPreViewTranslation = Parameters.CurrentPreViewTranslation;
	PassParameters->PreviousWorldToClip = Parameters.PreviousWorldToClip;
	PassParameters->PreviousScreenToWorld = Parameters.PreviousScreenToWorld;
	PassParameters->PreviousSceneColorPreExposureScale = Parameters.PreviousSceneColorPreExposureScale;
	PassParameters->PreviousSceneColorValid = Parameters.bPreviousSceneColorValid && bPreviousDepthValid ? 1.0f : 0.0f;
	PassParameters->PreviousDepth = GetScreenPassTextureViewportParameters(PreviousDepthViewport);
	PassParameters->PreviousSceneDepthTexture = PreviousDepthTexture.Texture;
	PassParameters->PreviousSceneDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RayLUTTexture = RayLUTRDG;
	PassParameters->RayLUTSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RayLUTZ = Parameters.RayLUTZ;
	PassParameters->RayLUTEnabled = Parameters.bAnalyticNoTransition ? 0.0f : 1.0f;
	// Keep these as raw texture-reference proxies. D3D12 resolves the queued reference switch
	// on the RHI Thread before this draw; RDG registration would unwrap the stale RT value.
	// Capture/AA explicitly leave both physical ping-pong cubes in SRV access.
	PassParameters->LocalCubeTexture = Parameters.LocalCubeTextureReference;
	PassParameters->LocalCubeSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->LinkedCubeTexture = Parameters.LinkedCubeTextureReference;
	PassParameters->LinkedCubeSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PortalCenterTranslated = Parameters.PortalCenterTranslated;
	PassParameters->PortalRadiusCm = Parameters.PortalRadiusCm;
	PassParameters->ThroatLengthCm = Parameters.ThroatLengthCm;
	PassParameters->ProxyRadiusCm = Parameters.ProxyRadiusCm;
	PassParameters->MetricOuterRadiusCm = Parameters.MetricOuterRadiusCm;
	PassParameters->DepthBiasCm = Parameters.DepthBiasCm;
	PassParameters->FilterDebugMode = CVarWPDNEGFilterDebug.GetValueOnRenderThread();
	PassParameters->SelfX = Parameters.SelfX;
	PassParameters->SelfY = Parameters.SelfY;
	PassParameters->SelfZ = Parameters.SelfZ;
	PassParameters->LinkedX = Parameters.LinkedX;
	PassParameters->LinkedY = Parameters.LinkedY;
	PassParameters->LinkedZ = Parameters.LinkedZ;
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	ClearUnusedGraphResources(PixelShader, PassParameters);
	RDG_EVENT_SCOPE_STAT(
		GraphBuilder,
		WPProductionComposite,
		"WP.ProductionComposite Mode=%u Pair=%llu View=%dx%d EndpointResources=LUT=%dx%dx%d/M%u/Z=%.5f Cube=%dx%d/M%u",
		static_cast<uint32>(Parameters.SceneMode),
		PairHandle,
		Output.ViewRect.Width(),
		Output.ViewRect.Height(),
		LUTDesc.Extent.X,
		LUTDesc.Extent.Y,
		LUTDesc.Depth,
		LUTDesc.NumMips,
		Parameters.RayLUTZ,
		Parameters.ExpectedLinkedCubeExtent.X,
		Parameters.ExpectedLinkedCubeExtent.Y,
		Parameters.ExpectedLinkedCubeMipCount);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GlobalShaderMap,
		RDG_EVENT_NAME("WP.ProductionComposite Mode=%u Pair=%llu", static_cast<uint32>(Parameters.SceneMode), PairHandle),
		PixelShader,
		PassParameters,
		Output.ViewRect);
	INC_DWORD_STAT(STAT_WP_ProductionCompositePasses);
	INC_FLOAT_STAT_BY(
		STAT_WP_CompositeMegapixels,
		static_cast<double>(FMath::Max(Output.ViewRect.Area(), 0)) / 1'000'000.0);

	return MoveTemp(Output);
}
