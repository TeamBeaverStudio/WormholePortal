// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

class AWormholePortalActor;
class IPropertyHandle;

/** Dynamic LUT-domain bounds for the portal metric fields in the Details panel. */
class FWormholePortalActorDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;

private:
	TOptional<float> GetPortalRadiusValue() const;
	TOptional<float> GetTransitionLengthValue() const;
	TOptional<float> GetPortalRadiusMin() const;
	TOptional<float> GetPortalRadiusMax() const;
	TOptional<float> GetTransitionLengthMax() const;
	FText GetDomainSummary() const;

	void SetPortalRadius(float NewValue);
	void SetPortalRadiusCommitted(float NewValue, ETextCommit::Type CommitType);
	void SetTransitionLength(float NewValue);
	void SetTransitionLengthCommitted(float NewValue, ETextCommit::Type CommitType);

	TArray<TWeakObjectPtr<AWormholePortalActor>> Portals;
	TSharedPtr<IPropertyHandle> PortalRadiusProperty;
	TSharedPtr<IPropertyHandle> ThroatHalfLengthProperty;
	TSharedPtr<IPropertyHandle> TransitionLengthProperty;
};
