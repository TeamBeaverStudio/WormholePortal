// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPVoxelBaker.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "IO/IoHash.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "ObjectTools.h"

#include "Voxel/WPVoxelData.h"
#include "WPSettings.h"
#include "Transit/WPTransitComponent.h"

#define LOCTEXT_NAMESPACE "WPVoxelBaker"

bool FWPVoxelBaker::Bake(UWPTransitComponent* TransitComp, FText& OutMessage)
{
	if (!IsValid(TransitComp))
	{
		OutMessage = LOCTEXT("InvalidComponent", "Transit Component is invalid");
		return false;
	}

	const UWPSettings* Settings = GetDefault<UWPSettings>();
	FString GeneratedRootPath;
	FString PathError;
	if (!IsValid(Settings)
		|| !Settings->TryResolveGeneratedVoxelPath(GeneratedRootPath, PathError))
	{
		if (PathError.IsEmpty())
		{
			PathError = TEXT("Wormhole Portal settings are unavailable.");
		}
		OutMessage = FText::FromString(PathError);
		return false;
	}
	
	if (TransitComp->VoxelSize <= KINDA_SMALL_NUMBER || TransitComp->MaxVoxelCount < 1 || TransitComp->MaxVoxelCount > 512)
	{
		OutMessage = LOCTEXT("Invalid Settings", "Voxel Size is bigger than 0, MaxVoxel Count range is 1 to 512");
		return false;
	}
	
	TArray<UPrimitiveComponent*> SourcePrimitives;
	GatherPrimitives(TransitComp, OUT SourcePrimitives);
	if (SourcePrimitives.IsEmpty())
	{
		OutMessage = LOCTEXT("NoMesh", "No Bake Primitive");
		return false;
	}
	
	TArray<UWPVoxelData*> BakedData;
	TArray<UWPVoxelData*> CreatedData;
	int32 TotalVoxelCount = 0;
	
	for (UPrimitiveComponent* SourcePrimitive : SourcePrimitives)
	{
		const bool bAlreadyBaked = BakedData.ContainsByPredicate([SourcePrimitive](const UWPVoxelData* Data)
		{
			return IsValid(Data) && Data->IsValidFor(SourcePrimitive);
		});
		if (bAlreadyBaked) continue;
		
		FText PrimitiveMessage;
		UWPVoxelData* Data = BakePrimitive(
			SourcePrimitive,
			GeneratedRootPath,
			TransitComp->VoxelSize,
			TransitComp->MaxVoxelCount,
			OUT CreatedData,
			OUT PrimitiveMessage);
		if (!IsValid(Data))
		{
			OutMessage = FText::Format(LOCTEXT("Primitive Bake Failed", "{0} : {1}"), FText::FromString(GetNameSafe(SourcePrimitive)), PrimitiveMessage);
			RollbackData(CreatedData);
			return false;
		}
		BakedData.Add(Data);
		TotalVoxelCount += Data->VoxelCenters.Num();
	}
	
	TArray<UPackage*> CreatedPackages;
	CreatedPackages.Reserve(CreatedData.Num());
	
	for (UWPVoxelData* Data : CreatedData)
	{
		UPackage* Package = IsValid(Data) ? Data->GetOutermost() : nullptr;
		if (IsValid(Package))
		{
			CreatedPackages.AddUnique(Package);
		}
	}
	
	if (!CreatedPackages.IsEmpty())
	{
		TArray<UPackage*> FailedPackages;
		const FEditorFileUtils::EPromptReturnCode SaveResult = FEditorFileUtils::PromptForCheckoutAndSave(
			CreatedPackages,
			false,
			false,
			&FailedPackages,
			false,
			false);
		if (SaveResult != FEditorFileUtils::PR_Success)
		{
			// 저장에 성공한 Package는 유지하고, 디스크에 기록되지 않은 새 Asset만 제거합니다.
			RollbackData(CreatedData);
			
			OutMessage = LOCTEXT("SaveFailed", "Failed to Save Voxel Asset");
			return false;
		}
	}
	
	TransitComp->Modify();
	TransitComp->VoxelData.Reset(BakedData.Num());
	
	for (UWPVoxelData* Data : BakedData)
	{
		TransitComp->VoxelData.Add(Data);
	}
	
	TransitComp->MarkPackageDirty();
	TransitComp->PostEditChange();
	
	if (CreatedPackages.IsEmpty())
	{
		OutMessage = LOCTEXT("BakeReuseComplete", "Matching baked voxel data already exists and has been assigned to the Transit Component.");
	}
	else
	{
		OutMessage = FText::Format(LOCTEXT("BakeComplete", "Voxel bake complete. The resulting data was assigned to the Transit Component. Voxel Data asset count: {0}; voxel count: {1}."),
			FText::AsNumber(BakedData.Num()), FText::AsNumber(TotalVoxelCount));
	}

	return true;
}

