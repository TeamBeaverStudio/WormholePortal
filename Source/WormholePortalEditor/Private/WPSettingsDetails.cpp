// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPSettingsDetails.h"

#include "DetailCategoryBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "PropertyHandle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WPSettings.h"

#define LOCTEXT_NAMESPACE "FWPSettingsDetails"

namespace
{
	TSharedRef<IPropertyHandle> GetSettingsProperty(
		IDetailLayoutBuilder& DetailLayout,
		const FName PropertyName)
	{
		return DetailLayout.GetProperty(PropertyName, UWPSettings::StaticClass());
	}

}

TSharedRef<IDetailCustomization> FWPSettingsDetails::MakeInstance()
{
	return MakeShared<FWPSettingsDetails>();
}

void FWPSettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	const TSharedRef<IPropertyHandle> LowestVisibleHandle = GetSettingsProperty(
		DetailLayout,
		GET_MEMBER_NAME_CHECKED(UWPSettings, CaptureLowestVisibleResolution));
	const TSharedRef<IPropertyHandle> ResolutionTiersHandle = GetSettingsProperty(
		DetailLayout,
		GET_MEMBER_NAME_CHECKED(UWPSettings, CaptureResolutionTiers));
	const TSharedRef<IPropertyHandle> InsideResolutionHandle = GetSettingsProperty(
		DetailLayout,
		GET_MEMBER_NAME_CHECKED(UWPSettings, CaptureInsideSafeProxyResolution));

	// Remove the automatic rows before rebuilding the section under the real Scene Capture
	// category. This avoids promoting a pipe-delimited path into a separate top-level category.
	DetailLayout.HideProperty(LowestVisibleHandle);
	DetailLayout.HideProperty(ResolutionTiersHandle);
	DetailLayout.HideProperty(InsideResolutionHandle);

	// Build Dynamic Cubemap Resolution as a real Scene Capture child group. Category display
	// mode gives it the same hierarchy and visual treatment as the automatically generated
	// Bundled Show Flags subcategory instead of displaying "Scene Capture|..." at the root.
	IDetailCategoryBuilder& SceneCaptureCategory = DetailLayout.EditCategory(TEXT("Scene Capture"));
	IDetailGroup& DynamicResolutionGroup = SceneCaptureCategory.AddGroup(
		TEXT("Dynamic Cubemap Resolution"),
		LOCTEXT("DynamicCubemapResolutionGroup", "Dynamic Cubemap Resolution"),
		false,
		true);
	DynamicResolutionGroup.SetDisplayMode(EDetailGroupDisplayMode::Category);

	// Reuse the reflected handles so Unreal's standard array header supplies the element count,
	// add, delete, reorder, undo, Config persistence, and reset-to-default behavior.
	DynamicResolutionGroup.AddPropertyRow(LowestVisibleHandle);
	DynamicResolutionGroup.AddPropertyRow(ResolutionTiersHandle);
	DynamicResolutionGroup.AddPropertyRow(InsideResolutionHandle);
}

TSharedRef<IPropertyTypeCustomization> FWPCaptureResolutionTierCustomization::MakeInstance()
{
	return MakeShared<FWPCaptureResolutionTierCustomization>();
}

void FWPCaptureResolutionTierCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> StructPropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	(void)CustomizationUtils;
	const TSharedPtr<IPropertyHandle> StartHandle = StructPropertyHandle->GetChildHandle(
		GET_MEMBER_NAME_CHECKED(FWPCaptureResolutionTier, StartScreenHeightPercent));
	const TSharedPtr<IPropertyHandle> ResolutionHandle = StructPropertyHandle->GetChildHandle(
		GET_MEMBER_NAME_CHECKED(FWPCaptureResolutionTier, Resolution));
	if (!StartHandle.IsValid() || !ResolutionHandle.IsValid())
	{
		HeaderRow.NameContent()[StructPropertyHandle->CreatePropertyNameWidget()];
		return;
	}

	const int32 TierIndex = StructPropertyHandle->GetIndexInArray();
	const FText TierLabel = FText::Format(
		LOCTEXT("TierLabelFormat", "Tier {0}"),
		FText::AsNumber(TierIndex == INDEX_NONE ? 1 : TierIndex + 1));
	const FText TierTooltip = FText::Format(
		LOCTEXT("TierTooltipFormat",
			"Screen-height threshold and Cubemap face resolution for Tier {0}."),
		FText::AsNumber(TierIndex == INDEX_NONE ? 1 : TierIndex + 1));

	HeaderRow
	.NameContent()
	[
		StructPropertyHandle->CreatePropertyNameWidget(TierLabel, TierTooltip)
	]
	.ValueContent()
	.MinDesiredWidth(360.0f)
	.MaxDesiredWidth(560.0f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("StartLabel", "Start (%)"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.MinWidth(80.0f)
		.Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			StartHandle->CreatePropertyValueWidget()
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("ResolutionLabel", "Resolution"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.MinWidth(80.0f)
		[
			ResolutionHandle->CreatePropertyValueWidget()
		]
	];
}

void FWPCaptureResolutionTierCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> StructPropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	(void)StructPropertyHandle;
	(void)ChildBuilder;
	(void)CustomizationUtils;
}

#undef LOCTEXT_NAMESPACE
