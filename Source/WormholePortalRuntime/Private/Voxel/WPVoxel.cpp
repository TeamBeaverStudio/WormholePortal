// Copyright 2026 Team Beaver. All Rights Reserved.


#include "Voxel/WPVoxel.h"

#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "DrawDebugHelpers.h"

#include "Transit/WPTransitRun.h"
#include "Voxel/WPVoxelData.h"
#include "Voxel/WPVoxelOverlapAccelerator.h"
#include "WPLog.h"
#include "WPSettings.h"
#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitTypes.h"

bool FWPVoxel::GatherPairs(FWPTransitRun& Run)
{
	Run.VoxelPairs.Reset();
	
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	AActor* TwinActor = Run.TwinActor;
	if (!IsValid(TransitComp) || !IsValid(TwinActor)) return false;
	
	TInlineComponentArray<UPrimitiveComponent*> TwinPrimitives(TwinActor);
	TMap<FName, UPrimitiveComponent*> TwinByName;
	for (UPrimitiveComponent* TwinPrimitive : TwinPrimitives)
	{
		if (IsValid(TwinPrimitive) && TwinPrimitive->GetOwner() == TwinActor)
		{
			TwinByName.Add(TwinPrimitive->GetFName(), TwinPrimitive);
		}
	}
	
	// Prepares every supported Transit Primitive for Voxel Collision.
	for (UPrimitiveComponent* Master : TransitComp->GetTransitPrimitives())
	{
		if (!IsValid(Master)) continue;
		
		const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Master);
		
		const bool bSupportedStaticMesh = IsValid(StaticMesh) && !StaticMesh->IsA<UInstancedStaticMeshComponent>() && IsValid(StaticMesh->GetStaticMesh());
		const bool bSupportedShape = Master->IsA<UBoxComponent>() || Master->IsA<USphereComponent>() || Master->IsA<UCapsuleComponent>();
		if (!bSupportedStaticMesh && !bSupportedShape) continue;
		
		UPrimitiveComponent* Twin = TwinByName.FindRef(Master->GetFName());
		if (!IsValid(Twin) || Twin->GetClass() != Master->GetClass()) continue;

		const UWPVoxelData* VoxelData = TransitComp->FindVoxelData(Master);
		if (!IsValid(VoxelData))
		{
			WP_LOG(TransitComp, Warning,
					TEXT("Voxel Skipped. Actor=%s, Primitive=%s, Reason=No Matching Voxel Data"),
					*GetNameSafe(TransitComp->GetOwner()),
					*GetNameSafe(Master));
			
			Run.VoxelPairs.Reset();
			return false;
		}

		FWPVoxelPair& Pair = Run.VoxelPairs.AddDefaulted_GetRef();
		Pair.Master = Master;
		Pair.Twin = Twin;
		Pair.VoxelData = const_cast<UWPVoxelData*>(VoxelData);
		Pair.Mode = TransitComp->GetVoxelOverlapMode();
	}
	
	return true;
}

bool FWPVoxel::Begin(FWPTransitRun& Run)
{
	int32 FailedIndex = INDEX_NONE;

	for (int32 PairIndex = 0; PairIndex < Run.VoxelPairs.Num(); ++PairIndex)
	{
		if (!PrepPair(Run.VoxelPairs[PairIndex]))
		{
			FailedIndex = PairIndex;
			break;
		}
	}

	if (FailedIndex == INDEX_NONE) return true;

	// 로그 전용: Voxel 적용 실패 로그에 Actor와 Primitive 문맥을 남기기 위한 조회입니다.
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	UPrimitiveComponent* FailedPrimitive = Run.VoxelPairs[FailedIndex].Master.Get();

	if (IsValid(TransitComp))
	{
		WP_LOG(TransitComp, Warning, TEXT("Voxel Transit skipped. Actor=%s, Primitive=%s, Reason=Voxel Body swap failed"), *GetNameSafe(TransitComp->GetOwner()), *GetNameSafe(FailedPrimitive));
	}

	// 앞쪽 Pair가 교체된 상태라면 함께 원본 Body로 복원
	Reset(Run);
	return false;
}

