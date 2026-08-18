// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "PixelFormat.h"
#include "RenderGraphFwd.h"

class FRDGBuilder;
class FRHITexture;
class FSceneView;
struct FWPRenderThreadPacket;
struct FScreenPassRenderTarget;
struct FScreenPassTexture;

/** Selects the color contract while retaining the production analytic ray implementation. */
enum class EWPCompositeSceneMode : uint8
{
	Production = 0,
	PreparedWormhole = 1,
	PreparedCoverageMask = 2,
	PreparedVelocityOverride = 3
};

/** Deterministic fail-closed reason for a rejected production composite pass. */
enum class EWPCompositeFailureReason : uint8
{
	None = 0,
	InvalidSceneColor,
	InvalidParameters,
	SceneDepthUnavailable,
	InvalidDepthSlice,
	InvalidDepthBinding,
	LUTUnavailable,
	InvalidLUTContract,
	InvalidLUTDimension,
	InvalidLUTExtent,
	InvalidLUTMipCount,
	InvalidLUTFormat,
	LUTRegistrationFailed,
	ForcedResourceFailure,
	LocalCubeUnavailable,
	InvalidLocalCubeReferenceProxy,
	InvalidLocalCubeContract,
	InvalidLocalCubeDimension,
	InvalidLocalCubeExtent,
	InvalidLocalCubeFormat,
	InvalidLocalCubeMipCount,
	LocalCubeContentNotReady,
	LocalCubeRegistrationFailed,
	LinkedCubeUnavailable,
	InvalidLinkedCubeReferenceProxy,
	InvalidLinkedCubeContract,
	InvalidLinkedCubeDimension,
	InvalidLinkedCubeExtent,
	InvalidLinkedCubeFormat,
	InvalidLinkedCubeMipCount,
	LinkedCubeContentNotReady,
	LinkedCubeRegistrationFailed,
	ShaderUnavailable,
	OutputUnavailable
};

/** Immutable inputs for one endpoint's production or prepared fullscreen composite. */
struct FWPCompositePassParameters
{
	EWPCompositeSceneMode SceneMode = EWPCompositeSceneMode::Production;
	FVector3f CurrentPreViewTranslation = FVector3f::ZeroVector;
	FMatrix44f PreviousWorldToClip = FMatrix44f::Identity;
	FMatrix44f PreviousScreenToWorld = FMatrix44f::Identity;
	float PreviousSceneColorPreExposureScale = 1.0f;
	bool bPreviousSceneColorValid = false;
	FRDGTextureRef PreviousSceneDepthTexture = nullptr;
	FIntRect PreviousSceneDepthViewRect;
	/** Immutable custom-view foreground used to replace a farther prepared endpoint without losing front translucency. */
	FRDGTextureRef PreparedForegroundTexture = nullptr;
	FIntRect PreparedForegroundViewRect;
	FVector3f PortalCenterTranslated = FVector3f::ZeroVector;
	float PortalRadiusCm = 0.0f;
	float ThroatLengthCm = 0.0f;
	float ProxyRadiusCm = 0.0f;
	/** Exact finite production projection distance input; this is not the proxy radius. */
	float MetricOuterRadiusCm = 0.0f;
	float DepthBiasCm = 0.0f;
	FVector3f SelfX = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f SelfY = FVector3f(0.0f, 1.0f, 0.0f);
	FVector3f SelfZ = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f LinkedX = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f LinkedY = FVector3f(0.0f, 1.0f, 0.0f);
	FVector3f LinkedZ = FVector3f(0.0f, 0.0f, 1.0f);
	FRHITexture* RayLUTTexture = nullptr;
	/** Exact constant-radius path: no authored/generated LUT is required or sampled. */
	bool bAnalyticNoTransition = false;
	/** Logical endpoint coordinate on the shared volume LUT's normalized T/rho axis. */
	float RayLUTZ = 0.0f;
	FIntVector ExpectedLUTExtent = FIntVector::ZeroValue;
	EPixelFormat ExpectedLUTFormat = PF_Unknown;
	uint32 ExpectedLUTMipCount = 0;
	EWPRayLUTDimension ExpectedLUTDimension = EWPRayLUTDimension::Unknown;
	uint32 LUTLayoutVersion = 0;
	uint32 LUTGeneration = 0;
	uint32 LUTRevision = 0;

	/**
	 * Endpoint-local stable FRHITextureReference proxy. Never unwrap this on the Render Thread:
	 * D3D12 applies reference switches on the RHI Thread immediately before the consuming draw.
	 */
	FRHITexture* LocalCubeTextureReference = nullptr;
	FIntPoint ExpectedLocalCubeExtent = FIntPoint::ZeroValue;
	EPixelFormat ExpectedLocalCubeFormat = PF_Unknown;
	uint32 ExpectedLocalCubeMipCount = 0;
	uint32 LocalCubeLayoutVersion = 0;
	uint32 LocalCubeResourceGeneration = 0;
	/** Result of the public contract's full validation, including declared dimension. */
	bool bLocalCubeContractValid = false;
	/** Content generation: startup readiness gate only, never resource identity. */
	uint32 LocalCubeCaptureGeneration = 0;

	/** Endpoint-linked stable FRHITextureReference proxy; same ordering contract as local. */
	FRHITexture* LinkedCubeTextureReference = nullptr;
	FIntPoint ExpectedLinkedCubeExtent = FIntPoint::ZeroValue;
	EPixelFormat ExpectedLinkedCubeFormat = PF_Unknown;
	uint32 ExpectedLinkedCubeMipCount = 0;
	uint32 LinkedCubeLayoutVersion = 0;
	uint32 LinkedCubeResourceGeneration = 0;
	/** Result of the public contract's full validation, including declared dimension. */
	bool bLinkedCubeContractValid = false;
	/** Content generation: startup readiness gate only, never resource identity. */
	uint32 LinkedCubeCaptureGeneration = 0;

	/** Production ownership rollback test hook; normal rendering always leaves this false. */
	bool bForceResourceFailure = false;
};

/** Builds one endpoint from the existing immutable production packet. */
bool BuildWPCompositePassParametersForEndpoint(
	const FWPRenderThreadPacket& Packet,
	EWPSide Side,
	const FVector3d& PreViewTranslation,
	FWPCompositePassParameters& OutParameters);

const TCHAR* GetWPCompositeFailureReasonName(EWPCompositeFailureReason Reason);

/**
 * Side-effect-free production pass preflight. Multi-pair ownership uses this to
 * reject an entire pair before either endpoint enters the global composite chain.
 */
bool ValidateWPCompositePass(
	const FSceneView& View,
	const FScreenPassTexture& SceneColor,
	const FScreenPassTexture& BaseSceneColor,
	FRDGTextureRef SceneDepthTexture,
	int32 SceneDepthArraySlice,
	const FWPCompositePassParameters& Parameters,
	bool& bOutShaderAvailable,
	bool& bOutSceneDepthArray,
	EWPCompositeFailureReason& OutFailureReason);

FScreenPassTexture AddWPCompositePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassTexture& SceneColor,
	const FScreenPassTexture& BaseSceneColor,
	const FScreenPassRenderTarget& OverrideOutput,
	FRDGTextureRef SceneDepthTexture,
	int32 SceneDepthArraySlice,
	const FWPCompositePassParameters& Parameters,
	uint64 PairHandle,
	bool& bOutShaderAvailable,
	bool& bOutSceneDepthArray,
	EWPCompositeFailureReason& OutFailureReason);
