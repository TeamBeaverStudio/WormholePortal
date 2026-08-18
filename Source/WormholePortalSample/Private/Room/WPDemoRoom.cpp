// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Room/WPDemoRoom.h"

#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Room/WPDemoRoomManager.h"
#include "TimerManager.h"
#include "WPLog.h"

DEFINE_LOG_CATEGORY_STATIC(LogWPDemoRoom, Log, All);

AWPDemoRoom::AWPDemoRoom(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);

	RoomRoot = CreateDefaultSubobject<USceneComponent>(TEXT("RoomRoot"));
	RoomRoot->SetMobility(EComponentMobility::Static);
	SetRootComponent(RoomRoot);
}

bool AWPDemoRoom::IsRoomActive() const
{
	return bInitializedForManager && bMembershipValid && bRoomActive;
}

void AWPDemoRoom::OnRoomReset_Implementation()
{
}

void AWPDemoRoom::OnRoomActivated_Implementation()
{
}

void AWPDemoRoom::OnRoomDeactivated_Implementation()
{
}

bool AWPDemoRoom::CaptureMembershipForManager(AWPDemoRoomManager* Manager)
{
	if (!IsValid(Manager) || Manager->GetWorld() != GetWorld())
	{
		WP_LOG(this, Error,
			TEXT("Room membership capture requires a valid Manager in the same World. Room=%s Manager=%s"),
			*GetNameSafe(this), *GetNameSafe(Manager));
		return false;
	}

	if (bMembershipCaptured)
	{
		if (OwningManager.Get() == Manager)
		{
			return bMembershipValid;
		}

		if (OwningManager.IsValid())
		{
			WP_LOG(this, Error,
				TEXT("Room is already owned by another live Manager. Room=%s ExistingManager=%s RequestedManager=%s"),
				*GetNameSafe(this), *GetNameSafe(OwningManager.Get()), *GetNameSafe(Manager));
			return false;
		}
	}

	OwningManager = Manager;
	bMembershipCaptured = true;
	bInitializedForManager = false;
	bMembershipValid = true;
	bLifecycleInProgress = false;
	bRoomActive = false;
	bResetPrepared = false;
	ManagedActorSnapshot.Reset();
	ActorInitialStates.Reset();
	ComponentInitialStates.Reset();

	TSet<AActor*> ExcludedActorSet;
	for (int32 ActorIndex = 0; ActorIndex < ExcludedActors.Num(); ++ActorIndex)
	{
		AActor* ExcludedActor = ExcludedActors[ActorIndex].Get();
		if (!IsValid(ExcludedActor))
		{
			WP_LOG(this, Warning,
				TEXT("Room contains an invalid excluded Actor reference that will be skipped. Room=%s ActorIndex=%d"),
				*GetNameSafe(this), ActorIndex);
			continue;
		}

		if (ExcludedActor->GetWorld() != GetWorld())
		{
			WP_LOG(this, Error,
				TEXT("Room contains an excluded Actor from another World. Room=%s Actor=%s ActorIndex=%d"),
				*GetNameSafe(this), *GetNameSafe(ExcludedActor), ActorIndex);
			bMembershipValid = false;
			continue;
		}

		CollectExcludedSubtree(ExcludedActor, ExcludedActorSet);
	}

	TArray<AActor*> ManagedRoots;
	GetAttachedActors(ManagedRoots, true, false);
	for (int32 ActorIndex = 0; ActorIndex < AdditionalManagedActors.Num(); ++ActorIndex)
	{
		AActor* AdditionalActor = AdditionalManagedActors[ActorIndex].Get();
		if (!IsValid(AdditionalActor))
		{
			WP_LOG(this, Warning,
				TEXT("Room contains an invalid additional Actor reference that will be skipped. Room=%s ActorIndex=%d"),
				*GetNameSafe(this), ActorIndex);
			continue;
		}

		if (AdditionalActor->GetWorld() != GetWorld())
		{
			WP_LOG(this, Error,
				TEXT("Room contains an additional Actor from another World. Room=%s Actor=%s ActorIndex=%d"),
				*GetNameSafe(this), *GetNameSafe(AdditionalActor), ActorIndex);
			bMembershipValid = false;
			continue;
		}

		ManagedRoots.Add(AdditionalActor);
	}

	TSet<AActor*> VisitedActors;
	for (AActor* ManagedRoot : ManagedRoots)
	{
		CollectManagedSubtree(ManagedRoot, ExcludedActorSet, VisitedActors);
	}

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("Room membership captured before BeginPlay. Room=%s ManagedActorCount=%d Valid=%d"),
		*GetNameSafe(this),
		ManagedActorSnapshot.Num(),
		bMembershipValid ? 1 : 0);
	#endif

	return bMembershipValid;
}

