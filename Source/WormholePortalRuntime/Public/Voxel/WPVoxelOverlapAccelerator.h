// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;
struct FWPVoxelBVHNode;

enum class EWPVoxelPlaneClass : uint8
{
	Inside,
	Outside,
	Intersecting
};

/**
 * @brief Builds the Chunk/BVH acceleration data at Bake time and evaluates it at Runtime to approximate
 *        Overlap Events for the compound Voxel Body.
 *
 * The real Voxel Body keeps GenerateOverlapEvents disabled while this class is active (see FWPVoxel::PrepPair).
 * Instead, a small number of approximate active-region Boxes are queried per frame and relayed through the
 * same BeginComponentOverlap/EndComponentOverlap contract the engine's own UpdateOverlaps uses.
 */
class WORMHOLEPORTALRUNTIME_API FWPVoxelOverlapAccelerator
{
public:
	// Assigns every Voxel Center to a Chunk grid cell and computes each Chunk's tight Local Bounds.
	static void BuildChunkData(const TArray<FVector>& Centers, const FVector& VoxelExtent, int32 ChunkGridResolution, 
		OUT TArray<int32>& OutVoxelChunkIndices, OUT TArray<FBox>& OutChunkLocalBounds);
	
	// Builds a median-split binary BVH over Voxel Centers.
	static void BuildBVHData(const TArray<FVector>& Centers, const FVector& VoxelExtent, int32 MaxVoxelsPerLeaf, 
		OUT TArray<FWPVoxelBVHNode>& OutNodes);
	
	// Traverses the baked BVH and collects this frame's active-region Boxes for one Component.
	static void CollectActiveBoundsBVH(const TArray<FWPVoxelBVHNode>& Nodes, UPrimitiveComponent* Comp, const FVector& Surface, const FVector& Normal, 
		OUT TArray<FBox>& OutBoxes);
	
	// Same traversal as CollectActiveBoundsBVH, but records every visited Node and its classification, not just the adopted ones. Debug-draw only.
	static void CollectDebugBVHNodes(const TArray<FWPVoxelBVHNode>& Nodes, UPrimitiveComponent* Comp,
		const FVector& Surface, const FVector& Normal, OUT TArray<FBox>& OutBoxes, OUT TArray<EWPVoxelPlaneClass>& OutClasses);
	
	// Merges Chunk Boxes that touch on the fixed grid into fewer, larger Boxes to reduce Scene Query count.
	static void MergeAdjacentChunkBounds(const TArray<FBox>& ChunkWorldBounds, int32 GridResolution, 
		OUT TArray<FBox>& OutMergedBoxes);
	
	// Queries ActiveBounds, diffs the result against last frame, and relays Begin/EndComponentOverlap.
	static void RelayOverlapDiff(UPrimitiveComponent* Comp, const TArray<FBox>& ActiveBounds, 
		OUT TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutLastOverlaps);
	
	// Fires the final EndComponentOverlap for any approximate Overlap still open when Crossing ends.
	static void Cleanup(UPrimitiveComponent* Comp,
		OUT TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutLastOverlaps);
	
private:
	// Recursively visits Nodes, skipping Fully Inactive ones and adopting a Box only at a Fully Active Node or a Leaf.
	static void CollectActiveBoundsBVHRecursive(const TArray<FWPVoxelBVHNode>& Nodes, int32 NodeIndex, 
		const FTransform& CompTransform, const FVector& Surface, const FVector& Normal, 
		OUT TArray<FBox>& OutBoxes);
	
	// Cheap center+radius classification instead of transforming all 8 corners every visit.
	// Scale is approximated conservatively by its largest component.
	static EWPVoxelPlaneClass ClassifyBoxAgainstPlane(const FVector& LocalMin, const FVector& LocalMax, const FTransform& CompTransform, 
		const FVector& Surface, const FVector& Normal);
	
	// Recursive median-split-by-longest-axis BVH construction.
	static int32 BuildVoxelBVHNode(const TArray<FVector>& Centers, const FVector& VoxelExtent, TArray<int32>& VoxelOrder, 
		int32 Start, int32 Count, int32 MaxVoxelsPerLeaf, OUT TArray<FWPVoxelBVHNode>& OutNodes);
	
	static void CollectDebugBVHNodesRecursive(const TArray<FWPVoxelBVHNode>& Nodes, int32 NodeIndex,
		const FTransform& CompTransform, const FVector& Surface, const FVector& Normal,
		OUT TArray<FBox>& OutBoxes, OUT TArray<EWPVoxelPlaneClass>& OutClasses);
};
