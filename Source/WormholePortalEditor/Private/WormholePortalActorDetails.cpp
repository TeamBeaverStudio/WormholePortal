// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WormholePortalActorDetails.h"

#include "DetailCategoryBuilder.h"
#include "IDetailGroup.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "WormholePortalActor.h"

#define LOCTEXT_NAMESPACE "FWormholePortalActorDetails"

namespace
{
	constexpr float MinPortalRadiusCm = 1.0f;
}

TSharedRef<IDetailCustomization> FWormholePortalActorDetails::MakeInstance()
{
	return MakeShared<FWormholePortalActorDetails>();
}

void FWormholePortalActorDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	Portals.Reset();
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailLayout.GetObjectsBeingCustomized(Objects);
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (AWormholePortalActor* Portal = Cast<AWormholePortalActor>(Object.Get()))
		{
			Portals.Add(Portal);
		}
	}

	PortalRadiusProperty = DetailLayout.GetProperty(
		TEXT("PortalRadius"), AWormholePortalActor::StaticClass());
	ThroatHalfLengthProperty = DetailLayout.GetProperty(
		TEXT("ThroatHalfLength"), AWormholePortalActor::StaticClass());
	TransitionLengthProperty = DetailLayout.GetProperty(
		TEXT("TransitionLength"), AWormholePortalActor::StaticClass());
	if (!PortalRadiusProperty.IsValid()
		|| !ThroatHalfLengthProperty.IsValid()
		|| !TransitionLengthProperty.IsValid())
	{
		return;
	}

	DetailLayout.HideProperty(PortalRadiusProperty);
	DetailLayout.HideProperty(ThroatHalfLengthProperty);
	DetailLayout.HideProperty(TransitionLengthProperty);

	IDetailCategoryBuilder& PortalCategory = DetailLayout.EditCategory(TEXT("Wormhole Portal"));
	IDetailGroup& MetricGroup = PortalCategory.AddGroup(
		TEXT("Metric"),
		LOCTEXT("MetricGroup", "Metric"),
		false,
		true);

	MetricGroup.AddWidgetRow()
	.NameContent()
	[
		PortalRadiusProperty->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(150.0f)
	[
		SNew(SNumericEntryBox<float>)
		.Value(this, &FWormholePortalActorDetails::GetPortalRadiusValue)
		.MinValue(this, &FWormholePortalActorDetails::GetPortalRadiusMin)
		.MinSliderValue(this, &FWormholePortalActorDetails::GetPortalRadiusMin)
		.MaxValue(this, &FWormholePortalActorDetails::GetPortalRadiusMax)
		.MaxSliderValue(this, &FWormholePortalActorDetails::GetPortalRadiusMax)
		.AllowSpin(true)
		.OnValueChanged(this, &FWormholePortalActorDetails::SetPortalRadius)
		.OnValueCommitted(this, &FWormholePortalActorDetails::SetPortalRadiusCommitted)
	];

	MetricGroup.AddPropertyRow(ThroatHalfLengthProperty.ToSharedRef());

	MetricGroup.AddWidgetRow()
	.NameContent()
	[
		TransitionLengthProperty->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(150.0f)
	[
		SNew(SNumericEntryBox<float>)
		.Value(this, &FWormholePortalActorDetails::GetTransitionLengthValue)
		.MinValue(0.0f)
		.MinSliderValue(0.0f)
		.MaxValue(this, &FWormholePortalActorDetails::GetTransitionLengthMax)
		.MaxSliderValue(this, &FWormholePortalActorDetails::GetTransitionLengthMax)
		.AllowSpin(true)
		.OnValueChanged(this, &FWormholePortalActorDetails::SetTransitionLength)
		.OnValueCommitted(this, &FWormholePortalActorDetails::SetTransitionLengthCommitted)
	];

	MetricGroup.AddWidgetRow()
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("ActiveLUTDomain", "Active LUT Domain"))
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(250.0f)
	[
		SNew(STextBlock)
		.Text(this, &FWormholePortalActorDetails::GetDomainSummary)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	];
}

TOptional<float> FWormholePortalActorDetails::GetPortalRadiusValue() const
{
	float Value = 0.0f;
	return PortalRadiusProperty.IsValid()
		&& PortalRadiusProperty->GetValue(Value) == FPropertyAccess::Success
		? TOptional<float>(Value)
		: TOptional<float>();
}

TOptional<float> FWormholePortalActorDetails::GetTransitionLengthValue() const
{
	float Value = 0.0f;
	return TransitionLengthProperty.IsValid()
		&& TransitionLengthProperty->GetValue(Value) == FPropertyAccess::Success
		? TOptional<float>(Value)
		: TOptional<float>();
}

