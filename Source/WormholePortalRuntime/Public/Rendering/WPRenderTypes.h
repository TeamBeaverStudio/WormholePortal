// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UVolumeTexture;
class UTextureRenderTargetCube;

/**
 * Production-compositor state for one Pair.
 * Disabled submits no rendering work.
 * Warmup renders the same production output while waiting for a successful Render
 * Thread ACK for the same epoch.
 * Production is the committed state after validation succeeds.
 */
enum class EWPPairOwnershipMode : uint8
{
	Disabled = 0,
	Warmup,
	Production
};

inline const TCHAR* GetWPPairOwnershipModeName(const EWPPairOwnershipMode Mode)
{
	switch (Mode)
	{
	case EWPPairOwnershipMode::Disabled: return TEXT("Disabled");
	case EWPPairOwnershipMode::Warmup: return TEXT("Warmup");
	case EWPPairOwnershipMode::Production: return TEXT("Production");
	default: return TEXT("Unknown");
	}
}

/**
 * POD snapshot that returns completed production-warmup successes and failures from the
 * Render Thread to the Game Thread.
 * It must be consumed only when its epoch exactly matches the current request,
 * preventing stale commands from changing
 * screen ownership.
 */
struct FWPPairOwnershipFeedback
{
	uint64 WarmupSucceededEpoch = 0;
	uint64 WarmupSucceededPacketSequence = 0;
	uint64 ProductionFailedEpoch = 0;
	uint64 ProductionFailedPacketSequence = 0;
	uint64 WarmupPassCount = 0;
	uint64 ProductionFailureCount = 0;
	/**
	 * Latest Pair visibility observed by the Renderer from an accepted primary ownership
	 * view.
	 * Runtime considers both endpoints invisible only after confirming a coherent snapshot,
	 * the current OwnershipEpoch, and a fresh sample.
	 */
	uint64 VisibilityOwnershipEpoch = 0;
	uint64 VisibilityPacketSequence = 0;
	uint64 VisibilitySampleSequence = 0;
	/**
	 * Exact endpoint bits observed in the completed Player View render frame.
	 * Bit 0 is endpoint A and bit 1 is endpoint B. Keeping the identity here lets the
	 * Runtime intersect frustum and occlusion results before running detailed Metric rays.
	 */
	uint8 VisibleEndpointMask = 0;

	uint32 GetVisibleEndpointCount() const
	{
		return ((VisibleEndpointMask & 0x1u) != 0 ? 1u : 0u)
			+ ((VisibleEndpointMask & 0x2u) != 0 ? 1u : 0u);
	}
	/** Final even seqlock version enclosing a coherent mailbox snapshot. */
	uint64 SnapshotVersion = 0;
	/** Number of torn-read attempts discarded because they overlapped a writer. */
	uint32 SnapshotReadRetryCount = 0;
	/**
	 * False when bounded seqlock retries were exhausted, requiring the consumer to fall
	 * back conservatively.
	 */
	bool bSnapshotCoherent = true;
};

/**
 * Pair snapshot consumed by the Game Thread ownership controller and production
 * compositor.
 */
struct FWPPairOwnershipSnapshot
{
	FGuid PairId;
	FName StableSelectorNameA = NAME_None;
	FName StableSelectorNameB = NAME_None;
	EWPPairOwnershipMode RequestedOwnership = EWPPairOwnershipMode::Disabled;
	EWPPairOwnershipMode EffectiveOwnership = EWPPairOwnershipMode::Disabled;
	uint64 OwnershipEpoch = 0;
	bool bEndpointAReady = false;
	bool bEndpointBReady = false;
	bool bOwnershipInputsReady = false;

	bool ShouldRunProductionPath() const
	{
		return EffectiveOwnership == EWPPairOwnershipMode::Warmup
			|| EffectiveOwnership == EWPPairOwnershipMode::Production;
	}

	bool IsProductionCommitted() const
	{
		return EffectiveOwnership == EWPPairOwnershipMode::Production;
	}

	/** Complete production-renderer activation contract for a published packet. */
	bool IsReadyForRendering(const uint64 PacketSequence) const
	{
		return PairId.IsValid()
			&& RequestedOwnership == EWPPairOwnershipMode::Production
			&& (EffectiveOwnership == EWPPairOwnershipMode::Warmup
				|| EffectiveOwnership == EWPPairOwnershipMode::Production)
			&& OwnershipEpoch != 0
			&& PacketSequence != 0
			&& bEndpointAReady
			&& bEndpointBReady
			&& bOwnershipInputsReady;
	}
};

/** Side of the Portal Pair currently viewed by the Camera or reference view. */
enum class EWPSide : uint8
{
	None,
	SideA,
	SideB
};

