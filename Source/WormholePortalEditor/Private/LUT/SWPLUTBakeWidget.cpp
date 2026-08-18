// Copyright 2026 Team Beaver. All Rights Reserved.

#include "LUT/SWPLUTBakeWidget.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "WPSettings.h"

#define LOCTEXT_NAMESPACE "SWPLUTBakeWidget"

void SWPLUTBakeWidget::Construct(const FArguments& InArgs)
{
	ParentWindow = InArgs._ParentWindow;
	ProjectDescriptor = GetProjectDescriptor();
	BuildOptions();
	SelectInitialOptions();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(15.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "LUT Bake Settings"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 20.0f, 0.0f, 0.0f)
			[
				MakeQualityRow()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 20.0f, 0.0f, 0.0f)
			[
				MakeDomainRow()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 8.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SWPLUTBakeWidget::GetSummaryText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Bake", "Bake"))
					.OnClicked(this, &SWPLUTBakeWidget::OnBakeClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked(this, &SWPLUTBakeWidget::OnCancelClicked)
				]
			]
		]
	];
}

TSharedRef<SWidget> SWPLUTBakeWidget::MakeQualityComboRow(TSharedPtr<FQualityPresetOption> Option)
{
	return SNew(STextBlock)
		.Text(Option.IsValid() ? Option->Name : FText::GetEmpty());
}

TSharedRef<SWidget> SWPLUTBakeWidget::MakeDomainComboRow(TSharedPtr<FDomainPresetOption> Option)
{
	return SNew(STextBlock)
		.Text(Option.IsValid() ? Option->Name : FText::GetEmpty());
}

FWPLUTDescriptor SWPLUTBakeWidget::GetProjectDescriptor()
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	return IsValid(Settings)
		? Settings->LUTDescriptor.GetSanitized()
		: FWPLUTDescriptor::MakeDefault().GetSanitized();
}

int32 SWPLUTBakeWidget::ComputeQualityDistance(
	const FWPLUTDescriptor& Descriptor,
	const FQualityPresetOption& Option)
{
	return FMath::Abs(Descriptor.ImpactSamples - Option.ImpactSamples)
		+ FMath::Abs(Descriptor.TransitionSamples - Option.TransitionSamples) * 4
		+ FMath::Abs(Descriptor.RatioSamples - Option.RatioSamples) * 8
		+ FMath::Abs(Descriptor.IntegrationSteps - Option.IntegrationSteps);
}

bool SWPLUTBakeWidget::IsNearlyDomain(
	const FWPLUTDescriptor& Descriptor,
	const FDomainPresetOption& Option)
{
	return FMath::IsNearlyEqual(Descriptor.TransitionRatioMin, Option.TransitionRatioMin, 1.0e-4f)
		&& FMath::IsNearlyEqual(Descriptor.TransitionRatioMax, Option.TransitionRatioMax, 1.0e-4f);
}

