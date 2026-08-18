// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chaos/Declares.h"
#include "Engine/EngineTypes.h"
#include "Transit/WPTransitTypes.h"
#include "WPTransitRun.generated.h"

class AActor;
class UWPVoxelData;
class UBodySetup;
class UPrimitiveComponent;
class UWPTransitComponent;
class AWormholePortalActor;
class FWormholeTransitJointCallback;

/**
 * @brief Runtime state for a pair of Voxel Bodies used by one Transit.
 */
USTRUCT()
struct FWPVoxelPair
{
	GENERATED_BODY()
	
	// Source-space Primitive using Voxel Collision during Transit.
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> Master;
	
	// Destination-space Primitive using the same Voxel Collision.
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> Twin;
	
	// Master BodySetup restored when Transit ends.
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> OriginMasterBodySetup;
	
	// Twin BodySetup restored when Transit ends.
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> OriginTwinBodySetup;
	
	// Editor Bake result used by the Master and Twin.
	UPROPERTY(Transient)
	TObjectPtr<UWPVoxelData> VoxelData;
	
	// Current Query and Physics enabled state for each Master Voxel Shape.
	TArray<uint8> MasterState;

	// Current Query and Physics enabled state for each Twin Voxel Shape.
	TArray<uint8> TwinState;
	
	// Indicates whether the Master and Twin Physics Bodies use the Voxel BodySetup.
	bool bUsingVoxelBody = false;
};

/**
 * @brief Stores the original state and Master/Twin references for one Physics Transit Component pair.
 */
USTRUCT()
struct FWPTransitComponentPair
{
	GENERATED_BODY()
	
	// Master Physics Primitive in Source space.
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> Master;
	
	// Twin Physics Primitive in Destination space.
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> Twin;
	
	// Master mass before Transit.
	float OldMass = 0.f;
	
	// Master mass scale before Transit.
	float OldMassScale = 1.0f;
	
	// Master Mass Override value before Transit.
	float OldMassOverride = 0.0f;
	
	// Indicates whether the Master used Mass Override before Transit.
	bool bHadMassOverride = false;
	
	// Indicates whether the Master used gravity before Transit.
	bool bHadGravity = true;
	
	// Indicates whether the Master simulated physics before Transit.
	bool bWasSim = false;
	
	// Master Collision setting before Transit.
	ECollisionEnabled::Type OldCollision = ECollisionEnabled::NoCollision;
	
	// Indicates whether the Master mass was changed during Transit.
	bool bMassChanged = false;
};

/**
 * @brief State maintained by the Simulated Physics Handler for the lifetime of a Run.
 *
 * Handler objects are shared, so Actor-specific Pairs, Solver, and Callback are stored in this struct.
 */
USTRUCT()
struct FWPPhysicsState
{
	GENERATED_BODY()

	/** @brief Master/Twin Component pairs used for state restoration and Commit. */
	UPROPERTY(Transient)
	TArray<FWPTransitComponentPair> Pairs;
	
	/** @brief Chaos Solver on which the Callback is registered. */
	Chaos::FPhysicsSolver* Solver = nullptr;
	
	/** @brief Chaos Callback that links the Master and Twin Bodies during Crossing. */
	FWormholeTransitJointCallback* Callback = nullptr;
	
	/** Logging only. Prevents repeated output of the same unsupported-Chaos warning. */
	bool bChaosWarned = false;
};

/**
 * @brief Complete Runtime state for one in-progress Actor Transit.
 *
 * The Subsystem and selected Handler share this struct until Commit or Cancel.
 */
USTRUCT()
struct FWPTransitRun
{
	GENERATED_BODY()

	/** World-local sequence identifying a successfully started Transit. Zero indicates partially initialized state. */
	uint64 Sequence = 0;

	/** Monotonic time, in seconds, at which Started was committed. Used to compute elapsed time for terminal events. */
	double StartTimestampSeconds = 0.0;

	/** @brief Actor handling type resolved when Transit starts and held fixed until it ends. */
	EWPTransitType TransitType = EWPTransitType::Auto;

	/** @brief Source-to-Destination Mapping snapshot built from the Portal pair when Transit started. */
	FWPTransform Mapping;

	/** @brief Indicates whether the Mapping, entry point, and reflection plane can be used together by this Run. */
	bool bMappingValid = false;

	/**
	 * @brief Indicates whether this Run's active relationship was published through the Master Component RepState.
	 *
	 * Prevents publication of an unnecessary inactive RepState after partial initialization failure, and ensures that
	 * a successfully started Run publishes the inactive state for the same Sequence exactly once before shutdown.
	 */
	bool bNetworkStatePublished = false;

	/** @brief Original Actor that started Transit in Source space. */
	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> MasterActor;
	
	/** @brief Temporary Twin Actor of the same class created in Destination space. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> TwinActor;

	/** @brief Twin visual Primitives used only for Material Clip and Skeletal pose handling. */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> TwinVisualParts;
	
	/** @brief Source Portal entered by the Actor. */
	UPROPERTY(Transient)
	TWeakObjectPtr<AWormholePortalActor> Source;
	
	/** @brief Destination Portal where the Twin is created and Commit occurs. */
	UPROPERTY(Transient)
	TWeakObjectPtr<AWormholePortalActor> Dest;
	
	/** @brief Position-reflection plane selected at Transit start and held fixed until Commit or Cancel. */
	EWPTransitPlane SelectedPlane = EWPTransitPlane::YZ;
	
	/** @brief Entry point where the Actor-center trajectory first intersects the Source surface. */
	FVector EntryPoint = FVector::ZeroVector;

	/** @brief Velocity direction at Transit start. */
	FVector MoveDir = FVector::ZeroVector;
	
	/** @brief Transit Component that owns the Run's Master Actor and Phase. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UWPTransitComponent> TransitComponent;
	
	/** @brief Master/Twin Body and Chaos Callback state used only by the Physics Handler. */
	UPROPERTY(Transient)
	FWPPhysicsState PhysicsState;
	
	/** @brief Master/Twin Pairs used for Voxel Body replacement and plane tests regardless of Actor type. */
	UPROPERTY(Transient)
	TArray<FWPVoxelPair> VoxelPairs;
};
