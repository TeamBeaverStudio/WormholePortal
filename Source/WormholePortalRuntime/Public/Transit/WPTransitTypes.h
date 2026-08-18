// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WPTransform.h"
#include "Engine/NetSerialization.h"
#include "WPTransitTypes.generated.h"

class AActor;
class AWormholePortalActor;
class UActorComponent;

// Reference point used as the Master Actor center when computing a tangent plane.
UENUM()
enum class EWPTransitCenter : uint8
{
	RootLocation UMETA(DisplayName = "Root Component Location"),// Uses the Root Component's World Location.
	ActorOBBCenter UMETA(DisplayName = "Actor OBB Center")		// Uses the Actor-space OBB center computed from all Transit target Primitives.
};

/**
 * @brief Transit handling type applied to an Actor.
 *
 * Auto inspects the Actor configuration once at runtime to select the concrete handling type.
 */
UENUM(BlueprintType)
enum class EWPTransitType : uint8
{
	Auto		UMETA(DisplayName = "Auto"),
	Physics		UMETA(DisplayName = "Physics"),
	Character	UMETA(DisplayName = "Character"),
	Projectile	UMETA(DisplayName = "Projectile"),
	Pawn		UMETA(DisplayName = "Pawn"),
	
	MAX			UMETA(Hidden, DisplayName = "MAX")
};

/**
 * @brief Role of an Actor in the current Transit.
 */
UENUM(BlueprintType)
enum class EWPTransitRole : uint8
{
	None			UMETA(DisplayName = "None"),
	Master			UMETA(DisplayName = "Master"),
	Twin			UMETA(DisplayName = "Twin"),
	
	MAX				UMETA(Hidden, DisplayName = "MAX")
};

/**
 * @brief Current Transit phase of an Actor.
 * Phase stores only in-progress state; the terminal Transit result is tracked separately.
 */
UENUM(BlueprintType)
enum class EWPTransitPhase : uint8
{
	Idle			UMETA(DisplayName = "Idle"), 		// The Actor is not interacting with a Wormhole.
	Check			UMETA(DisplayName = "Check"),		// The Actor is being checked against Wormhole traversal requirements.
	Crossing		UMETA(DisplayName = "Crossing"),	// The checks passed, and the Master and Twin coexist in their respective spaces.
	Cooldown = 4	UMETA(DisplayName = "Cooldown"),	// Prevents the same Actor from immediately reentering a Portal after Commit.
	
	MAX				UMETA(Hidden, DisplayName = "MAX")
};

// Delivers the previous and new states when the Transit Phase changes.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWPTransitPhaseChanged, EWPTransitPhase, PreviousPhase, EWPTransitPhase, NewPhase);

// Delivers the valid Twin Actor when it is created or removed.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnWPTwinEvent, AActor*, TwinActor);

// Result of the most recent Transit attempt.
// Uses None when no result is available yet.
UENUM(BlueprintType)
enum class EWPTransitResult : uint8
{
	None			UMETA(DisplayName = "None"),			// No Transit result is available yet.
	Rejected		UMETA(DisplayName = "Reject"),			// The Check failed before Transit could start.
	Committed		UMETA(DisplayName = "Committed"),		// Transit completed by moving the Master to Destination space.
	Cancelled		UMETA(DisplayName = "Cancelled"),		// Transit failed after Crossing began, leaving the Master in Source space.
	            	
	MAX				UMETA(Hidden, DisplayName = "MAX")
};

