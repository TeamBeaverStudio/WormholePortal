// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Room/WPDemoRoomManager.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Room/WPDemoRoom.h"
#include "Templates/UnrealTemplate.h"
#include "WPLog.h"

DEFINE_LOG_CATEGORY_STATIC(LogWPDemoRoomManager, Log, All);

AWPDemoRoomManager::AWPDemoRoomManager(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
}

void AWPDemoRoomManager::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	UWorld* World = GetWorld();
	if (!IsValid(World)
		|| !World->IsGameWorld()
		|| World->bIsTearingDown)
	{
		return;
	}

	if (World->GetBegunPlay())
	{
		// A Manager spawned after world startup has missed the deterministic
		// pre-BeginPlay phase. Capture the best available runtime membership.
		PrepareRoomMembershipBeforeBeginPlay();
		return;
	}

	if (WorldPreBeginPlayHandle.IsValid())
	{
		return;
	}

	// The World broadcasts this only after every Actor has completed component
	// initialization and immediately before the first Actor receives BeginPlay.
	WorldPreBeginPlayHandle = World->OnWorldPreBeginPlay.AddUObject(
		this, &AWPDemoRoomManager::HandleWorldPreBeginPlay);
	WorldBegunPlayHandle = World->GetOnBeginPlayEvent().AddUObject(
		this, &AWPDemoRoomManager::HandleWorldBegunPlay);
}

void AWPDemoRoomManager::BeginPlay()
{
	Super::BeginPlay();

	CurrentRoomIndex = INDEX_NONE;
	bConfigurationValid = false;
	bInitializationComplete = false;
	bTransitionInProgress = false;

	UWorld* World = GetWorld();
	if (!IsValid(World) || World->bIsTearingDown)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room lifecycle initialization failed because the World is unavailable. Manager=%s"),
			*GetNameSafe(this));
		#endif
		bInitializationComplete = true;
		return;
	}

	// This is a fallback for unusual startup paths that did not broadcast the
	// normal world pre-BeginPlay phase.
	if (!bMembershipPreparationComplete)
	{
		RemoveWorldPreBeginPlayDelegate();
		PrepareRoomMembershipBeforeBeginPlay();
	}

	if (!bMembershipPreparationValid)
	{
		RemoveWorldBegunPlayDelegate();
		bInitializationComplete = true;
		return;
	}

	// A Manager spawned after world startup misses the normal begun-play event,
	// so it finalizes immediately using the best available membership snapshot.
	if (World->GetBegunPlay())
	{
		RemoveWorldBegunPlayDelegate();
		InitializeRoomsForPlay();
	}
	else if (!WorldBegunPlayHandle.IsValid())
	{
		WorldBegunPlayHandle = World->GetOnBeginPlayEvent().AddUObject(
			this, &AWPDemoRoomManager::HandleWorldBegunPlay);
	}
}

void AWPDemoRoomManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	TearDownRoomLifecycle();
	Super::EndPlay(EndPlayReason);
}

void AWPDemoRoomManager::Destroyed()
{
	TearDownRoomLifecycle();
	Super::Destroyed();
}

bool AWPDemoRoomManager::SetCurrentRoomToNext()
{
	if (!bInitializationComplete || !bConfigurationValid || !Rooms.IsValidIndex(CurrentRoomIndex))
	{
		return false;
	}

	if (CurrentRoomIndex >= Rooms.Num() - 1)
	{
		return false;
	}

	return SetCurrentRoomByIndex(CurrentRoomIndex + 1);
}

bool AWPDemoRoomManager::SetCurrentRoomToPrevious()
{
	if (!bInitializationComplete || !bConfigurationValid || !Rooms.IsValidIndex(CurrentRoomIndex))
	{
		return false;
	}

	if (CurrentRoomIndex <= 0)
	{
		return false;
	}

	return SetCurrentRoomByIndex(CurrentRoomIndex - 1);
}

