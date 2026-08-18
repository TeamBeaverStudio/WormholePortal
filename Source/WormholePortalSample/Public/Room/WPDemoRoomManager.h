// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "WPDemoRoomManager.generated.h"

class AWPDemoRoom;

/** Broadcast after the current room has changed and the target room is active. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
	FOnWPDemoRoomChanged,
	AWPDemoRoom*, PreviousRoom,
	int32, PreviousRoomIndex,
	AWPDemoRoom*, CurrentRoom,
	int32, CurrentRoomIndex);

/**
 * Coordinates the active room in an ordered set of demo rooms.
 *
 * This Actor never queries or moves a player. Callers change only the logical
 * current Room by index. Only the current Room remains active; a Room is reset
 * to its captured initial state whenever it becomes current.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Demo Room Manager"))
class WORMHOLEPORTALSAMPLE_API AWPDemoRoomManager : public AInfo
{
	GENERATED_BODY()

public:
	AWPDemoRoomManager(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

public:
	/** Makes the next ordered Room current. Does not move the player. */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Demo|Room")
	bool SetCurrentRoomToNext();

	/** Makes the previous ordered Room current. Does not move the player. */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Demo|Room")
	bool SetCurrentRoomToPrevious();

	/**
	 * Makes the Room at RoomIndex current.
	 *
	 * The previous Room is deactivated and the target Room is reset and activated
	 * synchronously before this function returns.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Demo|Room")
	bool SetCurrentRoomByIndex(int32 RoomIndex);

	/** Returns the active Room, or nullptr when initialization failed. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Demo|Room")
	AWPDemoRoom* GetCurrentRoom() const;

	/** Returns the active Room's zero-based array index, or INDEX_NONE when invalid. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Demo|Room")
	int32 GetCurrentRoomIndex() const;

	/** Raised once after a successful synchronous room change. */
	UPROPERTY(BlueprintAssignable, Category = "Wormhole Portal|Demo|Room")
	FOnWPDemoRoomChanged OnRoomChanged;

private:
	/** Placed Room anchors in navigation order. Indices are zero-based. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Demo|Rooms",
		meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<AWPDemoRoom>> Rooms;

	/** Room made current after all placed Actors have completed BeginPlay. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Demo|Rooms",
		meta = (AllowPrivateAccess = "true", ClampMin = "0"))
	int32 InitialRoomIndex = 0;

	/** Current position in Rooms, or INDEX_NONE when initialization failed. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Wormhole Portal|Demo|State",
		meta = (AllowPrivateAccess = "true"))
	int32 CurrentRoomIndex = INDEX_NONE;

	/** Delegate registered until the World reaches the pre-BeginPlay membership phase. */
	FDelegateHandle WorldPreBeginPlayHandle;

	/** Delegate registered until UWorld marks all placed Actor BeginPlay complete. */
	FDelegateHandle WorldBegunPlayHandle;

	/** True after the Manager has attempted to freeze authored Room membership. */
	bool bMembershipPreparationComplete = false;

	/** True only when pre-BeginPlay Room membership validation succeeded. */
	bool bMembershipPreparationValid = false;

	/** True only after configuration and initial lifecycle setup both succeed. */
	bool bConfigurationValid = false;

	/** True after this Manager has attempted its one-time play initialization. */
	bool bInitializationComplete = false;

	/** Blocks lifecycle callbacks from starting a nested room transition. */
	bool bTransitionInProgress = false;

	/** Called immediately before the World dispatches BeginPlay to any placed Actor. */
	void HandleWorldPreBeginPlay();

	/** Called when UWorld marks all placed Actor BeginPlay complete. */
	void HandleWorldBegunPlay(bool bHasBegunPlay);

	/** Freezes Room membership before physics components can detach during BeginPlay. */
	void PrepareRoomMembershipBeforeBeginPlay();

	/** Captures runtime initial state and establishes the initial active Room. */
	void InitializeRoomsForPlay();

	/** Validates authoring data, claims Rooms, and checks frozen Actor ownership. */
	bool ValidateConfiguration();

	/** Removes the World pre-begin-play callback when it is still registered. */
	void RemoveWorldPreBeginPlayDelegate();

	/** Removes the World begun-play callback when it is still registered. */
	void RemoveWorldBegunPlayDelegate();

	/** Idempotently removes delegates, releases claims, and invalidates runtime state. */
	void TearDownRoomLifecycle();

	/** Releases every Room currently claimed by this Manager. */
	void ReleaseClaimedRooms();

	/** Leaves no Room current after an unrecoverable lifecycle operation failure. */
	void FailClosedAfterLifecycleError(const TCHAR* Operation, AWPDemoRoom* Room);

	/** Returns whether a Room reference is still owned and usable by this Manager. */
	bool IsRoomReady(const AWPDemoRoom* Room) const;

#if WITH_DEV_AUTOMATION_TESTS
	friend struct FWPDemoRoomManagerTestAccessor;
#endif
};
