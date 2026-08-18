// Copyright 2026 Team Beaver. All Rights Reserved.

#include "SWPTransitManagerSettingsPanel.h"

#include "GameFramework/Actor.h"
#include "PropertyCustomizationHelpers.h"
#include "WPTransitManagerEditorSettings.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SWPTransitManagerSettingsPanel"

void SWPTransitManagerSettingsPanel::Construct(const FArguments& InArgs)
{
	(void)InArgs;

	LoadSettings();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(10.f)
		[
			SNew(SBox)
			.WidthOverride(480.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						return bAutoApplyToReadyActors
							? ECheckBoxState::Checked
							: ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						bAutoApplyToReadyActors = NewState == ECheckBoxState::Checked;
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AutoApplyTransitToReadyActors", "Automatically Apply Transit to Ready Actors"))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExcludedClassesLabel", "Excluded Classes"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.Padding(0.f, 0.f, 4.f, 0.f)
					[
						SNew(SClassPropertyEntryBox)
						.MetaClass(AActor::StaticClass())
						.SelectedClass(this, &SWPTransitManagerSettingsPanel::GetPendingExcludedClass)
						.OnSetClass(this, &SWPTransitManagerSettingsPanel::OnPendingExcludedClassChanged)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("AddExcludedClass", "Add"))
						.OnClicked(this, &SWPTransitManagerSettingsPanel::AddPendingExcludedClass)
					]
				]
				+ SVerticalBox::Slot()
				.MaxHeight(140.f)
				.Padding(0.f, 0.f, 0.f, 8.f)
				[
					SAssignNew(ExcludedClassListView, SListView<FExcludedClassEntryPtr>)
					.ListItemsSource(&ExcludedClassEntries)
					.OnGenerateRow(this, &SWPTransitManagerSettingsPanel::GenerateExcludedClassRow)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				[
					SNew(SButton)
					.Text(LOCTEXT("SaveAsDefault", "Save as Default"))
					.OnClicked(this, &SWPTransitManagerSettingsPanel::SaveSettingsAsDefault)
				]
			]
		]
	];
}

bool SWPTransitManagerSettingsPanel::ShouldAutoApplyToReadyActors() const
{
	return bAutoApplyToReadyActors;
}

FWPTransitManagerOptions SWPTransitManagerSettingsPanel::MakeCheckOptions() const
{
	FWPTransitManagerOptions Options;
	for (const FExcludedClassEntryPtr& Entry : ExcludedClassEntries)
	{
		if (!Entry.IsValid() || Entry->Class.IsNull())
		{
			continue;
		}

		if (UClass* ExcludedClass = Entry->Class.LoadSynchronous())
		{
			Options.ExcludedClasses.Add(ExcludedClass);
		}
	}
	return Options;
}

FReply SWPTransitManagerSettingsPanel::SaveSettingsAsDefault()
{
	SaveSettings();
	return FReply::Handled();
}

FReply SWPTransitManagerSettingsPanel::AddPendingExcludedClass()
{
	if (!IsValid(PendingExcludedClass))
	{
		return FReply::Handled();
	}

	for (const FExcludedClassEntryPtr& Entry : ExcludedClassEntries)
	{
		if (Entry.IsValid() && Entry->Class.Get() == PendingExcludedClass)
		{
			PendingExcludedClass = nullptr;
			return FReply::Handled();
		}
	}

	FExcludedClassEntryPtr NewEntry = MakeShared<FWPTransitManagerExcludedClassEntry>();
	NewEntry->Class = PendingExcludedClass;
	ExcludedClassEntries.Add(NewEntry);
	PendingExcludedClass = nullptr;

	if (ExcludedClassListView.IsValid())
	{
		ExcludedClassListView->RequestListRefresh();
	}

	return FReply::Handled();
}

void SWPTransitManagerSettingsPanel::LoadSettings()
{
	const UWPTransitManagerEditorSettings* Settings = GetDefault<UWPTransitManagerEditorSettings>();
	bAutoApplyToReadyActors = Settings->bAutoApplyToReadyActors;

	ExcludedClassEntries.Reset();
	for (const TSoftClassPtr<AActor>& ExcludedClass : Settings->ExcludedClasses)
	{
		FExcludedClassEntryPtr Entry = MakeShared<FWPTransitManagerExcludedClassEntry>();
		Entry->Class = ExcludedClass;
		ExcludedClassEntries.Add(Entry);
	}
}

void SWPTransitManagerSettingsPanel::SaveSettings()
{
	UWPTransitManagerEditorSettings* Settings = GetMutableDefault<UWPTransitManagerEditorSettings>();
	Settings->bAutoApplyToReadyActors = bAutoApplyToReadyActors;
	Settings->ExcludedClasses.Reset();
	for (const FExcludedClassEntryPtr& Entry : ExcludedClassEntries)
	{
		if (Entry.IsValid() && !Entry->Class.IsNull())
		{
			Settings->ExcludedClasses.Add(Entry->Class);
		}
	}

	Settings->SaveConfig();
}

void SWPTransitManagerSettingsPanel::RemoveExcludedClass(const FExcludedClassEntryPtr& Entry)
{
	ExcludedClassEntries.Remove(Entry);
	if (ExcludedClassListView.IsValid())
	{
		ExcludedClassListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SWPTransitManagerSettingsPanel::GenerateExcludedClassRow(
	FExcludedClassEntryPtr Entry,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FExcludedClassEntryPtr>, OwnerTable)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([Entry]()
				{
					if (!Entry.IsValid() || Entry->Class.IsNull())
					{
						return FText::GetEmpty();
					}

					if (UClass* Class = Entry->Class.LoadSynchronous())
					{
						return FText::FromString(Class->GetName());
					}

					return FText::FromString(Entry->Class.ToString());
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("RemoveExcludedClass", "Remove"))
				.OnClicked_Lambda([this, Entry]()
				{
					RemoveExcludedClass(Entry);
					return FReply::Handled();
				})
			]
		];
}

const UClass* SWPTransitManagerSettingsPanel::GetPendingExcludedClass() const
{
	return PendingExcludedClass;
}

void SWPTransitManagerSettingsPanel::OnPendingExcludedClassChanged(const UClass* NewClass)
{
	PendingExcludedClass = const_cast<UClass*>(NewClass);
}

#undef LOCTEXT_NAMESPACE
