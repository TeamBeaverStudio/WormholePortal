// Copyright 2026 Team Beaver. All Rights Reserved.

#include "SWPTransitManagerWidget.h"

#include "SWPTransitManagerActorRow.h"
#include "SWPTransitManagerSettingsPanel.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "SWPTransitManagerWidget"

SWPTransitManagerWidget::~SWPTransitManagerWidget()
{
	if (PostUndoRedoHandle.IsValid())
	{
		FEditorDelegates::PostUndoRedo.Remove(PostUndoRedoHandle);
	}
}

void SWPTransitManagerWidget::Construct(const FArguments& InArgs)
{
	(void)InArgs;

	SAssignNew(SettingsPanel, SWPTransitManagerSettingsPanel);
	RebuildStatusFilterOptions();
	ActorController.RebuildActorEntries(false, MakeCheckOptions());
	ApplyFilters();
	PostUndoRedoHandle = FEditorDelegates::PostUndoRedo.AddRaw(this, &SWPTransitManagerWidget::HandlePostUndoRedo);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshActors", "Refresh Actors"))
				.OnClicked(this, &SWPTransitManagerWidget::RefreshActors)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CheckActors", "Check Actors"))
				.OnClicked(this, &SWPTransitManagerWidget::CheckActors)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("ApplyTransitToReadyActors", "Apply Transit to Ready Actors"))
				.OnClicked(this, &SWPTransitManagerWidget::ApplyReadyActors)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Settings", "Settings"))
				.OnClicked(this, &SWPTransitManagerWidget::OpenSettingsPopup)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f, 0.f, 6.f, 6.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("NameFilterHint", "Name Filter: Actor Label or Actor Name"))
				.OnTextChanged(this, &SWPTransitManagerWidget::OnNameFilterTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StatusFilterLabel", "Status Filter"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SComboBox<TSharedPtr<EWPTransitManagerStatusFilter>>)
				.OptionsSource(&StatusFilterOptions)
				.InitiallySelectedItem(SelectedStatusFilterOption)
				.OnSelectionChanged(this, &SWPTransitManagerWidget::OnStatusFilterSelectionChanged)
				.OnGenerateWidget_Lambda([this](TSharedPtr<EWPTransitManagerStatusFilter> Option)
				{
					return SNew(STextBlock)
						.Text(Option.IsValid() ? GetStatusFilterText(*Option) : FText::GetEmpty());
				})
				[
					SNew(STextBlock)
					.Text(this, &SWPTransitManagerWidget::GetSelectedStatusFilterText)
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		.Padding(6.f, 0.f, 6.f, 6.f)
		[
			SAssignNew(ActorListView, SListView<FWPTransitManagerActorEntryPtr>)
			.ListItemsSource(&FilteredActorEntries)
			.OnGenerateRow(this, &SWPTransitManagerWidget::GenerateActorRow)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(TEXT("Transit"))
				.DefaultLabel(LOCTEXT("TransitColumn", "Transit"))
				.FixedWidth(72.f)
				+ SHeaderRow::Column(TEXT("ActorLabel"))
				.DefaultLabel(LOCTEXT("ActorLabelColumn", "Actor Label"))
				.FillWidth(0.45f)
				+ SHeaderRow::Column(TEXT("Status"))
				.DefaultLabel(LOCTEXT("StatusColumn", "Status"))
				.FillWidth(0.55f)
			)
		]
	];
}

void SWPTransitManagerWidget::SetTransitChecked(FWPTransitManagerActorEntryPtr Entry, ECheckBoxState NewState)
{
	ActorController.SetTransitEnabled(Entry, NewState == ECheckBoxState::Checked, MakeCheckOptions());
	
	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
}

FReply SWPTransitManagerWidget::RefreshActors()
{
	ActorController.RebuildActorEntries(false, MakeCheckOptions());
	
	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
	return FReply::Handled();
}

FReply SWPTransitManagerWidget::CheckActors()
{
	const FWPTransitManagerOptions Options = MakeCheckOptions();
	
	ActorController.CheckAll(Options);
	
	if (SettingsPanel.IsValid() && SettingsPanel->ShouldAutoApplyToReadyActors())
	{
		ActorController.ApplyReadyActors(Options);
	}

	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
	
	return FReply::Handled();
}

FReply SWPTransitManagerWidget::ApplyReadyActors()
{
	ActorController.ApplyReadyActors(MakeCheckOptions());
	
	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
	
	return FReply::Handled();
}

FReply SWPTransitManagerWidget::OpenSettingsPopup()
{
	if (!SettingsPanel.IsValid())
	{
		return FReply::Handled();
	}

	FSlateApplication::Get().PushMenu(
		AsShared(),
		FWidgetPath(),
		SettingsPanel.ToSharedRef(),
		FSlateApplication::Get().GetCursorPos(),
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));

	return FReply::Handled();
}

