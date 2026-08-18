// Copyright 2026 Team Beaver. All Rights Reserved.

#include "SWPTransitManagerActorRow.h"

#include "SWPTransitManagerWidget.h"

#include "GameFramework/Actor.h"
#include "Transit/WPTransitComponent.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

void SWPTransitManagerActorRow::Construct(
	const FArguments& InArgs,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	Entry = InArgs._Entry;
	OwnerWidget = InArgs._OwnerWidget;
	SMultiColumnTableRow<FWPTransitManagerActorEntryPtr>::Construct(FSuperRowType::FArguments(), OwnerTable);
}

TSharedRef<SWidget> SWPTransitManagerActorRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (ColumnName == TEXT("Transit"))
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([Entry = Entry]()
				{
					AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
					if (!IsValid(Actor))
					{
						return ECheckBoxState::Unchecked;
					}

					TInlineComponentArray<UWPTransitComponent*> TransitComponents(Actor);
					for (const UWPTransitComponent* TransitComponent : TransitComponents)
					{
						if (IsValid(TransitComponent) && TransitComponent->GetTransitEnabled())
						{
							return ECheckBoxState::Checked;
						}
					}

					return ECheckBoxState::Unchecked;
				})
				.IsEnabled_Lambda([Entry = Entry]()
				{
					AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
					return IsValid(Actor) &&
						((Entry->bWasChecked && Entry->ResolveResult.IsPassed()) ||
							Actor->FindComponentByClass<UWPTransitComponent>());
				})
				.OnCheckStateChanged_Lambda([OwnerWidget = OwnerWidget, Entry = Entry](ECheckBoxState NewState)
				{
					if (OwnerWidget)
					{
						OwnerWidget->SetTransitChecked(Entry, NewState);
					}
				})
			];
	}

	if (ColumnName == TEXT("ActorLabel"))
	{
		return SNew(STextBlock)
			.Text_Lambda([Entry = Entry]()
			{
				const AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
				return IsValid(Actor) ? FText::FromString(Actor->GetActorLabel()) : FText::GetEmpty();
			});
	}

	return SNew(STextBlock)
		.Text_Lambda([Entry = Entry]()
		{
			return Entry.IsValid() && Entry->bWasChecked
				? Entry->StatusText
				: FText::GetEmpty();
		});
}
