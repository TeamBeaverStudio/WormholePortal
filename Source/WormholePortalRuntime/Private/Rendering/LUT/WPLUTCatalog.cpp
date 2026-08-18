// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/LUT/WPLUTCatalog.h"

#include "Rendering/LUT/WPLUTAsset.h"

void UWPLUTCatalog::PostLoad()
{
	Super::PostLoad();

	if (ActiveLUTAsset.IsNull())
	{
		ActiveDescriptor = FWPLUTDescriptor::MakeDefault();
		ActiveBuildHash.Reset();
	}
}

#if WITH_EDITOR
void UWPLUTCatalog::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UWPLUTCatalog, ActiveLUTAsset))
	{
		RefreshActiveMetadataFromLoadedAsset(ActiveLUTAsset.LoadSynchronous());
		MarkPackageDirty();
	}
}
#endif

TSoftObjectPtr<UWPLUTAsset> UWPLUTCatalog::GetActiveLUTAsset(
	FWPLUTDescriptor* OutResolvedDescriptor) const
{
	if (OutResolvedDescriptor)
	{
		*OutResolvedDescriptor = ActiveDescriptor.GetSanitized();
	}
	return ActiveLUTAsset;
}

void UWPLUTCatalog::SetActiveLUTAsset(UWPLUTAsset* Asset)
{
	RefreshActiveMetadataFromLoadedAsset(Asset);
}

void UWPLUTCatalog::AddOrUpdate(UWPLUTAsset* Asset)
{
	SetActiveLUTAsset(Asset);
}

void UWPLUTCatalog::RefreshActiveMetadataFromLoadedAsset(UWPLUTAsset* Asset)
{
	if (!Asset)
	{
		ActiveLUTAsset.Reset();
		ActiveDescriptor = FWPLUTDescriptor::MakeDefault();
		ActiveBuildHash.Reset();
		return;
	}

	ActiveLUTAsset = Asset;
	ActiveDescriptor = Asset->Descriptor.GetSanitized();
	ActiveBuildHash = Asset->BuildHash.IsEmpty()
		? ActiveDescriptor.MakeBuildHash()
		: Asset->BuildHash;
}
