// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPTransitManagerActorController.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "Engine/StaticMesh.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Components/StaticMeshComponent.h"

#include "Transit/WPTransitComponent.h"

#define LOCTEXT_NAMESPACE "WPTransitManagerActorController"

UWPTransitComponent* FWPTransitManagerActorController::FindTransitComp(AActor* Actor)
{
	if (!IsValid(Actor)) return nullptr;

	UWPTransitComponent* FirstComp = nullptr;
	TInlineComponentArray<UWPTransitComponent*> TransitComps(Actor);
	for (UWPTransitComponent* TransitComp : TransitComps)
	{
		if (!IsValid(TransitComp)) continue;
		if (FirstComp == nullptr) FirstComp = TransitComp;
		// 중복 Component가 있는 잘못된 구성에서도 실제로 켜진 Component를 우선 표시하고 제어합니다.
		if (TransitComp->GetTransitEnabled()) return TransitComp;
	}
	return FirstComp;
}

bool FWPTransitManagerActorController::IsExcluded(const AActor* Actor,
	const FWPTransitManagerOptions& Options)
{
	if (!IsValid(Actor)) return false;
	for (const TSubclassOf<AActor>& ExcludedClass : Options.ExcludedClasses)
	{
		if (const UClass* ActorClass = ExcludedClass.Get(); IsValid(ActorClass) &&
			Actor->IsA(ActorClass))
		{
			return true;
		}
	}
	return false;
}

FText FWPTransitManagerActorController::GetStatusText(const FWPTransitResolveResult& Result)
{
	if (Result.IsPassed()) return LOCTEXT("Ready", "Ready");

	switch (Result.FailReason)
	{
	case EWPTransitResolveFailReason::UnsupportedActor:
		return LOCTEXT("UnsupportedActor", "Not Supported: Actor does not match a supported Transit type.");
	case EWPTransitResolveFailReason::TransitDisabled:
		return LOCTEXT("TransitDisabled", "Needs Setup: Transit is disabled.");
	case EWPTransitResolveFailReason::MissingPrimitives:
		return LOCTEXT("NoPrimitives", "Needs Setup: No movable collision Primitive was found.");
	case EWPTransitResolveFailReason::NoPhysicsBody:
		return LOCTEXT( "NoPhysicsBody", "Needs Setup: No supported Primitive is simulating Physics." );
	case EWPTransitResolveFailReason::MissingVoxelData:
		{
			TArray<FString> MissingSourceNames;
			MissingSourceNames.Reserve(Result.FailedComponents.Num());
			
			for (const TWeakObjectPtr<UActorComponent>& FailedComponent : Result.FailedComponents)
			{
				const UActorComponent* Component = FailedComponent.Get();
				if (!IsValid(Component)) continue;
				
				const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(FailedComponent.Get());
				const UStaticMesh* SM = IsValid(SMC) ? SMC->GetStaticMesh() : nullptr;
				
				if (IsValid(SM))
				{
					MissingSourceNames.AddUnique(SM->GetName());
				}
				else
				{
					MissingSourceNames.AddUnique(Component->GetName());
				}
			}
			
			if (MissingSourceNames.IsEmpty())
			{
				return LOCTEXT("NoVoxelDataForPrimitive", "Needs Setup: Bake Voxel Body for a supported Primitive");
			}
			
			return FText::Format(LOCTEXT("NoVoxelDataWithPrimitives", "Needs Setup: Bake Voxel Body for {0}"),
				FText::FromString(FString::Join(MissingSourceNames, TEXT(", "))));
		}
	case EWPTransitResolveFailReason::TypeMismatch:
		return LOCTEXT("TypeMismatch", "Needs Setup: Actor does not match the selected Transit type.");
	case EWPTransitResolveFailReason::MissingMovement:
		return LOCTEXT("MissingMovement", "Needs Setup: Required Movement Component was not found.");
	case EWPTransitResolveFailReason::InvalidMoveOwner:
		return LOCTEXT("InvalidMoveOwner", "Needs Setup: Movement Component has an invalid owner.");
	case EWPTransitResolveFailReason::InvalidUpdatedPart:
		return LOCTEXT("InvalidUpdatedPart", "Needs Setup: Movement Component must update the Transit Primitive.");
	case EWPTransitResolveFailReason::InvalidRootPart:
		return LOCTEXT("InvalidRootPart", "Needs Setup: Transit Primitive must be the Actor Root Component.");
	case EWPTransitResolveFailReason::InvalidPartOwner:
		return LOCTEXT("InvalidPartOwner", "Needs Setup: Transit Primitive has an invalid owner.");
	case EWPTransitResolveFailReason::PartNotMovable:
		return LOCTEXT("PartNotMovable", "Needs Setup: Transit Primitive must be Movable.");
	case EWPTransitResolveFailReason::PartNoCollision:
		return LOCTEXT("PartNoCollision", "Needs Setup: Transit Primitive collision is disabled.");
	case EWPTransitResolveFailReason::UnsupportedPart:
		return LOCTEXT("UnsupportedPart", "Needs Setup: Transit Primitive type is not supported.");
	case EWPTransitResolveFailReason::ExcludedPart:
		return LOCTEXT("ExcludedPart", "Needs Setup: Transit Primitive is excluded from Transit.");
	case EWPTransitResolveFailReason::SimPhysics:
		return LOCTEXT("SimPhysics", "Needs Setup: Required Component must not simulate Physics.");
	case EWPTransitResolveFailReason::InvalidMoveType:
		return LOCTEXT("InvalidMoveType", "Needs Setup: Movement Component type is not supported by the selected Transit type.");
	case EWPTransitResolveFailReason::NoOverlapPart:
		return LOCTEXT( "NoOverlapPart", "Needs Setup: No Transit Primitive can generate a Portal overlap." );
	default:
		return LOCTEXT("UnknownStatus", "Needs Setup: Transit settings are invalid.");
	}
}

