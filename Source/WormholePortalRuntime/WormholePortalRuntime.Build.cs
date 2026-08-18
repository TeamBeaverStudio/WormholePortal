// Copyright 2026 Team Beaver. All Rights Reserved.

using UnrealBuildTool;

public class WormholePortalRuntime : ModuleRules
{
	public WormholePortalRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Chaos",
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"PhysicsCore"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ChaosSolverEngine",
				"RenderCore",
				"RHI"
			}
			);
	}
}