/** Diagnostic metric region containing the reference view. */
enum class EWPRegion : uint8
{
	Flat,
	Transition,
	Throat
};

/**
 * On-wire format identifier for a Ray LUT.
 * Enumerates only physical storage formats so the Game Thread and Render Thread can
 * validate
 * the same texture contract without exposing EPixelFormat through the Runtime public
 * API.
 */
enum class EWPRayLUTFormat : uint8
{
	Unknown,
	RGBA32Float
};

inline const TCHAR* GetWPRayLUTFormatName(const EWPRayLUTFormat Format)
{
	switch (Format)
	{
	case EWPRayLUTFormat::RGBA32Float: return TEXT("RGBA32Float");
	default: return TEXT("Unknown");
	}
}

/** Required RHI texture dimension for a Ray LUT. */
enum class EWPRayLUTDimension : uint8
{
	Unknown,
	Texture3D
};

inline const TCHAR* GetWPRayLUTDimensionName(const EWPRayLUTDimension Dimension)
{
	switch (Dimension)
	{
	case EWPRayLUTDimension::Texture3D: return TEXT("Texture3D");
	default: return TEXT("Unknown");
	}
}

/**
 * Immutable contract defining how to interpret the Ray LUT for one endpoint.
 * Generation increments after each successful CPU population and UpdateResource
 * submission.
 * Revision is the content fingerprint of the metric and layout.
 * Both values must be compared to detect resource recreation with otherwise identical
 * content.
 */
struct FWPRayLUTContract
{
	uint32 LayoutVersion = 0;
	uint32 Generation = 0;
	uint32 Revision = 0;
	FIntVector ExpectedExtent = FIntVector::ZeroValue;
	EWPRayLUTFormat ExpectedFormat = EWPRayLUTFormat::Unknown;
	uint32 ExpectedMipCount = 0;
	EWPRayLUTDimension ExpectedDimension = EWPRayLUTDimension::Unknown;

	bool IsValid() const
	{
		return LayoutVersion != 0
			&& Generation != 0
			&& Revision != 0
			&& ExpectedExtent.X > 0
			&& ExpectedExtent.Y > 0
			&& ExpectedExtent.Z > 0
			&& ExpectedFormat != EWPRayLUTFormat::Unknown
			&& ExpectedMipCount > 0
			&& ExpectedDimension == EWPRayLUTDimension::Texture3D;
	}

	bool operator==(const FWPRayLUTContract& Other) const
	{
		return LayoutVersion == Other.LayoutVersion
			&& Generation == Other.Generation
			&& Revision == Other.Revision
			&& ExpectedExtent == Other.ExpectedExtent
			&& ExpectedFormat == Other.ExpectedFormat
			&& ExpectedMipCount == Other.ExpectedMipCount
			&& ExpectedDimension == Other.ExpectedDimension;
	}

