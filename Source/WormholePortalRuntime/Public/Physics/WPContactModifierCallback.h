// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chaos/ParticleHandleFwd.h"
#include "Chaos/SimCallbackObject.h"

namespace Chaos
{
	class FCollisionContactModifier;
	class FSingleParticlePhysicsProxy;
}

struct FWPTransform;
enum class EWPTransitPlane : uint8;

/**
 * @brief Pair of Chaos Particles for the Master and Twin.
 *
 * One Actor Transit may contain multiple StaticMeshComponent Pairs.
 * The Game Thread captures only the proxies; OnPreSimulate_Internal resolves
 * each proxy's current Particle on the Physics Thread.
 */
struct FWPTransitParticlePair
{
	// Physics Proxy for the Master Actor.
	Chaos::FSingleParticlePhysicsProxy* MasterProxy = nullptr;
	
	// Physics Proxy for the Twin Actor.
	Chaos::FSingleParticlePhysicsProxy* TwinProxy = nullptr;
	
	/** Physics Particle for the Master Actor. */
	Chaos::FGeometryParticleHandle* MasterParticle = nullptr;
	
	/** Physics Particle for the Twin Actor. */
	Chaos::FGeometryParticleHandle* TwinParticle = nullptr;
	
	/** Indicates whether the Master has a valid external Contact in the current Physics Step. */
	bool bMasterContact = false;
	
	/** Indicates whether the Twin has a valid external Contact in the current Physics Step. */
	bool bTwinContact = false;
};

/**
 * 
 */
class WORMHOLEPORTALRUNTIME_API FWormholeTransitJointCallback final 
	: public Chaos::TSimCallbackObject<
	Chaos::FSimCallbackNoInput, 
	Chaos::FSimCallbackNoOutput,
	Chaos::ESimCallbackOptions::Presimulate |
	Chaos::ESimCallbackOptions::PreSolve | 
	Chaos::ESimCallbackOptions::ContactModification |
	Chaos::ESimCallbackOptions::ParticleUnregister>
{
public:
	/**
	 * @brief Creates a Chaos Callback that constrains the Master and Twin to the same
	 *        Transit position.
	 * @param InParticlePairs Master and Twin Physics Pairs to synchronize.
	 * @param InMapping Coordinate transform between Source and Destination.
	 * @param InEntryPoint Entry point established on the Source surface.
	 * @param InSelectedPlane Plane used to reflect positions in Source Portal local space.
	 */
	FWormholeTransitJointCallback(
		const TArray<FWPTransitParticlePair>& InParticlePairs,
		const FWPTransform& InMapping,
		const FVector& InEntryPoint,
		EWPTransitPlane InSelectedPlane);
	
private:
	virtual void OnPreSimulate_Internal() override;
	virtual void OnContactModification_Internal(Chaos::FCollisionContactModifier& Modifier) override;
	virtual void OnPreSolve_Internal() override;
	virtual void OnParticleUnregistered_Internal(TArray<TTuple<Chaos::FUniqueIdx, Chaos::FSingleParticlePhysicsProxy*>>& UnregisteredProxies) override;
	
	// =============================== Chaos <-> Unreal Helper ===============================
	
	/** Converts a Chaos vector to an Unreal FVector. */
	static FVector				ToFVector(const Chaos::FVec3& Vector);

	/** Converts an Unreal FVector to a Chaos vector. */
	static Chaos::FVec3			ToChaosVector(const FVector& Vector);

	/** Converts a Chaos rotation to an Unreal FQuat. */
	static FQuat				ToFQuat(const Chaos::FRotation3& Rotation);
	
	/** Maps a Source-surface-relative position to its Destination-surface-relative position. */
	FVector						MapPosition(const FVector& SourcePosition) const;

	/** Maps a Source direction vector to a Destination direction vector. */
	FVector						MapDirection(const FVector& SourceDirection) const;

	/** Maps a Destination direction vector back to a Source direction vector. */
	FVector						UnmapDirection(const FVector& DestinationDirection) const;

	/** Corrects position and linear-velocity differences between the Master and Twin. */
	void						FixLinear(const FWPTransitParticlePair& Pair, float DeltaSeconds);

	/** Corrects rotation and angular-velocity differences between the Master and Twin. */
	void						FixAngular(const FWPTransitParticlePair& Pair, float DeltaSeconds);
	
	// Determines the Master and Twin correction weights from their external Contact state.
	static void					GetMoveWeight(const FWPTransitParticlePair& Pair, OUT float& OutMasterWeight, OUT float& OutTwinWeight);

	/** Tests whether the specified Particle belongs to the Master Particle list. */
	bool						IsMasterParticle(const Chaos::FGeometryParticleHandle* Particle) const;

	/** Tests whether the specified Particle belongs to the Twin Particle list. */
	bool						IsTwinParticle(const Chaos::FGeometryParticleHandle* Particle) const;
	
	// =======================================================================================
	
	// Synchronizes the Sleep state of the Master and Twin.
	bool SyncSleep(const FWPTransitParticlePair& Pair);
	
	/** Master and Twin Particle Pairs managed by the Solver Callback. */
	TArray<FWPTransitParticlePair> ParticlePairs;
	
	/** Portal centers used to limit Contact removal to each seam sphere. */
	FVector SourceCenter = FVector::ZeroVector;
	FVector DestCenter = FVector::ZeroVector;
	
	// Surface positions that divide Contact ownership regions in Source and Destination space.
	FVector SourceSurface = FVector::ZeroVector;
	FVector DestSurface = FVector::ZeroVector;
	
	// Normals that point outward from each Portal center toward the side where the object should exist.
	FVector SourceNormal = FVector::ZeroVector;
	FVector DestNormal = FVector::ZeroVector;
	
	/** Radii of the Source and Destination seam spheres. */
	float SourceRadius = 0.0f;
	float DestRadius = 0.0f;

	/** Rotation that transports directions from the Source coordinate system to the Destination coordinate system. */
	FQuat TransportRotation = FQuat::Identity;

	/** Cumulative count of contact points disabled during ContactModification. */
	TAtomic<uint32> DisabledContactCount{ 0 };
	
};
