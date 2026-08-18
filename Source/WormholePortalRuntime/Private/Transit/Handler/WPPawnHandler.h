// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/Handler/WPTransitHandler.h"


class APawn;
class UPawnMovementComponent;
class USceneComponent;
class UWPTransitComponent;

struct FWPTransitRun;
struct FWPTransform;

/**
 * @brief Handles Transit for a general Pawn whose Pawn Movement directly moves the Root.
 *
 * Character and Projectile actors are handled by their dedicated Handlers.
 * A Controller is optional. Control Rotation is mapped when a Controller exists, and
 * owning-client correction is sent only through a PlayerController.
 */
class FWPPawnHandler : public IWPTransitHandler
{
public:	
	/** @brief Returns the Pawn Movement's current World-space velocity. */
	virtual FVector GetVelocity(const UWPTransitComponent* TransitComp) const override;

	/**
	 * @brief Tests whether the Pawn shape has completely crossed inside the Source surface.
	 *
	 * Uses complete passage of the Source Master, rather than clearance of the Twin from the exit, 
	 * as the completion condition so Transit can finish even when the preserved movement direction is parallel to the selected Destination exit plane.
	 */
	virtual bool HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
		const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const override;
	
	/** @brief Maps Pawn location and velocity, plus Control Rotation when a Controller exists. */
	virtual bool Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const override;
	
private:
	/** @brief Returns the Transit Component's Owner as a Pawn. */
	static APawn* GetPawn(const UWPTransitComponent* TransitComp);
	
	/** @brief Returns the Pawn Movement selected by the Resolver. */
	static UPawnMovementComponent* GetMove(const UWPTransitComponent* TransitComp);
	
	/** @brief Returns the Root Component actually moved by the Pawn Movement. */
	static USceneComponent* GetRoot(const UWPTransitComponent* TransitComp, const UPawnMovementComponent* MoveComp);
};
