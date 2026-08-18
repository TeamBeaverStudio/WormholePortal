// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/WPTransitRun.h"
#include "Subsystems/WorldSubsystem.h"
#include "WPTransform.h"
#include "WPTransitSubsystem.generated.h"

class AWormholePortalActor;
class UPrimitiveComponent;
class UWPTransitComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPTransitStarted, const FWPTransitEvent&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPTransitCommitted, const FWPTransitEvent&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPTransitCancelled, const FWPTransitEvent&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWPTransitRejected, const FWPTransitEvent&);

/**
 * @brief Tickable World Subsystem that coordinates the complete Wormhole Transit
 *        lifecycle.
 *
 * Both Listen Servers and Dedicated Servers create and Commit Transit state under
 * server authority.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPTransitSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void			Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void			Deinitialize() override;
	virtual void			Tick(float DeltaTime) override;
	virtual TStatId 		GetStatId() const override;
	virtual bool			IsTickable() const override;
	
	// Delegates broadcast for the corresponding Transit lifecycle phase.
	FOnWPTransitStarted&	OnTransitStarted()	{ return TransitStartedDelegate; }
	FOnWPTransitCommitted&	OnTransitCommitted(){ return TransitCommittedDelegate; }
	FOnWPTransitCancelled&	OnTransitCancelled(){ return TransitCancelledDelegate; }
	FOnWPTransitRejected&	OnTransitRejected()	{ return TransitRejectedDelegate;}

	/**
	 * @brief Returns an Actor's role in the current Transit.
	 * @param Actor Actor to inspect.
	 * @return The Master or Twin role in the active Run, or None if no active relationship
	 *         exists.
	 */
	EWPTransitRole			GetTransitRole(const AActor* Actor) const;

	/**
	 * @brief Returns the counterpart Actor in the current Transit.
	 * @param Actor Master or Twin Actor.
	 * @return The Twin for a Master, the Master for a Twin, or nullptr if no active relationship exists.
	 */
	AActor*					GetCounterpartActor(const AActor* Actor) const;

	/**
	 * @brief Returns whether a Source-space point has crossed the Transit entry tangent plane.
	 * @param Actor Master or Twin Actor in the active Run.
	 * @param SourceWorldPoint World-space position to test in Source space.
	 * @return true if the point is on the Portal-interior side of the tangent plane.
	 */
	bool					IsPointAcross(const AActor* Actor, const FVector& SourceWorldPoint) const;

	/** Attempts to start Transit for an Actor detected by a Portal Overlap. */
	bool					TryStart(UWPTransitComponent* TransitComp, AWormholePortalActor* SourcePortal, UPrimitiveComponent* OverlappedComponent);

	/**
	 * @brief Cancels the active Transit owned by the specified Component.
	 * @param TransitComponent Transit Component of the Master whose active Transit is canceled.
	 * @return true if the active Run is found and canceled in a server-authoritative World.
	 */
	bool					CancelTransit(UWPTransitComponent* TransitComponent);

private:
	friend class UWPTransitComponent;
	
	/** In-progress Transit Runs, one per Actor. */
	UPROPERTY(Transient)
	TArray<FWPTransitRun> Runs;

	/** Returns true if the current World has server authority to modify Transit state. */
	bool HasAuthority() const;
	
	// ********************** Transit Life Cycle Section **********************
	
	// Creates the Twin Actor and registers a new Transit Run.
	bool Start(UWPTransitComponent* TransitComp, AActor* MasterActor, AWormholePortalActor* SourcePortal,
		const FWPTransform& Mapping, OUT EWPTransitFailReason& OutFailReason);

	/** Updates Cancel and Commit conditions for an in-progress Run. */
	void UpdateRun(FWPTransitRun& Run);

	// Reports a Transit attempt that could not start as a Rejected event.
	void Reject(const UWPTransitComponent* TransitComponent, AActor* MasterActor, AWormholePortalActor* SourcePortal, EWPTransitFailReason FailReason);

	/** Cancels Transit and restores Master state. */
	void Cancel(FWPTransitRun& Run, EWPTransitFailReason FailReason);

	/** Applies Destination state to the Master and completes Transit. */
	void Commit(FWPTransitRun& Run, const FWPTransform& Mapping);
	
	// Removes a finished Run from the active list and clears its Twin Actor and Portal references.
	void CloseRun(FWPTransitRun& Run);
	
	// ************************************************************************
	
	/**
 	 * @brief Creates a Twin Actor in Destination space using the Master as a Spawn Template.
 	 *
 	 * Uses the Engine Template Spawn result without rebuilding the user's Component
 	 * hierarchy. Initializes only Transit-owned Actor state.
 	 * @param Run Transit state that records the created Twin.
 	 * @param Master Template Actor used to create the Twin.
 	 * @param Mapping Portal Mapping from Source to Destination.
 	 * @return true if the Twin and Transit-owned state are initialized successfully.
 	 */
	bool CreateTwinActor(FWPTransitRun& Run, AActor* Master, const FWPTransform& Mapping);

	// Removes the Twin Actor owned by Transit.
	void RemoveTwinActor(FWPTransitRun& Run);

	/** Removes invalid Runs and rebuilds the active list. */
	void CleanRuns();

	/** Returns true if the Portal can be used by the current Transit. */
	bool CanUseGate(const AWormholePortalActor* Gate) const;
	
	/** Retrieves the Master Actor from the Transit Component that started the Run. */
	static AActor* GetMaster(const FWPTransitRun& Run);

	/** Finds the active Run containing the Master or Twin Actor. */
	const FWPTransitRun* FindRun(const AActor* Actor) const;

	/** Creates a value snapshot for native delegates before Run references are cleared. */
	FWPTransitEvent BuildTransitEvent(const FWPTransitRun& Run, EWPTransitResult Result, EWPTransitFailReason FailReason = EWPTransitFailReason::None) const;

	FOnWPTransitStarted		TransitStartedDelegate;
	FOnWPTransitCommitted	TransitCommittedDelegate;
	FOnWPTransitCancelled	TransitCancelledDelegate;
	FOnWPTransitRejected	TransitRejectedDelegate;

	/** Assigns a monotonically increasing sequence to each successfully started Transit in this World. */
	uint64 LastTransitSequence = 0;
};