void FWPVoxel::Reset(FWPTransitRun& Run)
{
	for (FWPVoxelPair& Pair : Run.VoxelPairs)
	{
		ResetPair(Pair);
	}

	Run.VoxelPairs.Reset();
}

void FWPVoxel::Update(FWPTransitRun& Run, const FVector& SourceSurface, const FVector& SourceNormal, const FVector& DestSurface, const FVector& DestNormal)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FWPVoxel::Update);
	
	for (FWPVoxelPair& Pair : Run.VoxelPairs)
	{
		const UWPVoxelData* VoxelData = Pair.VoxelData.Get();
		if (!Pair.bUsingVoxelBody || !IsValid(VoxelData)) continue;
		
		TArray<FBox> MasterActiveBounds;
		TArray<FBox> TwinActiveBounds;
		
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::UpdateMaster);
			UpdatePrimitive(Pair.Master, VoxelData, Pair.Mode, SourceSurface, SourceNormal, Pair.MasterState, MasterActiveBounds);
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::UpdateTwin);
			UpdatePrimitive(Pair.Twin, VoxelData, Pair.Mode, DestSurface, DestNormal, Pair.TwinState, TwinActiveBounds);
		}
		
		if (Pair.Mode == EWPVoxelOverlapMode::BVH)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::CollectBVHBounds);
			FWPVoxelOverlapAccelerator::CollectActiveBoundsBVH(VoxelData->VoxelBVHNodes, Pair.Master.Get(), SourceSurface, SourceNormal, MasterActiveBounds);
			FWPVoxelOverlapAccelerator::CollectActiveBoundsBVH(VoxelData->VoxelBVHNodes, Pair.Twin.Get(), DestSurface, DestNormal, TwinActiveBounds);
			
			// Debug Draw shows the actual traversal including pruned Nodes, not just the optimized final result.
			Pair.DebugMasterActiveBounds.Reset();
			Pair.DebugMasterBVHClasses.Reset();
			FWPVoxelOverlapAccelerator::CollectDebugBVHNodes(VoxelData->VoxelBVHNodes, Pair.Master.Get(), SourceSurface, SourceNormal, Pair.DebugMasterActiveBounds, Pair.DebugMasterBVHClasses);
			
			Pair.DebugTwinActiveBounds.Reset();
			Pair.DebugTwinBVHClasses.Reset();
			FWPVoxelOverlapAccelerator::CollectDebugBVHNodes(VoxelData->VoxelBVHNodes, Pair.Twin.Get(), DestSurface, DestNormal, Pair.DebugTwinActiveBounds, Pair.DebugTwinBVHClasses);
		}
		else if (Pair.Mode == EWPVoxelOverlapMode::Chunk)
		{
			// Debug Draw shows each Chunk's own raw Box before merging, not the merged Box used for the query.
			Pair.DebugMasterActiveBounds = MasterActiveBounds;
			Pair.DebugTwinActiveBounds = TwinActiveBounds;
			Pair.DebugMasterBVHClasses.Reset();
			Pair.DebugTwinBVHClasses.Reset();
			
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::MergeChunkBounds);
			TArray<FBox> MergedMaster, MergedTwin;
			FWPVoxelOverlapAccelerator::MergeAdjacentChunkBounds(MasterActiveBounds, VoxelData->ChunkGridResolution, MergedMaster);
			FWPVoxelOverlapAccelerator::MergeAdjacentChunkBounds(TwinActiveBounds, VoxelData->ChunkGridResolution, MergedTwin);
			MasterActiveBounds = MoveTemp(MergedMaster);
			TwinActiveBounds = MoveTemp(MergedTwin);
		}
		else
		{
			Pair.DebugMasterActiveBounds.Reset();
			Pair.DebugTwinActiveBounds.Reset();
			Pair.DebugMasterBVHClasses.Reset();
			Pair.DebugTwinBVHClasses.Reset();
		}
		
		// Copied into the Pair before Relay so Debug Draw can use the exact Boxes that were queried.
		Pair.DebugMasterActiveBounds = MasterActiveBounds;
		Pair.DebugTwinActiveBounds = TwinActiveBounds;
		
		if (Pair.Mode == EWPVoxelOverlapMode::Chunk || Pair.Mode == EWPVoxelOverlapMode::BVH)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::RelayOverlaps);
			FWPVoxelOverlapAccelerator::RelayOverlapDiff(Pair.Master.Get(), MasterActiveBounds, Pair.LastMasterOverlaps);
			FWPVoxelOverlapAccelerator::RelayOverlapDiff(Pair.Twin.Get(), TwinActiveBounds, Pair.LastTwinOverlaps);
			
		}
	}
}

