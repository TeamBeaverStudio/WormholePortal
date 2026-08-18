// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/WPSelectedCubeCapture.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "LegacyScreenPercentageDriver.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "SceneCaptureRendering.h"
#include "SceneInterface.h"
#include "ScenePrivate.h"
#include "SceneRenderBuilderInterface.h"
#include "SceneRendering.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "TextureResource.h"
#include "WPLog.h"

namespace
{
	constexpr uint8 WPAllCubeFacesMask = 0x3f;
	constexpr int32 WPCubeCaptureFaceCount = 6;
	constexpr int32 WPCubeAtlasColumns = 3;
	constexpr int32 WPCubeAtlasRows = 2;

	constexpr int32 WPCubeFaceViewportOffsets[WPCubeCaptureFaceCount][2] =
	{
		{ 0, 0 },
		{ 1, 0 },
		{ 2, 0 },
		{ 0, 1 },
		{ 1, 1 },
		{ 2, 1 }
	};

	int32 CountSelectedFaces(const uint8 FaceMask)
	{
		int32 Count = 0;
		for (int32 FaceIndex = 0; FaceIndex < WPCubeCaptureFaceCount; ++FaceIndex)
		{
			Count += (FaceMask & static_cast<uint8>(1u << FaceIndex)) != 0 ? 1 : 0;
		}
		return Count;
	}

	FMatrix CalcCubeFaceTransform(const ECubeFace Face)
	{
		static const FVector XAxis(1.0f, 0.0f, 0.0f);
		static const FVector YAxis(0.0f, 1.0f, 0.0f);
		static const FVector ZAxis(0.0f, 0.0f, 1.0f);

		FVector Up = YAxis;
		FVector Direction = XAxis;
		switch (Face)
		{
		case CubeFace_PosX:
			Direction = XAxis;
			break;
		case CubeFace_NegX:
			Direction = -XAxis;
			break;
		case CubeFace_PosY:
			Up = -ZAxis;
			Direction = YAxis;
			break;
		case CubeFace_NegY:
			Up = ZAxis;
			Direction = -YAxis;
			break;
		case CubeFace_PosZ:
			Direction = ZAxis;
			break;
		case CubeFace_NegZ:
			Direction = -ZAxis;
			break;
		default:
			break;
		}

		const FVector Right = Up ^ Direction;
		return FBasisVectorMatrix(Right, Up, Direction, FVector::ZeroVector);
	}

	void GetShowOnlyAndHiddenComponents(
		USceneCaptureComponent& CaptureComponent,
		TSet<FPrimitiveComponentId>& HiddenPrimitives,
		TOptional<TSet<FPrimitiveComponentId>>& ShowOnlyPrimitives)
	{
		for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent
			: CaptureComponent.HiddenComponents)
		{
			if (const UPrimitiveComponent* PrimitiveComponent = WeakComponent.Get())
			{
				HiddenPrimitives.Add(PrimitiveComponent->GetPrimitiveSceneId());
			}
		}

