// Copyright 2026 Team Beaver. All Rights Reserved.


#include "Voxel/WPVoxelData.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"

bool UWPVoxelData::IsValidFor(const UStaticMesh* Mesh) const
{
	const UBodySetup* SourceBody = IsValid(Mesh) ? Mesh->GetBodySetup() : nullptr;
	
	return IsValid(Mesh) 
		&& IsValid(SourceBody) 
		&& SourceMesh == Mesh
		&& SourceShapeClass == nullptr
		&& SourceBodyGuid == SourceBody->BodySetupGuid
		&& HasValidVoxelBody();
}

bool UWPVoxelData::IsValidFor(const UPrimitiveComponent* Primitive) const
{
	if (!IsValid(Primitive)) return false;
	
	if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Primitive))
	{
		if (SMC->IsA<UInstancedStaticMeshComponent>()) return false;
		
		return IsValidFor(SMC->GetStaticMesh());
	}
	
	UClass* ShapeClass = nullptr;
	FVector ShapeSize = FVector::ZeroVector;
	if (!GetShapeSignature(Primitive, OUT ShapeClass, OUT ShapeSize)) return false;
	
	return SourceMesh == nullptr && SourceShapeClass.Get() == ShapeClass && SourceShapeSize == ShapeSize && HasValidVoxelBody();
}

bool UWPVoxelData::MatchesBake(const UPrimitiveComponent* Primitive, const FString& InBakeKey, const FString& InBakeResultHash) const
{
	return !InBakeKey.IsEmpty() && !InBakeResultHash.IsEmpty() && BakeKey == InBakeKey && BakeResultHash == InBakeResultHash && IsValidFor(Primitive);
}

bool UWPVoxelData::HasValidVoxelBody() const
{
	const UBodySetup* VoxelBody = VoxelBodySetup.Get();
	return VoxelSize > KINDA_SMALL_NUMBER
			&& !VoxelCenters.IsEmpty()
			&& IsValid(VoxelBody)
			&& VoxelBody->CollisionTraceFlag == CTF_UseSimpleAsComplex
			&& VoxelBody->AggGeom.BoxElems.Num() == VoxelCenters.Num()
			&& VoxelBody->AggGeom.GetElementCount() == VoxelCenters.Num();
}

bool UWPVoxelData::GetShapeSignature(const UPrimitiveComponent* Primitive, UClass*& OutShapeClass, FVector& OutShapeSize)
{
	OutShapeClass = nullptr;
	OutShapeSize = FVector::ZeroVector;
	
	if (const UBoxComponent* Box = Cast<UBoxComponent>(Primitive))
	{
		OutShapeClass = UBoxComponent::StaticClass();
		OutShapeSize = Box->GetUnscaledBoxExtent();
		return true;
	}
	
	if (const USphereComponent* Sphere = Cast<USphereComponent>(Primitive))
	{
		OutShapeClass = USphereComponent::StaticClass();
		OutShapeSize.X = Sphere->GetUnscaledSphereRadius();
		return true;
	}
	
	if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Primitive))
	{
		OutShapeClass = UCapsuleComponent::StaticClass();
		OutShapeSize.X = Capsule->GetUnscaledCapsuleRadius();
		OutShapeSize.Y = Capsule->GetUnscaledCapsuleHalfHeight();
		return true;
	}
	
	return false;
}
