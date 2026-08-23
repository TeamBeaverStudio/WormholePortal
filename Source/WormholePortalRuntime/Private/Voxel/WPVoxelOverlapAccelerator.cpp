// Copyright 2026 Team Beaver. All Rights Reserved.


#include "Voxel/WPVoxelOverlapAccelerator.h"

#include "Engine/OverlapResult.h"
#include "Voxel/WPVoxelData.h"

void FWPVoxelOverlapAccelerator::BuildChunkData(const TArray<FVector>& Centers, int32 ChunkGridResolution,
	OUT TArray<int32>& OutVoxelChunkIndices, OUT TArray<FBox>& OutChunkLocalBounds)
{
	OutVoxelChunkIndices.Reset();
	
	const int32 Resolution = FMath::Max(1, ChunkGridResolution);
	const int32 ChunkCount = Resolution * Resolution * Resolution;
	OutChunkLocalBounds.Init(FBox(ForceInit), ChunkCount);
	
	if (Centers.IsEmpty()) return;
	
	FBox Bounds(ForceInit);
	
	for (const FVector& Center : Centers) Bounds += Center;
	
	// Guards against dividing by zero when the Mesh is degenerate along one axis.
	const FVector CellSize = Bounds.GetSize() / static_cast<float>(Resolution);
	const FVector SafeCellSize(
		FMath::Max(CellSize.X, UE_KINDA_SMALL_NUMBER),
		FMath::Max(CellSize.Y, UE_KINDA_SMALL_NUMBER),
		FMath::Max(CellSize.Z, UE_KINDA_SMALL_NUMBER));
	
	OutVoxelChunkIndices.Reserve(Centers.Num());
	for (const FVector& Center : Centers)
	{
		const FVector Local = (Center - Bounds.Min) / SafeCellSize;
		const int32 CellX = FMath::Clamp(FMath::FloorToInt(Local.X), 0, Resolution - 1);
		const int32 CellY = FMath::Clamp(FMath::FloorToInt(Local.Y), 0, Resolution - 1);
		const int32 CellZ = FMath::Clamp(FMath::FloorToInt(Local.Z), 0, Resolution - 1);
		const int32 ChunkIndex = (CellX * Resolution + CellY) * Resolution + CellZ;
		
		OutVoxelChunkIndices.Add(ChunkIndex);
		OutChunkLocalBounds[ChunkIndex] += Center;
	}
}

void FWPVoxelOverlapAccelerator::BuildBVHData(const TArray<FVector>& Centers, int32 MaxVoxelsPerLeaf,
	OUT TArray<FWPVoxelBVHNode>& OutNodes)
{
	OutNodes.Reset();
	if (Centers.IsEmpty()) return;
	
	TArray<int32> VoxelOrder;
	VoxelOrder.Reserve(Centers.Num());
	for (int32 i = 0; i < Centers.Num(); ++i) VoxelOrder.Add(i);
	BuildVoxelBVHNode(Centers, VoxelOrder, 0, Centers.Num(), FMath::Max(1, MaxVoxelsPerLeaf), OutNodes);
}

void FWPVoxelOverlapAccelerator::CollectActiveBoundsBVH(const TArray<FWPVoxelBVHNode>& Nodes, UPrimitiveComponent* Comp,
	const FVector& Surface, const FVector& Normal, OUT TArray<FBox>& OutBoxes)
{
	if (!IsValid(Comp) || Nodes.IsEmpty()) return;
	
	CollectActiveBoundsBVHRecursive(Nodes, 0, Comp->GetComponentTransform(), Surface, Normal, OutBoxes);
}

void FWPVoxelOverlapAccelerator::CollectActiveBoundsBVHRecursive(const TArray<FWPVoxelBVHNode>& Nodes, int32 NodeIndex,
	const FTransform& CompTransform, const FVector& Surface, const FVector& Normal, OUT TArray<FBox>& OutBoxes)
{
	if (!Nodes.IsValidIndex(NodeIndex)) return;
	
	const FWPVoxelBVHNode& Node = Nodes[NodeIndex];
	const EWPVoxelPlaneClass Classification = ClassifyBoxAgainstPlane(Node.BoundsMin, Node.BoundsMax, CompTransform, Surface, Normal);
	
	if (Classification == EWPVoxelPlaneClass::Outside) return;
	
	const bool bLeaf = Node.LeftChildIndex == INDEX_NONE;
	if (Classification == EWPVoxelPlaneClass::Inside || bLeaf)
	{
		// Exact 8-corner transform runs only for a Node that is actually kept, not for every visited Node.
		OutBoxes.Add(FBox(Node.BoundsMin, Node.BoundsMax).TransformBy(CompTransform));
		return;
	}
	
	CollectActiveBoundsBVHRecursive(Nodes, Node.LeftChildIndex, CompTransform, Surface, Normal, OutBoxes);
	CollectActiveBoundsBVHRecursive(Nodes, Node.RightChildIndex, CompTransform, Surface, Normal, OutBoxes);
}

