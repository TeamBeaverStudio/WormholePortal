// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/Handler/WPTransitHandler.h"

class UProjectileMovementComponent;
class USceneComponent;
class UWPTransitComponent;

struct FWPTransitRun;
struct FWPTransform;

/**
 * @brief Handles Transit for an Actor whose Projectile Movement moves its
 *        UpdatedComponent.
 */
class FWPProjectileHandler : public IWPTransitHandler
{
public:
	/** @brief Returns the Projectile Movement's current World-space velocity. */
	virtual FVector GetVelocity(const UWPTransitComponent* TransitComp) const override;

	/**
	 * @brief Tests whether the Projectile shape has completely crossed inside the Source
	 *        surface.
	 *
	 * Uses complete passage of the Source Master, rather than clearance of the Twin from
	 * the exit, as the
	 * completion condition so Transit can finish even when the preserved flight direction
	 * is parallel to the
	 * selected Destination exit plane.
	 */
	virtual bool HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
		const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const override;
	
	/** @brief Maps the UpdatedComponent location and Projectile Velocity into Destination space. */
	virtual bool Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const override;
	
private:
	/** @brief Returns the Projectile Movement selected by the Resolver. */
	static UProjectileMovementComponent* GetMove(const UWPTransitComponent* TransitComp);
	
	/** @brief Returns the UpdatedComponent actually moved by the Projectile Movement. */
	static USceneComponent* GetRoot(const UWPTransitComponent* TransitComp, const UProjectileMovementComponent* MoveComp);
};
