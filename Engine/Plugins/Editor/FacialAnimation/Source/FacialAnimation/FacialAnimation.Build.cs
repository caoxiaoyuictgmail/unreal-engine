// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class FacialAnimation : ModuleRules
	{
		public FacialAnimation(TargetInfo Target)
		{			
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"InputCore",
					"Engine",
                }
			);
		}
	}
}