void FWPVoxelBaker::GatherPrimitives(UWPTransitComponent* TransitComp, OUT TArray<UPrimitiveComponent*>& OutPrimitives)
{
	OutPrimitives.Reset();
	if (!IsValid(TransitComp))
	{
		return;
	}
	
	TArray<UPrimitiveComponent*> Primitives;
	for(UPrimitiveComponent* Primitive : TransitComp->TransitPrimitives)
	{
		if (IsValid(Primitive))
		{
			Primitives.AddUnique(Primitive);
		}
	}
	
	if (Primitives.IsEmpty())
	{
		// Level에 생성된 Component일 경우
		if (AActor* Owner = TransitComp->GetOwner())
		{
			Owner->GetComponents<UPrimitiveComponent>(OUT Primitives);
		}
		else // Blueprint Editor에 전달된 SCS Component Template의 경우
		{
			UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(TransitComp->GetOuter());
			USimpleConstructionScript* ConstructionScript = IsValid(BlueprintClass) ? BlueprintClass->SimpleConstructionScript.Get() : nullptr;
			if (IsValid(ConstructionScript))
			{
				for (USCS_Node* Node : ConstructionScript->GetAllNodes())
				{
					UPrimitiveComponent* Primitive = IsValid(Node) ? Cast<UPrimitiveComponent>(Node->ComponentTemplate) : nullptr;

					if (IsValid(Primitive))
					{
						Primitives.AddUnique(Primitive);
					}
				}
			}
		}
		
		
	}
	
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		if (!IsValid(Primitive) || Primitive->GetCollisionEnabled() == ECollisionEnabled::NoCollision) continue;
		
		if(UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Primitive))
		{
			if (!SMC->IsA<UInstancedStaticMeshComponent>() && IsValid(SMC->GetStaticMesh()))
			{
				OutPrimitives.AddUnique(SMC);
			}
			continue;
		}
		
		if (Primitive->IsA<UBoxComponent>() || Primitive->IsA<USphereComponent>() || Primitive->IsA<UCapsuleComponent>())
		{
			OutPrimitives.AddUnique(Primitive);
		}
	}
}

void FWPVoxelBaker::RollbackData(const TArray<UWPVoxelData*>& CreatedData)
{
	TArray<UObject*> DataToDelete;
	DataToDelete.Reserve(CreatedData.Num());
	
	for (UWPVoxelData* Data : CreatedData)
	{
		UPackage* Package = IsValid(Data) ? Data->GetOutermost() : nullptr;
		if (!IsValid(Package) || FPackageName::DoesPackageExist(Package->GetName()))
		{
			continue;
		}
		
		DataToDelete.Add(Data);
	}
	
	if (!DataToDelete.IsEmpty())
	{
		ObjectTools::DeleteObjectsUnchecked(DataToDelete);
	}
}