TOptional<float> FWormholePortalActorDetails::GetPortalRadiusMin() const
{
	float MinRadius = MinPortalRadiusCm;
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalPtr : Portals)
	{
		const AWormholePortalActor* Portal = PortalPtr.Get();
		if (!Portal || Portal->GetTransitionLength() <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FWPLUTDescriptor Descriptor = Portal->GetEffectiveLUTDescriptor();
		MinRadius = FMath::Max(
			MinRadius,
			Portal->GetTransitionLength() / Descriptor.TransitionRatioMax);
	}
	return MinRadius;
}

TOptional<float> FWormholePortalActorDetails::GetPortalRadiusMax() const
{
	float MaxRadius = TNumericLimits<float>::Max();
	bool bConstrained = false;
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalPtr : Portals)
	{
		const AWormholePortalActor* Portal = PortalPtr.Get();
		if (!Portal || Portal->GetTransitionLength() <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FWPLUTDescriptor Descriptor = Portal->GetEffectiveLUTDescriptor();
		MaxRadius = FMath::Min(
			MaxRadius,
			Portal->GetTransitionLength() / Descriptor.TransitionRatioMin);
		bConstrained = true;
	}
	return bConstrained ? TOptional<float>(FMath::Max(MinPortalRadiusCm, MaxRadius)) : TOptional<float>();
}

TOptional<float> FWormholePortalActorDetails::GetTransitionLengthMax() const
{
	float MaxLength = TNumericLimits<float>::Max();
	bool bFoundPortal = false;
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalPtr : Portals)
	{
		const AWormholePortalActor* Portal = PortalPtr.Get();
		if (!Portal)
		{
			continue;
		}
		const FWPLUTDescriptor Descriptor = Portal->GetEffectiveLUTDescriptor();
		MaxLength = FMath::Min(
			MaxLength,
			Portal->GetPortalRadius() * Descriptor.TransitionRatioMax);
		bFoundPortal = true;
	}
	return bFoundPortal ? TOptional<float>(MaxLength) : TOptional<float>();
}

FText FWormholePortalActorDetails::GetDomainSummary() const
{
	if (Portals.IsEmpty())
	{
		return LOCTEXT("NoPortalDomain", "No portal selected");
	}

	float RatioMin = 0.0f;
	float RatioMax = TNumericLimits<float>::Max();
	float PositiveLengthMin = 0.0f;
	float LengthMax = TNumericLimits<float>::Max();
	bool bFoundPortal = false;
	for (const TWeakObjectPtr<AWormholePortalActor>& PortalPtr : Portals)
	{
		const AWormholePortalActor* Portal = PortalPtr.Get();
		if (!Portal)
		{
			continue;
		}
		const FWPLUTDescriptor Descriptor = Portal->GetEffectiveLUTDescriptor();
		RatioMin = FMath::Max(RatioMin, Descriptor.TransitionRatioMin);
		RatioMax = FMath::Min(RatioMax, Descriptor.TransitionRatioMax);
		PositiveLengthMin = FMath::Max(
			PositiveLengthMin,
			Portal->GetPortalRadius() * Descriptor.TransitionRatioMin);
		LengthMax = FMath::Min(
			LengthMax,
			Portal->GetPortalRadius() * Descriptor.TransitionRatioMax);
		bFoundPortal = true;
	}

	return bFoundPortal
		? FText::Format(
			LOCTEXT("LUTDomainSummaryFormat", "T/rho [{0}, {1}] | T = 0 or [{2}, {3}] cm"),
			FText::AsNumber(RatioMin), FText::AsNumber(RatioMax),
			FText::AsNumber(PositiveLengthMin), FText::AsNumber(LengthMax))
		: LOCTEXT("InvalidPortalDomain", "No valid portal selected");
}

void FWormholePortalActorDetails::SetPortalRadius(const float NewValue)
{
	if (PortalRadiusProperty.IsValid())
	{
		PortalRadiusProperty->SetValue(NewValue);
	}
}

void FWormholePortalActorDetails::SetPortalRadiusCommitted(
	const float NewValue,
	const ETextCommit::Type CommitType)
{
	(void)CommitType;
	SetPortalRadius(NewValue);
}

void FWormholePortalActorDetails::SetTransitionLength(const float NewValue)
{
	if (TransitionLengthProperty.IsValid())
	{
		TransitionLengthProperty->SetValue(NewValue);
	}
}

void FWormholePortalActorDetails::SetTransitionLengthCommitted(
	const float NewValue,
	const ETextCommit::Type CommitType)
{
	(void)CommitType;
	SetTransitionLength(NewValue);
}

#undef LOCTEXT_NAMESPACE