void FWPTransitManagerActorController::RebuildActorEntries(bool bRunCheck,
	const FWPTransitManagerOptions& Options)
{
	ActorEntries.Reset();
	UWorld* World = GetEditorWorld();
	if (!World) return;

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor)) continue;

		FWPTransitManagerActorEntryPtr Entry = MakeShared<FWPTransitManagerActorEntry>();
		Entry->Actor = Actor;
		RefreshActorEntry(Entry, bRunCheck, Options);
		ActorEntries.Add(Entry);
	}
}

void FWPTransitManagerActorController::CheckAll(const FWPTransitManagerOptions& Options)
{
	for (const FWPTransitManagerActorEntryPtr& Entry : ActorEntries)
	{
		CheckActorEntry(Entry, Options);
	}
}

void FWPTransitManagerActorController::ApplyReadyActors(const FWPTransitManagerOptions& Options)
{
	const FScopedTransaction Transaction(LOCTEXT("ApplyReadyActorsTransaction",
		"Apply Wormhole Transit Components"));
	for (const FWPTransitManagerActorEntryPtr& Entry : ActorEntries)
	{
		AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
		if (!IsValid(Actor) || !Entry->bWasChecked || !Entry->ResolveResult.IsPassed() ||
			Actor->FindComponentByClass<UWPTransitComponent>())
		{
			continue;
		}

		// 목록을 검사한 뒤 Actor 구성이 바뀌었을 수 있으므로 실제 변경 직전에 다시 판정합니다.
		CheckActorEntry(Entry, Options);
		if (Entry->ResolveResult.IsPassed())
		{
			AddTransitComponent(Entry, Entry->ResolveResult, Options);
		}
	}
}

void FWPTransitManagerActorController::SetTransitEnabled(
	const FWPTransitManagerActorEntryPtr& Entry, bool bEnabled,
	const FWPTransitManagerOptions& Options)
{
	if (!Entry.IsValid() || !Entry->Actor.IsValid()) return;

	AActor* Actor = Entry->Actor.Get();
	const bool bWasChecked = Entry->bWasChecked;
	UWPTransitComponent* TransitComponent = FindTransitComp(Actor);
	const bool bIsTransitEnabled = IsValid(TransitComponent) && TransitComponent->GetTransitEnabled();
	if (bEnabled == bIsTransitEnabled) return;

	if (bEnabled)
	{
		if (IsValid(TransitComponent))
		{
			// Blueprint에서 상속된 Component는 삭제하지 않고 기존 설정을 그대로 다시 활성화합니다.
			const FScopedTransaction Transaction(LOCTEXT("EnableTransitComponentTransaction",
				"Enable Wormhole Transit Component"));
			Actor->Modify();
			TransitComponent->Modify();
			TransitComponent->SetTransitEnabled(true);
			TransitComponent->RefreshFromOwner();
			Actor->MarkPackageDirty();
		}
		else
		{
			CheckActorEntry(Entry, Options);
			if (!Entry->ResolveResult.IsPassed()) return;

			const FScopedTransaction Transaction(LOCTEXT("AddTransitComponentTransaction",
				"Add Wormhole Transit Component"));
			AddTransitComponent(Entry, Entry->ResolveResult, Options);
		}
	}
	else
	{
		const FScopedTransaction Transaction(LOCTEXT("DisableTransitComponentTransaction",
			"Disable Wormhole Transit Component"));
		DisableTransitComponents(Entry);
	}

	RefreshActorEntry(Entry, bWasChecked, Options);
}

const TArray<FWPTransitManagerActorEntryPtr>& FWPTransitManagerActorController::GetActorEntries() const
{
	return ActorEntries;
}