UWPVoxelData* FWPVoxelBaker::BakePrimitive(
	UPrimitiveComponent* SourcePrimitive,
	const FString& GeneratedRootPath,
	float RequestedSize,
	int32 MaxVoxelCount,
	TArray<UWPVoxelData*>& OutCreatedData,
	FText& OutMessage)
{
	if (!IsValid(SourcePrimitive))
	{
		OutMessage = LOCTEXT("InvalidSourcePrimitive", "Source Primitive is invalid");
		return nullptr;
	}
	UStaticMesh* SourceMesh = nullptr;
	UClass* SourceShapeClass = nullptr;
	FVector SourceShapeSize = FVector::ZeroVector;
	UBodySetup* SourceBodySetup = nullptr;
	FString SourceAssetHash;
	FString SourceName;
	
	if (const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(SourcePrimitive))
	{
		if (StaticMesh->IsA<UInstancedStaticMeshComponent>())
		{
			OutMessage = LOCTEXT("UnsupportedInstancedMesh", "Instanced Static Mesh is not supported");
			return nullptr;
		}
		
		SourceMesh = StaticMesh->GetStaticMesh();
		SourceBodySetup = IsValid(SourceMesh) ? SourceMesh->GetBodySetup() : nullptr;
		SourceName = GetNameSafe(SourceMesh);
		
		if (!IsValid(SourceMesh) || !IsValid(SourceBodySetup))
		{
			OutMessage = LOCTEXT("NoStaticMeshBodySetup", "Source Static Mesh has no BodySetup");
			return nullptr;
		}
		
		if (!TryGetSourceAssetHash(SourceMesh, OUT SourceAssetHash, OUT OutMessage))
		{
			return nullptr;
		}
	}
	else
	{
		UShapeComponent* Shape = Cast<UShapeComponent>(SourcePrimitive);
		if (!IsValid(Shape) || !UWPVoxelData::GetShapeSignature(SourcePrimitive, OUT SourceShapeClass, OUT SourceShapeSize))
		{
			OutMessage = LOCTEXT("UnsupportedPrimitive", "Source Primitive type is not supported");
			return nullptr;
		}
		
		SourceBodySetup = Shape->GetBodySetup();
		SourceName = GetNameSafe(SourceShapeClass);
		
		if (!IsValid(SourceBodySetup))
		{
			OutMessage = LOCTEXT("NoShapeBodySetup", "Source Shape has no BodySetup");
			return nullptr;
		}
	}
	
	TArray<FVector> Centers;
	float FinalVoxelSize = 0.f;
	if (!BuildVoxels(SourceBodySetup, RequestedSize, MaxVoxelCount, OUT Centers, OUT FinalVoxelSize, OUT OutMessage))
	{
		return nullptr;
	}
	
	const FString BakeResultHash = MakeResultHash(Centers, FinalVoxelSize);
	const FString BakeKey = MakeKey(SourcePrimitive, SourceAssetHash, RequestedSize, MaxVoxelCount, BakeResultHash);
	
	if (BakeKey.IsEmpty())
	{
		OutMessage = LOCTEXT("InvalidBakeKey", "Failed to create the Bake Key");
		return nullptr;
	}
	
	if (UWPVoxelData* ReusableData = FindReusableData(
		SourcePrimitive,
		GeneratedRootPath,
		BakeKey,
		BakeResultHash))
	{
		return ReusableData;
	}
	
	const FString SafeSourceName = ObjectTools::SanitizeObjectName(SourceName);
	const FString ShortKey = BakeKey.Left(16);
	const FString BaseDataName = FString::Printf(TEXT("Voxel_DataAsset_%s_%s"), *SafeSourceName, *ShortKey);
	
	FString DataName;
	FString DataPackageName;
	MakeUniqueDataAssetNames(GeneratedRootPath, BaseDataName, OUT DataName, OUT DataPackageName);
	
	UPackage* DataPackage = CreatePackage(*DataPackageName);
	if (!IsValid(DataPackage) || FindObject<UObject>(DataPackage, *DataName) != nullptr)
	{
		OutMessage = LOCTEXT("DataPackageFailed", "Can't make Voxel Data Asset Package");
		return nullptr;
	}
	
	UWPVoxelData* Data = NewObject<UWPVoxelData>(DataPackage, UWPVoxelData::StaticClass(), *DataName,
		RF_Public | RF_Standalone | RF_Transactional);
	
	if (!IsValid(Data))
	{
		OutMessage = LOCTEXT("DataAssetFailed", "Failed to Create Voxel Data Asset");
		return nullptr;
	}
	
	Data->VoxelBodySetup = CreateVoxelBodySetup(Data, SourceBodySetup, Centers, FinalVoxelSize);
	
	if (!IsValid(Data->VoxelBodySetup))
	{
		OutMessage = LOCTEXT("VoxelBodySetupFailed", "Failed to create the collision-only Voxel BodySetup");
		return nullptr;
	}
	
	Data->SourceMesh = SourceMesh;
	Data->SourceBodyGuid = IsValid(SourceMesh) ? SourceBodySetup->BodySetupGuid : FGuid();
	Data->SourceShapeClass = SourceShapeClass;
	Data->SourceShapeSize = SourceShapeSize;
	
	Data->VoxelCenters = MoveTemp(Centers);
	Data->VoxelExtent = FVector(FinalVoxelSize * 0.5f);
	Data->VoxelSize = FinalVoxelSize;
	Data->RequestedVoxelSize = RequestedSize;
	Data->RequestedMaxVoxelCount = MaxVoxelCount;
	Data->SourceAssetHash = SourceAssetHash;
	Data->BakerVersion = CurrentBakeVersion;
	Data->BakeResultHash = BakeResultHash;
	Data->BakeKey = BakeKey;
	
	Data->MarkPackageDirty();
	Data->PostEditChange();
	
	FAssetRegistryModule::AssetCreated(Data);
	OutCreatedData.Add(Data);
	
	return Data;
}

