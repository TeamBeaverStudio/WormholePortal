// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "WPTransitManagerEditorSettings.generated.h"

class AActor;


/**
 * @brief Stores actor-check conditions used by the Transit Manager widget.
 *
 * Persists values edited in the SWPTransitManagerWidget Settings popup as per-project
 * user settings.
 * The widget reloads these values when reopened; they are not part of Runtime game
 * configuration.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class UWPTransitManagerEditorSettings : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * @brief Constructs the settings object and initializes the default excluded-class
	 *        list.
	 */
	UWPTransitManagerEditorSettings();
	
	/**
	 * @brief Controls whether Transit is applied automatically to Ready actors immediately
	 *        after Check Actors.
	 *
	 * When false, only resolve results are displayed; the user must invoke Apply Transit to
	 * Ready Actors to modify actors.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Check Behavior")
	bool bAutoApplyToReadyActors = false;

	/**
	 * @brief Stores Actor classes excluded from Transit candidacy.
	 *
	 * Actors of a listed class or any subclass resolve as NotSupported and are displayed as
	 * Not Supported in the UI.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Excluded Classes")
	TArray<TSoftClassPtr<AActor>> ExcludedClasses;
};