bool AWPDemoRoom::CaptureInitialStateForManager(AWPDemoRoomManager* Manager)
{
	if (!IsValid(Manager)
		|| Manager->GetWorld() != GetWorld()
		|| OwningManager.Get() != Manager
		|| !bMembershipCaptured
		|| !bMembershipValid)
	{
		WP_LOG(this, Error,
			TEXT("Room initial-state capture requires valid pre-BeginPlay membership ownership. Room=%s Manager=%s"),
			*GetNameSafe(this), *GetNameSafe(Manager));
		return false;
	}

	if (bInitializedForManager)
	{
		return true;
	}

	CaptureInitialState();
	bInitializedForManager = true;
	bLifecycleInProgress = false;
	bRoomActive = true;
	bResetPrepared = false;

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("Room initial state captured after BeginPlay. Room=%s ManagedActorCount=%d ActorStateCount=%d ComponentStateCount=%d"),
		*GetNameSafe(this),
		ManagedActorSnapshot.Num(),
		ActorInitialStates.Num(),
		ComponentInitialStates.Num());
	#endif

	return true;
}

void AWPDemoRoom::ReleaseManager(AWPDemoRoomManager* Manager)
{
	if (OwningManager.Get() != Manager)
	{
		return;
	}

	ManagedActorSnapshot.Reset();
	ActorInitialStates.Reset();
	ComponentInitialStates.Reset();
	OwningManager.Reset();
	bMembershipCaptured = false;
	bInitializedForManager = false;
	bMembershipValid = false;
	bLifecycleInProgress = false;
	bRoomActive = false;
	bResetPrepared = false;
}

bool AWPDemoRoom::ResetForManager(AWPDemoRoomManager* Manager)
{
	if (!IsLifecycleRequestValid(Manager, TEXT("reset")))
	{
		return false;
	}

	if (bLifecycleInProgress)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room reset rejected during another lifecycle callback. Room=%s"),
			*GetNameSafe(this));
		#endif
		return false;
	}

	if (bRoomActive)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room reset requires the Room to be inactive. Room=%s"),
			*GetNameSafe(this));
		#endif
		return false;
	}

	bLifecycleInProgress = true;
	bResetPrepared = false;

	RestorePassiveInitialState();

	const TWeakObjectPtr<AWPDemoRoom> WeakThis(this);
	const TWeakObjectPtr<AWPDemoRoomManager> WeakManager(Manager);
	OnRoomReset();
	if (!WeakThis.IsValid() || !WeakManager.IsValid())
	{
		return false;
	}

	bResetPrepared = true;
	bLifecycleInProgress = false;
	return true;
}

bool AWPDemoRoom::ActivateForManager(AWPDemoRoomManager* Manager)
{
	if (!IsLifecycleRequestValid(Manager, TEXT("activation")))
	{
		return false;
	}

	if (bLifecycleInProgress)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room activation rejected during another lifecycle callback. Room=%s"),
			*GetNameSafe(this));
		#endif
		return false;
	}

	if (bRoomActive)
	{
		return true;
	}

	if (!bResetPrepared)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room activation requires a successful reset first. Room=%s"),
			*GetNameSafe(this));
		#endif
		return false;
	}

	bLifecycleInProgress = true;
	RestoreRuntimeInitialState();
	bRoomActive = true;
	bResetPrepared = false;

	const TWeakObjectPtr<AWPDemoRoom> WeakThis(this);
	const TWeakObjectPtr<AWPDemoRoomManager> WeakManager(Manager);
	OnRoomActivated();
	if (!WeakThis.IsValid() || !WeakManager.IsValid())
	{
		return false;
	}

	bLifecycleInProgress = false;
	return true;
}