void SWPLUTBakeWidget::BuildOptions()
{
	QualityOptions.Add(MakeShared<FQualityPresetOption>(FQualityPresetOption{
		LOCTEXT("FastName", "Fast"),
		LOCTEXT("FastDesc", "Fast iteration quality. Lower bake time and memory."),
		256, 24, 12, 64 }));
	QualityOptions.Add(MakeShared<FQualityPresetOption>(FQualityPresetOption{
		LOCTEXT("BalancedName", "Balanced"),
		LOCTEXT("BalancedDesc", "Default project-quality balance."),
		512, 48, 24, 192 }));
	QualityOptions.Add(MakeShared<FQualityPresetOption>(FQualityPresetOption{
		LOCTEXT("HighName", "High"),
		LOCTEXT("HighDesc", "Higher LUT density and integration accuracy."),
		768, 64, 32, 384 }));
	QualityOptions.Add(MakeShared<FQualityPresetOption>(FQualityPresetOption{
		LOCTEXT("CinematicName", "Cinematic"),
		LOCTEXT("CinematicDesc", "Highest shipped preset. Slowest bake, best final quality."),
		1024, 96, 48, 768 }));

	DomainOptions.Add(MakeShared<FDomainPresetOption>(FDomainPresetOption{
		LOCTEXT("AutoName", "Current Level Auto"),
		LOCTEXT("AutoDesc", "Fits the domain to the T/rho ratios used by portals in the current level."),
		EWPLUTBakeDomainPreset::CurrentLevelAuto, 0.5f, 8.0f }));
	DomainOptions.Add(MakeShared<FDomainPresetOption>(FDomainPresetOption{
		LOCTEXT("StandardName", "Standard"),
		LOCTEXT("StandardDesc", "T/rho 0.5 to 8."),
		EWPLUTBakeDomainPreset::Standard, 0.5f, 8.0f }));
	DomainOptions.Add(MakeShared<FDomainPresetOption>(FDomainPresetOption{
		LOCTEXT("WideName", "Wide"),
		LOCTEXT("WideDesc", "T/rho 0.25 to 16."),
		EWPLUTBakeDomainPreset::Wide, 0.25f, 16.0f }));
	DomainOptions.Add(MakeShared<FDomainPresetOption>(FDomainPresetOption{
		LOCTEXT("NarrowName", "Narrow"),
		LOCTEXT("NarrowDesc", "T/rho 1 to 4."),
		EWPLUTBakeDomainPreset::Narrow, 1.0f, 4.0f }));
}

void SWPLUTBakeWidget::SelectInitialOptions()
{
	SelectedQuality = QualityOptions.Num() > 0 ? QualityOptions[0] : nullptr;
	int32 BestDistance = TNumericLimits<int32>::Max();
	for (const TSharedPtr<FQualityPresetOption>& Option : QualityOptions)
	{
		if (!Option.IsValid())
		{
			continue;
		}
		const int32 Distance = ComputeQualityDistance(ProjectDescriptor, *Option);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			SelectedQuality = Option;
		}
	}

	SelectedDomain = DomainOptions.Num() > 1 ? DomainOptions[1] : nullptr;
	for (const TSharedPtr<FDomainPresetOption>& Option : DomainOptions)
	{
		if (Option.IsValid()
			&& Option->Preset != EWPLUTBakeDomainPreset::CurrentLevelAuto
			&& IsNearlyDomain(ProjectDescriptor, *Option))
		{
			SelectedDomain = Option;
			break;
		}
	}
}

TSharedRef<SWidget> SWPLUTBakeWidget::MakeQualityRow()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("QualityLabel", "Quality Preset"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SComboBox<TSharedPtr<FQualityPresetOption>>)
			.OptionsSource(&QualityOptions)
			.InitiallySelectedItem(SelectedQuality)
			.OnGenerateWidget_Static(&SWPLUTBakeWidget::MakeQualityComboRow)
			.OnSelectionChanged(this, &SWPLUTBakeWidget::OnQualityChanged)
			[
				SNew(STextBlock)
				.Text(this, &SWPLUTBakeWidget::GetSelectedQualityName)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SWPLUTBakeWidget::GetSelectedQualityDescription)
		];
}

TSharedRef<SWidget> SWPLUTBakeWidget::MakeDomainRow()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DomainLabel", "Domain Preset"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 5.0f, 0.0f, 0.0f)
		[
			SNew(SComboBox<TSharedPtr<FDomainPresetOption>>)
			.OptionsSource(&DomainOptions)
			.InitiallySelectedItem(SelectedDomain)
			.OnGenerateWidget_Static(&SWPLUTBakeWidget::MakeDomainComboRow)
			.OnSelectionChanged(this, &SWPLUTBakeWidget::OnDomainChanged)
			[
				SNew(STextBlock)
				.Text(this, &SWPLUTBakeWidget::GetSelectedDomainName)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SWPLUTBakeWidget::GetSelectedDomainDescription)
		];
}

