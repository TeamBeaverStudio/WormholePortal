// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"
#include "IPropertyTypeCustomization.h"

/**
 * Places the Dynamic Cubemap resolution settings inside the Scene Capture category. The
 * Lowest and Inside resolutions remain scalar properties, while Resolution Tiers uses the
 * standard Unreal array row so users receive built-in add, delete, and reorder controls.
 */
class FWPSettingsDetails final : public IDetailCustomization
{
public:
	/** Creates one short-lived customization instance for a Wormhole Portal settings view. */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** Builds the Scene Capture child group and inserts the array property in visual order. */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;
};

/**
 * Presents one FWPCaptureResolutionTier array element on a single row. The array index is
 * displayed as "Tier N", followed by the Start (%) and Resolution controls. Array ownership,
 * add/delete/reorder commands, undo, Config persistence, and reset behavior remain managed by
 * Unreal's standard Details panel implementation.
 */
class FWPCaptureResolutionTierCustomization final : public IPropertyTypeCustomization
{
public:
	/** Creates one short-lived customization instance for a reflected tier element. */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** Builds the compact one-line element header used inside Resolution Tiers. */
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> StructPropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	/** Intentionally adds no expanded children because both fields are visible in the header. */
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> StructPropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;
};