UBodySetup* FWPVoxelBaker::CreateVoxelBodySetup(UWPVoxelData* Data, const UBodySetup* SourceBodySetup,
	const TArray<FVector>& Centers, float VoxelSize)
{
	if (!IsValid(Data) || !IsValid(SourceBodySetup) || Centers.IsEmpty() || VoxelSize <= KINDA_SMALL_NUMBER)
	{
		return nullptr;
	}
	
	UBodySetup* VoxelBodySetup = NewObject<UBodySetup>(Data, TEXT("VoxelBodySetup"), RF_Transactional);
	if (!IsValid(VoxelBodySetup))
	{
		return nullptr;
	}
	
	VoxelBodySetup->CopyBodySetupProperty(SourceBodySetup);
	VoxelBodySetup->bSharedCookedData = false;
	VoxelBodySetup->bNeverNeedsCookedCollisionData = true;
	VoxelBodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	VoxelBodySetup->AggGeom.EmptyElements();
	VoxelBodySetup->AggGeom.BoxElems.Reserve(Centers.Num());
	
	for (const FVector& Center : Centers)
	{
		FKBoxElem& Box = VoxelBodySetup->AggGeom.BoxElems.AddDefaulted_GetRef();
		Box.Center = Center;
		Box.X = Box.Y = Box.Z = VoxelSize;
		Box.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box.SetContributeToMass(true);
	}
	
	VoxelBodySetup->InvalidatePhysicsData();
	const bool bValidGeometry = VoxelBodySetup->AggGeom.BoxElems.Num() == Centers.Num() 
	&& VoxelBodySetup->AggGeom.GetElementCount() == Centers.Num();
	
	return bValidGeometry ? VoxelBodySetup : nullptr;
}

