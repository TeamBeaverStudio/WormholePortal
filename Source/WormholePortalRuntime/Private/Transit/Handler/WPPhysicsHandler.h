// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/Handler/WPTransitHandler.h"

class UPrimitiveComponent;
class UWorld;

struct FWPTransform;
struct FWPTransitComponentPair;
struct FWPTransitRun;

namespace Chaos
{
	class FSingleParticlePhysicsProxy;
}

/**
 * @brief Handles Transit for supported single-body Physics Primitives.
 *
 * During Crossing, distributes mass between the Master and Twin and connects the two
 * Bodies through a Chaos Callback. On Commit or Cancel, restores the Master's original physics settings.
 */
class FWPPhysicsHandler : public IWPTransitHandler
{
public:
	/**
	 * @brief Returns whether this Handler supports the Primitive as a single Physics Body.
	 * @param Part Primitive Component to inspect.
	 * @return true for a non-instanced Static Mesh Component with a valid Static Mesh, or for a
	 * Shape Component; otherwise false.
	 */
	static bool SupportsPart(const UPrimitiveComponent* Part);
	
	/** @brief Reads every Twin Body state before applying the complete set to the Master. */
	virtual bool Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const override;
	
	/** @brief Removes the Chaos connection and restores the Master's pre-Transit physics state. */
	virtual void Cancel(FWPTransitRun& Run) const override;

	/** @brief Returns the World-space velocity of the first moving supported Physics Primitive. */
	virtual FVector GetVelocity(const UWPTransitComponent* TransitComponent) const override;
	
	/**
	 * @brief Returns whether the bounds of every tracked Twin Physics Component have cleared the Destination exit surface.
	 * @param Run Run containing the Physics Component pairs.
	 * @param SourceSurface Source surface supplied by the common Handler contract.
	 * @param EntryNormal Source entry normal supplied by the common Handler contract.
	 * @param DestSurface World-space point on the Destination exit surface.
	 * @param ExitNormal World-space direction pointing outward from the Destination.
	 * @return true if every tracked Twin Physics Component has cleared the exit surface.
	 */
	virtual bool HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface, const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const override;
	
	/** @brief Maps supported Twin Physics Bodies into Destination space and prepares their Run state. */
	virtual bool Begin(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const override;

	/** @brief Retries registration for any Chaos Callback that has not yet been registered. */
	virtual bool Update(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const override;
	
private:
	/** @brief Registers valid Master/Twin Physics Proxy pairs with the Chaos correction Callback. */
	static void BindChaos(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping);

	/** @brief Unregisters the Chaos Callback and disables Contact Modification. */
	static void UnbindChaos(FWPTransitRun& Run);

	/**
	 * @brief Saves the Master state before Transit begins and distributes mass between the Master and Twin.
	 * @param Pair Master/Twin Component Pair to prepare.
	 */
	static void PrepPair(FWPTransitComponentPair& Pair);
	
	/**
	 * @brief Restores the Master physics state saved by PrepPair.
	 * @param Pair Master/Twin Component Pair to restore.
	 */
	static void ResetPair(FWPTransitComponentPair& Pair);
	
	/** @brief Reads the latest Transform, linear velocity, and angular velocity from a Physics Body. */
	static bool ReadBody(UPrimitiveComponent* Part, OUT FTransform& OutTransform, OUT FVector& OutLinear, OUT FVector& OutAngular);
	
	/** @brief Returns the Chaos Proxy associated with the Primitive's Game Thread Physics Actor. */
	static Chaos::FSingleParticlePhysicsProxy* GetProxy(UPrimitiveComponent* Part);
	
};