void SWPTransitManagerWidget::RebuildStatusFilterOptions()
{
	StatusFilterOptions.Reset();
	StatusFilterOptions.Add(MakeShared<EWPTransitManagerStatusFilter>(EWPTransitManagerStatusFilter::All));
	StatusFilterOptions.Add(MakeShared<EWPTransitManagerStatusFilter>(EWPTransitManagerStatusFilter::Passed));
	StatusFilterOptions.Add(MakeShared<EWPTransitManagerStatusFilter>(EWPTransitManagerStatusFilter::NotSupported));
	StatusFilterOptions.Add(MakeShared<EWPTransitManagerStatusFilter>(EWPTransitManagerStatusFilter::NeedsSetup));
	SelectedStatusFilterOption = StatusFilterOptions[0];
}

void SWPTransitManagerWidget::ApplyFilters()
{
	FilteredActorEntries.Reset();

	const FString NameFilterString = NameFilterText.ToString();
	const EWPTransitManagerStatusFilter StatusFilter = SelectedStatusFilterOption.IsValid()
		? *SelectedStatusFilterOption
		: EWPTransitManagerStatusFilter::All;

	for (const FWPTransitManagerActorEntryPtr& Entry : ActorController.GetActorEntries())
	{
		AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
		if (!IsValid(Actor))
		{
			continue;
		}

		if (!NameFilterString.IsEmpty() &&
			!Actor->GetActorLabel().Contains(NameFilterString, ESearchCase::IgnoreCase) &&
			!Actor->GetName().Contains(NameFilterString, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (StatusFilter == EWPTransitManagerStatusFilter::Passed &&
			(!Entry->bWasChecked || Entry->ResolveResult.Status != EWPTransitResolveStatus::Passed))
		{
			continue;
		}

		if (StatusFilter == EWPTransitManagerStatusFilter::NotSupported &&
			(!Entry->bWasChecked || Entry->ResolveResult.Status != EWPTransitResolveStatus::NotSupported))
		{
			continue;
		}

		if (StatusFilter == EWPTransitManagerStatusFilter::NeedsSetup &&
			(!Entry->bWasChecked || Entry->ResolveResult.Status != EWPTransitResolveStatus::NeedsSetup))
		{
			continue;
		}

		FilteredActorEntries.Add(Entry);
	}
}

FWPTransitManagerOptions SWPTransitManagerWidget::MakeCheckOptions() const
{
	return SettingsPanel.IsValid()
		? SettingsPanel->MakeCheckOptions()
		: FWPTransitManagerOptions();
}

FText SWPTransitManagerWidget::GetSelectedStatusFilterText() const
{
	return SelectedStatusFilterOption.IsValid()
		? GetStatusFilterText(*SelectedStatusFilterOption)
		: GetStatusFilterText(EWPTransitManagerStatusFilter::All);
}

FText SWPTransitManagerWidget::GetStatusFilterText(EWPTransitManagerStatusFilter StatusFilter) const
{
	switch (StatusFilter)
	{
	case EWPTransitManagerStatusFilter::All:				return LOCTEXT("StatusFilterAll", "All");
	case EWPTransitManagerStatusFilter::Passed:			return LOCTEXT("StatusFilterReady", "Ready");
	case EWPTransitManagerStatusFilter::NotSupported:	return LOCTEXT("StatusFilterNotSupported", "Not Supported");
	case EWPTransitManagerStatusFilter::NeedsSetup:	return LOCTEXT("StatusFilterNeedsSetup", "Needs Setup");
	default:												return FText::GetEmpty();
	}
}

TSharedRef<ITableRow> SWPTransitManagerWidget::GenerateActorRow(FWPTransitManagerActorEntryPtr Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SWPTransitManagerActorRow, OwnerTable)
		.Entry(Entry)
		.OwnerWidget(this);
}

void SWPTransitManagerWidget::OnStatusFilterSelectionChanged(TSharedPtr<EWPTransitManagerStatusFilter> NewSelection, ESelectInfo::Type SelectInfo)
{
	(void)SelectInfo;

	if (NewSelection.IsValid())
	{
		SelectedStatusFilterOption = NewSelection;
		
		ApplyFilters();
		
		if (ActorListView.IsValid())
		{
			ActorListView->RequestListRefresh();
		}
	}
}

void SWPTransitManagerWidget::OnNameFilterTextChanged(const FText& NewText)
{
	NameFilterText = NewText;
	
	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
}

void SWPTransitManagerWidget::HandlePostUndoRedo()
{
	ActorController.RebuildActorEntries(false, MakeCheckOptions());
	
	ApplyFilters();
	
	if (ActorListView.IsValid())
	{
		ActorListView->RequestListRefresh();
	}
}

#undef LOCTEXT_NAMESPACE