bool AWPDemoRoom::DeactivateForManager(AWPDemoRoomManager* Manager)
{
	if (!IsLifecycleRequestValid(Manager, TEXT("deactivation")))
	{
		return false;
	}

	if (bLifecycleInProgress)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room deactivation rejected during another lifecycle callback. Room=%s"),
			*GetNameSafe(this));
		#endif
		return false;
	}

	if (!bRoomActive)
	{
		return true;
	}

	bLifecycleInProgress = true;

	const TWeakObjectPtr<AWPDemoRoom> WeakThis(this);
	const TWeakObjectPtr<AWPDemoRoomManager> WeakManager(Manager);
	OnRoomDeactivated();
	if (!WeakThis.IsValid() || !WeakManager.IsValid())
	{
		return false;
	}

	for (const TWeakObjectPtr<AActor>& ManagedActorPtr : ManagedActorSnapshot)
	{
		AActor* ManagedActor = ManagedActorPtr.Get();
		if (!IsValid(ManagedActor))
		{
			continue;
		}

		if (IsProtectedActor(ManagedActor))
		{
			continue;
		}

		DeactivateActor(ManagedActor);
	}

	bRoomActive = false;
	bResetPrepared = false;
	bLifecycleInProgress = false;
	return true;
}

void AWPDemoRoom::CollectExcludedSubtree(AActor* RootActor, TSet<AActor*>& InOutExcludedActors)
{
	if (!IsValid(RootActor) || InOutExcludedActors.Contains(RootActor))
	{
		return;
	}

	InOutExcludedActors.Add(RootActor);

	TArray<AActor*> AttachedActors;
	RootActor->GetAttachedActors(AttachedActors, true, false);
	for (AActor* AttachedActor : AttachedActors)
	{
		CollectExcludedSubtree(AttachedActor, InOutExcludedActors);
	}
}

void AWPDemoRoom::CollectManagedSubtree(
	AActor* RootActor,
	const TSet<AActor*>& ExcludedActorSet,
	TSet<AActor*>& InOutVisitedActors)
{
	if (!IsValid(RootActor) || InOutVisitedActors.Contains(RootActor))
	{
		return;
	}
	InOutVisitedActors.Add(RootActor);

	if (RootActor->GetWorld() != GetWorld())
	{
		WP_LOG(this, Error,
			TEXT("Room membership reached an Actor from another World. Room=%s Actor=%s"),
			*GetNameSafe(this), *GetNameSafe(RootActor));
		bMembershipValid = false;
		return;
	}

	if (ExcludedActorSet.Contains(RootActor))
	{
		return;
	}

	if (RootActor->IsA<AWPDemoRoom>())
	{
		WP_LOG(this, Error,
			TEXT("A Demo Room cannot be nested inside another Demo Room's managed hierarchy. Room=%s NestedRoom=%s"),
			*GetNameSafe(this), *GetNameSafe(RootActor));
		bMembershipValid = false;
		return;
	}

	ManagedActorSnapshot.Add(RootActor);

	TArray<AActor*> AttachedActors;
	RootActor->GetAttachedActors(AttachedActors, true, false);
	for (AActor* AttachedActor : AttachedActors)
	{
		CollectManagedSubtree(AttachedActor, ExcludedActorSet, InOutVisitedActors);
	}
}