bool AWPDemoRoomManager::SetCurrentRoomByIndex(const int32 RoomIndex)
{
	if (!bInitializationComplete || !bConfigurationValid)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room change rejected because initialization did not succeed. Manager=%s RequestedIndex=%d"),
			*GetNameSafe(this), RoomIndex);
		#endif
		return false;
	}

	if (bTransitionInProgress)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room change rejected because another lifecycle transition is in progress. Manager=%s RequestedIndex=%d"),
			*GetNameSafe(this), RoomIndex);
		#endif
		return false;
	}

	if (!Rooms.IsValidIndex(RoomIndex))
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room change rejected because the requested index is out of bounds. Manager=%s RequestedIndex=%d RoomCount=%d"),
			*GetNameSafe(this), RoomIndex, Rooms.Num());
		#endif
		return false;
	}

	if (!Rooms.IsValidIndex(CurrentRoomIndex))
	{
		WP_LOG(this, Error,
			TEXT("Room change rejected because the current index is invalid. Manager=%s CurrentIndex=%d"),
			*GetNameSafe(this), CurrentRoomIndex);
		bConfigurationValid = false;
		CurrentRoomIndex = INDEX_NONE;
		return false;
	}

	if (RoomIndex == CurrentRoomIndex)
	{
		return false;
	}

	AWPDemoRoom* PreviousRoom = Rooms[CurrentRoomIndex].Get();
	AWPDemoRoom* TargetRoom = Rooms[RoomIndex].Get();
	if (!IsRoomReady(PreviousRoom) || !IsRoomReady(TargetRoom))
	{
		WP_LOG(this, Error,
			TEXT("Room change rejected because a lifecycle Room is no longer ready. Manager=%s PreviousRoom=%s TargetRoom=%s"),
			*GetNameSafe(this), *GetNameSafe(PreviousRoom), *GetNameSafe(TargetRoom));
		bConfigurationValid = false;
		CurrentRoomIndex = INDEX_NONE;
		return false;
	}

	const int32 PreviousRoomIndex = CurrentRoomIndex;
	TGuardValue<bool> TransitionGuard(bTransitionInProgress, true);

	// The old index deliberately remains visible during the deactivation callback.
	if (!PreviousRoom->DeactivateForManager(this))
	{
		FailClosedAfterLifecycleError(TEXT("deactivate"), PreviousRoom);
		return false;
	}

	if (!IsValid(this))
	{
		return false;
	}

	CurrentRoomIndex = RoomIndex;

	if (!IsRoomReady(TargetRoom) || !TargetRoom->ResetForManager(this))
	{
		FailClosedAfterLifecycleError(TEXT("reset"), TargetRoom);
		return false;
	}

	if (!IsValid(this))
	{
		return false;
	}

	if (!IsRoomReady(TargetRoom) || !TargetRoom->ActivateForManager(this))
	{
		FailClosedAfterLifecycleError(TEXT("activate"), TargetRoom);
		return false;
	}

	if (!IsValid(this))
	{
		return false;
	}

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("Current Room changed. Manager=%s PreviousRoom=%s PreviousIndex=%d CurrentRoom=%s CurrentIndex=%d"),
		*GetNameSafe(this),
		*GetNameSafe(PreviousRoom),
		PreviousRoomIndex,
		*GetNameSafe(TargetRoom),
		RoomIndex);
	#endif

	OnRoomChanged.Broadcast(PreviousRoom, PreviousRoomIndex, TargetRoom, RoomIndex);
	return true;
}

AWPDemoRoom* AWPDemoRoomManager::GetCurrentRoom() const
{
	if (!Rooms.IsValidIndex(CurrentRoomIndex))
	{
		return nullptr;
	}

	AWPDemoRoom* CurrentRoom = Rooms[CurrentRoomIndex].Get();
	return IsValid(CurrentRoom) ? CurrentRoom : nullptr;
}

int32 AWPDemoRoomManager::GetCurrentRoomIndex() const
{
	return CurrentRoomIndex;
}

void AWPDemoRoomManager::HandleWorldPreBeginPlay()
{
	RemoveWorldPreBeginPlayDelegate();
	PrepareRoomMembershipBeforeBeginPlay();
}

void AWPDemoRoomManager::HandleWorldBegunPlay(const bool bHasBegunPlay)
{
	if (!bHasBegunPlay)
	{
		return;
	}

	RemoveWorldBegunPlayDelegate();
	InitializeRoomsForPlay();
}

void AWPDemoRoomManager::PrepareRoomMembershipBeforeBeginPlay()
{
	if (bMembershipPreparationComplete)
	{
		return;
	}

	bMembershipPreparationComplete = true;
	bMembershipPreparationValid = false;
	bConfigurationValid = false;
	CurrentRoomIndex = INDEX_NONE;

	UWorld* World = GetWorld();
	if (!IsValid(World) || World->bIsTearingDown)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room membership preparation failed because the World is unavailable. Manager=%s"),
			*GetNameSafe(this));
		#endif
		return;
	}

	if (!ValidateConfiguration())
	{
		RemoveWorldBegunPlayDelegate();
		return;
	}

	bMembershipPreparationValid = true;
}