void FWPVoxel::DrawVoxelDebugBox(UWorld* World, const FWPTransitRun& Run)
{
	if (World == nullptr) return;
	
	auto DrawVoxel = [World](const UPrimitiveComponent* Comp, const UWPVoxelData* VoxelData, const TArray<uint8>& States)
	{
		if (!IsValid(Comp) || !IsValid(VoxelData) || States.Num() != VoxelData->VoxelCenters.Num()) return;
	
		const FTransform CompTransform = Comp->GetComponentTransform();
		const FVector DrawExtent = VoxelData->VoxelExtent * CompTransform.GetScale3D().GetAbs();
		for (int32 VoxelIndex = 0; VoxelIndex < VoxelData->VoxelCenters.Num(); ++VoxelIndex)
		{
			const bool bActive = States[VoxelIndex] != 0;
			DrawDebugBox(
				World,
				CompTransform.TransformPosition(VoxelData->VoxelCenters[VoxelIndex]),
				DrawExtent,
				CompTransform.GetRotation(),
				bActive ? FColor::Green : FColor::Red,
				false,
				0.0f,
				0,
				5.0f
			);
		}
	};
	
	for (const FWPVoxelPair& Pair : Run.VoxelPairs)
	{
		const UWPVoxelData* VoxelData = Pair.VoxelData.Get();
		if (Pair.bUsingVoxelBody && IsValid(VoxelData))
		{
			DrawVoxel(Pair.Master, VoxelData, Pair.MasterState);
			DrawVoxel(Pair.Twin, VoxelData, Pair.TwinState);
		}
	}
}

void FWPVoxel::DrawChunkDebugBox(UWorld* World, const FWPTransitRun& Run)
{
	if (World == nullptr) return;
	
	auto DrawGridAndActive = [World](const UPrimitiveComponent* Comp, const UWPVoxelData* Data, const TArray<FBox>& ActiveBounds)
	{
		if (!IsValid(Comp) || !IsValid(Data)) return;
		const FTransform CompTransform = Comp->GetComponentTransform();
		
		// Full grid, including empty Chunks, drawn dim gray to show the partition scheme itself.
		for (const FBox& LocalBox : Data->ChunkLocalBounds)
		{
			if (!LocalBox.IsValid) continue;
			const FBox WorldBox = LocalBox.TransformBy(CompTransform);
			DrawDebugBox(World, WorldBox.GetCenter(), WorldBox.GetExtent(), FQuat::Identity, FColor(80, 80, 80), false, 0.0f, 0, 1.0f);
		}
		
		// Active Chunks: only ones with a Voxel this frame, drawn bright Cyan at their raw pre-merge size.
		for (const FBox& Box : ActiveBounds)
		{
			if (!Box.IsValid) continue;
			DrawDebugBox(World, Box.GetCenter(), Box.GetExtent(), FQuat::Identity, FColor::Cyan, false, 0.0f, 0, 3.0f);
		}
	};
	
	for (const FWPVoxelPair& Pair : Run.VoxelPairs)
	{
		const UWPVoxelData* VoxelData = Pair.VoxelData.Get();
		if (Pair.bUsingVoxelBody && Pair.Mode == EWPVoxelOverlapMode::Chunk && IsValid(VoxelData))
		{
			DrawGridAndActive(Pair.Master.Get(), VoxelData, Pair.DebugMasterActiveBounds);
			DrawGridAndActive(Pair.Twin.Get(), VoxelData, Pair.DebugTwinActiveBounds);
		}
	}
}

