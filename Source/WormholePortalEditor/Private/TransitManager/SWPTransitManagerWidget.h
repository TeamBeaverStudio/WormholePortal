// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WPTransitManagerActorController.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
class SWPTransitManagerSettingsPanel;
template <typename ItemType> class SListView;

/**
 * @brief Filter that selects which actor status to display in the Transit Manager list.
 *
 * Maps shared Runtime resolve results to the widget's status-filter options.
 */
enum class EWPTransitManagerStatusFilter : uint8
{
	
	All,			// Displays all actors.
	Passed,		// Displays actors with Passed resolve status, shown as Ready in the UI.
	NotSupported,	// Displays actors resolved as NotSupported because they are system actors or match an exclusion setting.
	NeedsSetup		// Displays supported actors resolved as NeedsSetup because required Transit conditions are not met.
};

/**
 * @brief Manages the Transit Manager's main UI and actor-list filters.
 *
 * FWPTransitManagerActorController handles actor checks and Component changes.
 * SWPTransitManagerSettingsPanel handles settings editing and persistence.
 */
class SWPTransitManagerWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWPTransitManagerWidget) {}
	SLATE_END_ARGS()

	/** @brief Unregisters the Undo/Redo callback and cleans up the widget. */
	virtual ~SWPTransitManagerWidget() override;

	/**
	 * @brief Loads actors from the current Editor World and constructs the Transit Manager
	 *        UI.
	 * @param InArgs Slate arguments supplied when the widget is created.
	 */
	void Construct(const FArguments& InArgs);

	/**
	 * @brief Changes Transit Component application state to match an actor row's checkbox.
	 * @param Entry Actor entry whose checkbox was changed by the user.
	 * @param NewState New checkbox state.
	 */
	void SetTransitChecked(FWPTransitManagerActorEntryPtr Entry, ECheckBoxState NewState);

private:
	/** @brief Actor entries that pass the name and status filters and are displayed in the list. */
	TArray<FWPTransitManagerActorEntryPtr> FilteredActorEntries;

	/** @brief Actor list widget that displays FilteredActorEntries. */
	TSharedPtr<SListView<FWPTransitManagerActorEntryPtr>> ActorListView;

	/** @brief Controller responsible for actor collection, checks, and Component changes. */
	FWPTransitManagerActorController ActorController;

	/** @brief Panel that edits and holds the Transit Manager's current check and auto-apply settings. */
	TSharedPtr<SWPTransitManagerSettingsPanel> SettingsPanel;

	/** @brief Handle for the callback that refreshes the actor list after Undo or Redo. */
	FDelegateHandle PostUndoRedoHandle;

	/** @brief Available status-filter options for the actor list. */
	TArray<TSharedPtr<EWPTransitManagerStatusFilter>> StatusFilterOptions;

	/** @brief Status-filter option currently selected by the user. */
	TSharedPtr<EWPTransitManagerStatusFilter> SelectedStatusFilterOption;

	/** @brief Current search text applied to Actor Label and actor name. */
	FText NameFilterText;

	/**
	 * @brief Rebuilds the actor list from the current Editor World without resolve results.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply RefreshActors();

	/**
	 * @brief Checks every actor's Transit conditions and automatically applies Transit to
	 *        Ready actors when configured.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply CheckActors();

	/**
	 * @brief Applies a Transit Component to actors that were Ready in the latest check and
	 *        still pass the pre-apply recheck.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply ApplyReadyActors();

	/**
	 * @brief Opens the Settings popup used for Transit checks and application.
	 * @return FReply indicating that the button input was handled.
	 */
	FReply OpenSettingsPopup();

	/** @brief Builds the actor status-filter combo-box options and selects All. */
	void RebuildStatusFilterOptions();

	/** @brief Applies the current name and status filters to all actor entries. */
	void ApplyFilters();

	/**
	 * @brief Builds resolve options from the current SettingsPanel values.
	 * @return Resolve options corresponding to the current settings.
	 */
	FWPTransitManagerOptions MakeCheckOptions() const;

	/** @brief Returns the display text for the currently selected actor status filter. */
	FText GetSelectedStatusFilterText() const;

	/**
	 * @brief Converts an actor status filter to user-facing display text.
	 * @param StatusFilter Status filter to describe.
	 * @return Display text for the status filter.
	 */
	FText GetStatusFilterText(EWPTransitManagerStatusFilter StatusFilter) const;

	/**
	 * @brief Generates a multi-column row widget for the actor list.
	 * @param Entry Actor entry displayed by the row.
	 * @param OwnerTable Table view that owns the generated row.
	 * @return Table row that displays the actor entry.
	 */
	TSharedRef<ITableRow> GenerateActorRow(FWPTransitManagerActorEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable);

	/**
	 * @brief Refilters the displayed actor list when the status-filter selection changes.
	 * @param NewSelection Newly selected status-filter option.
	 * @param SelectInfo Input method that triggered the selection change.
	 */
	void OnStatusFilterSelectionChanged(TSharedPtr<EWPTransitManagerStatusFilter> NewSelection, ESelectInfo::Type SelectInfo);

	/**
	 * @brief Refilters the displayed actor list when the name search text changes.
	 * @param NewText New search text applied to Actor Label and actor name.
	 */
	void OnNameFilterTextChanged(const FText& NewText);

	/** @brief Rebuilds the actor list from the actual Level state after Undo or Redo. */
	void HandlePostUndoRedo();
};