void FWPTransitManagerActorController::RefreshActorEntry(
	const FWPTransitManagerActorEntryPtr& Entry, bool bRunCheck,
	const FWPTransitManagerOptions& Options)
{
	if (!Entry.IsValid()) return;
	if (!bRunCheck)
	{
		Entry->bWasChecked = false;
		Entry->ResolveResult = FWPTransitResolveResult();
		Entry->StatusText = FText::GetEmpty();
		return;
	}
	CheckActorEntry(Entry, Options);
}

void FWPTransitManagerActorController::CheckActorEntry(
	const FWPTransitManagerActorEntryPtr& Entry, const FWPTransitManagerOptions& Options)
{
	if (!Entry.IsValid()) return;

	AActor* Actor = Entry->Actor.Get();
	if (IsExcluded(Actor, Options))
	{
		Entry->ResolveResult = FWPTransitResolveResult();
		Entry->ResolveResult.Status = EWPTransitResolveStatus::NotSupported;
		Entry->ResolveResult.FailReason = EWPTransitResolveFailReason::UnsupportedActor;
		Entry->StatusText = LOCTEXT("ExcludedClass",
			"Not Supported: Actor class is excluded by Transit Manager settings.");
	}
	else
	{
		UWPTransitComponent* TransitComponent = FindTransitComp(Actor);
		// Component가 없으면 Auto로 지원 종류를 찾고, 이미 있으면 사용자가 지정한 종류만 검사합니다.
		const EWPTransitType RequestedType = IsValid(TransitComponent) ? TransitComponent->GetUnresolvedTransitType() : EWPTransitType::Auto;
		Entry->ResolveResult = FWPTransitTypeResolver::Resolve(Actor, TransitComponent, RequestedType);
		Entry->StatusText = GetStatusText(Entry->ResolveResult);
	}
	Entry->bWasChecked = true;
}

void FWPTransitManagerActorController::AddTransitComponent(
	const FWPTransitManagerActorEntryPtr& Entry, const FWPTransitResolveResult& ResolveResult,
	const FWPTransitManagerOptions& Options)
{
	AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
	if (!IsValid(Actor) || !ResolveResult.IsPassed() ||
		Actor->FindComponentByClass<UWPTransitComponent>())
	{
		return;
	}

	Actor->Modify();
	UWPTransitComponent* TransitComponent = NewObject<UWPTransitComponent>(Actor,
		UWPTransitComponent::StaticClass(), NAME_None, RF_Transactional);
	TransitComponent->CreationMethod = EComponentCreationMethod::Instance;
	Actor->AddInstanceComponent(TransitComponent);
	TransitComponent->OnComponentCreated();
	TransitComponent->RegisterComponent();

	TransitComponent->Modify();
	TransitComponent->SetTransitEnabled(true);
	TransitComponent->SetTransitType(ResolveResult.TransitType);
	TransitComponent->RefreshFromOwner();
	Actor->MarkPackageDirty();
	CheckActorEntry(Entry, Options);
}

void FWPTransitManagerActorController::DisableTransitComponents(
	const FWPTransitManagerActorEntryPtr& Entry)
{
	AActor* Actor = Entry.IsValid() ? Entry->Actor.Get() : nullptr;
	if (!IsValid(Actor)) return;

	TInlineComponentArray<UWPTransitComponent*> TransitComponents(Actor);
	if (TransitComponents.IsEmpty()) return;
	Actor->Modify();

	TArray<UActorComponent*> ComponentsToDelete;
	for (UWPTransitComponent* TransitComponent : TransitComponents)
	{
		// 레벨 인스턴스에 Manager가 추가한 Component만 실제로 삭제할 수 있습니다.
		if (IsValid(TransitComponent) && FComponentEditorUtils::CanDeleteComponent(TransitComponent))
		{
			ComponentsToDelete.Add(TransitComponent);
		}
	}
	if (!ComponentsToDelete.IsEmpty())
	{
		UActorComponent* ComponentToSelect = nullptr;
		FComponentEditorUtils::DeleteComponents(ComponentsToDelete, ComponentToSelect);
	}

	TransitComponents.Reset();
	Actor->GetComponents(TransitComponents);
	for (UWPTransitComponent* TransitComponent : TransitComponents)
	{
		// Blueprint 클래스에서 상속된 Component는 인스턴스에서 삭제할 수 없으므로 기능만 끕니다.
		if (!IsValid(TransitComponent) || FComponentEditorUtils::CanDeleteComponent(TransitComponent)) continue;
		TransitComponent->Modify();
		TransitComponent->SetTransitEnabled(false);
	}

	Actor->MarkPackageDirty();
	Entry->bWasChecked = false;
	Entry->ResolveResult = FWPTransitResolveResult();
	Entry->StatusText = FText::GetEmpty();
}

UWorld* FWPTransitManagerActorController::GetEditorWorld() const
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

#undef LOCTEXT_NAMESPACE