FWPLUTDescriptor SWPLUTBakeWidget::MakeSelectedDescriptor() const
{
	FWPLUTDescriptor Descriptor = ProjectDescriptor.GetSanitized();
	if (SelectedQuality.IsValid())
	{
		Descriptor.ImpactSamples = SelectedQuality->ImpactSamples;
		Descriptor.TransitionSamples = SelectedQuality->TransitionSamples;
		Descriptor.RatioSamples = SelectedQuality->RatioSamples;
		Descriptor.IntegrationSteps = SelectedQuality->IntegrationSteps;
	}
	if (SelectedDomain.IsValid()
		&& SelectedDomain->Preset != EWPLUTBakeDomainPreset::CurrentLevelAuto)
	{
		Descriptor.TransitionRatioMin = SelectedDomain->TransitionRatioMin;
		Descriptor.TransitionRatioMax = SelectedDomain->TransitionRatioMax;
	}
	return Descriptor.GetSanitized();
}

void SWPLUTBakeWidget::OnQualityChanged(
	TSharedPtr<FQualityPresetOption> NewSelection,
	ESelectInfo::Type)
{
	SelectedQuality = NewSelection;
}

void SWPLUTBakeWidget::OnDomainChanged(
	TSharedPtr<FDomainPresetOption> NewSelection,
	ESelectInfo::Type)
{
	SelectedDomain = NewSelection;
}

FText SWPLUTBakeWidget::GetSelectedQualityName() const
{
	return SelectedQuality.IsValid() ? SelectedQuality->Name : FText::GetEmpty();
}

FText SWPLUTBakeWidget::GetSelectedQualityDescription() const
{
	return SelectedQuality.IsValid() ? SelectedQuality->Description : FText::GetEmpty();
}

FText SWPLUTBakeWidget::GetSelectedDomainName() const
{
	return SelectedDomain.IsValid() ? SelectedDomain->Name : FText::GetEmpty();
}

FText SWPLUTBakeWidget::GetSelectedDomainDescription() const
{
	return SelectedDomain.IsValid() ? SelectedDomain->Description : FText::GetEmpty();
}

FText SWPLUTBakeWidget::GetSummaryText() const
{
	const FWPLUTDescriptor Descriptor = MakeSelectedDescriptor();
	const FIntVector Dimensions = Descriptor.GetDimensions();
	const double MiB = static_cast<double>(Dimensions.X)
		* static_cast<double>(Dimensions.Y)
		* static_cast<double>(Dimensions.Z)
		* static_cast<double>(sizeof(FLinearColor)) / (1024.0 * 1024.0);
	const double EstimatedSamples = static_cast<double>(Dimensions.X)
		* static_cast<double>(Dimensions.Y)
		* static_cast<double>(Dimensions.Z)
		* static_cast<double>(Descriptor.IntegrationSteps)
		* 6.0;
	const FText DomainText = SelectedDomain.IsValid()
		&& SelectedDomain->Preset == EWPLUTBakeDomainPreset::CurrentLevelAuto
		? LOCTEXT("AutoDomainSummary", "Current level auto-fit")
		: FText::Format(
			LOCTEXT("FixedDomainSummary", "T/rho {0} - {1}"),
			FText::AsNumber(Descriptor.TransitionRatioMin),
			FText::AsNumber(Descriptor.TransitionRatioMax));

	return FText::Format(
		LOCTEXT(
			"Summary",
			"Descriptor preview:\n{0} x {1} x {2}, {3} integration steps\n~{4} MiB\n~{5}M quadrature sample\n\nDomain:\n{6}."),
		FText::AsNumber(Dimensions.X),
		FText::AsNumber(Dimensions.Y),
		FText::AsNumber(Dimensions.Z),
		FText::AsNumber(Descriptor.IntegrationSteps),
		FText::AsNumber(MiB),
		FText::AsNumber(EstimatedSamples / 1000000.0),
		DomainText);
}

FReply SWPLUTBakeWidget::OnCancelClicked()
{
	if (const TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FReply SWPLUTBakeWidget::OnBakeClicked()
{
	const FWPLUTDescriptor Descriptor = MakeSelectedDescriptor();
	const EWPLUTBakeDomainPreset DomainPreset = SelectedDomain.IsValid()
		? SelectedDomain->Preset
		: EWPLUTBakeDomainPreset::Standard;
	if (const TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	FWPLUTBaker::BakeAllInCurrentEditorWorld(Descriptor, DomainPreset);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE