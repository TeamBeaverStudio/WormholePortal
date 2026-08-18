// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class AActor;
class AWormholePortalActor;
class ULightComponent;
class UWorld;

enum class EWPPortalLightInspectorItemType : uint8
{
	Portal,
	Light
};

/** Read-only view model for one Portal or Light row in the Portal Light Inspector. */
struct FWPPortalLightInspectorItem
{
	EWPPortalLightInspectorItemType Type = EWPPortalLightInspectorItemType::Portal;
	TWeakObjectPtr<AWormholePortalActor> Portal;
	TWeakObjectPtr<ULightComponent> Light;
	uint32 Revision = 0;
	TArray<TSharedPtr<FWPPortalLightInspectorItem>> Children;
};

using FWPPortalLightInspectorItemPtr = TSharedPtr<FWPPortalLightInspectorItem>;

/**
 * Displays UWPPortalLightCollectionSubsystem results for a PIE/SIE World.
 * This widget reads only the Runtime Subsystem snapshot and does not recompute
 * influence relationships.
 */
class SWPPortalLightInspectorWidget final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWPPortalLightInspectorWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(
		const FGeometry& AllottedGeometry,
		double InCurrentTime,
		float InDeltaTime) override;

private:
	UWorld* ResolveInspectionWorld() const;
	void RebuildTree();

	FReply HandleRefreshClicked();
	FReply HandleExpandAllClicked();
	FReply HandleCollapseAllClicked();

	void HandleAutoRefreshChanged(ECheckBoxState NewState);
	ECheckBoxState GetAutoRefreshCheckState() const;

	TSharedRef<ITableRow> GenerateRow(
		FWPPortalLightInspectorItemPtr Item,
		const TSharedRef<STableViewBase>& OwnerTable) const;

	void GetTreeChildren(
		FWPPortalLightInspectorItemPtr Item,
		TArray<FWPPortalLightInspectorItemPtr>& OutChildren) const;

	void HandleSelectionChanged(
		FWPPortalLightInspectorItemPtr Item,
		ESelectInfo::Type SelectInfo) const;

	void HandleItemDoubleClicked(FWPPortalLightInspectorItemPtr Item) const;
	void HandleExpansionChanged(FWPPortalLightInspectorItemPtr Item, bool bExpanded);

	FText GetStatusText() const;
	static AActor* ResolveItemActor(const FWPPortalLightInspectorItemPtr& Item);

private:
	TArray<FWPPortalLightInspectorItemPtr> RootItems;
	TSharedPtr<STreeView<FWPPortalLightInspectorItemPtr>> TreeView;

	TSet<TObjectKey<AWormholePortalActor>> KnownPortalKeys;
	TSet<TObjectKey<AWormholePortalActor>> ExpandedPortalKeys;

	FText StatusText;
	double LastAutoRefreshSeconds = 0.0;
	bool bAutoRefresh = true;
};
