// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LUT/WPLUTBaker.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class SWindow;

class SWPLUTBakeWidget final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWPLUTBakeWidget) {}
		SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	struct FQualityPresetOption
	{
		FText Name;
		FText Description;
		int32 ImpactSamples = 512;
		int32 TransitionSamples = 48;
		int32 RatioSamples = 24;
		int32 IntegrationSteps = 192;
	};

	struct FDomainPresetOption
	{
		FText Name;
		FText Description;
		EWPLUTBakeDomainPreset Preset = EWPLUTBakeDomainPreset::Standard;
		float TransitionRatioMin = 0.5f;
		float TransitionRatioMax = 8.0f;
	};

	static TSharedRef<SWidget> MakeQualityComboRow(TSharedPtr<FQualityPresetOption> Option);
	static TSharedRef<SWidget> MakeDomainComboRow(TSharedPtr<FDomainPresetOption> Option);
	static FWPLUTDescriptor GetProjectDescriptor();
	static int32 ComputeQualityDistance(
		const FWPLUTDescriptor& Descriptor,
		const FQualityPresetOption& Option);
	static bool IsNearlyDomain(
		const FWPLUTDescriptor& Descriptor,
		const FDomainPresetOption& Option);

	void BuildOptions();
	void SelectInitialOptions();
	TSharedRef<SWidget> MakeQualityRow();
	TSharedRef<SWidget> MakeDomainRow();
	FWPLUTDescriptor MakeSelectedDescriptor() const;

	void OnQualityChanged(
		TSharedPtr<FQualityPresetOption> NewSelection,
		ESelectInfo::Type SelectInfo);
	void OnDomainChanged(
		TSharedPtr<FDomainPresetOption> NewSelection,
		ESelectInfo::Type SelectInfo);

	FText GetSelectedQualityName() const;
	FText GetSelectedQualityDescription() const;
	FText GetSelectedDomainName() const;
	FText GetSelectedDomainDescription() const;
	FText GetSummaryText() const;

	FReply OnCancelClicked();
	FReply OnBakeClicked();

private:
	TWeakPtr<SWindow> ParentWindow;
	FWPLUTDescriptor ProjectDescriptor;
	TArray<TSharedPtr<FQualityPresetOption>> QualityOptions;
	TArray<TSharedPtr<FDomainPresetOption>> DomainOptions;
	TSharedPtr<FQualityPresetOption> SelectedQuality;
	TSharedPtr<FDomainPresetOption> SelectedDomain;
};
