// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBodySetup;
class UPrimitiveComponent;
class UWorld;
class UWPVoxelData;
enum class EWPVoxelOverlapMode : uint8;

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
	
	// Draws the current Master/Twin Voxel Shape Blocking states for an active Run.
	static void DrawVoxelDebugBox(UWorld* World, const FWPTransitRun& Run);
	
	// Draws the last Chunk Overlap approximation Boxes computed by Update() for an active Run.
	static void DrawChunkDebugBox(UWorld* World, const FWPTransitRun& Run);
	
	// Draws the last BVH Overlap approximation Boxes computed by Update() for an active Run.
	static void DrawBVHDebugBox(UWorld* World, const FWPTransitRun& Run);
	
private:
	// Reinitializes one Physics Body with a different collision definition.
	static bool SetBodySetup(UPrimitiveComponent* Comp, UBodySetup* NewBodySetup);

	// Applies Voxel Collision to one Master/Twin Pair.
	static bool PrepPair(FWPVoxelPair& Pair);

	// Restores one Master/Twin Pair to its original BodySetups.
	static void ResetPair(FWPVoxelPair& Pair);
	
	// Enables or disables a Component's Voxel Shapes relative to the specified plane. In Chunk Mode, also
	// accumulates each active Voxel's World Center into OutActiveChunkBounds, indexed by its owning Chunk.
	static void UpdatePrimitive(UPrimitiveComponent* Comp, const UWPVoxelData* VoxelData, EWPVoxelOverlapMode Mode, 
		const FVector& Surface, const FVector& Normal, 
		OUT TArray<uint8>& InOutState, OUT TArray<FBox>& OutActiveChunkBounds);
};