bool FWPVoxelBaker::BuildVoxels(UBodySetup* SourceBodySetup, float RequestedSize, int32 MaxVoxelCount,
                                TArray<FVector>& OutCenters, float& OutVoxelSize, FText& OutMessage)
{
	OutCenters.Reset();
	OutVoxelSize = 0.0f;
	
	if (!IsValid(SourceBodySetup))
	{
		OutMessage = LOCTEXT("InvalidBody", "Invalid Source Body Setup");
		return false;
	}

	const FKAggregateGeom& AggGeom = SourceBodySetup->AggGeom;
	const int32 SupportedShapeCount = AggGeom.SphereElems.Num() + AggGeom.BoxElems.Num() + AggGeom.SphylElems.Num() + AggGeom.ConvexElems.Num();
	if (SupportedShapeCount < 1)
	{
		OutMessage = LOCTEXT("NoSimpleCollision", "At least one supported Simple Collision shape is required: Sphere, Box, Capsule, or Convex.");
		return false;
	}

	if (!AggGeom.TaperedCapsuleElems.IsEmpty() || !AggGeom.LevelSetElems.IsEmpty() || !AggGeom.SkinnedLevelSetElems.IsEmpty()
		|| !AggGeom.MLLevelSetElems.IsEmpty() || !AggGeom.SkinnedTriangleMeshElems.IsEmpty())
	{
		OutMessage = LOCTEXT( "UnsupportedCollision", "Voxel baking supports only these Simple Collision shapes: Sphere, Box, Capsule, and Convex.");
		return false;
	}

	if (!SourceBodySetup->bCreatedPhysicsMeshes)
	{
		SourceBodySetup->CreatePhysicsMeshes();
	}
	for (const FKConvexElem& Convex : AggGeom.ConvexElems)
	{
		if (!Convex.GetChaosConvexMesh())
		{
			OutMessage = LOCTEXT( "ConvexCookFailed", "Failed to build the physics mesh for the source Convex Collision.");
			return false;
		}
	}

	const FBox Bounds = AggGeom.CalcAABB(FTransform::Identity);
	if (!Bounds.IsValid || Bounds.GetSize().IsNearlyZero())
	{
		OutMessage = LOCTEXT("InvalidBounds", "The Simple Collision bounds are invalid.");
		return false;
	}

	float VoxelSize = FMath::Max(RequestedSize, 1.0f);
	const int64 MaxSampleCount = FMath::Max<int64>( static_cast<int64>(MaxVoxelCount) * 64, MaxVoxelCount);

	for (int32 Attempt = 0; Attempt < 12; ++Attempt)
	{
		const FVector BoundsSize = Bounds.GetSize();
		const FIntVector GridSize(
			FMath::Max(1, FMath::CeilToInt(BoundsSize.X / VoxelSize)),
			FMath::Max(1, FMath::CeilToInt(BoundsSize.Y / VoxelSize)),
			FMath::Max(1, FMath::CeilToInt(BoundsSize.Z / VoxelSize)));
		const int64 SampleCount = static_cast<int64>(GridSize.X) * GridSize.Y * GridSize.Z;

		if (SampleCount > MaxSampleCount)
		{
			const double SizeScale = FMath::Pow(static_cast<double>(SampleCount) / MaxSampleCount, 1.0 / 3.0);
			VoxelSize *= static_cast<float>(SizeScale * 1.01);
			continue;
		}

		OutCenters.Reset();
		OutCenters.Reserve(FMath::Min<int64>(SampleCount, MaxVoxelCount + 1));
		const FVector GridExtent = FVector(GridSize) * VoxelSize;
		const FVector GridMin = Bounds.GetCenter() - GridExtent * 0.5f;

		int64 OccupiedCount = 0;
		for (int32 X = 0; X < GridSize.X; ++X)
		{
			for (int32 Y = 0; Y < GridSize.Y; ++Y)
			{
				for (int32 Z = 0; Z < GridSize.Z; ++Z)
				{
					const FVector Center = GridMin + FVector(X + 0.5f, Y + 0.5f, Z + 0.5f) * VoxelSize;
					if (SourceBodySetup->GetShortestDistanceToPoint(Center, FTransform::Identity, true) > UE_SMALL_NUMBER)
					{
						continue;
					}

					++OccupiedCount;
					if (OutCenters.Num() < MaxVoxelCount)
					{
						OutCenters.Add(Center);
					}
				}
			}
		}

		if (OccupiedCount > 0 && OccupiedCount <= MaxVoxelCount)
		{
			OutVoxelSize = VoxelSize;
			return true;
		}

		if (OccupiedCount == 0)
		{
			OutMessage = LOCTEXT("NoOccupiedVoxel", "No voxel centers were generated inside the Simple Collision at the current Voxel Size. Reduce the Voxel Size and try again.");
			return false;
		}

		const double SizeScale = FMath::Pow(static_cast<double>(OccupiedCount) / MaxVoxelCount , 1.0 / 3.0);
		VoxelSize *= static_cast<float>(FMath::Max(SizeScale * 1.05, 1.05));
	}

	OutMessage = LOCTEXT( "VoxelLimitFailed", "Could not determine a Voxel Size within the Max Voxel Count limit.");
	return false;
}