void AWPDemoRoom::CaptureInitialState()
{
	ActorInitialStates.Reset();
	ComponentInitialStates.Reset();

	for (const TWeakObjectPtr<AActor>& ManagedActorPtr : ManagedActorSnapshot)
	{
		AActor* Actor = ManagedActorPtr.Get();
		if (!IsValid(Actor))
		{
			continue;
		}

		FActorInitialState& ActorState = ActorInitialStates.AddDefaulted_GetRef();
		ActorState.Actor = Actor;
		ActorState.WorldTransform = Actor->GetActorTransform();
		ActorState.bTickEnabled = Actor->IsActorTickEnabled();
		ActorState.bHiddenInGame = Actor->IsHidden();
		ActorState.bCollisionEnabled = Actor->GetActorEnableCollision();

		if (USceneComponent* ActorRootComponent = Actor->GetRootComponent())
		{
			ActorState.AttachParentComponent = ActorRootComponent->GetAttachParent();
			ActorState.AttachSocketName = ActorRootComponent->GetAttachSocketName();
			ActorState.RelativeTransform = ActorRootComponent->GetRelativeTransform();
			ActorState.bWasAttached = ActorRootComponent->GetAttachParent() != nullptr;
		}

		TInlineComponentArray<UActorComponent*> Components(Actor);
		for (UActorComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			FComponentInitialState& ComponentState = ComponentInitialStates.AddDefaulted_GetRef();
			ComponentState.Component = Component;
			ComponentState.bTickEnabled = Component->IsComponentTickEnabled();
			ComponentState.bActive = Component->IsActive();

			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				ComponentState.bIsSceneComponent = true;
				ComponentState.AttachParentComponent = SceneComponent->GetAttachParent();
				ComponentState.AttachSocketName = SceneComponent->GetAttachSocketName();
				ComponentState.WorldTransform = SceneComponent->GetComponentTransform();
				ComponentState.RelativeTransform = SceneComponent->GetRelativeTransform();
				ComponentState.bWasAttached = SceneComponent->GetAttachParent() != nullptr;
				ComponentState.bVisible = SceneComponent->IsVisible();
				ComponentState.bHiddenInGame = SceneComponent->bHiddenInGame;
			}

			if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
			{
				ComponentState.bIsPrimitiveComponent = true;
				ComponentState.CollisionProfileName = PrimitiveComponent->GetCollisionProfileName();
				ComponentState.CollisionObjectType = PrimitiveComponent->GetCollisionObjectType();
				ComponentState.CollisionResponses = PrimitiveComponent->GetCollisionResponseToChannels();
				ComponentState.CollisionEnabled = PrimitiveComponent->GetCollisionEnabled();
				ComponentState.bGenerateOverlapEvents = PrimitiveComponent->GetGenerateOverlapEvents();
				ComponentState.bSimulatePhysics = PrimitiveComponent->IsSimulatingPhysics();
				ComponentState.LinearVelocity = PrimitiveComponent->GetPhysicsLinearVelocity();
				ComponentState.AngularVelocityInDegrees =
					PrimitiveComponent->GetPhysicsAngularVelocityInDegrees();
			}
		}
	}
}