void FWPVoxelOverlapAccelerator::MergeAdjacentChunkBounds(const TArray<FBox>& ChunkWorldBounds, int32 GridResolution,
	OUT TArray<FBox>& OutMergedBoxes)
{
	OutMergedBoxes.Reset();
	
	const int32 ChunkCount = ChunkWorldBounds.Num();
	if (ChunkCount == 0 || GridResolution <= 0) return;
	
	// Union-Find groups Chunks that are face-adjacent on the grid, reducing the number of Scene Queries.
	TArray<int32> Parent;
	Parent.SetNumUninitialized(ChunkCount);
	for (int32 i = 0; i < ChunkCount; ++i) Parent[i] = i;
	const auto FindRoot = [&Parent](int32 Index)
	{
		while (Parent[Index]!=Index)
		{
			Parent[Index] = Parent[Parent[Index]];
			Index = Parent[Index];
		}
		return Index;
	};
	
	const int32 ResSq = GridResolution * GridResolution;
	
	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		if (!ChunkWorldBounds[ChunkIndex].IsValid) continue;
		const int32 X = ChunkIndex / ResSq;
		const int32 Y = (ChunkIndex / GridResolution) % GridResolution;
		const int32 Z = ChunkIndex % GridResolution;
		
		// Checking only the +X/+Y/+Z neighbor is enough; Union-Find still connects the full group.
		if (X + 1 < GridResolution && ChunkWorldBounds[ChunkIndex + ResSq].IsValid)
		{
			Parent[FindRoot(ChunkIndex)] = FindRoot(ChunkIndex + ResSq);
		}
		if (Y + 1 < GridResolution && ChunkWorldBounds[ChunkIndex + GridResolution].IsValid)
		{
			Parent[FindRoot(ChunkIndex)] = FindRoot(ChunkIndex + GridResolution);
		}
		if (Z + 1 < GridResolution && ChunkWorldBounds[ChunkIndex + 1].IsValid)
		{
			Parent[FindRoot(ChunkIndex)] = FindRoot(ChunkIndex + 1);
		}
	}
	
	TMap<int32, FBox> MergedByRoot;
	for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
	{
		if (!ChunkWorldBounds[ChunkIndex].IsValid) continue;
		
		const int32 Root = FindRoot(ChunkIndex);
		if (FBox* Existing = MergedByRoot.Find(Root))
		{
			*Existing += ChunkWorldBounds[ChunkIndex];
		}
		else
		{
			MergedByRoot.Add(Root, ChunkWorldBounds[ChunkIndex]);
		}
	}
	
	MergedByRoot.GenerateValueArray(OutMergedBoxes);
}

void FWPVoxelOverlapAccelerator::RelayOverlapDiff(UPrimitiveComponent* Comp, const TArray<FBox>& ActiveBounds,
	OUT TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutLastOverlaps)
{
	UWorld* World = IsValid(Comp) ? Comp->GetWorld() : nullptr;
	if (World == nullptr) return;
	
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FWPVoxelOverlapAccelerator), false);
	Params.AddIgnoredComponent(Comp);
	
	TArray<TWeakObjectPtr<UPrimitiveComponent>> NewOverlaps;
	for (const FBox& Box : ActiveBounds)
	{
		if (!Box.IsValid) continue;
		
		TArray<FOverlapResult> Results;
		bool bResult = World->OverlapMultiByChannel(Results, Box.GetCenter(), FQuat::Identity, Comp->GetCollisionObjectType(), 
			FCollisionShape::MakeBox(Box.GetExtent()), Params);
		
		if (!bResult) continue;
		
		for (const FOverlapResult& Result : Results)
		{
			UPrimitiveComponent* HitComp = Result.GetComponent();
			if (IsValid(HitComp) && HitComp != Comp && HitComp->GetGenerateOverlapEvents())
			{
				NewOverlaps.AddUnique(HitComp);
			}
		}
	}
	
	// Begin/EndComponentOverlap require GetGenerateOverlapEvents() true on both sides, so it is toggled only for the relay.
	Comp->SetGenerateOverlapEvents(true);
	
	for (const TWeakObjectPtr<UPrimitiveComponent>& OldOverlap : InOutLastOverlaps)
	{
		if (OldOverlap.IsValid() && !NewOverlaps.Contains(OldOverlap))
		{
			Comp->EndComponentOverlap(FOverlapInfo(OldOverlap.Get()), true, false);
		}
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& NewOverlap : NewOverlaps)
	{
		if (NewOverlap.IsValid() && !InOutLastOverlaps.Contains(NewOverlap))
		{
			Comp->BeginComponentOverlap(FOverlapInfo(NewOverlap.Get()), true);
		}
	}
	
	Comp->SetGenerateOverlapEvents(false);
	InOutLastOverlaps = MoveTemp(NewOverlaps);
}

