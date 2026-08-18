// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WormholePortalRenderer.h"

#include "Engine/Engine.h"
#include "Rendering/IWPRenderer.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Rendering/WormholeLayer/WPWormholeLayerRenderPass.h"
#include "ShaderCore.h"
#include "WPRendererService.h"
#include "WPLog.h"

namespace
{
	const TCHAR* const WPShaderVirtualDirectory = TEXT("/Plugin/WormholePortal");

	FString NormalizeWPShaderDirectory(const FString& Directory)
	{
		FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(NormalizedDirectory);
		return NormalizedDirectory;
	}

	bool AreWPShaderDirectoriesEquivalent(const FString& Left, const FString& Right)
	{
		const FString NormalizedLeft = NormalizeWPShaderDirectory(Left);
		const FString NormalizedRight = NormalizeWPShaderDirectory(Right);
#if PLATFORM_WINDOWS
		return NormalizedLeft.Equals(NormalizedRight, ESearchCase::IgnoreCase);
#else
		return NormalizedLeft.Equals(NormalizedRight, ESearchCase::CaseSensitive);
#endif
	}

	void RegisterWPShaderDirectoryMapping()
	{
		if (FPlatformProperties::RequiresCookedData())
		{
			return;
		}

		// Log-only: Records the CPU time spent registering the shader directory mapping.
		const double StartSeconds = FPlatformTime::Seconds();

		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(TEXT("WormholePortal"));
		if (!Plugin.IsValid())
		{
			WP_LOG(nullptr, Error,
				TEXT("[GameThread][RendererModule][ShaderMapping] Registration failed. VirtualDirectory=%s Plugin=WormholePortal Reason=PluginDescriptorNotFound CpuMs=%.3f"),
				WPShaderVirtualDirectory,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			return;
		}

		const FString ShaderDirectory = NormalizeWPShaderDirectory(
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		if (!FPaths::DirectoryExists(ShaderDirectory))
		{
			WP_LOG(nullptr, Error,
				TEXT("[GameThread][RendererModule][ShaderMapping] Registration failed. VirtualDirectory=%s RealDirectory=%s Reason=ShaderDirectoryMissing CpuMs=%.3f"),
				WPShaderVirtualDirectory, *ShaderDirectory,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			return;
		}

		if (const FString* ExistingDirectory =
			AllShaderSourceDirectoryMappings().Find(WPShaderVirtualDirectory))
		{
			const bool bSameDirectory =
				AreWPShaderDirectoriesEquivalent(*ExistingDirectory, ShaderDirectory);
			if (bSameDirectory)
			{
#if !UE_BUILD_SHIPPING
				WP_LOG(nullptr, Verbose,
					TEXT("[GameThread][RendererModule][ShaderMapping] Registration reused. VirtualDirectory=%s RealDirectory=%s Action=AlreadyRegistered Conflict=0 CpuMs=%.3f"),
					WPShaderVirtualDirectory, *ShaderDirectory,
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
			}
			else
			{
				WP_LOG(nullptr, Error,
					TEXT("[GameThread][RendererModule][ShaderMapping] Registration conflict preserved without overwrite. VirtualDirectory=%s RequestedRealDirectory=%s ExistingRealDirectory=%s Action=ConflictPreserved Conflict=1 CpuMs=%.3f"),
					WPShaderVirtualDirectory, *ShaderDirectory, **ExistingDirectory,
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			}
			return;
		}

		AddShaderSourceDirectoryMapping(WPShaderVirtualDirectory, ShaderDirectory);
		const FString* RegisteredDirectory =
			AllShaderSourceDirectoryMappings().Find(WPShaderVirtualDirectory);
		if (RegisteredDirectory
			&& AreWPShaderDirectoriesEquivalent(*RegisteredDirectory, ShaderDirectory))
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(nullptr, Verbose,
				TEXT("[GameThread][RendererModule][ShaderMapping] Registration completed. VirtualDirectory=%s RealDirectory=%s Action=Registered Conflict=0 CpuMs=%.3f"),
				WPShaderVirtualDirectory, *ShaderDirectory,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		else
		{
#if !UE_BUILD_SHIPPING
			WP_LOG(nullptr, Verbose,
				TEXT("[GameThread][RendererModule][ShaderMapping] Registration request was not retained by RenderCore. VirtualDirectory=%s RealDirectory=%s Action=Skipped Reason=ShaderCompilationDisabled CpuMs=%.3f"),
				WPShaderVirtualDirectory, *ShaderDirectory,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
	}
}

void FWormholePortalRendererModule::StartupModule()
{
#if !UE_BUILD_SHIPPING
	// Log-only: Records the renderer module startup CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	RegisterWPShaderDirectoryMapping();
	RendererService = MakeUnique<FWPRendererService>();
	IModularFeatures::Get().RegisterModularFeature(
		IWPRenderer::GetModularFeatureName(), RendererService.Get());
	if (GEngine)
	{
		RegisterWormholeLayerRenderPassViewExtension();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FWormholePortalRendererModule::HandlePostEngineInit);
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(nullptr, Verbose,
		TEXT("[GameThread][RendererModule] Started. ServiceId=%llu Feature=%s CpuMs=%.3f"),
		RendererService->GetServiceId(), *IWPRenderer::GetModularFeatureName().ToString(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void FWormholePortalRendererModule::ShutdownModule()
{
#if !UE_BUILD_SHIPPING
	// Log-only: Records the renderer module shutdown CPU time.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	ShutdownWPWormholeLayerRenderPassViewExtension_GameThread();
	WormholeLayerRenderPassViewExtension.Reset();
	if (RendererService)
	{
		IModularFeatures::Get().UnregisterModularFeature(
			IWPRenderer::GetModularFeatureName(), RendererService.Get());
		RendererService->Shutdown();
		RendererService.Reset();
	}
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(nullptr, Verbose,
		TEXT("[GameThread][RendererModule] Stopped. Feature=%s CpuMs=%.3f"),
		*IWPRenderer::GetModularFeatureName().ToString(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void FWormholePortalRendererModule::HandlePostEngineInit()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RegisterWormholeLayerRenderPassViewExtension();
}

void FWormholePortalRendererModule::RegisterWormholeLayerRenderPassViewExtension()
{
	if (WormholeLayerRenderPassViewExtension.IsValid() || !RendererService) return;
	WormholeLayerRenderPassViewExtension = CreateWPWormholeLayerRenderPassViewExtension(*RendererService);
}

IMPLEMENT_MODULE(FWormholePortalRendererModule, WormholePortalRenderer)