void AWPDemoRoom::RestorePassiveInitialState()
{
	// Stop physics before changing authored attachment relationships and transforms.
	for (const FComponentInitialState& ComponentState : ComponentInitialStates)
	{
		UActorComponent* Component = ComponentState.Component.Get();
		AActor* ComponentOwner = IsValid(Component) ? Component->GetOwner() : nullptr;
		if (!IsValid(Component) || !IsValid(ComponentOwner) || IsProtectedActor(ComponentOwner))
		{
			continue;
		}

		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
		{
			PrimitiveComponent->SetSimulatePhysics(false);
		}
	}

	for (const FActorInitialState& ActorState : ActorInitialStates)
	{
		AActor* Actor = ActorState.Actor.Get();
		if (!IsValid(Actor) || IsProtectedActor(Actor))
		{
			continue;
		}

		if (ActorState.bWasAttached)
		{
			if (USceneComponent* AttachParent = ActorState.AttachParentComponent.Get())
			{
				Actor->AttachToComponent(
					AttachParent,
					FAttachmentTransformRules::KeepRelativeTransform,
					ActorState.AttachSocketName);
				if (USceneComponent* ActorRootComponent = Actor->GetRootComponent())
				{
					ActorRootComponent->SetRelativeTransform(ActorState.RelativeTransform);
				}
			}
			else
			{
				WP_LOG(this, Warning,
					TEXT("Initial Actor attachment parent is no longer valid; restoring world transform instead. Room=%s Actor=%s"),
					*GetNameSafe(this), *GetNameSafe(Actor));
				Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
				Actor->SetActorTransform(ActorState.WorldTransform);
			}
		}
		else
		{
			Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			Actor->SetActorTransform(ActorState.WorldTransform);
		}
	}

	for (const FComponentInitialState& ComponentState : ComponentInitialStates)
	{
		UActorComponent* Component = ComponentState.Component.Get();
		AActor* ComponentOwner = IsValid(Component) ? Component->GetOwner() : nullptr;
		if (!IsValid(Component) || !IsValid(ComponentOwner) || IsProtectedActor(ComponentOwner))
		{
			continue;
		}

		if (ComponentState.bIsSceneComponent)
		{
			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				if (ComponentState.bWasAttached)
				{
					if (USceneComponent* AttachParent = ComponentState.AttachParentComponent.Get())
					{
						SceneComponent->AttachToComponent(
							AttachParent,
							FAttachmentTransformRules::KeepRelativeTransform,
							ComponentState.AttachSocketName);
						SceneComponent->SetRelativeTransform(ComponentState.RelativeTransform);
					}
					else
					{
						SceneComponent->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
						SceneComponent->SetWorldTransform(ComponentState.WorldTransform);
					}
				}
				else
				{
					SceneComponent->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
					SceneComponent->SetWorldTransform(ComponentState.WorldTransform);
				}
			}
		}

		if (ComponentState.bIsPrimitiveComponent)
		{
			if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
			{
				PrimitiveComponent->SetCollisionProfileName(ComponentState.CollisionProfileName, false);
				PrimitiveComponent->SetCollisionObjectType(ComponentState.CollisionObjectType);
				PrimitiveComponent->SetCollisionResponseToChannels(ComponentState.CollisionResponses);
				PrimitiveComponent->SetGenerateOverlapEvents(false);
				PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				PrimitiveComponent->SetSimulatePhysics(false);
			}
		}
	}
}

void AWPDemoRoom::RestoreRuntimeInitialState()
{
	for (const FComponentInitialState& ComponentState : ComponentInitialStates)
	{
		UActorComponent* Component = ComponentState.Component.Get();
		AActor* ComponentOwner = IsValid(Component) ? Component->GetOwner() : nullptr;
		if (!IsValid(Component) || !IsValid(ComponentOwner) || IsProtectedActor(ComponentOwner))
		{
			continue;
		}

		if (ComponentState.bIsSceneComponent)
		{
			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				SceneComponent->SetVisibility(ComponentState.bVisible, false);
				SceneComponent->SetHiddenInGame(ComponentState.bHiddenInGame, false);
			}
		}

		if (ComponentState.bIsPrimitiveComponent)
		{
			if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
			{
				PrimitiveComponent->SetCollisionProfileName(ComponentState.CollisionProfileName, false);
				PrimitiveComponent->SetCollisionObjectType(ComponentState.CollisionObjectType);
				PrimitiveComponent->SetCollisionResponseToChannels(ComponentState.CollisionResponses);
				PrimitiveComponent->SetGenerateOverlapEvents(
					ComponentState.bGenerateOverlapEvents);
				PrimitiveComponent->SetCollisionEnabled(ComponentState.CollisionEnabled);
				PrimitiveComponent->SetSimulatePhysics(ComponentState.bSimulatePhysics);

				if (ComponentState.bSimulatePhysics)
				{
					PrimitiveComponent->SetPhysicsLinearVelocity(ComponentState.LinearVelocity);
					PrimitiveComponent->SetPhysicsAngularVelocityInDegrees(
						ComponentState.AngularVelocityInDegrees);
				}
			}
		}

		if (ComponentState.bActive)
		{
			Component->Activate(true);
		}
		else
		{
			Component->Deactivate();
			if (IsValid(Component))
			{
				Component->SetActiveFlag(false);
			}
		}

		if (IsValid(Component))
		{
			Component->SetComponentTickEnabled(ComponentState.bTickEnabled);
		}
	}

	for (const FActorInitialState& ActorState : ActorInitialStates)
	{
		AActor* Actor = ActorState.Actor.Get();
		if (!IsValid(Actor) || IsProtectedActor(Actor))
		{
			continue;
		}

		Actor->SetActorHiddenInGame(ActorState.bHiddenInGame);
		Actor->SetActorEnableCollision(ActorState.bCollisionEnabled);
		Actor->SetActorTickEnabled(ActorState.bTickEnabled);
	}
}

