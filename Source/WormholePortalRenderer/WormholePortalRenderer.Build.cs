// Copyright 2026 Team Beaver. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class WormholePortalRenderer : ModuleRules
{
	public WormholePortalRenderer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Projects",
				"RenderCore",
				"Renderer",
				"RHI",
				"WormholePortalRuntime"
			});

		// Selected Cubemap rendering is isolated in WPSelectedCubeCapture.cpp. It uses renderer-private
		// SceneRenderBuilder APIs without modifying Engine source files.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer", "Private"));
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer", "Internal"));
	}
}
