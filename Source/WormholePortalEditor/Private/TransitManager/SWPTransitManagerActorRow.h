// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WPTransitManagerActorController.h"
#include "Widgets/Views/STableRow.h"

class SWPTransitManagerWidget;

/**
 * @brief Multi-column list row for one Transit Manager actor entry.
 *
 * Displays Transit Component state, Actor Label, and the latest resolve result.
 * Forwards Transit checkbox input to the owning SWPTransitManagerWidget.
 */
class SWPTransitManagerActorRow : public SMultiColumnTableRow<FWPTransitManagerActorEntryPtr>
{
public:
	SLATE_BEGIN_ARGS(SWPTransitManagerActorRow) {}
		SLATE_ARGUMENT(FWPTransitManagerActorEntryPtr, Entry)
		SLATE_ARGUMENT(SWPTransitManagerWidget*, OwnerWidget)
	SLATE_END_ARGS()

	/**
	 * @brief Stores the actor entry and owning widget, then constructs the multi-column
	 *        list row.
	 * @param InArgs Slate arguments containing the actor entry and the owner that handles
	 *               row input.
	 * @param OwnerTable Table view that owns the generated row.
	 */
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable);

	/**
	 * @brief Generates the Transit checkbox or actor-status text for the requested column.
	 * @param ColumnName Name of the column to generate.
	 * @return Slate widget displayed in that column.
	 */
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	/** @brief Actor entry displayed by this row. */
	FWPTransitManagerActorEntryPtr Entry;

	/** @brief Transit Manager widget that handles Transit checkbox input. */
	SWPTransitManagerWidget* OwnerWidget = nullptr;
};
