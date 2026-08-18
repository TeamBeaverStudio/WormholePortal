// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WPTransitManagerActorController.h"
#include "UObject/SoftObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
template <typename ItemType> class SListView;

/** @brief Data item for one excluded-class row in Transit Manager Settings. */
struct FWPTransitManagerExcludedClassEntry
{
	/** @brief Soft reference to an Actor class excluded from Transit candidacy. */
	TSoftClassPtr<AActor> Class;
};

/**
 * @brief Settings popup for editing Transit Manager check and auto-apply behavior.
 *
 * Loads and saves per-project user settings and builds resolve options from the values
 * currently being edited.
 */
class SWPTransitManagerSettingsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWPTransitManagerSettingsPanel) {}
	SLATE_END_ARGS()

	/**
	 * @brief Loads the saved settings and constructs the Settings popup.
	 * @param InArgs Slate arguments supplied when the widget is created.
	 */
	void Construct(const FArguments& InArgs);

	/**
	 * @brief Returns whether Transit should be applied automatically to Ready actors
	 *        immediately after Check Actors.
	 * @return true when automatic application is enabled.
	 */
	bool ShouldAutoApplyToReadyActors() const;

	/**
	 * @brief Builds Transit resolve options from the current excluded-class list.
	 * @return Resolve options for the Transit Manager.
	 */
	FWPTransitManagerOptions MakeCheckOptions() const;

private:
	/** @brief Shared-pointer alias for an excluded-class list entry. */
	using FExcludedClassEntryPtr = TSharedPtr<FWPTransitManagerExcludedClassEntry>;

	/** @brief Whether to apply Transit automatically to Ready actors immediately after Check Actors. */
	bool bAutoApplyToReadyActors = false;

	/** @brief Class temporarily selected in the class picker for addition to the excluded list. */
	UClass* PendingExcludedClass = nullptr;

	/** @brief Current excluded-class entries, initialized from the saved settings. */
	TArray<FExcludedClassEntryPtr> ExcludedClassEntries;

	/** @brief List widget that displays ExcludedClassEntries in the Settings UI. */
	TSharedPtr<SListView<FExcludedClassEntryPtr>> ExcludedClassListView;

	/**
	 * @brief Saves the current values as per-project user defaults.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply SaveSettingsAsDefault();

	/**
	 * @brief Adds the Actor class selected in the class picker to the excluded list without
	 *        duplicates.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply AddPendingExcludedClass();

	/** @brief Loads the per-project user settings into the current values and excluded-class list. */
	void LoadSettings();

	/** @brief Saves the current values and excluded-class list to the per-project user settings. */
	void SaveSettings();

	/**
	 * @brief Removes the specified entry from the excluded-class list.
	 * @param Entry Excluded-class entry to remove.
	 */
	void RemoveExcludedClass(const FExcludedClassEntryPtr& Entry);

	/**
	 * @brief Generates a row widget for the excluded-class list.
	 * @param Entry Excluded-class entry displayed by the row.
	 * @param OwnerTable Table view that owns the generated row.
	 * @return Table row containing the excluded class and a remove button.
	 */
	TSharedRef<ITableRow> GenerateExcludedClassRow(FExcludedClassEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable);

	/**
	 * @brief Supplies the class currently selected as pending to the excluded-class picker.
	 * @return Selected UClass, or nullptr when no class is selected.
	 */
	const UClass* GetPendingExcludedClass() const;

	/**
	 * @brief Stores the class selected in the excluded-class picker as the pending value.
	 * @param NewClass Actor class newly selected by the user.
	 */
	void OnPendingExcludedClassChanged(const UClass* NewClass);
};
