// Copyright 2026 Team Beaver. All Rights Reserved.

using UnrealBuildTool;

public class WormholePortalEditor : ModuleRules
{
    public WormholePortalEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "WormholePortalRuntime",
                "Slate",
                "SlateCore",
                "UnrealEd",
                "AssetRegistry",
                "Settings",
                "DeveloperSettings",
                "DeveloperToolSettings",
                "SharedSettingsWidgets",
                "Projects",
                "EditorFramework",
                "InputCore",
                "LevelEditor",
                "ToolMenus",
                "PropertyEditor",
                "ClassViewer",
            }
        );
    }
}