		for (const TObjectPtr<AActor>& Actor : CaptureComponent.HiddenActors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (const UPrimitiveComponent* PrimitiveComponent =
					Cast<UPrimitiveComponent>(Component))
				{
					HiddenPrimitives.Add(PrimitiveComponent->GetPrimitiveSceneId());
				}
			}
		}

		if (CaptureComponent.PrimitiveRenderMode
			!= ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList)
		{
			return;
		}

		ShowOnlyPrimitives.Emplace();
		for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent
			: CaptureComponent.ShowOnlyComponents)
		{
			if (const UPrimitiveComponent* PrimitiveComponent = WeakComponent.Get())
			{
				ShowOnlyPrimitives->Add(PrimitiveComponent->GetPrimitiveSceneId());
			}
		}

		for (const TObjectPtr<AActor>& Actor : CaptureComponent.ShowOnlyActors)
		{
			if (!IsValid(Actor))
			{
				continue;
			}
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (const UPrimitiveComponent* PrimitiveComponent =
					Cast<UPrimitiveComponent>(Component))
				{
					ShowOnlyPrimitives->Add(PrimitiveComponent->GetPrimitiveSceneId());
				}
			}
		}
	}

	class FWPSelectedCubeAtlasRenderTarget final : public FRenderTarget
	{
	public:
		FWPSelectedCubeAtlasRenderTarget(
			FTextureRenderTargetCubeResource& InCubeResource,
			const FIntPoint InFaceExtent)
			: CubeResource(InCubeResource)
		{
			const FIntPoint AtlasExtent(
				InFaceExtent.X * WPCubeAtlasColumns,
				InFaceExtent.Y * WPCubeAtlasRows);
			AtlasDesc = FPooledRenderTargetDesc::Create2DDesc(
				AtlasExtent,
				PF_Unknown,
				FClearValueBinding::Black,
				TexCreate_None,
				TexCreate_ShaderResource | TexCreate_RenderTargetable,
				false);
		}

		void InitRHI(FRHICommandListImmediate& RHICmdList)
		{
			AtlasDesc.Format = CubeResource.GetRenderTargetTexture()->GetFormat();
			GRenderTargetPool.FindFreeElement(
				RHICmdList, AtlasDesc, PooledRenderTarget,
				TEXT("WP.SelectedCubeAtlas"));
			check(PooledRenderTarget);
			RenderTargetTexture = PooledRenderTarget->GetRHI();
		}

		virtual const FTextureRHIRef& GetRenderTargetTexture() const override
		{
			return RenderTargetTexture;
		}

		virtual FIntPoint GetSizeXY() const override
		{
			return AtlasDesc.Extent;
		}

		virtual float GetDisplayGamma() const override
		{
			return 1.0f;
		}

	private:
		FTextureRenderTargetCubeResource& CubeResource;
		FPooledRenderTargetDesc AtlasDesc;
		TRefCountPtr<IPooledRenderTarget> PooledRenderTarget;
		FTextureRHIRef RenderTargetTexture;
	};

	bool CaptureNeedsSceneColor(const ESceneCaptureSource CaptureSource)
	{
		return CaptureSource == SCS_SceneColorHDR
			|| CaptureSource == SCS_SceneColorHDRNoAlpha
			|| CaptureSource == SCS_SceneColorSceneDepth;
	}
}