void FWPVoxel::DrawBVHDebugBox(UWorld* World, const FWPTransitRun& Run)
{
	if (World == nullptr) return;
	
	auto ClassColor = [](EWPVoxelPlaneClass Classification)
	{
		switch (Classification)
		{
		case EWPVoxelPlaneClass::Inside: return FColor::Green;		// 채택됨
		case EWPVoxelPlaneClass::Outside: return FColor::Red;		// 가지치기로 버려짐
		default: return FColor::Yellow;								// 더 파고듦
		}
	};
	
	auto DrawNodes = [World, ClassColor](const TArray<FBox>& Boxes, const TArray<EWPVoxelPlaneClass>& Classes)
	{
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			DrawDebugBox(World, Boxes[Index].GetCenter(), Boxes[Index].GetExtent(), FQuat::Identity, ClassColor(Classes[Index]), false, 0.0f, 0, 2.0f);
		}
	};
	
	for (const FWPVoxelPair& Pair : Run.VoxelPairs)
	{
		if (Pair.bUsingVoxelBody && Pair.Mode == EWPVoxelOverlapMode::BVH)
		{
			DrawNodes(Pair.DebugMasterActiveBounds, Pair.DebugMasterBVHClasses);
			DrawNodes(Pair.DebugTwinActiveBounds, Pair.DebugTwinBVHClasses);
		}
	}
}

bool FWPVoxel::SetBodySetup(UPrimitiveComponent* Comp, UBodySetup* NewBodySetup)
{
	UWorld* World = IsValid(Comp) ? Comp->GetWorld() : nullptr;
	FPhysScene* PhysicsScene = IsValid(World) ? World->GetPhysicsScene() : nullptr;
	if (!IsValid(NewBodySetup) || PhysicsScene == nullptr) return false;
	
	FBodyInstance* Body = Comp->GetBodyInstance();
	if (Body == nullptr || !Body->IsValidBodyInstance()) return false;
	
	UBodySetup* PrevBodySetup = Body->GetBodySetup();
	if (!IsValid(PrevBodySetup)) return false;
	
	if (PrevBodySetup == NewBodySetup) return true;
	
	FTransform Transform = Body->GetUnrealWorldTransform(false, true);
	Transform.SetScale3D(Comp->GetComponentScale());
	
	const bool bWasSimulating = Body->IsInstanceSimulatingPhysics();
	const FVector LinearVelocity = bWasSimulating ? Body->GetUnrealWorldVelocity() : FVector::ZeroVector;
	const FVector AngularVelocity = bWasSimulating ? Body->GetUnrealWorldAngularVelocityInRadians() : FVector::ZeroVector;
	
	const auto InitBody = [Comp, Body, PhysicsScene, &Transform, &LinearVelocity, &AngularVelocity, bWasSimulating](UBodySetup* BodySetup)
	{
		Body->TermBody(true);
		Body->InitBody(BodySetup, Transform, Comp, PhysicsScene);
		if (!Body->IsValidBodyInstance() || Body->GetBodySetup() != BodySetup) return false;
		
		Comp->SetWorldTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
		Body->SetInstanceSimulatePhysics(bWasSimulating, false, true);
		
		if (bWasSimulating)
		{
			Body->SetLinearVelocity(LinearVelocity, false);
			Body->SetAngularVelocityInRadians(AngularVelocity, false);
		}
		return true;
	};
	
	if (InitBody(NewBodySetup)) return true;
	
	// Restores the previous collision immediately if the replacement fails.
	InitBody(PrevBodySetup);
	return false;
}