void AWPDemoRoomManager::InitializeRoomsForPlay()
{
	if (bInitializationComplete)
	{
		return;
	}

	bInitializationComplete = true;
	bConfigurationValid = false;
	CurrentRoomIndex = INDEX_NONE;

	UWorld* World = GetWorld();
	if (!IsValid(World) || World->bIsTearingDown)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("Room lifecycle initialization failed because the World is tearing down. Manager=%s"),
			*GetNameSafe(this));
		#endif
		return;
	}

	if (!bMembershipPreparationComplete || !bMembershipPreparationValid)
	{
		WP_LOG(this, Error,
			TEXT("Room lifecycle initialization failed because pre-BeginPlay membership was not prepared. Manager=%s"),
			*GetNameSafe(this));
		return;
	}

	for (AWPDemoRoom* Room : Rooms)
	{
		if (!IsValid(Room)
			|| Room->GetWorld() != World
			|| Room->OwningManager.Get() != this
			|| !Room->bMembershipCaptured
			|| !Room->bMembershipValid
			|| !Room->CaptureInitialStateForManager(this))
		{
			bMembershipPreparationValid = false;
			ReleaseClaimedRooms();
			return;
		}
	}

	TGuardValue<bool> TransitionGuard(bTransitionInProgress, true);

	for (AWPDemoRoom* Room : Rooms)
	{
		if (!IsRoomReady(Room) || !Room->DeactivateForManager(this))
		{
			FailClosedAfterLifecycleError(TEXT("initial deactivate"), Room);
			return;
		}

		if (!IsValid(this))
		{
			return;
		}
	}

	AWPDemoRoom* InitialRoom = Rooms[InitialRoomIndex].Get();
	CurrentRoomIndex = InitialRoomIndex;

	if (!IsRoomReady(InitialRoom) || !InitialRoom->ResetForManager(this))
	{
		FailClosedAfterLifecycleError(TEXT("initial reset"), InitialRoom);
		return;
	}

	if (!IsValid(this))
	{
		return;
	}

	if (!IsRoomReady(InitialRoom) || !InitialRoom->ActivateForManager(this))
	{
		FailClosedAfterLifecycleError(TEXT("initial activate"), InitialRoom);
		return;
	}

	if (!IsValid(this))
	{
		return;
	}

	bConfigurationValid = true;

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("Room lifecycle initialized. Manager=%s RoomCount=%d CurrentRoom=%s CurrentIndex=%d"),
		*GetNameSafe(this),
		Rooms.Num(),
		*GetNameSafe(InitialRoom),
		InitialRoomIndex);
	#endif
}

bool AWPDemoRoomManager::ValidateConfiguration()
{
	UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return false;
	}

	if (Rooms.IsEmpty())
	{
		WP_LOG(this, Error,
			TEXT("Room lifecycle configuration requires at least one Room. Manager=%s"),
			*GetNameSafe(this));
		return false;
	}

	if (!Rooms.IsValidIndex(InitialRoomIndex))
	{
		WP_LOG(this, Error,
			TEXT("InitialRoomIndex is out of bounds. Manager=%s InitialRoomIndex=%d RoomCount=%d"),
			*GetNameSafe(this), InitialRoomIndex, Rooms.Num());
		return false;
	}

	for (TActorIterator<AWPDemoRoomManager> ManagerIt(World); ManagerIt; ++ManagerIt)
	{
		AWPDemoRoomManager* OtherManager = *ManagerIt;
		if (OtherManager != this
			&& IsValid(OtherManager)
			&& !OtherManager->IsActorBeingDestroyed())
		{
			WP_LOG(this, Error,
				TEXT("Only one live Demo Room Manager may exist in a World. Manager=%s OtherManager=%s"),
				*GetNameSafe(this), *GetNameSafe(OtherManager));
			return false;
		}
	}

	bool bIsValid = true;
	TSet<const AWPDemoRoom*> UniqueRooms;

	// Validate references before claiming any Room so simple authoring errors do
	// not leave partial Manager ownership behind.
	for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
	{
		AWPDemoRoom* Room = Rooms[RoomIndex].Get();
		if (!IsValid(Room))
		{
			WP_LOG(this, Error,
				TEXT("Room configuration contains an invalid Room reference. Manager=%s RoomIndex=%d"),
				*GetNameSafe(this), RoomIndex);
			bIsValid = false;
			continue;
		}

		if (Room->GetWorld() != World)
		{
			WP_LOG(this, Error,
				TEXT("Room configuration references another World. Manager=%s RoomIndex=%d Room=%s"),
				*GetNameSafe(this), RoomIndex, *GetNameSafe(Room));
			bIsValid = false;
			continue;
		}

		if (UniqueRooms.Contains(Room))
		{
			WP_LOG(this, Error,
				TEXT("Room configuration contains a duplicate Room reference. Manager=%s RoomIndex=%d Room=%s"),
				*GetNameSafe(this), RoomIndex, *GetNameSafe(Room));
			bIsValid = false;
		}
		else
		{
			UniqueRooms.Add(Room);
		}
	}

	if (!bIsValid)
	{
		return false;
	}

	TArray<AWPDemoRoom*> ClaimedRooms;
	TMap<const AActor*, int32> FirstRoomByActor;

	for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
	{
		AWPDemoRoom* Room = Rooms[RoomIndex].Get();
		const bool bRoomInitialized = Room->CaptureMembershipForManager(this);

		if (Room->OwningManager.Get() == this)
		{
			ClaimedRooms.AddUnique(Room);
		}

		if (!bRoomInitialized)
		{
			bIsValid = false;
			continue;
		}

		for (const TWeakObjectPtr<AActor>& ManagedActorPtr : Room->ManagedActorSnapshot)
		{
			const AActor* ManagedActor = ManagedActorPtr.Get();
			if (!IsValid(ManagedActor))
			{
				continue;
			}

			if (const int32* FirstRoomIndex = FirstRoomByActor.Find(ManagedActor))
			{
				WP_LOG(this, Error,
					TEXT("Actor belongs to multiple Rooms. Manager=%s Actor=%s FirstRoomIndex=%d DuplicateRoomIndex=%d"),
					*GetNameSafe(this), *GetNameSafe(ManagedActor), *FirstRoomIndex, RoomIndex);
				bIsValid = false;
			}
			else
			{
				FirstRoomByActor.Add(ManagedActor, RoomIndex);
			}
		}
	}

	if (!bIsValid)
	{
		for (AWPDemoRoom* ClaimedRoom : ClaimedRooms)
		{
			if (IsValid(ClaimedRoom))
			{
				ClaimedRoom->ReleaseManager(this);
			}
		}
	}

	return bIsValid;
}