bool AddWPSelectedCubeCaptureRenderer(
	USceneCaptureComponentCube& CaptureComponent,
	const uint8 SelectedFaceMask,
	ISceneRenderBuilder& SceneRenderBuilder)
{
	check(IsInGameThread());
	const double BuildStartSeconds = FPlatformTime::Seconds();
	const uint8 SanitizedFaceMask = SelectedFaceMask & WPAllCubeFacesMask;
	UTextureRenderTargetCube* TextureTarget = CaptureComponent.TextureTarget;
	UWorld* World = CaptureComponent.GetWorld();
	FSceneInterface* Scene = World ? World->Scene : nullptr;
	if (SanitizedFaceMask == 0 || !IsValid(TextureTarget) || !Scene
		|| !CaptureComponent.IsRegistered())
	{
		WP_LOG(&CaptureComponent, Error,
			TEXT("[SelectedCubeBatch] Build rejected. Capture=%s Target=%s SelectedMask=0x%02x Registered=%d SceneReady=%d GameThreadWait=0 BuildGTMs=%.4f"),
			*GetNameSafe(&CaptureComponent), *GetNameSafe(TextureTarget),
			static_cast<uint32>(SanitizedFaceMask),
			CaptureComponent.IsRegistered() ? 1 : 0, Scene ? 1 : 0,
			(FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0);
		return false;
	}

	FTextureRenderTargetCubeResource* CubeResource =
		static_cast<FTextureRenderTargetCubeResource*>(
			TextureTarget->GameThread_GetRenderTargetResource());
	const FIntPoint FaceExtent(
		TextureTarget->GetSurfaceWidth(), TextureTarget->GetSurfaceHeight());
	if (!CubeResource || FaceExtent.X <= 0 || FaceExtent.Y <= 0)
	{
		WP_LOG(&CaptureComponent, Error,
			TEXT("[SelectedCubeBatch] Render target rejected. Capture=%s Target=%s SelectedMask=0x%02x Extent=%dx%d CubeResource=%d GameThreadWait=0 BuildGTMs=%.4f"),
			*GetNameSafe(&CaptureComponent), *GetNameSafe(TextureTarget),
			static_cast<uint32>(SanitizedFaceMask), FaceExtent.X, FaceExtent.Y,
			CubeResource ? 1 : 0,
			(FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0);
		return false;
	}

	FTransform CaptureTransform = CaptureComponent.GetComponentToWorld();
	const FVector ViewLocation = CaptureTransform.GetTranslation();
	if (CaptureComponent.bCaptureRotation)
	{
		CaptureTransform.SetTranslation(FVector::ZeroVector);
		CaptureTransform.SetScale3D(FVector::OneVector);
	}

	const float HalfFOVRadians = 90.0f * static_cast<float>(PI) / 360.0f;
	const float FarClip = CaptureComponent.MaxViewDistanceOverride > 0.0f
		&& CaptureComponent.bFiniteFarPlane
		? FMath::Max(CaptureComponent.MaxViewDistanceOverride, GNearClippingPlane)
		: GNearClippingPlane;

	TArray<FSceneCaptureViewInfo, TFixedAllocator<WPCubeCaptureFaceCount>> ViewInfos;
	TArray<int32, TFixedAllocator<WPCubeCaptureFaceCount>> DestinationSlices;
	for (int32 FaceIndex = 0; FaceIndex < WPCubeCaptureFaceCount; ++FaceIndex)
	{
		const uint8 FaceBit = static_cast<uint8>(1u << FaceIndex);
		if ((SanitizedFaceMask & FaceBit) == 0)
		{
			continue;
		}

		const ECubeFace Face = static_cast<ECubeFace>(FaceIndex);
		FSceneCaptureViewInfo& ViewInfo = ViewInfos.AddDefaulted_GetRef();
		ViewInfo.ViewRotationMatrix = CaptureComponent.bCaptureRotation
			? CaptureTransform.ToInverseMatrixWithScale() * CalcCubeFaceTransform(Face)
			: CalcCubeFaceTransform(Face);
		ViewInfo.ViewOrigin = ViewLocation;
		ViewInfo.ProjectionMatrix = FReversedZPerspectiveMatrix(
			HalfFOVRadians,
			HalfFOVRadians,
			1.0f,
			1.0f,
			GNearClippingPlane,
			FarClip);
		ViewInfo.StereoPass = EStereoscopicPass::eSSP_FULL;
		ViewInfo.StereoViewIndex = INDEX_NONE;
		const FIntPoint AtlasOffset(
			WPCubeFaceViewportOffsets[FaceIndex][0] * FaceExtent.X,
			WPCubeFaceViewportOffsets[FaceIndex][1] * FaceExtent.Y);
		ViewInfo.ViewRect = FIntRect(
			AtlasOffset.X, AtlasOffset.Y,
			AtlasOffset.X + FaceExtent.X, AtlasOffset.Y + FaceExtent.Y);
		ViewInfo.FOV = 90.0f;
		DestinationSlices.Add(FaceIndex);
	}

	if (ViewInfos.IsEmpty())
	{
		return false;
	}

	TUniquePtr<FWPSelectedCubeAtlasRenderTarget> AtlasTarget =
		MakeUnique<FWPSelectedCubeAtlasRenderTarget>(*CubeResource, FaceExtent);
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		AtlasTarget.Get(), Scene, CaptureComponent.ShowFlags)
		.SetResolveScene(!CaptureNeedsSceneColor(CaptureComponent.CaptureSource))
		.SetRealtimeUpdate(
			CaptureComponent.bCaptureEveryFrame
			|| CaptureComponent.bAlwaysPersistRenderingState));
	FSceneViewExtensionContext ViewExtensionContext(Scene);
	ViewFamily.ViewExtensions =
		GEngine->ViewExtensions->GatherActiveExtensions(ViewExtensionContext);
	ViewFamily.FrameNumber = Scene->GetFrameNumber();
	ViewFamily.FrameCounter = GFrameCounter;
	ViewFamily.bOverrideVirtualTextureThrottle =
		CaptureComponent.bOverrideVirtualTextureThrottle;
	if (CaptureNeedsSceneColor(CaptureComponent.CaptureSource))
	{
		ViewFamily.EngineShowFlags.PostProcessing = 0;
	}

	TArray<FSceneView*, TFixedAllocator<WPCubeCaptureFaceCount>> Views;
	Views.Reserve(ViewInfos.Num());
	for (int32 ViewIndex = 0; ViewIndex < ViewInfos.Num(); ++ViewIndex)
	{
		const FSceneCaptureViewInfo& ViewInfo = ViewInfos[ViewIndex];
		const int32 FaceIndex = DestinationSlices[ViewIndex];
		FSceneViewInitOptions ViewInitOptions;
		ViewInitOptions.SetViewRectangle(ViewInfo.ViewRect);
		ViewInitOptions.ViewFamily = &ViewFamily;
		ViewInitOptions.ViewActor = CaptureComponent.GetViewOwner();
		ViewInitOptions.ViewLocation = ViewInfo.ViewLocation;
		ViewInitOptions.ViewRotation = ViewInfo.ViewRotation;
		ViewInitOptions.ViewOrigin = ViewInfo.ViewOrigin;
		ViewInitOptions.ViewRotationMatrix = ViewInfo.ViewRotationMatrix;
		ViewInitOptions.BackgroundColor = FLinearColor::Black;
		ViewInitOptions.OverrideFarClippingPlaneDistance =
			CaptureComponent.MaxViewDistanceOverride;
		ViewInitOptions.StereoPass = ViewInfo.StereoPass;
		ViewInitOptions.StereoViewIndex = ViewInfo.StereoViewIndex;
		ViewInitOptions.ProjectionMatrix = ViewInfo.ProjectionMatrix;
		ViewInitOptions.SkylightScale = CaptureComponent.SkylightScale;
		ViewInitOptions.bIsSceneCapture = true;
		ViewInitOptions.bIsSceneCaptureCube = true;
		ViewInitOptions.FOV = ViewInfo.FOV;
		ViewInitOptions.DesiredFOV = ViewInfo.FOV;
		ViewInitOptions.SceneViewStateInterface = CaptureComponent.GetViewState(FaceIndex);
		ViewInitOptions.LODDistanceFactor = FMath::Clamp(
			CaptureComponent.LODDistanceFactor, 0.01f, 100.0f);
		ViewInitOptions.bSceneCaptureUsesRayTracing =
			CaptureComponent.bUseRayTracingIfEnabled;
		ViewInitOptions.bExcludeFromSceneTextureExtents =
			CaptureComponent.bExcludeFromSceneTextureExtents;
		if (World->GetWorldSettings())
		{
			ViewInitOptions.WorldToMetersScale =
				World->GetWorldSettings()->WorldToMeters;
		}
		if (CaptureNeedsSceneColor(CaptureComponent.CaptureSource))
		{
			ViewInitOptions.OverlayColor = FLinearColor::Black;
		}

		FSceneView* View = new FSceneView(ViewInitOptions);
		if (ViewIndex == 0)
		{
			View->bEyeAdaptationAllViewPixels = true;
		}
		GetShowOnlyAndHiddenComponents(
			CaptureComponent, View->HiddenPrimitives, View->ShowOnlyPrimitives);
		ViewFamily.Views.Add(View);
		Views.Add(View);
		View->StartFinalPostprocessSettings(ViewInfo.ViewOrigin);
		View->FinalPostProcessSettings.DynamicGlobalIlluminationMethod =
			EDynamicGlobalIlluminationMethod::None;
		View->FinalPostProcessSettings.ReflectionMethod = EReflectionMethod::None;
		View->FinalPostProcessSettings.LumenSurfaceCacheResolution = 0.5f;
		View->FinalPostProcessSettings.VignetteIntensity = 0.0f;
		View->FinalPostProcessSettings.LumenReflectionsScreenTraces = 0;
		View->FinalPostProcessSettings.LumenFinalGatherScreenTraces = 0;
		View->OverridePostProcessSettings(
			CaptureComponent.PostProcessSettings,
			CaptureComponent.PostProcessBlendWeight);
		View->EndFinalPostprocessSettings(ViewInitOptions);
		View->ViewLightingChannelMask =
			CaptureComponent.ViewLightingChannels.GetMaskForStruct();
	}

	ViewFamily.SceneCaptureSource = CaptureComponent.CaptureSource;
	ViewFamily.EngineShowFlags.ScreenPercentage = false;
	ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
		ViewFamily, 1.0f));
	for (const FSceneViewExtensionRef& Extension : ViewFamily.ViewExtensions)
	{
		Extension->SetupViewFamily(ViewFamily);
	}
	for (FSceneView* View : Views)
	{
		for (const FSceneViewExtensionRef& Extension : ViewFamily.ViewExtensions)
		{
			Extension->SetupView(ViewFamily, *View);
		}
	}

	FSceneRenderer* SceneRenderer =
		SceneRenderBuilder.CreateSceneRenderer(&ViewFamily);
	if (!SceneRenderer || SceneRenderer->Views.Num() != ViewInfos.Num())
	{
		WP_LOG(&CaptureComponent, Error,
			TEXT("[SelectedCubeBatch] SceneRenderer creation failed. Capture=%s SelectedMask=0x%02x SelectedFaces=%d SceneRenderer=%d RendererViews=%d ExpectedViews=%d ViewFamilies=1 SceneRenderers=%d GameThreadWait=0 BuildGTMs=%.4f"),
			*GetNameSafe(&CaptureComponent),
			static_cast<uint32>(SanitizedFaceMask), ViewInfos.Num(),
			SceneRenderer ? 1 : 0,
			SceneRenderer ? SceneRenderer->Views.Num() : 0,
			ViewInfos.Num(), SceneRenderer ? 1 : 0,
			(FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0);
		return false;
	}
	for (const int32 FaceIndex : DestinationSlices)
	{
		FSceneViewStateInterface* ViewStateInterface =
			CaptureComponent.GetViewState(FaceIndex);
		if (!ViewStateInterface)
		{
			continue;
		}
		const FFinalPostProcessSettings& FinalSettings =
			SceneRenderer->Views[DestinationSlices.Find(FaceIndex)]
				.FinalPostProcessSettings;
		if (FinalSettings.DynamicGlobalIlluminationMethod
				== EDynamicGlobalIlluminationMethod::Lumen
			|| FinalSettings.ReflectionMethod == EReflectionMethod::Lumen)
		{
			ViewStateInterface->AddLumenSceneData(
				Scene, FinalSettings.LumenSurfaceCacheResolution);
		}
		else
		{
			ViewStateInterface->RemoveLumenSceneData(Scene);
		}
	}

	SceneRenderBuilder.AddRenderer(
		SceneRenderer,
		FString(TEXT("WP.SelectedFaceSinglePass")),
		[AtlasTarget = MoveTemp(AtlasTarget), CubeResource, FaceExtent,
			DestinationSlices = MoveTemp(DestinationSlices), SanitizedFaceMask]
		(FRDGBuilder& GraphBuilder, const FSceneRenderFunctionInputs& Inputs)
		{
			const double RenderSetupStartSeconds = FPlatformTime::Seconds();
			RDG_EVENT_SCOPE(
				GraphBuilder, "WP.SelectedFaceSinglePass Mask=0x%02x Views=%d",
				static_cast<uint32>(SanitizedFaceMask), DestinationSlices.Num());
			AtlasTarget->InitRHI(GraphBuilder.RHICmdList);
			FRDGTextureRef AtlasTexture = RegisterExternalTexture(
				GraphBuilder, AtlasTarget->GetRenderTargetTexture(),
				TEXT("WP.SelectedCubeAtlas"));
			FRDGTextureRef CubeTexture = RegisterExternalTexture(
				GraphBuilder, CubeResource->TextureRHI,
				TEXT("WP.SelectedCubePersistentTarget"));
			AddClearRenderTargetPass(
				GraphBuilder, AtlasTexture, FLinearColor::Black,
				Inputs.Renderer->Views[0].UnscaledViewRect);
			Inputs.Renderer->Render(GraphBuilder, Inputs.SceneUpdateInputs);
			if (GetFeatureLevelShadingPath(
				Inputs.Renderer->Scene->GetFeatureLevel()) == EShadingPath::Mobile)
			{
				const FRenderTarget* FamilyTarget =
					Inputs.Renderer->ViewFamily.RenderTarget;
				FRDGTextureRef FamilyTexture = RegisterExternalTexture(
					GraphBuilder, FamilyTarget->GetRenderTargetTexture(),
					TEXT("WP.SelectedCubeMobileOutput"));
				const FMinimalSceneTextures& SceneTextures =
					Inputs.Renderer->GetActiveSceneTextures();
				CopySceneCaptureComponentToTarget(
					GraphBuilder, SceneTextures, FamilyTexture,
					Inputs.Renderer->ViewFamily, Inputs.Renderer->Views);
			}

			for (const int32 FaceIndex : DestinationSlices)
			{
				FRHICopyTextureInfo CopyInfo;
				CopyInfo.Size.X = FaceExtent.X;
				CopyInfo.Size.Y = FaceExtent.Y;
				CopyInfo.SourcePosition.X =
					WPCubeFaceViewportOffsets[FaceIndex][0] * FaceExtent.X;
				CopyInfo.SourcePosition.Y =
					WPCubeFaceViewportOffsets[FaceIndex][1] * FaceExtent.Y;
				CopyInfo.DestSliceIndex = FaceIndex;
				AddCopyTexturePass(
					GraphBuilder, AtlasTexture, CubeTexture, CopyInfo);
			}
			GraphBuilder.SetTextureAccessFinal(CubeTexture, ERHIAccess::SRVMask);
			UE_LOG(LogWormhole, VeryVerbose,
				TEXT("[RenderThread][SelectedCubeBatch] Graph staged. Frame=%u SelectedMask=0x%02x SelectedFaces=%d ViewFamilies=1 SceneRenderers=1 Views=%d Atlas=%dx%d Copies=%d UnselectedSlicesPreserved=1 GPUEvent=WP.SelectedFaceSinglePass RenderThreadWait=0 RenderSetupRTMs=%.4f"),
				GFrameNumberRenderThread,
				static_cast<uint32>(SanitizedFaceMask), DestinationSlices.Num(),
				Inputs.Renderer->Views.Num(),
				FaceExtent.X * WPCubeAtlasColumns,
				FaceExtent.Y * WPCubeAtlasRows,
				DestinationSlices.Num(),
				(FPlatformTime::Seconds() - RenderSetupStartSeconds) * 1000.0);
			return true;
		});

	WP_LOG(&CaptureComponent, Verbose,
		TEXT("[SelectedCubeBatch] Renderer staged. Frame=%llu Capture=%s Target=%s SelectedMask=0x%02x SelectedFaces=%d ViewFamilies=1 SceneRenderers=1 Views=%d Atlas=%dx%d DestinationSlicesPreserveIdentity=1 UnselectedSlicesPreserved=1 EngineModified=0 GameThreadWait=0 BuildGTMs=%.4f"),
		static_cast<unsigned long long>(GFrameCounter),
		*GetNameSafe(&CaptureComponent), *GetNameSafe(TextureTarget),
		static_cast<uint32>(SanitizedFaceMask),
		CountSelectedFaces(SanitizedFaceMask), ViewInfos.Num(),
		FaceExtent.X * WPCubeAtlasColumns,
		FaceExtent.Y * WPCubeAtlasRows,
		(FPlatformTime::Seconds() - BuildStartSeconds) * 1000.0);
	return true;
}