bool FWPVoxel::PrepPair(FWPVoxelPair& Pair)
{
	UPrimitiveComponent* Master = Pair.Master.Get();
	UPrimitiveComponent* Twin = Pair.Twin.Get();
	const UWPVoxelData* VoxelData = Pair.VoxelData.Get();
	UBodySetup* VoxelBodySetup = IsValid(VoxelData) ? VoxelData->VoxelBodySetup.Get() : nullptr;
	
	if (!IsValid(Master) || !IsValid(Twin) || 
		!IsValid(VoxelData) || !VoxelData->IsValidFor(Master) || !IsValid(VoxelBodySetup))
	{
		return false;
	}

	// Voxel Collision uses the current Master's collision contract in both spaces.
	Twin->SetCollisionEnabled(Master->GetCollisionEnabled());
	Twin->SetCollisionObjectType(Master->GetCollisionObjectType());
	Twin->SetCollisionResponseToChannels(Master->GetCollisionResponseToChannels());
	
	// Except in Native Mode, the real Voxel Body keeps GenerateOverlapEvents off; the Accelerator relays an approximation instead.
	if (Pair.Mode == EWPVoxelOverlapMode::Native)
	{
		Twin->SetGenerateOverlapEvents(Master->GetGenerateOverlapEvents());
	}
	else
	{
		Pair.bOriginGenerateOverlapEvents = Master->GetGenerateOverlapEvents();
		Master->SetGenerateOverlapEvents(false);
		Twin->SetGenerateOverlapEvents(false);
	}
	
	if (Twin->IsSimulatingPhysics() != Master->IsSimulatingPhysics())
	{
		Twin->SetSimulatePhysics(Master->IsSimulatingPhysics());
	}
	
	FBodyInstance* MasterBody = Master->GetBodyInstance();
	FBodyInstance* TwinBody = Twin->GetBodyInstance();
	Pair.OriginMasterBodySetup = MasterBody != nullptr ? MasterBody->GetBodySetup() : nullptr;
	Pair.OriginTwinBodySetup = TwinBody != nullptr ? TwinBody->GetBodySetup() : nullptr;
	
	if (!IsValid(Pair.OriginMasterBodySetup) || !IsValid(Pair.OriginTwinBodySetup))
	{
		Pair.OriginMasterBodySetup = nullptr;
		Pair.OriginTwinBodySetup = nullptr;
		return false;
	}
	
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const ECollisionChannel PortalCollisionChannel = Settings->PortalCollisionChannel;
	Pair.OriginMasterPCR = Master->GetCollisionResponseToChannel(PortalCollisionChannel);
	Pair.OriginTwinPCR = Twin->GetCollisionResponseToChannel(PortalCollisionChannel);
	Master->SetCollisionResponseToChannel(PortalCollisionChannel, ECR_Ignore);
	Twin->SetCollisionResponseToChannel(PortalCollisionChannel, ECR_Ignore);

	const bool bMasterReady = SetBodySetup(Master, VoxelBodySetup);
	const bool bTwinReady = bMasterReady && SetBodySetup(Twin, VoxelBodySetup);

	// Roll back Code (BodySetup 생성에 실패 시 Rollback 합니다)
	if (!bMasterReady || !bTwinReady)
	{
		SetBodySetup(Master, Pair.OriginMasterBodySetup);
		SetBodySetup(Twin, Pair.OriginTwinBodySetup);
		Master->SetCollisionResponseToChannel(PortalCollisionChannel, Pair.OriginMasterPCR);
		Twin->SetCollisionResponseToChannel(PortalCollisionChannel, Pair.OriginTwinPCR);
		if (Pair.Mode != EWPVoxelOverlapMode::Native)
		{
			Master->SetGenerateOverlapEvents(Pair.bOriginGenerateOverlapEvents);
			Twin->SetGenerateOverlapEvents(Pair.bOriginGenerateOverlapEvents);
		}
		Pair.OriginMasterBodySetup = nullptr;
		Pair.OriginTwinBodySetup = nullptr;
		Pair.OriginMasterPCR = ECR_MAX;
		Pair.OriginTwinPCR = ECR_MAX;
		return false;
	}
	
	Pair.MasterState.Init(1, VoxelData->VoxelCenters.Num());
	Pair.TwinState.Init(1, VoxelData->VoxelCenters.Num());
	Pair.bUsingVoxelBody = true;

	return true;
}