	bool operator!=(const FWPRayLUTContract& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * Public on-wire format for a Portal Cube.
 * Allows the resource contract declared by the Game Thread to be compared with the
 * actual texture
 * resolved on the Render Thread without exposing EPixelFormat across the
 * Runtime/Renderer boundary.
 */
enum class EWPCubeFormat : uint8
{
	Unknown,
	RGBA16Float
};

inline const TCHAR* GetWPCubeFormatName(const EWPCubeFormat Format)
{
	switch (Format)
	{
	case EWPCubeFormat::RGBA16Float: return TEXT("RGBA16Float");
	default: return TEXT("Unknown");
	}
}

/** Required RHI texture dimension for a Portal Cube. */
enum class EWPCubeDimension : uint8
{
	Unknown,
	TextureCube
};

inline const TCHAR* GetWPCubeDimensionName(const EWPCubeDimension Dimension)
{
	switch (Dimension)
	{
	case EWPCubeDimension::TextureCube: return TEXT("TextureCube");
	default: return TEXT("Unknown");
	}
}

/**
 * Immutable contract describing the Portal Cube allocation for one endpoint.
 * ResourceGeneration increments only when the UObject or RHI target is reallocated.
 * It is intentionally separate from CaptureGeneration, which increments for every
 * CaptureScene submission,
 * so a content-only cube update does not republish the Game Thread-to-Render Thread
 * packet.
 *
 * The Cube target is owned by the World's WP CaptureManager.
 * Calls to Init, Resize, UpdateResource, or ReleaseResource by an external consumer
 * violate the contract.
 * Allocation may change only through a path that increments the manager generation and
 * notifies the Registry.
 */
struct FWPCubeContract
{
	uint32 CubeLayoutVersion = 0;
	uint32 ResourceGeneration = 0;
	FIntPoint ExpectedExtent = FIntPoint::ZeroValue;
	EWPCubeFormat ExpectedFormat = EWPCubeFormat::Unknown;
	uint32 ExpectedMipCount = 0;
	EWPCubeDimension ExpectedDimension = EWPCubeDimension::Unknown;

	bool IsValid() const
	{
		return CubeLayoutVersion != 0
			&& ResourceGeneration != 0
			&& ExpectedExtent.X > 0
			&& ExpectedExtent.X == ExpectedExtent.Y
			&& ExpectedFormat != EWPCubeFormat::Unknown
			&& ExpectedMipCount > 0
			&& ExpectedDimension == EWPCubeDimension::TextureCube;
	}

	bool operator==(const FWPCubeContract& Other) const
	{
		return CubeLayoutVersion == Other.CubeLayoutVersion
			&& ResourceGeneration == Other.ResourceGeneration
			&& ExpectedExtent == Other.ExpectedExtent
			&& ExpectedFormat == Other.ExpectedFormat
			&& ExpectedMipCount == Other.ExpectedMipCount
			&& ExpectedDimension == Other.ExpectedDimension;
	}

	bool operator!=(const FWPCubeContract& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * Shared Game Thread/Render Thread value type containing only the metric values for one
 * Portal endpoint.
 */
struct FWPMetricSettings
{
	float PortalRadiusCm = 0.0f;
	float ThroatHalfLengthCm = 0.0f;
	float TransitionLengthCm = 0.0f;
	float MouthRadiusCm = 0.0f;
	// Outer radius of the shader metric; distinct from OuterRadiusCm, which is used for
	// Actor proxy bounds.
	float MetricOuterRadiusCm = 0.0f;
	float OuterRadiusCm = 0.0f;
	/**
	 * Stable identity of the dimensionless metric shape used by renderer ownership.
	 * Uniformly scaling rho, a, and T changes Revision so the new values are published,
	 * but preserves this identity so an already-valid LUT binding and ownership epoch do
	 * not enter warmup again. A ratio change produces a different identity.
	 */
	uint32 ResourceIdentityRevision = 1;
	uint32 Revision = 0;

	bool IsFiniteAndValid() const
	{
		return FMath::IsFinite(PortalRadiusCm)
			&& FMath::IsFinite(ThroatHalfLengthCm)
			&& FMath::IsFinite(TransitionLengthCm)
			&& FMath::IsFinite(MouthRadiusCm)
			&& FMath::IsFinite(MetricOuterRadiusCm)
			&& FMath::IsFinite(OuterRadiusCm)
			&& PortalRadiusCm > 0.0f
			&& ThroatHalfLengthCm >= 0.0f
			&& TransitionLengthCm >= 0.0f
			&& MouthRadiusCm >= PortalRadiusCm
			&& MetricOuterRadiusCm >= PortalRadiusCm
			&& OuterRadiusCm >= MouthRadiusCm
			&& ResourceIdentityRevision != 0
			&& Revision != 0;
	}

	bool IsCompatibleWith(const FWPMetricSettings& Other, const float Tolerance = KINDA_SMALL_NUMBER) const
	{
		return FMath::IsNearlyEqual(PortalRadiusCm, Other.PortalRadiusCm, Tolerance)
			&& FMath::IsNearlyEqual(ThroatHalfLengthCm, Other.ThroatHalfLengthCm, Tolerance)
			&& FMath::IsNearlyEqual(TransitionLengthCm, Other.TransitionLengthCm, Tolerance)
			&& FMath::IsNearlyEqual(MetricOuterRadiusCm, Other.MetricOuterRadiusCm, Tolerance);
	}
};

/**
 * Render-only appearance state for one Portal endpoint. Keeping this separate from
 * FWPMetricSettings makes it explicit that a visual animation must not alter physical collision,
 * analytic bounds, LUT identity, ownership warmup, or capture-resolution policy.
 */
struct FWPPortalVisualSettings
{
	/** Uniform scale applied only to compositor ray geometry. A value of 1 uses the full Metric size. */
	float UniformScale = 1.0f;

	bool IsFiniteAndValid() const
	{
		return FMath::IsFinite(UniformScale) && UniformScale > 0.0f;
	}
};

/**
 * Pair registration handle that distinguishes registrations across Renderer Service
 * restarts.
 */
struct FWPRenderHandle
{
	uint64 ServiceId = 0;
	uint64 Value = 0;

	bool IsValid() const
	{
		return ServiceId != 0 && Value != 0;
	}

	void Reset()
	{
		ServiceId = 0;
		Value = 0;
	}
};

/**
 * Immutable-by-convention packet created by UWPRuntimeSubsystem on the Game Thread.
 * Renderer Service converts UObject weak pointers to RHI references on the Game Thread
 * and then discards them.
 * UObject weak pointers are never passed to the SceneViewExtension Render Thread
 * snapshot.
 */
struct FWPRenderPacket
{
	FGuid PairId;
	FWPRenderHandle RenderHandle;

	// Production-requested pairs enter Warmup immediately. Rendering readiness and the
	// Production commit require both endpoints and immutable resources for the same epoch.
	FName StableSelectorNameA = NAME_None;
	FName StableSelectorNameB = NAME_None;
	EWPPairOwnershipMode RequestedOwnership = EWPPairOwnershipMode::Disabled;
	EWPPairOwnershipMode EffectiveOwnership = EWPPairOwnershipMode::Disabled;
	uint64 OwnershipEpoch = 0;

	// Do not reduce absolute World translations to float prematurely. Convert them to
	// View-relative translated-world float matrices immediately before Shader binding.
	FMatrix44d PortalAToWorld = FMatrix44d::Identity;
	FMatrix44d WorldToPortalA = FMatrix44d::Identity;
	FMatrix44d PortalBToWorld = FMatrix44d::Identity;
	FMatrix44d WorldToPortalB = FMatrix44d::Identity;

	// Preserve LWC positions as doubles and convert the actual Shader parameters relative
	// to the View's PreViewTranslation.
	FVector3d PortalACenterWorld = FVector3d::ZeroVector;
	FVector3d PortalBCenterWorld = FVector3d::ZeroVector;
	FVector3d ReferenceViewPositionWorld = FVector3d::ZeroVector;
	FVector3f ReferenceViewPositionPortalA = FVector3f::ZeroVector;
	FVector3f ReferenceViewPositionPortalB = FVector3f::ZeroVector;
	uint32 ReferenceViewActorId = 0;

	FWPMetricSettings MetricA;
	FWPMetricSettings MetricB;
	FWPPortalVisualSettings VisualA;
	FWPPortalVisualSettings VisualB;

	// The values below are a Runtime diagnostic baseline derived from the first
	// local-player Camera.
	// SceneViewExtension recomputes the actual mask-raster parameters for each FSceneView
	// and does not
	// use these values as authoritative rendering input.
	EWPSide CurrentSide = EWPSide::None;
	EWPSide EntrySide = EWPSide::None;
	EWPRegion Region = EWPRegion::Flat;
	float SignedEllCm = 0.0f;
	float TransitionAlpha = 1.0f;
	uint32 TransitActorId = 0;
	uint64 TransitEventSequence = 0;

	TWeakObjectPtr<UTextureRenderTargetCube> CubeA;
	TWeakObjectPtr<UTextureRenderTargetCube> CubeB;
	FWPCubeContract CubeContractA;
	FWPCubeContract CubeContractB;
	TWeakObjectPtr<UVolumeTexture> RayLUTA;
	TWeakObjectPtr<UVolumeTexture> RayLUTB;
	FWPRayLUTContract RayLUTContractA;
	FWPRayLUTContract RayLUTContractB;
	// TransitionLength <= KINDA_SMALL_NUMBER uses exact analytic defaults and must not sample a LUT.
	bool bAnalyticNoTransitionA = false;
	bool bAnalyticNoTransitionB = false;

	// Logical endpoint coordinate into the normalized T/rho axis of the shared volume LUT.
	float RayLUTZA = 0.0f;
	float RayLUTZB = 0.0f;
	uint32 RayLUTRevisionA = 0;
	uint32 RayLUTRevisionB = 0;

	uint64 PacketSequence = 0;
	uint32 CaptureGenerationA = 0;
	uint32 CaptureGenerationB = 0;
	bool bEnabled = false;
	bool bHasReferenceView = false;
	bool bTransitActive = false;
	bool bMetricCompatible = false;
	bool bResourcesReady = false;
	bool bCaptureReady = false;
	bool bScaleSupported = false;
	bool bOwnershipEndpointAReady = false;
	bool bOwnershipEndpointBReady = false;
	bool bOwnershipInputsReady = false;
	/**
	 * Game-Thread CPU occlusion result for production-composite suppression only.
	 * Frustum visibility feedback remains independent so a fully occluded Pair can keep
	 * observing the primary view and can resume without an occlusion/frustum feedback loop.
	 */
	uint8 CaptureOcclusionVisibleEndpointMask = 0x3;
	bool bCaptureOcclusionValid = false;
	/** False preserves the stock renderer path without RT visibility mailbox writes. */
	bool bCaptureVisibilityFeedbackEnabled = false;
};
