// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

class UWPTransitComponent;
class FReply;

/** @brief Adds a Voxel Bake button to the Transit Component Details panel. */
class FWPTransitComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;
	
private:
	FReply OnBakeClicked();
	
	TArray<TWeakObjectPtr<UWPTransitComponent>> TransitComponents;
};
