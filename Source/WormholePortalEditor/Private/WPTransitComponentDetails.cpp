// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPTransitComponentDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "ScopedTransaction.h"
#include "Transit/WPTransitComponent.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Voxel/WPVoxelBaker.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "PropertyHandle.h"
#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"

#define LOCTEXT_NAMESPACE "FWPTransitComponentDetails"

TSharedRef<IDetailCustomization> FWPTransitComponentDetails::MakeInstance()
{
	return MakeShared<FWPTransitComponentDetails>();
}

void FWPTransitComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	TransitComponents.Reset();
	
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailLayout.GetObjectsBeingCustomized(OUT Objects);
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (UWPTransitComponent* TransitComp = Cast<UWPTransitComponent>(Object.Get()))
		{
			TransitComponents.Add(TransitComp);
		}
	}
	
	TSharedPtr<IPropertyHandle> SizeProp = DetailLayout.GetProperty(
		GET_MEMBER_NAME_CHECKED(UWPTransitComponent, VoxelSize));
	TSharedPtr<IPropertyHandle> CountProp = DetailLayout.GetProperty(
		GET_MEMBER_NAME_CHECKED(UWPTransitComponent, MaxVoxelCount));
	TSharedPtr<IPropertyHandle> DataProp = DetailLayout.GetProperty(
		GET_MEMBER_NAME_CHECKED(UWPTransitComponent, VoxelData));
	
	DetailLayout.HideProperty(SizeProp);
	DetailLayout.HideProperty(CountProp);
	DetailLayout.HideProperty(DataProp);
	
	IDetailCategoryBuilder& WormholeCategory = DetailLayout.EditCategory(TEXT("Wormhole Portal"));
	IDetailGroup& VoxelGroup = WormholeCategory.AddGroup(TEXT("Voxel"), LOCTEXT("VoxelGroup", "Voxel"));
	
	// 선택한 모든 Transit Component에서 Voxel Collision을 사용하는 경우에만 설정을 활성화합니다.
	const auto AreVoxelControlsEnabled = [Components = TransitComponents]()
	{
		bool bHasValidComponent = false;
		for (const TWeakObjectPtr<UWPTransitComponent>& ComponentPtr : Components)
		{
			const UWPTransitComponent* TransitComp = ComponentPtr.Get();
			if (!IsValid(TransitComp)) continue;
			
			bHasValidComponent = true;
			
			if (!TransitComp->IsVoxelCollisionEnabled())
			{
				return false;
			}
		}
		return bHasValidComponent;
	};
	
	const TAttribute<bool> VoxelControlsEnabled = TAttribute<bool>::CreateLambda(AreVoxelControlsEnabled);
	const TAttribute<FText> BakeTooltip = TAttribute<FText>::CreateLambda([AreVoxelControlsEnabled]()
	{
		if (!AreVoxelControlsEnabled())
		{
			return LOCTEXT(
				"BakeVoxelBodyDisabledTooltip",
				"Enable Use Voxel Collision under Wormhole Portal > Transit > Advanced before baking.");
		}
		return LOCTEXT(
			"BakeVoxelBodyTooltip",
			"Create Voxel Collision Assets from the Selected Component's Primitive.");
	});
	
	VoxelGroup.AddWidgetRow()
	[
		SNew(SButton)
		.Text(LOCTEXT("BakeVoxelBody", "Bake Voxel Body"))
		.ToolTipText(BakeTooltip)
		.IsEnabled(VoxelControlsEnabled)
		.HAlign(HAlign_Center)
		.OnClicked(this, &FWPTransitComponentDetails::OnBakeClicked)
	];
	
	VoxelGroup.AddPropertyRow(SizeProp.ToSharedRef()).IsEnabled(VoxelControlsEnabled);
	VoxelGroup.AddPropertyRow(CountProp.ToSharedRef()).IsEnabled(VoxelControlsEnabled);
	VoxelGroup.AddPropertyRow(DataProp.ToSharedRef()).IsEnabled(VoxelControlsEnabled);
}

FReply FWPTransitComponentDetails::OnBakeClicked()
{
	const FScopedTransaction Transaction(LOCTEXT("BakeVoxelTransaction", "Bake Wormhole Voxel Body"));
	bool bAnyProcessed = false;
	bool bAllSucceeded = true;
	FText ResultMessage = LOCTEXT("No Component", "No valid Transit Components are available for baking.");
	
	for (const TWeakObjectPtr<UWPTransitComponent>& ComponentPtr : TransitComponents)
	{
		UWPTransitComponent* TransitComp = ComponentPtr.Get();
		if (!IsValid(TransitComp))
		{
			continue;
		}
		
		bAnyProcessed = true;
		
		FText BakeMessage;
		const bool bSucceeded = FWPVoxelBaker::Bake(TransitComp, OUT BakeMessage);
		
		if (!bSucceeded)
		{
			if (bAllSucceeded)
			{
				ResultMessage = BakeMessage;
			}
			bAllSucceeded = false;
		}
		else if (bAllSucceeded)
		{
			ResultMessage = BakeMessage;
		}
	}
	
	FNotificationInfo Info(ResultMessage);
	Info.ExpireDuration = 8.f;
	if (const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Notification->SetCompletionState(bAnyProcessed && bAllSucceeded ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