void AWPDemoRoom::DeactivateActor(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return;
	}

	UWorld* World = Actor->GetWorld();
	Actor->SetActorTickEnabled(false);
	Actor->SetActorHiddenInGame(true);
	Actor->SetActorEnableCollision(false);
	if (!IsValid(Actor))
	{
		return;
	}

	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		if (!IsValid(Actor))
		{
			return;
		}

		if (!IsValid(Component))
		{
			continue;
		}

		Component->Deactivate();
		if (!IsValid(Component))
		{
			continue;
		}

		Component->SetActiveFlag(false);
		Component->SetComponentTickEnabled(false);

		if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
		{
			PrimitiveComponent->SetSimulatePhysics(false);
			if (!IsValid(Actor) || !IsValid(Component))
			{
				continue;
			}

			PrimitiveComponent->SetGenerateOverlapEvents(false);
			if (!IsValid(Actor) || !IsValid(Component))
			{
				continue;
			}

			PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}

		if (!IsValid(Actor) || !IsValid(Component))
		{
			continue;
		}

		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			// Membership is explicit; do not propagate visibility into excluded or foreign Actors.
			SceneComponent->SetVisibility(false, false);
			SceneComponent->SetHiddenInGame(true, false);
		}

		if (IsValid(World) && IsValid(Component))
		{
			World->GetTimerManager().ClearAllTimersForObject(Component);
			FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
			LatentActionManager.RemoveActionsForObject(Component);
			// Removal is normally deferred until the next latent-action update. Flush only
			// this now-empty target so synchronous Room changes leave no pending work behind.
			LatentActionManager.ProcessLatentActions(Component, 0.0f);
		}
	}

	if (!IsValid(Actor))
	{
		return;
	}

	if (IsValid(World))
	{
		World->GetTimerManager().ClearAllTimersForObject(Actor);
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		LatentActionManager.RemoveActionsForObject(Actor);
		LatentActionManager.ProcessLatentActions(Actor, 0.0f);
	}
}

bool AWPDemoRoom::IsLifecycleRequestValid(
	const AWPDemoRoomManager* Manager,
	const TCHAR* OperationName) const
{
	if (!bInitializedForManager
		|| !bMembershipValid
		|| !IsValid(Manager)
		|| OwningManager.Get() != Manager
		|| Manager->GetWorld() != GetWorld())
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room %s rejected because ownership or membership is invalid. Room=%s Manager=%s"),
			OperationName, *GetNameSafe(this), *GetNameSafe(Manager));
		#endif
		return false;
	}

	return true;
}

bool AWPDemoRoom::IsProtectedActor(const AActor* Actor) const
{
	if (!IsValid(Actor))
	{
		return true;
	}

	const auto IsDirectlyProtected = [this](const AActor* Candidate)
	{
		if (!IsValid(Candidate)
			|| Candidate == this
			|| Candidate == OwningManager.Get()
			|| Candidate->IsA<AWPDemoRoom>()
			|| Candidate->IsA<AWPDemoRoomManager>())
		{
			return true;
		}

		if (const AController* Controller = Cast<AController>(Candidate))
		{
			return Controller->IsPlayerController();
		}

		if (const APawn* Pawn = Cast<APawn>(Candidate))
		{
			const AController* Controller = Pawn->GetController();
			return IsValid(Controller) && Controller->IsPlayerController();
		}

		return false;
	};

	if (IsDirectlyProtected(Actor))
	{
		return true;
	}

	// Protect equipment and other current subtrees attached below player or lifecycle metadata.
	TSet<const AActor*> VisitedParents;
	for (const AActor* Parent = Actor->GetAttachParentActor();
		IsValid(Parent) && !VisitedParents.Contains(Parent);
		Parent = Parent->GetAttachParentActor())
	{
		VisitedParents.Add(Parent);

		// Being authored below this Room is the normal inclusion path, not a protection boundary.
		if (Parent == this)
		{
			break;
		}

		if (IsDirectlyProtected(Parent))
		{
			return true;
		}
	}

	return false;
}
