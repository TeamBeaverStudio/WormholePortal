// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBodySetup;
class UPrimitiveComponent;
class UWorld;

struct FWPTransitRun;
struct FWPVoxelPair;

/**
 * @brief Manages Voxel Body preparation, plane updates, restoration, and debug drawing.
 */
class FWPVoxel
{
public:
	// Associates editor-baked data with the supported Primitive Pairs tracked by the Transit Run.
	static bool GatherPairs(FWPTransitRun& Run);

	// Replaces the collected Master/Twin Pairs with Voxel Bodies.
	static bool Begin(FWPTransitRun& Run);

	// Restores the Master and Twin to their original Physics Bodies.
	static void Reset(FWPTransitRun& Run);
	
	// Updates the Master and Twin Voxel Shape states relative to the Portal planes.
	static void Update(FWPTransitRun& Run, const FVector& SourceSurface, 
		const FVector& SourceNormal, const FVector& DestSurface, const FVector& DestNormal);
	
	/**
 	 * @brief Draws the current Master and Twin Voxel Shape states for an active Run.
 	 * @param World World used for transient debug drawing.
 	 * @param Run Transit Run containing the Voxel Pairs and current Shape states.
 	 */
	static void DrawDebug(UWorld* World, const FWPTransitRun& Run);
	
private:
	// Reinitializes one Physics Body with a different collision definition.
	static bool SetBodySetup(UPrimitiveComponent* Comp, UBodySetup* NewBodySetup);

	// Applies Voxel Collision to one Master/Twin Pair.
	static bool PrepPair(FWPVoxelPair& Pair);

	// Restores one Master/Twin Pair to its original BodySetups.
	static void ResetPair(FWPVoxelPair& Pair);
	
	// Enables or disables a Component's Voxel Shapes relative to the specified plane.
	static void UpdatePrimitive(UPrimitiveComponent* Comp, const TArray<FVector>& Centers, 
		const FVector& Surface, const FVector& Normal, TArray<uint8>& InOutState);
};