// Reason for a Transit failure. Cancel and Reject may share the same failure reason.
// Terminal lifecycle events deliver the concrete reason through FWPTransitEvent::FailReason.
UENUM(BlueprintType)
enum class EWPTransitFailReason : uint8
{
	None					UMETA(DisplayName = "None"),				// No failure occurred. Used with a Committed result.
	NotReady				UMETA(DisplayName = "NotReady"),			// Transit is not ready to start or continue.
	PortalUnavailable		UMETA(DisplayName = "PortalUnavailable"),	// The Source Portal or Destination Portal is unavailable.
	AlreadyInTransit		UMETA(DisplayName = "AlreadyInTransit"),	// The Actor is already participating in another Transit.
	CooldownActive			UMETA(DisplayName = "CooldownActive"),		// The reentry-prevention Cooldown is still active.
	DoesNotFitGate			UMETA(DisplayName = "DoesNotFitGate"),		// The Actor's projected traversal cross-section does not fit within the Source Portal gate.
	TwinCreationFailed = 7	UMETA(DisplayName = "TwinCreationFailed"),	// The Twin Actor could not be created successfully.
	PortalDestroyed			UMETA(DisplayName = "PortalDestroyed"),		// The Source Portal or Destination Portal was destroyed during Transit.
	InternalError			UMETA(DisplayName = "InternalError"),		// Internal error that does not expose implementation details externally.
	UnsupportedActor		UMETA(DisplayName = "UnsupportedActor"),	// The Actor does not match a supported Transit type.
	InvalidSetup			UMETA(DisplayName = "InvalidSetup"),		// The selected Transit type is configured incorrectly.
	TransitDisabled			UMETA(DisplayName = "TransitDisabled"),		// Transit is disabled on the Transit Component.
	MissingPrimitives		UMETA(DisplayName = "MissingPrimitives"),	// No Primitive is available for Transit.
	MissingPhysicsMesh		UMETA(DisplayName = "MissingPhysicsMesh"),	// No Simulated Physics Static Mesh is available.
	MissingVoxelData		UMETA(DisplayName = "MissingVoxelData"),	// Required Voxel Data is missing.
	InvalidVelocity			UMETA(DisplayName = "InvalidVelocity"),		// The selected Handler returned no usable movement velocity.
	InvalidCenter			UMETA(DisplayName = "InvalidCenter"),		// The Master center or derived entry direction is invalid.
	InvalidPlane			UMETA(DisplayName = "InvalidPlane"),		// A position-reflection plane could not be selected.
	BeginFailed				UMETA(DisplayName = "BeginFailed"),			// The selected Handler could not prepare Transit.
	VoxelBeginFailed		UMETA(DisplayName = "VoxelBeginFailed"),	// Voxel collision bodies could not be prepared.
	UpdateFailed			UMETA(DisplayName = "UpdateFailed"),		// The selected Handler could not update the Crossing state.
	RuntimeStateLost		UMETA(DisplayName = "RuntimeStateLost"),	// A required Runtime Actor, Component, or Handler became unavailable.
	MappingInvalid			UMETA(DisplayName = "MappingInvalid"),		// The stored Portal Mapping became invalid during Transit.
	InvalidRunState			UMETA(DisplayName = "InvalidRunState"),		// Stored Crossing directions or surfaces became invalid.
	ReturnedToSource		UMETA(DisplayName = "ReturnedToSource"),	// The Master moved back outside the Source entry surface.
	CommitFailed			UMETA(DisplayName = "CommitFailed"),		// The selected Handler could not commit Transit.
	CancelRequested			UMETA(DisplayName = "CancelRequested"),		// Cancellation was requested through the Transit Component API.
	SubsystemClosed			UMETA(DisplayName = "SubsystemClosed"),		// The Transit Subsystem shut down before completion.
	                    	
	MAX						UMETA(Hidden, DisplayName = "MAX")
};

/**
 * @brief Describes why the Resolver could not prepare an Actor for Transit.
 *
 * This reason is limited to Actor configuration checks shared by Runtime and Editor.
 * Transit lifecycle failures continue to use EWPTransitFailReason.
 */