bool FWPVoxelBaker::TryGetSourceAssetHash(const UStaticMesh* SourceMesh, FString& OutSourceAssetHash, FText& OutMessage)
{
	OutSourceAssetHash.Reset();
	
	if (!IsValid(SourceMesh))
	{
		OutMessage = LOCTEXT("InvalidSourceMesh", "The source Static Mesh is invalid.");
		return false;
	}
	
	// GetOutermost() : 해당 객체의 Outer 관계를 끝까지 따라가서 가장 바깥쪽 객체, 보통 그 객체를 담고 있는 UPackage를 반환
	UPackage* SourcePackage = SourceMesh->GetOutermost();
	if (!IsValid(SourcePackage))
	{
		OutMessage = LOCTEXT("InvalidSourcePackage", "The source Static Mesh package is invalid.");
		return false;
	}
	
	// 저장되지 않은 원본으로는 다시 확인할 수 있는 Asset 버전을 만들 수 없습니다.
	if (SourcePackage->IsDirty())
	{
		OutMessage = LOCTEXT("DirtySourcePackage", "Save the source Static Mesh, then try baking again.");
		return false;
	}
	
	const TOptional<FAssetPackageData> PackageData = FAssetRegistryModule::GetRegistry().GetAssetPackageDataCopy(SourcePackage->GetFName());
	
	// PackageData가 TOptional 타입일 때, 안에 실제 값이 들어 있는지 확인하는 코드
	if (!PackageData.IsSet())
	{
		OutMessage = LOCTEXT("MissingSourcePackageData", "Could not find saved package data for the source Static Mesh.");
		return false;
	}
	
	const FIoHash PackageHash = PackageData->GetPackageSavedHash();
	
	if (PackageHash.IsZero())
	{
		OutMessage = LOCTEXT( "MissingSourcePackageHash", "Could not find the content hash for the source Static Mesh.");
		return false;
	}
	
	OutSourceAssetHash = LexToString(PackageHash);
	return true;
}

FString FWPVoxelBaker::MakeResultHash(const TArray<FVector>& Centers, float FinalVoxelSize)
{
	// FIoHashBuilder: 여러 데이터를 차례대로 넣어서 하나의 해시값을 만드는 도구
	// Voxel Size, Voxel Count, 모든 Voxel 위치를 합친 결과를 대표하는 짧은 식별값 반환
	FIoHashBuilder HashBuilder;
	HashBuilder.Update(&FinalVoxelSize, sizeof(FinalVoxelSize));
	
	const int32 VoxelCount = Centers.Num();
	HashBuilder.Update(&VoxelCount, sizeof(VoxelCount));
	
	for (const FVector& Center : Centers)
	{
		const double Coordinates[3] = {Center.X, Center.Y, Center.Z};
		HashBuilder.Update(Coordinates, sizeof(Coordinates));
	}
	
	return LexToString(HashBuilder.Finalize());
}

