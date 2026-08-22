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
#include "WPLog.h"
#include "WPSettings.h"
#include "Transit/WPTransitComponent.h"

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
		
		const int32 VoxelCount = Pair.VoxelData->VoxelCenters.Num();
		
		// Master + Twin Voxel Count
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::UpdateMaster);
			UpdatePrimitive(Pair.Master, VoxelData->VoxelCenters, SourceSurface, SourceNormal, Pair.MasterState);
		}
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::UpdateTwin);
			UpdatePrimitive(Pair.Twin, VoxelData->VoxelCenters, DestSurface, DestNormal, Pair.TwinState);
		}
	}
}

void FWPVoxel::DrawDebug(UWorld* World, const FWPTransitRun& Run)
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
	Twin->SetGenerateOverlapEvents(Master->GetGenerateOverlapEvents());
	
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
		Twin->SetCollisionResponseToChannel(PortalCollisionChannel, Pair.OriginMasterPCR);
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
	Pair.bUsingVoxelBody = false;
}

void FWPVoxel::UpdatePrimitive(UPrimitiveComponent* Comp, const TArray<FVector>& Centers, const FVector& Surface, const FVector& Normal, TArray<uint8>& InOutState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FWPVoxel::UpdatePrimitive);
	
	FBodyInstance* Body = IsValid(Comp) ? Comp->GetBodyInstance() : nullptr;
	if (Body == nullptr || !Body->IsValidBodyInstance() || Centers.Num() != InOutState.Num()) return;
	
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero()) return;
	
	const FTransform CompTransform = Comp->GetComponentTransform();
	const ECollisionEnabled::Type ActiveCollision = Comp->GetCollisionEnabled();
	bool bFilterChanged = false;
	
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(WPVoxel::ClassifyVoxelShapes);
		
		for (int32 VoxelIndex = 0; VoxelIndex < Centers.Num();++VoxelIndex)
		{
			const FVector WorldCenter = CompTransform.TransformPosition(Centers[VoxelIndex]);
			const bool bActive = FVector::DotProduct(WorldCenter - Surface, SafeNormal) > 0.0f;
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
