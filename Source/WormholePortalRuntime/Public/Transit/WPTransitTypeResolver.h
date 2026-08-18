// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/WPTransitTypes.h"
#include "UObject/ObjectPtr.h"

class AActor;
class UActorComponent;
class UMovementComponent;
class UPrimitiveComponent;
class USceneComponent;
class UWPTransitComponent;

/** @brief Transit readiness state shared by Runtime and Editor code. */
enum class EWPTransitResolveStatus : uint8
{
	Passed,			// Transit can start with the current configuration.
	NotSupported,	// The Actor is unsupported or explicitly excluded.
	NeedsSetup		// The type is supported, but required Component or Asset configuration is missing.
};

/**
 * @brief Actor Transit resolution result shared by Runtime and Editor code.
 *
 * Resolves the Handler type and its required Components in one pass so Editor
 * validation
 * and actual Runtime startup use the same conditions.
 */
struct WORMHOLEPORTALRUNTIME_API FWPTransitResolveResult
{
	/** @brief Resolved Transit type. On failure, the requested type or Auto may remain unchanged. */
	EWPTransitType TransitType = EWPTransitType::Auto;

	/** @brief Indicates whether the Actor is ready, unsupported, or missing required setup. */
	EWPTransitResolveStatus Status = EWPTransitResolveStatus::NotSupported;

	/** @brief Concrete reason reported to users and Runtime code when Status is not Passed. */
	EWPTransitResolveFailReason FailReason = EWPTransitResolveFailReason::UnsupportedActor;

	/** @brief Components that directly caused the Resolve failure. */
	TArray<TWeakObjectPtr<UActorComponent>> FailedComponents;

	/** @brief Primitives used for overlap validation, gate-fit checks, and traversal tests. */
	TArray<TObjectPtr<UPrimitiveComponent>> TransitPrimitives;

	/** @brief Movement Component used by the Character, Projectile, or Pawn Handler. */
	TObjectPtr<UMovementComponent> MovementComponent = nullptr;

	/** @brief Returns true if the Transit Component can be prepared from this result. */
	bool IsPassed() const { return Status == EWPTransitResolveStatus::Passed; }
};

/**
 * @brief Resolves an Actor's Transit type and required Components using one shared rule set.
 *
 * Auto resolution uses the priority Character, Projectile, Pawn, then Physics.
 * When a concrete type is requested, only that type is checked; resolution does not
 * fall back to another Handler.
 */
class WORMHOLEPORTALRUNTIME_API FWPTransitTypeResolver
{
public:
	/**
	 * @brief Validates the requested Transit type and gathers its required Components.
	 * @param Actor Actor to inspect.
	 * @param TransitComponent Transit Component whose existing configuration and Voxel Data are inspected.
	 * @param RequestedType Auto to resolve from the Actor configuration; otherwise, the only type to validate.
	 * @return Resolution status and gathered Components.
	 */
	static FWPTransitResolveResult	Resolve(const AActor* Actor, const UWPTransitComponent* TransitComponent, EWPTransitType RequestedType);

private:
	/** @brief Selects the first supported type matching the Actor structure according to Auto priority. */
	static EWPTransitType			DetectType(const AActor* Actor);

	/** @brief Validates the Character's CMC, Root Capsule, Ragdoll state, and Voxel Data. */
	static FWPTransitResolveResult	ResolveCharacter(const AActor* Actor, const UWPTransitComponent* TransitComponent);

	/** @brief Validates Projectile Movement, the Updated Primitive, and Voxel Data. */
	static FWPTransitResolveResult	ResolveProjectile(const AActor* Actor, const UWPTransitComponent* TransitComponent);

	/** @brief Validates Pawn Movement, the Root Primitive, and Voxel Data for a general Pawn. */
	static FWPTransitResolveResult	ResolvePawn(const AActor* Actor, const UWPTransitComponent* TransitComponent);

	/** @brief Validates supported single-body Physics Primitives and Voxel Data. */
	static FWPTransitResolveResult	ResolvePhysics(const AActor* Actor, const UWPTransitComponent* TransitComponent);

	/**
	 * @brief Creates a resolution result initialized with common defaults.
	 * @param TransitType Resolved or requested Transit type.
	 * @param Status Resolve status to store.
	 * @param FailReason Failure reason to store.
	 * @param FailedComponent Component that directly caused the failure, if known.
	 * @return Initialized Resolve result.
	 */
	static FWPTransitResolveResult	MakeResult(EWPTransitType TransitType, EWPTransitResolveStatus Status, 
		EWPTransitResolveFailReason FailReason, UActorComponent* FailedComponent = nullptr);
	
	/**
	 * @brief Returns why a Primitive cannot be selected for Transit.
	 * @param Actor Actor expected to own the Primitive.
	 * @param Comp Primitive to inspect.
	 * @return None when the Primitive is usable; otherwise, the first failed condition.
	 */
	static EWPTransitResolveFailReason GetPartFail(const AActor* Actor, const UPrimitiveComponent* Comp);
	

	/** @brief Returns whether a Primitive satisfies common ownership, Mobility, Collision, and exclusion requirements. */
	static bool						CanUsePart(const AActor* Actor, const UPrimitiveComponent* Component);

	/**
	 * @brief Gathers collision Static Meshes shared by Character, Projectile, and Pawn handling.
	 * @param Actor Actor that owns the Static Meshes.
	 * @param InOutParts Existing collection containing the movement Root; receives usable Static Meshes without duplicates.
	 */
	static void GatherTransitStaticMeshes(const AActor* Actor, TArray<TObjectPtr<UPrimitiveComponent>>& InOutParts);

	/**
 	 * @brief Returns whether every supported Voxel Primitive has matching Voxel Data.
 	 * @param TransitComponent Transit Component whose Voxel Data is inspected.
 	 * @param Parts Resolver-selected Primitives to inspect.
 	 * @param OutFailedComponents Receives every supported Primitive missing matching Voxel Data.
 	 * @return true when Voxel validation is unnecessary or every supported Primitive has matching Voxel Data.
 	 */
	static bool HasVoxelData(const UWPTransitComponent* TransitComponent, const TArray<TObjectPtr<UPrimitiveComponent>>& Parts,
		TArray<TWeakObjectPtr<UActorComponent>>& OutFailedComponents);
	
	/**
	 * @brief Returns whether a Primitive can generate a BeginOverlap event with the Portal Trigger.
	 * @param Component Primitive Component to inspect.
	 * @return true when Query Collision and Generate Overlap Events are enabled and the Collision
	 * Responses on both sides permit the overlap.
	 */
	static bool CanPortalOverlap(const UPrimitiveComponent* Component);
	
	/**
	 * @brief Returns whether a Primitive that passed the common checks is a supported simulated
	 * Physics Body.
	 * @param Comp Primitive Component to inspect.
	 * @return true when the Primitive is a supported type with Physics Collision and Simulate
	 * Physics enabled.
	 */
	static bool IsPhysicsBody(const UPrimitiveComponent* Comp);
};
