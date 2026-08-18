// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "WPDemoRoom.generated.h"

class AWPDemoRoomManager;
class UActorComponent;
class UPrimitiveComponent;
class USceneComponent;

/**
 * Placeable metadata anchor for one demo room.
 *
 * Actors attached below this anchor, plus AdditionalManagedActors, are captured once
 * before BeginPlay can detach physics Actors. ExcludedActors and their attached subtrees
 * are removed from that snapshot. Their runtime initial state is captured separately
 * after BeginPlay. The owning Manager can then reset and switch this room between its
 * active and inactive states without moving the player.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Demo Room"))
class WORMHOLEPORTALSAMPLE_API AWPDemoRoom : public AActor
{
	GENERATED_BODY()

public:
	AWPDemoRoom(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Returns whether this Room is currently the Manager's active Room. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Demo|Room")
	bool IsRoomActive() const;

protected:
	/**
	 * Restores puzzle-specific state that cannot be captured generically.
	 * The Room is still inactive while this event runs.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Wormhole Portal|Demo|Room",
		meta = (DisplayName = "On Room Reset"))
	void OnRoomReset();
	virtual void OnRoomReset_Implementation();

	/** Starts room-specific behavior after the captured runtime state has been restored. */
	UFUNCTION(BlueprintNativeEvent, Category = "Wormhole Portal|Demo|Room",
		meta = (DisplayName = "On Room Activated"))
	void OnRoomActivated();
	virtual void OnRoomActivated_Implementation();

	/** Stops room-specific behavior immediately before the generic inactive state is applied. */
	UFUNCTION(BlueprintNativeEvent, Category = "Wormhole Portal|Demo|Room",
		meta = (DisplayName = "On Room Deactivated"))
	void OnRoomDeactivated();
	virtual void OnRoomDeactivated_Implementation();

private:
	struct FActorInitialState
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<USceneComponent> AttachParentComponent;
		FName AttachSocketName = NAME_None;
		FTransform WorldTransform = FTransform::Identity;
		FTransform RelativeTransform = FTransform::Identity;
		bool bWasAttached = false;
		bool bTickEnabled = false;
		bool bHiddenInGame = false;
		bool bCollisionEnabled = false;
	};

	struct FComponentInitialState
	{
		TWeakObjectPtr<UActorComponent> Component;
		TWeakObjectPtr<USceneComponent> AttachParentComponent;
		FName AttachSocketName = NAME_None;
		FTransform WorldTransform = FTransform::Identity;
		FTransform RelativeTransform = FTransform::Identity;
		FName CollisionProfileName = NAME_None;
		FCollisionResponseContainer CollisionResponses;
		FVector LinearVelocity = FVector::ZeroVector;
		FVector AngularVelocityInDegrees = FVector::ZeroVector;
		TEnumAsByte<ECollisionChannel> CollisionObjectType = ECC_WorldStatic;
		ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
		bool bTickEnabled = false;
		bool bActive = false;
		bool bIsSceneComponent = false;
		bool bWasAttached = false;
		bool bVisible = false;
		bool bHiddenInGame = false;
		bool bIsPrimitiveComponent = false;
		bool bGenerateOverlapEvents = false;
		bool bSimulatePhysics = false;
	};

	/** Static, non-rendering anchor used to organize room Actors in the World Outliner. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Demo|Room",
		meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> RoomRoot;

	/**
	 * Extra roots that cannot be attached below this Room.
	 * Each valid root and its recursively attached subtree are included.
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Demo|Membership",
		meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<AActor>> AdditionalManagedActors;

	/**
	 * Actors that must remain outside this Room's lifecycle.
	 * Each valid Actor and its recursively attached subtree take precedence over all inclusion paths.
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Demo|Membership",
		meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<AActor>> ExcludedActors;

	/** Pre-BeginPlay membership snapshot. Runtime attachment changes do not alter room ownership. */
	TArray<TWeakObjectPtr<AActor>> ManagedActorSnapshot;

	/** Common Actor state captured when the Manager initializes this Room. */
	TArray<FActorInitialState> ActorInitialStates;

	/** Common Component state captured when the Manager initializes this Room. */
	TArray<FComponentInitialState> ComponentInitialStates;

	/** Manager that initialized this Room. A live Room cannot belong to two Managers. */
	TWeakObjectPtr<AWPDemoRoomManager> OwningManager;

	bool bMembershipCaptured = false;
	bool bInitializedForManager = false;
	bool bMembershipValid = false;
	bool bLifecycleInProgress = false;
	bool bRoomActive = false;
	bool bResetPrepared = false;

	/** Claims this Room and freezes deterministic membership before Actor BeginPlay. */
	bool CaptureMembershipForManager(AWPDemoRoomManager* Manager);

	/** Captures the frozen membership's runtime initial state after Actor BeginPlay. */
	bool CaptureInitialStateForManager(AWPDemoRoomManager* Manager);

	/** Rolls back a Manager claim after sequence-wide validation fails. */
	void ReleaseManager(AWPDemoRoomManager* Manager);

	/** Restores attachments, transforms, and passive state while keeping the Room inactive. */
	bool ResetForManager(AWPDemoRoomManager* Manager);

	/** Restores the exact captured runtime flags and makes the Room active. */
	bool ActivateForManager(AWPDemoRoomManager* Manager);

	/** Hides and stops the Room while preserving enough state for a later reset. */
	bool DeactivateForManager(AWPDemoRoomManager* Manager);

	/** Recursively collects one exclusion root and its attached subtree. */
	static void CollectExcludedSubtree(AActor* RootActor, TSet<AActor*>& InOutExcludedActors);

	/** Recursively captures one managed root while pruning excluded and nested Room subtrees. */
	void CollectManagedSubtree(
		AActor* RootActor,
		const TSet<AActor*>& ExcludedActorSet,
		TSet<AActor*>& InOutVisitedActors);

	/** Captures the common initial state of the immutable membership snapshot. */
	void CaptureInitialState();

	/** Reattaches Actors and Components and restores transforms and passive collision configuration. */
	void RestorePassiveInitialState();

	/** Restores captured visibility, activity, tick, collision, overlap, and physics flags. */
	void RestoreRuntimeInitialState();

	/** Applies the generic hidden and stopped state to one managed Actor. */
	static void DeactivateActor(AActor* Actor);

	/** Checks that a lifecycle request came from this Room's valid owning Manager. */
	bool IsLifecycleRequestValid(const AWPDemoRoomManager* Manager, const TCHAR* OperationName) const;

	/** Prevents lifecycle changes to metadata and the currently player-controlled actors. */
	bool IsProtectedActor(const AActor* Actor) const;

	friend class AWPDemoRoomManager;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FWPDemoRoomTestAccessor;
#endif
};