UENUM()
enum class EWPTransitResolveFailReason : uint8
{
	None				UMETA(DisplayName = "None"),				// No failure occurred. Used with a Passed result.
	UnsupportedActor	UMETA(DisplayName = "UnsupportedActor"),	// The Actor does not match a supported Transit type.
	TransitDisabled		UMETA(DisplayName = "TransitDisabled"),		// Transit is disabled on the Transit Component.
	MissingPrimitives	UMETA(DisplayName = "MissingPrimitives"),	// No Primitive is available for Transit.
	NoPhysicsBody		UMETA(DisplayName = "NoPhysicsBody"),		// No supported Primitive is available as a simulated Physics Body.
	MissingVoxelData	UMETA(DisplayName = "MissingVoxelData"),	// Required Voxel Data is missing.
	TypeMismatch		UMETA(DisplayName = "TypeMismatch"),		// The Actor class does not match the selected Transit type.
	MissingMovement		UMETA(DisplayName = "MissingMovement"),		// The required Movement Component is missing.
	InvalidMoveOwner	UMETA(DisplayName = "InvalidMoveOwner"),	// The Movement Component does not belong to the expected Actor.
	InvalidUpdatedPart	UMETA(DisplayName = "InvalidUpdatedPart"),	// The Movement Component does not update the required Primitive.
	InvalidRootPart		UMETA(DisplayName = "InvalidRootPart"),		// The required Primitive is not the Actor Root Component.
	InvalidPartOwner	UMETA(DisplayName = "InvalidPartOwner"),	// The Primitive belongs to another Actor.
	PartNotMovable		UMETA(DisplayName = "PartNotMovable"),		// The Primitive Mobility is not Movable.
	PartNoCollision		UMETA(DisplayName = "PartNoCollision"),		// Collision is disabled on the Primitive.
	UnsupportedPart		UMETA(DisplayName = "UnsupportedPart"),		// The Primitive type is not supported for Transit.
	ExcludedPart		UMETA(DisplayName = "ExcludedPart"),		// The Primitive is explicitly excluded from Transit.
	SimPhysics			UMETA(DisplayName = "SimPhysics"),			// A required movement-based Component is simulating Physics.
	InvalidMoveType		UMETA(DisplayName = "InvalidMoveType"),		// The Movement Component type is unsupported by the selected Transit type.
	NoOverlapPart		UMETA(DisplayName = "NoOverlapPart"),		// No Transit Primitive can generate BeginOverlap with the Portal Trigger.
	
	MAX					UMETA(Hidden, DisplayName = "MAX")
};

// Reference plane used to reflect a position in Source Portal Local space.
UENUM()
enum class EWPTransitPlane : uint8
{          	
	YZ = 0 	UMETA(DisplayName = "YZ Plane"), // Reflects the Source Portal Local X component.
	XZ = 1 	UMETA(DisplayName = "XZ Plane"), // Reflects the Source Portal Local Y component.
	XY = 2 	UMETA(DisplayName = "XY Plane"), // Reflects the Source Portal Local Z component.
	       	
	MAX = 3 UMETA(Hidden)
};

// Returns whether the value is a valid Transit position-reflection plane.
constexpr bool IsValidTransitPlane(const EWPTransitPlane Plane)
{
	return Plane >= EWPTransitPlane::YZ && Plane < EWPTransitPlane::MAX;
}

// Final plane priority used when both tangent-plane and movement-direction angles remain tied.
UENUM()
enum class EWPPlanePriority : uint8
{ 
	YZ_XZ_XY = 0 	UMETA(DisplayName = "YZ > XZ > XY"),
	YZ_XY_XZ = 1 	UMETA(DisplayName = "YZ > XY > XZ"),
	XZ_YZ_XY = 2 	UMETA(DisplayName = "XZ > YZ > XY"),
	XZ_XY_YZ = 3 	UMETA(DisplayName = "XZ > XY > YZ"),
	XY_YZ_XZ = 4 	UMETA(DisplayName = "XY > YZ > XZ"),
	XY_XZ_YZ = 5 	UMETA(DisplayName = "XY > XZ > YZ"),
	
	// Number of priority values available to users.
	MAX = 6			UMETA(Hidden)
};

/**
 * @brief Replicated snapshot that delivers an active Transit relationship to clients,
 *        including late-joining clients.
 *
 * Does not transmit the server Run object or the visual-component arrays.
 * To prevent a client from rebuilding a Mapping from stale Portal state immediately after a Link, Transform,
 * or Metric change, the server publishes the exact Mapping snapshot used to start
 * Transit together with the relationship data.
 */
USTRUCT()
struct WORMHOLEPORTALRUNTIME_API FWPTransitRepState
{
	GENERATED_BODY()

	/** @brief Twin Actor that exists in the space opposite the current Master. */
	UPROPERTY()
	TObjectPtr<AActor> CounterpartActor;

	/** @brief Source Portal entered by the Actor. */
	UPROPERTY()
	TObjectPtr<AWormholePortalActor> SourcePortal;

	/** @brief Destination Portal where the Twin was created. */
	UPROPERTY()
	TObjectPtr<AWormholePortalActor> DestinationPortal;

