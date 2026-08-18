// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Transit/WPTransitTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Transit/WPTransitRun.h"
#include "WPTransitComponent.generated.h"

class UMovementComponent;
class UPrimitiveComponent;
class USceneComponent;

class UWPVoxelData;
class FWPCharacterHandler;
class IWPTransitHandler;

/**
 * @brief Transit relationship currently resolved by one client-side Component.
 *
 * Stores the same Run snapshot on the existing Master and Twin Components instead of
 * creating a global relationship Map.
 * Does not include server Physics or Voxel execution state.
 */
struct FWPTransitClientState
{
	/** @brief Client-local Run snapshot read by the Public API. */
	FWPTransitRun Run;

	/** @brief Role of this Component's Owner in the current relationship. */
	EWPTransitRole Role = EWPTransitRole::None;

	/** @brief Indicates whether RepState was applied successfully. */
	bool bApplied = false;
};

/**
 * @brief Component that explicitly opts an Actor into Wormhole Portal Transit.
 *
 * When an Actor overlaps a Portal Trigger, the system looks for this Component to
 * determine Transit eligibility.
 * Only Actors to which the Transit Manager or a user explicitly adds this Component can
 * start Transit.
 *
 * A participating Actor must own exactly one Transit Component.
 *
 * The Component does not perform Portal traversal or Physics synchronization directly.
 * It is the authoring contract that describes how the Actor can participate in Transit.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class WORMHOLEPORTALRUNTIME_API UWPTransitComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWPTransitComponent();	
	
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
	
	// **************************** Public API *****************************
	
	// **************************** Delegate API *****************************
	/**
	 * @brief Fired when the Master's Transit Phase changes.
	 * @param PreviousPhase Previous Phase of the Actor.
	 * @param NewPhase Current Phase.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Wormhole Portal|Transit")
	FOnWPTransitPhaseChanged 						OnPhaseChanged;

	/**
	 * @brief Fired when Twin Actor creation and Transit Run preparation are complete.
	 * @param TwinActor Twin Actor opposite the Master Actor to which this Component is attached.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Wormhole Portal|Transit")
	FOnWPTwinEvent									OnTwinCreated;
	
	/**
	 * @brief Fired immediately before FinishSpawning and the Twin's Construction Script execute.
	 *
	 * The callback executes synchronously. Template-based Actor and Instance Component
	 * duplication is complete,
	 * although Native Component registration and PostActorCreated may already have occurred.
	 *
	 * Use this point to set Actor values consumed by the Twin's Construction Script.
	 * Pawn Controller, PlayerState, and Auto Possess policy are owned by the user.
	 * The Twin may be destroyed, but callers must not invoke FinishSpawning directly.
	 * Perform only synchronous work; do not use latent operations such as Delay.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Wormhole Portal|Transit")
	FOnWPTwinEvent									OnTwinPreparing;
	
	/**
	 * @brief Fired immediately before the Twin Actor is removed.
	 * @param TwinActor Twin Actor opposite the Master Actor to which this Component is attached.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Wormhole Portal|Transit")
	FOnWPTwinEvent									OnTwinRemoving;
	
	// **************************** Transit API *****************************

	// Returns the Transit Phase of the Actor that owns this TransitComponent.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	EWPTransitPhase									GetPhase() const;
	
	// Returns the Owner's role in the current Transit.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	EWPTransitRole									GetTransitRole() const;
	
	// Returns the Actor opposite the Owner in the current Transit.
	// Returns the Twin for a Master and the Master for a Twin.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	AActor*											GetCounterPartActor() const;

	// Returns the Master Actor for the current Transit.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	AActor*											GetMasterActor() const;

	// Returns the Twin Actor for the current Transit.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	AActor*											GetTwinActor() const;

	// Returns the Source Portal entered by the Actor.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	AWormholePortalActor*							GetSourcePortal() const;

	// Returns the Destination Portal linked to the Source Portal.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	AWormholePortalActor*							GetDestPortal() const;

	// Returns the concrete Transit handling type after Auto resolution.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	EWPTransitType									GetTransitType() const;

	// Returns the World-local number identifying the current Transit.
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	int64											GetTransitSequence() const;

	/**
	 * @brief Cancels the active Transit and restores the Owner to Idle.
	 * @return true if an authoritative World found and cancelled the active Transit.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Transit")
	bool											CancelTransit();
	
	/**
	 * @brief Returns the Source entry point and normal for the current Transit.
	 * @param OutEntryPoint World-space entry point on the Source Portal surface.
	 * @param OutEntryNormal World-space direction from the Source Portal center toward the entry point.
	 * @return true if the active Transit has valid entry information.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											GetEntryData(FVector& OutEntryPoint, FVector& OutEntryNormal) const;

	/**
	 * @brief Returns the Destination exit point and normal for the current Transit.
	 * @param OutExitPoint World-space exit point on the Destination Portal surface.
	 * @param OutExitNormal World-space direction from the Destination Portal center toward the exit point.
	 * @return true if the active Transit has valid exit information.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											GetExitData(FVector& OutExitPoint, FVector& OutExitNormal) const;

	// **************************** Transform API *****************************
	/**
	 * @brief Maps a World-space position from Source space into Destination space for the current Transit.
	 * @param SourcePoint World-space position in Source space.
	 * @param OutDestPoint Receives the mapped World-space position in Destination space.
	 * @return true if the current Mapping performed the conversion.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											MapPoint(const FVector& SourcePoint, FVector& OutDestPoint) const;

	/**
	 * @brief Maps a World-space direction from Source space into Destination space for the current Transit.
	 * @param SourceDirection World-space direction in Source space.
	 * @param OutDestDirection Receives the mapped World-space direction in Destination space.
	 * @return true if the current Mapping performed the conversion.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											MapDirection(const FVector& SourceDirection, FVector& OutDestDirection) const;

	/**
	 * @brief Maps a World-space rotation from Source space into Destination space for the current Transit.
	 * @param SourceRotation World-space rotation in Source space.
	 * @param OutDestRotation Receives the mapped World-space rotation in Destination space.
	 * @return true if the current Mapping performed the conversion.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											MapRotation(const FRotator& SourceRotation, FRotator& OutDestRotation) const;

	/**
	 * @brief Maps a World Transform from Source space into Destination space for the current Transit.
	 * @param SourceTransform World Transform in Source space.
	 * @param OutDestTransform Receives the mapped World Transform in Destination space.
	 * @return true if the current Mapping performed the conversion.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											MapTransform(const FTransform& SourceTransform, FTransform& OutDestTransform) const;

	/**
	 * @brief Returns the current Transit's Source-to-Destination Mapping to C++ callers.
	 * @param OutMapping Receives the Mapping used by the current Run.
	 * @return true if the active Transit has a valid Mapping.
	 */
	bool											GetMapping(FWPTransform& OutMapping) const;

	/**
	 * @brief Returns whether a point in Source space has crossed the current Transit entry tangent plane.
	 * @param SourceWorldPoint World-space position to test in Source space.
	 * @return true if the Owner is an active Master or Twin and the point lies on the Portal-interior side.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Transit")
	bool											IsPointInsidePortal(const FVector& SourceWorldPoint) const;
	
	// **************************** Refresh API *****************************
	/**
	 * @brief Re-resolves the Owner Actor and refreshes the Transit type, Handler, and Component cache.
	 *
	 * Auto resolves the concrete type from the current Actor configuration. A concrete
	 * requested type does not
	 * fall back to another type. Refresh is permitted only in Idle so the fixed execution
	 * configuration of an
	 * active Transit cannot change.
	 * @return true if refresh was performed in Idle; false if rejected because the Component was in another Phase.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Transit")
	bool											RefreshFromOwner();
	
	// Getter
	EWPTransitType									GetUnresolvedTransitType()	const	{ return TransitType;}
	bool											GetTransitEnabled()			const	{ return bTransitEnabled; }
	const TArray<TObjectPtr<UPrimitiveComponent>>&	GetTransitPrimitives()		const	{ return TransitPrimitives;}
	UMovementComponent*								GetMovementComponent()		const	{ return MovementComponent.Get();}
	bool											IsVoxelCollisionEnabled()	const 	{ return bUseVoxelCollision;}
	bool											IsMaterialClipEnabled()		const	{ return bUseMaterialClip;}
	
	// Setter
	void 											SetTransitEnabled(bool InTransitEnabled) { bTransitEnabled = InTransitEnabled; }
	void 											SetTransitType(EWPTransitType InType) { TransitType = InType; }
	const UWPVoxelData* 							FindVoxelData(const UPrimitiveComponent* Primitive) const;

private:
	friend class UWPTransitSubsystem;
	friend class FWPCharacterHandler;
	friend class FWPVoxelBaker;
	friend class FWPTransitComponentDetails;
	
	/**
	 * @brief Fires the change event after applying the replicated Phase on a client.
	 * @param PreviousPhase Client Transit Phase before replication was applied.
	 */
	UFUNCTION()
	void										OnRep_Phase(EWPTransitPhase PreviousPhase) const;

	/**
	 * @brief Applies the received Transit relationship snapshot to the local state of the
	 *        two existing Components.
	 *
	 * If a referenced NetGUID is unresolved, the function returns without creating state.
	 * It safely reapplies the same snapshot when Unreal Engine invokes RepNotify again
	 * after the reference resolves.
	 */
	UFUNCTION()
	void										OnRep_TransitRepState();
	
	// Applies the server-authoritative Character location, velocity, and view state to the owning client.
	UFUNCTION(Client, Reliable)
	void										ClientSyncChar(FVector DestLocation, FRotator DestRotation, FVector DestVelocity, FRotator DestControl);
	
	// Starts the post-Commit reentry lockout and enables Component Tick only while needed.
	void										StartTransitIgnore();

	/** @brief Recomputes the handling type and Component cache from the current Owner configuration. */
	void										ResolveFromOwner();
	
	// Returns the shared Handler resolved by RefreshFromOwner.
	const IWPTransitHandler*					GetHandler() const { return TransitHandler; }

	// Returns the concrete Transit type after Auto resolution.
	EWPTransitType								GetResolvedType() const { return ResolvedType; }
	
	// Changes the current Phase and fires the change event in the authoritative Transit lifecycle.
	void										SetPhase(EWPTransitPhase NewPhase);
	
	// Finds the currently active Run.
	const FWPTransitRun*						FindActiveRun() const;

	/** @brief Publishes the server Run's active relationship as a replicated snapshot. */
	void										PublishTransitRepState(const FWPTransitRun& Run);

	/** @brief Publishes the terminal state of a successfully started Run as an inactive snapshot with the same Sequence. */
	void										PublishTransitEnd(uint64 Sequence);

	/**
 	 * @brief Resets reflected Transit state copied from the Master by Template Spawn.
 	 *
 	 * Plain C++ runtime members keep their constructor defaults and are not reset here.
 	 */
	void										ResetReplicatedStateForTwin();

	/** @brief Returns whether active RepState values and Object references are safe to apply on a client. */
	static bool									IsTransitRepStateValid(const FWPTransitRepState& State);

	/**
	 * @brief Restores the server-replicated Mapping snapshot as the client's Runtime Mapping.
	 * @param State Relationship and Mapping snapshot published by the server when Transit started.
	 * @param OutMapping Receives the Mapping used by the client Public API and visual processing.
	 * @return true if every Mapping value is valid.
	 */
	static bool									BuildReplicatedMapping(const FWPTransitRepState& State, FWPTransform& OutMapping);

	/**
 	 * @brief Collects the visual Primitives required by the active visual features and pairs
 	 *        them with name- and class-matched counterpart Primitives.
 	 *
 	 * Skeletal Mesh pairs are collected for Leader Pose.
 	 * Static Mesh pairs are collected only when Material Clip is enabled.
 	 * Dedicated Servers do not collect visual pairs.
 	 * 
 	 * @param CounterpartComponent Transit Component owned by the counterpart Actor.
 	 * @param OutPairs Receives matched Master/Twin visual Primitive pairs.
 	 */
	void										GatherVisualPairs(UWPTransitComponent* CounterpartComponent, OUT TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& OutPairs);
	
	/**
 	 * @brief Stores matched Twin visual Primitives in the Run and links Skeletal Mesh poses when rendering is available.
 	 * @param Run Run that receives the Twin visual Primitive references.
 	 * @param VisualPairs Name- and class-matched Master/Twin visual Primitive pairs.
 	 */
	void										ApplyTwinVisualPairs(FWPTransitRun& Run, const TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& VisualPairs) const;
	
	/**
 	 * @brief Applies Material Clip to the collected Master and Twin visual Primitives.
 	 * @param Run Active Run containing the Portal Mapping and Twin visual references.
 	 */
	void										ApplyVisualClip(const FWPTransitRun& Run) const;
	
	/**
 	 * @brief Clears Material Clip CPD from the current Transit Components.
 	 * @param Run Run whose Twin visual Components are cleared.
 	 */
	void										ClearVisualClip(const FWPTransitRun& Run);

	/** @brief Builds the local Run used by the client Public API from the RepState relationship and server Mapping snapshot. */
	void										ApplyClientTransit(UWPTransitComponent* CounterpartComponent, const TArray<TPair<UPrimitiveComponent*, UPrimitiveComponent*>>& VisualPairs);

	/**
	 * @brief Clears Clip, Leader Pose, and local relationship state for the client Master/Twin.
	 * @param Sequence Sequence of the relationship to clear.
	 * @param bMarkClosed When true, blocks reapplication of a late active snapshot with the same Sequence.
	 * @param bBroadcastRemoving When true, fires the Master's OnTwinRemoving once.
	 */
	void										ClearClientTransit(uint64 Sequence, bool bMarkClosed, bool bBroadcastRemoving);

	/**
	 * @brief Returns the Mapping snapshot stored in an active Run.
	 * @param Run Active Transit Run from which to retrieve the Mapping.
	 * @param OutMapping Receives the Mapping snapshot stored in the Run.
	 * @return true if the Run's Mapping, entry point, and reflection plane can be used together.
	 */
	static bool									TryGetRunMapping(const FWPTransitRun& Run, FWPTransform& OutMapping);
	
	// Stateless shared Handler corresponding to ResolvedType.
	const IWPTransitHandler*					TransitHandler = nullptr;

	// Concrete handling type resolved by RefreshFromOwner.
	EWPTransitType								ResolvedType = EWPTransitType::Auto;
	
	// Controls whether this Actor may Transit. When false, an Overlap does not start Transit.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Transit", meta = (AllowPrivateAccess = "true"))
	bool										bTransitEnabled = true;
	
	// Controls whether Transit debug geometry is displayed for the Actor that owns this Component.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Debug", meta = (AllowPrivateAccess = "true"))
	bool										bDrawDebug = false;

	// Transit handling type applied to this Actor. Auto resolves it once from the Actor configuration at runtime.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Transit", meta = (AllowPrivateAccess = "true"))
	EWPTransitType								TransitType = EWPTransitType::Auto;

	/** Duration for which the same Actor is prevented from reentering the Destination Portal after Commit. */
	UPROPERTY(EditAnywhere, Category = "Wormhole Portal|Transit", meta = (ClampMin = "0.0"))
	float										IgnoreTime = 0.15f;
	
	// Controls whether Voxel Collision is used during Transit.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Transit|Advanced", meta = (AllowPrivateAccess = "true"))
	bool bUseVoxelCollision = false;
	
	// Controls whether Material Visual Clipping is used during Transit.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Transit|Advanced", meta = (AllowPrivateAccess = "true"))
	bool bUseMaterialClip = false;
	
	// Requested edge length of one Voxel for the Editor Bake.
	UPROPERTY(EditAnywhere, Category = "Wormhole Portal|Voxel", meta = (ClampMin = "1.0", UIMin = "1.0"))
	float										VoxelSize = 20.f;
	
	// Maximum number of Voxel Box Shapes generated for one Static Mesh.
	UPROPERTY(EditAnywhere, Category = "Wormhole Portal|Voxel", meta = (ClampMin = "1", ClampMax = "512", UIMin = "1", UIMax = "512"))
	int32										MaxVoxelCount = 256;
	
	// Transit-specific Voxel Assets generated by the Editor Module and assigned to this Component.
	UPROPERTY(VisibleAnywhere, Category = "Wormhole Portal|Voxel")
	TArray<TObjectPtr<UWPVoxelData>>			VoxelData;
	
	// Cache of Transit-test Primitives gathered by RefreshFromOwner() from the current Owner configuration.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPrimitiveComponent>>		TransitPrimitives;
	
	/** @brief Locally remembers Master Components whose Material Clip CPD must be cleared when the current Transit ends. */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UPrimitiveComponent>>		ClipParts;

	// MovementComponent used as the server Commit reference for a MovementComponent-based Actor.
	UPROPERTY(Transient)
	TObjectPtr<UMovementComponent>				MovementComponent;
	
	/** @brief Latest failure reason produced by ResolveFromOwner. */
	EWPTransitResolveFailReason					ResolveFailReason = EWPTransitResolveFailReason::None;

	/** @brief Components identified by the latest Resolver failure. */
	TArray<TWeakObjectPtr<UActorComponent>>		FailedComponents;
	
	// Master-center reference used to compute the tangent plane when Transit starts.
	UPROPERTY(EditDefaultsOnly, Category = "Wormhole Portal|Transit")
	EWPTransitCenter							CenterMode = EWPTransitCenter::RootLocation;
	
	// Current Wormhole Transit Phase of the Actor.
	UPROPERTY(ReplicatedUsing = OnRep_Phase, VisibleInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Transit", meta = (AllowPrivateAccess = "true"))
	EWPTransitPhase								Phase = EWPTransitPhase::Idle;

	/**
	 * @brief Latest server-authoritative snapshot of the active Transit relationship.
	 *
	 * This state is sent only when the Master Actor is replicated. Active state is never
	 * published on the Twin Component.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_TransitRepState)
	FWPTransitRepState							TransitRepState;

	/** @brief Current Component-local relationship restored by the client from RepState. */
	FWPTransitClientState						ClientTransitState;

	/** @brief Highest Sequence for which terminal state was applied. Late active state at or below this value is ignored. */
	uint64										LastClosedTransitSequence = 0;
	
	// Remaining post-Commit reentry lockout time.
	UPROPERTY(Transient)
	float										RemainingIgnoreTime = 0.0f;
};