FString FWPVoxelBaker::MakeKey(const UPrimitiveComponent* SourcePrimitive, const FString& SourceAssetHash, float RequestedSize,
	int32 MaxVoxelCount, const FString& BakeResultHash)
{
	FString SourceKey;
	if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(SourcePrimitive))
	{
		UStaticMesh* SM = SMC->GetStaticMesh();
		const UBodySetup* BodySetup = IsValid(SM) ? SM->GetBodySetup() : nullptr;
		
		if (SMC->IsA<UInstancedStaticMeshComponent>() || !IsValid(SM) || !IsValid(BodySetup) || SourceAssetHash.IsEmpty())
		{
			return FString();
		}
		
		SourceKey = FString::Printf(TEXT("%s|%s|%s"), *GetPathNameSafe(SM), *BodySetup->BodySetupGuid.ToString(EGuidFormats::Digits), *SourceAssetHash);
	}
	else
	{
		UClass* ShapeClass = nullptr;
		FVector ShapeSize = FVector::ZeroVector;
		if (!UWPVoxelData::GetShapeSignature(SourcePrimitive, OUT ShapeClass, OUT ShapeSize))
		{
			return FString();
		}
		
		SourceKey = FString::Printf(TEXT("%s|%s|%s|%s"), *GetPathNameSafe(ShapeClass),
			*LexToString(FMath::AsUInt(ShapeSize.X)), *LexToString(FMath::AsUInt(ShapeSize.Y)), *LexToString(FMath::AsUInt(ShapeSize.Z)));
	}
	
	const uint32 RequestedSizeBits = FMath::AsUInt(RequestedSize);
	const FString KeySource = FString::Printf(TEXT("%s|%08X|%d|%d|%s"),
		*SourceKey, RequestedSizeBits, MaxVoxelCount, CurrentBakeVersion, *BakeResultHash);
	
	const FTCHARToUTF8 Utf8Key(*KeySource);
	return LexToString(FIoHash::HashBuffer(Utf8Key.Get(), static_cast<uint64>(Utf8Key.Length())));
}

UWPVoxelData* FWPVoxelBaker::FindReusableData(
	UPrimitiveComponent* SourcePrimitive,
	const FString& GeneratedRootPath,
	const FString& BakeKey,
	const FString& BakeResultHash)
{
	if (!IsValid(SourcePrimitive) || BakeKey.IsEmpty() || BakeResultHash.IsEmpty()) return nullptr;
	
	// Asset Registry에서 어떤 에셋을 찾을지 조건을 지정하는 검색 필터
	// 아래 의미: UWPVoxelData Type의 지정한 Voxel 폴더 안에 있으며, BakeKey값이 이번에 찾는 값과 같은 에셋을 찾는다.
	FARFilter Filter;
	Filter.ClassPaths.Add(UWPVoxelData::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(GeneratedRootPath));
	Filter.TagsAndValues.Add(GET_MEMBER_NAME_CHECKED(UWPVoxelData, BakeKey),BakeKey);
	Filter.bRecursivePaths = true;
	
	TArray<FAssetData> FoundAssets;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, OUT FoundAssets);
	
	// 같은 결과가 여러 개라면 항상 같은 Asset을 고르도록 이름순으로 검사한다.
	FoundAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.LexicalLess(Right.PackageName);
		});
	
	for (const FAssetData& AssetData : FoundAssets)
	{
		UWPVoxelData* Data = Cast<UWPVoxelData>(AssetData.GetAsset());
		
		if (!IsValid(Data) || !Data->MatchesBake(SourcePrimitive, BakeKey, BakeResultHash))
		{
			continue;
		}
		
		UPackage* DataPackage = Data->GetOutermost();
		
		// 저장되지 않은 변경이 있는 기존 Asset은 재사용하지 않습니다.
		if (!IsValid(DataPackage) || DataPackage->IsDirty())
		{
			continue;
		}
		
		return Data;
	}
	
	return nullptr;
}

void FWPVoxelBaker::MakeUniqueDataAssetNames(const FString& RootPath, const FString& BaseDataName, FString& OutDataName, FString& OutDataPackageName)
{
	const auto IsPackageAvailable = [](const FString& PackageName)
	{
		return FindPackage(nullptr, *PackageName) == nullptr && !FPackageName::DoesPackageExist(PackageName);	
	};
	
	for (int32 Suffix = 0;;++Suffix)
	{
		const FString SuffixText = Suffix == 0 ? FString() : FString::Printf(TEXT("_%d"), Suffix);
		
		OutDataName = BaseDataName + SuffixText;
		OutDataPackageName = RootPath/OutDataName;
		if (IsPackageAvailable(OutDataPackageName))
		{
			return;
		}
	}
}

#undef LOCTEXT_NAMESPACE
