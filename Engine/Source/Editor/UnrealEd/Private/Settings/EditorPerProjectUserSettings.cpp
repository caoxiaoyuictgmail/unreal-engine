// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "Editor/EditorPerProjectUserSettings.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "UnrealEdMisc.h"
#include "BlueprintPaletteFavorites.h"

#define LOCTEXT_NAMESPACE "EditorPerProjectUserSettings"

UEditorPerProjectUserSettings::UEditorPerProjectUserSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	//Default to high quality
	MaterialQualityLevel = 1;
	bMonitorEditorPerformance = true;
	BlueprintFavorites = CreateDefaultSubobject<UBlueprintPaletteFavorites>(TEXT("BlueprintFavorites"));
	SCSViewportCameraSpeed = 4;
	AssetViewerProfileIndex = 0;

	SimplygonServerIP = "127.0.0.1";
	SimplygonSwarmDelay = 5000;
	bEnableSwarmDebugging = false;
	SwarmNumOfConcurrentJobs = 16;
	SwarmMaxUploadChunkSizeInMB = 100;
	SwarmIntermediateFolder = FPaths::ConvertRelativePathToFull(FPaths::GameIntermediateDir() + TEXT("Simplygon/"));
}

void UEditorPerProjectUserSettings::PostInitProperties()
{
	Super::PostInitProperties();

	//Ensure the material quality cvar is set to the settings loaded.
	static IConsoleVariable* MaterialQualityLevelVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaterialQualityLevel"));
	MaterialQualityLevelVar->Set(MaterialQualityLevel, ECVF_SetByScalability);
}

void UEditorPerProjectUserSettings::PostEditChangeProperty( FPropertyChangedEvent& PropertyChangedEvent )
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Name = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	if (Name == FName(TEXT("bUseCurvesForDistributions")))
	{
		extern ENGINE_API uint32 GDistributionType;
		//GDistributionType == 0 for curves
		GDistributionType = (bUseCurvesForDistributions) ? 0 : 1;
	}

	if (!FUnrealEdMisc::Get().IsDeletePreferences())
	{
		SaveConfig();
	}

	UserSettingChangedEvent.Broadcast(Name);
}


#undef LOCTEXT_NAMESPACE