void FWPVoxel::ResetPair(FWPVoxelPair& Pair)
{
	if (!Pair.bUsingVoxelBody)
	{
		return;
	}
	
	// Fires any remaining EndComponentOverlap before restoring the original Body.
	FWPVoxelOverlapAccelerator::Cleanup(Pair.Master.Get(), Pair.LastMasterOverlaps);
	FWPVoxelOverlapAccelerator::Cleanup(Pair.Twin.Get(), Pair.LastTwinOverlaps);

	UPrimitiveComponent* Master = Pair.Master.Get();
	UPrimitiveComponent* Twin = Pair.Twin.Get();

	if (IsValid(Master) && IsValid(Pair.OriginMasterBodySetup))
	{
		SetBodySetup(Master, Pair.OriginMasterBodySetup);
	}

	if (IsValid(Twin) && IsValid(Pair.OriginTwinBodySetup))
	{
		SetBodySetup(Twin, Pair.OriginTwinBodySetup);
	}
	
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const ECollisionChannel PortalCollisionChannel = Settings->PortalCollisionChannel;
	
	if (IsValid(Master))
	{
		Master->SetCollisionResponseToChannel(PortalCollisionChannel, Pair.OriginMasterPCR);
		if (Pair.Mode != EWPVoxelOverlapMode::Native)
		{
			Master->SetGenerateOverlapEvents(Pair.bOriginGenerateOverlapEvents);
		}
	}
	if (IsValid(Twin))
	{
		Twin->SetCollisionResponseToChannel(PortalCollisionChannel, Pair.OriginTwinPCR);
	}

	Pair.MasterState.Reset();
	Pair.TwinState.Reset();
	Pair.OriginMasterBodySetup = nullptr;
	Pair.OriginTwinBodySetup = nullptr;
	Pair.OriginMasterPCR = ECR_MAX;
	Pair.OriginTwinPCR = ECR_MAX;
	Pair.bOriginGenerateOverlapEvents = false;
	Pair.bUsingVoxelBody = false;
}

void FWPVoxel::UpdatePrimitive(UPrimitiveComponent* Comp, const UWPVoxelData* VoxelData, EWPVoxelOverlapMode Mode,
	const FVector& Surface, const FVector& Normal, 
	OUT TArray<uint8>& InOutState, OUT TArray<FBox>& OutActiveChunkBounds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FWPVoxel::UpdatePrimitive);
	
	FBodyInstance* Body = IsValid(Comp) ? Comp->GetBodyInstance() : nullptr;
	if (!IsValid(VoxelData) || Body == nullptr || !Body->IsValidBodyInstance() || VoxelData->VoxelCenters.Num() != InOutState.Num()) return;
	
	const TArray<FVector>& Centers = VoxelData->VoxelCenters;
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero()) return;
	
	const FTransform CompTransform = Comp->GetComponentTransform();
	const ECollisionEnabled::Type ActiveCollision = Comp->GetCollisionEnabled();
	bool bFilterChanged = false;
	// Only in Chunk Mode, prepares one empty Box per Chunk to accumulate this frame's active Voxel Centers.
	const bool bAccumulateChunks = Mode == EWPVoxelOverlapMode::Chunk && VoxelData->VoxelChunkIndices.Num() == Centers.Num();
	// World-space Extent, including the Component's Scale, computed once outside the loop.
	const FVector WorldVoxelExtent = VoxelData->VoxelExtent * CompTransform.GetScale3D().GetAbs();
	if (bAccumulateChunks)
	{
		OutActiveChunkBounds.Init(FBox(ForceInit), VoxelData->ChunkLocalBounds.Num());
	}
	
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::ClassifyVoxelShapes);
		
		for (int32 VoxelIndex = 0; VoxelIndex < Centers.Num();++VoxelIndex)
		{
			const FVector WorldCenter = CompTransform.TransformPosition(Centers[VoxelIndex]);
			const bool bActive = FVector::DotProduct(WorldCenter - Surface, SafeNormal) > 0.0f;
			
			// Even when the Blocking state is unchanged, an active Voxel must still be accumulated for the Overlap
			// approximation, so this runs before the early continue below.
			if (bAccumulateChunks && bActive)
			{
				OutActiveChunkBounds[VoxelData->VoxelChunkIndices[VoxelIndex]] += FBox(WorldCenter - WorldVoxelExtent, WorldCenter + WorldVoxelExtent);
			}
			
			if (InOutState[VoxelIndex] == static_cast<uint8>(bActive)) continue;
		
			// 숨은 shape만 끄고, 다시 보이는 shape는 component의 원리 query, physics 방식 사용
			Body->SetShapeCollisionEnabled(VoxelIndex, bActive ? ActiveCollision  : ECollisionEnabled::NoCollision, false);
		
			InOutState[VoxelIndex] = static_cast<uint8>(bActive);
			bFilterChanged = true;
		}
	}
	
	if (bFilterChanged)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::UpdatePhysicsFilterData);

		Body->UpdatePhysicsFilterData();
	}
}