void AWPDemoRoomManager::RemoveWorldPreBeginPlayDelegate()
{
	if (!WorldPreBeginPlayHandle.IsValid())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->OnWorldPreBeginPlay.Remove(WorldPreBeginPlayHandle);
	}

	WorldPreBeginPlayHandle.Reset();
}

void AWPDemoRoomManager::RemoveWorldBegunPlayDelegate()
{
	if (!WorldBegunPlayHandle.IsValid())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetOnBeginPlayEvent().Remove(WorldBegunPlayHandle);
	}

	WorldBegunPlayHandle.Reset();
}

void AWPDemoRoomManager::TearDownRoomLifecycle()
{
	RemoveWorldPreBeginPlayDelegate();
	RemoveWorldBegunPlayDelegate();
	ReleaseClaimedRooms();

	CurrentRoomIndex = INDEX_NONE;
	bMembershipPreparationComplete = false;
	bMembershipPreparationValid = false;
	bConfigurationValid = false;
	bInitializationComplete = true;
	bTransitionInProgress = false;
}

void AWPDemoRoomManager::ReleaseClaimedRooms()
{
	for (AWPDemoRoom* Room : Rooms)
	{
		if (IsValid(Room) && Room->OwningManager.Get() == this)
		{
			Room->ReleaseManager(this);
		}
	}
}

void AWPDemoRoomManager::FailClosedAfterLifecycleError(
	const TCHAR* Operation,
	AWPDemoRoom* Room)
{
	WP_LOG(this, Error,
		TEXT("Room lifecycle failed during %s; the Manager is now invalid. Manager=%s Room=%s"),
		Operation,
		*GetNameSafe(this),
		*GetNameSafe(Room));

	CurrentRoomIndex = INDEX_NONE;
	bConfigurationValid = false;

	// Best effort only: a lifecycle implementation may already have failed
	// midway. Keeping requests disabled and deactivating all remaining Rooms is
	// safer than selecting a partially active Room.
	for (AWPDemoRoom* ConfiguredRoom : Rooms)
	{
		if (IsRoomReady(ConfiguredRoom))
		{
			ConfiguredRoom->DeactivateForManager(this);
		}
	}
}

bool AWPDemoRoomManager::IsRoomReady(const AWPDemoRoom* Room) const
{
	return IsValid(Room)
		&& Room->GetWorld() == GetWorld()
		&& Room->OwningManager.Get() == this
		&& Room->bMembershipCaptured
		&& Room->bInitializedForManager
		&& Room->bMembershipValid
		&& !Room->bLifecycleInProgress;
}