void FWPVoxelOverlapAccelerator::Cleanup(UPrimitiveComponent* Comp,
	OUT TArray<TWeakObjectPtr<UPrimitiveComponent>>& InOutLastOverlaps)
{
	if (InOutLastOverlaps.IsEmpty()) return;

	if (IsValid(Comp))
	{
		Comp->SetGenerateOverlapEvents(true);
		for (const TWeakObjectPtr<UPrimitiveComponent>& OldOverlap : InOutLastOverlaps)
		{
			if (OldOverlap.IsValid())
			{
				Comp->EndComponentOverlap(FOverlapInfo(OldOverlap.Get()), true, false);
			}
		}
		Comp->SetGenerateOverlapEvents(false);
	}

	InOutLastOverlaps.Reset();
}


EWPVoxelPlaneClass FWPVoxelOverlapAccelerator::ClassifyBoxAgainstPlane(const FVector& LocalMin, const FVector& LocalMax,
	const FTransform& CompTransform, const FVector& Surface, const FVector& Normal)
{
	// 8점 검사 -> 육면체를 감싸는 구 검사로 근사.
	const FVector LocalCenter = (LocalMin + LocalMax) * 0.5f;
	const float LocalRadius = (LocalMax - LocalMin).Size() * 0.5f;
	const float SafeRadius = LocalRadius * CompTransform.GetScale3D().GetAbsMax();
	
	const FVector WorldCenter = CompTransform.TransformPosition(LocalCenter);
	const float SignedDist = FVector::DotProduct(WorldCenter - Surface, Normal);
	
	if (SignedDist > SafeRadius) return EWPVoxelPlaneClass::Inside;
	if (SignedDist < -SafeRadius) return EWPVoxelPlaneClass::Outside;
	return EWPVoxelPlaneClass::Intersecting;
}

int32 FWPVoxelOverlapAccelerator::BuildVoxelBVHNode(const TArray<FVector>& Centers, TArray<int32>& VoxelOrder,
	int32 Start, int32 Count, int32 MaxVoxelsPerLeaf, TArray<FWPVoxelBVHNode>& OutNodes)
{
	FBox Bounds(ForceInit);
	for (int32 i = Start; i < Start + Count; ++i) Bounds += Centers[VoxelOrder[i]];
	
	const int32 NodeIndex = OutNodes.Add(FWPVoxelBVHNode());
	OutNodes[NodeIndex].BoundsMin = Bounds.Min;
	OutNodes[NodeIndex].BoundsMax = Bounds.Max;
	
	if (Count <= MaxVoxelsPerLeaf) return NodeIndex;
	
	const FVector Extent = Bounds.GetSize();
	int32 Axis = 0;														// X-Axis
	if (Extent.Y >= Extent.X && Extent.Y >= Extent.Z) Axis = 1;			// Y-Axis
	else if (Extent.Z >= Extent.X && Extent.Z >= Extent.Y) Axis = 2;	// Z-Axis
	
	TArrayView<int32> Range(VoxelOrder.GetData() + Start, Count);
	Algo::Sort(Range, [&Centers, Axis](int32 A, int32 B){return Centers[A][Axis] < Centers[B][Axis];});
	const int32 Mid = Count / 2;
	const int32 LeftIndex = BuildVoxelBVHNode(Centers, VoxelOrder, Start, Mid, MaxVoxelsPerLeaf, OutNodes);
	const int32 RightIndex = BuildVoxelBVHNode(Centers, VoxelOrder, Start + Mid, Count - Mid, MaxVoxelsPerLeaf, OutNodes);
	
	// OutNodes.Add may reallocate during recursion, so child indices are written after the recursive calls return.
	OutNodes[NodeIndex].LeftChildIndex = LeftIndex;
	OutNodes[NodeIndex].RightChildIndex = RightIndex;
	return NodeIndex;
}
