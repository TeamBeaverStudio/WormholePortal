// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/WPTransitTypes.h"

class UWPTransitComponent;
class UWorld;

struct FWPTransform;
struct FWPTransitRun;

/**
 * @brief Internal interface for running Actor-specific Transit behavior through a
 *        common lifecycle.
 *
 * The Subsystem invokes this contract without branching on concrete Actor types. The
 * base implementation
 * updates the Twin Transform and performs the common passage test; type-specific
 * Handlers override only
 * the stages they require.
 */
class IWPTransitHandler
{
public:
	virtual ~IWPTransitHandler() = default;

	/** @brief Returns the World-space velocity used to select the tangent plane and compute the entry direction. */
	virtual FVector GetVelocity(const UWPTransitComponent* TransitComponent) const = 0;

	/** @brief Prepares type-specific Run state after Twin creation. The base implementation succeeds without creating additional state. */
	virtual bool 	Begin(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const;

	/** @brief Updates the Twin and type-specific state from the current Master state during Crossing. */
	virtual bool 	Update(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const;

	/** @brief Returns whether the logical body for this Actor type has completely crossed the Portal boundary. */
	virtual bool 	HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface, const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const;

	/** @brief Applies the Twin's final state to the Master. */
	virtual bool 	Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const = 0;

	/** @brief Restores type-specific temporary state modified during Begin to its original values. */
	virtual void 	Cancel(FWPTransitRun& Run) const;
};

/**
 * @brief Returns the shared Handler for a resolved Transit type.
 *
 * Handlers do not retain Run state; all persistent state is stored in FWPTransitRun.
 */
class FWPTransitHandlers
{
public:
	/**
	 * @brief Finds the shared Handler for the specified Transit type.
	 * @param TransitType Transit type resolved by the Resolver.
	 * @return Shared Handler for a supported type, or nullptr for Auto or an invalid value.
	 */
	static const IWPTransitHandler* Get(EWPTransitType TransitType);
};