	/** @brief World-space entry point resolved on the Source Portal surface. */
	UPROPERTY()
	FVector_NetQuantize100 EntryPoint = FVector::ZeroVector;

	/** @brief World-space center of the Source Portal captured when Transit started. */
	UPROPERTY()
	FVector_NetQuantize100 SourceCenter = FVector::ZeroVector;

	/** @brief World-space center of the Destination Portal captured when Transit started. */
	UPROPERTY()
	FVector_NetQuantize100 DestinationCenter = FVector::ZeroVector;

	/** @brief Source Portal rotation that transforms the entry-point reflection plane from Local space into World space. */
	UPROPERTY()
	FQuat SourceRotation = FQuat::Identity;

	/** @brief Rotation that maps Source-space directions and rotations into Destination space. */
	UPROPERTY()
	FQuat TransportRotation = FQuat::Identity;

	/** @brief Effective Source Portal traversal radius captured when Transit started. */
	UPROPERTY()
	float SourceCoreRadius = 0.0f;

	/** @brief Effective Destination Portal traversal radius captured when Transit started. */
	UPROPERTY()
	float DestinationCoreRadius = 0.0f;

	/** @brief Monotonically increasing number that identifies Transit start order within the same World. */
	UPROPERTY()
	uint64 Sequence = 0;

	/** @brief Position-reflection plane that determines the correspondence between the entry point and Destination exit point. */
	UPROPERTY()
	EWPTransitPlane SelectedPlane = EWPTransitPlane::YZ;

	/** @brief Concrete Actor handling type resolved by the server after Auto detection. */
	UPROPERTY()
	EWPTransitType TransitType = EWPTransitType::Auto;

	/** @brief true for an active relationship; false for the terminal state of the same Sequence. */
	UPROPERTY()
	bool bActive = false;
};

/**
 * @brief Game Thread-only lifecycle value snapshot delivered by the Transit Subsystem to external systems.
 *
 * Copies only the required values so Delegate receivers do not retain long-lived
 * references to internal Run state.
 */
struct WORMHOLEPORTALRUNTIME_API FWPTransitEvent
{
	/** @brief Original Actor that performed the Transit. */
	TWeakObjectPtr<AActor> Actor;

	/** @brief Twin Actor created in Destination space. */
	TWeakObjectPtr<AActor> TwinActor;

	/** @brief Source Portal entered by the Actor. */
	TWeakObjectPtr<AWormholePortalActor> SourcePortal;

	/** @brief Destination Portal from which the Actor exits. */
	TWeakObjectPtr<AWormholePortalActor> DestinationPortal;

	/** World-space position of the Source seam entry point captured when Transit started. */
	FVector EntryPointWorld = FVector::ZeroVector;

	/** Normalized World-space movement direction captured when Transit started. */
	FVector MoveDirectionWorld = FVector::ZeroVector;
	
	// Position-reflection plane selected when Transit starts and held fixed until it ends.
	EWPTransitPlane SelectedPlane = EWPTransitPlane::YZ;

	/** First-person camera-path Mapping snapshot built from the Source and Destination Portal values. */
	FWPTransform Mapping;

	// Indicates whether the entry point, selected plane, and Mapping can be used for first-person camera-path calculations.
	bool bPathMappingValid = false;

	/** @brief Terminal result represented by this event. Uses None for a Started event. */
	EWPTransitResult Result = EWPTransitResult::None;

	/** @brief Failure reason for a Rejected or Cancelled result. */
	EWPTransitFailReason FailReason = EWPTransitFailReason::None;
	
	/** @brief Resolver reason for a configuration-based Rejected result. */
	EWPTransitResolveFailReason ResolveFailReason = EWPTransitResolveFailReason::None;

	/** @brief Components that directly caused the Resolver rejection. */
	TArray<TWeakObjectPtr<UActorComponent>> FailedComponents;

	/** @brief Actor handling type resolved when Transit started. */
	EWPTransitType TransitType = EWPTransitType::Auto;

	/** @brief Number that identifies a successfully started Transit within the same World. */
	uint64 Sequence = 0;

	/** @brief Monotonic time, in seconds, at which the event was created. */
	double TimestampSeconds = 0.0;

	/** @brief Time elapsed, in milliseconds, from Started to this event. */
	double TransitElapsedMs = 0.0;
};
