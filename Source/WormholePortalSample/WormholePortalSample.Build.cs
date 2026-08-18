// Copyright 2026 Team Beaver. All Rights Reserved.

using UnrealBuildTool;

public class WormholePortalSample : ModuleRules
{
	public WormholePortalSample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EnhancedInput",
			"WormholePortalRuntime"
		});
	}
}
