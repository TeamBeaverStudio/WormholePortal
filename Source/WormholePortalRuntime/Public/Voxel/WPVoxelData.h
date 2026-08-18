// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WPVoxelData.generated.h"

class UBodySetup;
class UStaticMesh;
class FWPVoxelBaker;
class UPrimitiveComponent;
/**
 * @brief Shared Asset containing a Static Mesh's Simple Collision baked into Voxel Shapes.
 *
 * Created in the Editor. Runtime uses the generated Voxel centers and Voxel BodySetup.
 * The Asset also stores information required to compare an existing Asset with the current Bake result.
 *
 * To keep older Voxel Assets usable at Runtime, IsValidFor() validates only Runtime data, not Bake metadata.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPVoxelData : public UDataAsset
{
	GENERATED_BODY()
	
public:
	// Original Static Mesh used as Bake input. Null for Shape Component data.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Voxel")
	TObjectPtr<UStaticMesh> SourceMesh;
	
	// Local-space half extent of one Voxel.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Voxel")
	FVector VoxelExtent = FVector::ZeroVector;
	
	// Final Voxel edge length adjusted to satisfy the Max Voxel Count limit.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Voxel")
	float VoxelSize = 0.f;
	
	// ======================================================================================
	// The fields below are stored only for internal Asset management and are not exposed to
	// Details or Blueprint.
	// ======================================================================================
	
	// Collision-only Voxel BodySetup owned by this Data Asset.
	UPROPERTY(Instanced)
	TObjectPtr<UBodySetup> VoxelBodySetup;
	
	// Center of each Voxel Box Shape in the original Mesh's local space.
	UPROPERTY()
	TArray<FVector> VoxelCenters;
	
	// Static Mesh BodySetup change identifier at Bake time.
	UPROPERTY()
	FGuid SourceBodyGuid;
	
	// Supported Shape Class used as Bake input
	UPROPERTY()
	TObjectPtr<UClass> SourceShapeClass;
	
	// Unscaled Shape size used as Bake input.
	// Box stores Extent XYZ, Sphere stores Radius X, and Capsule stores Radius X and Half Height Y.
	UPROPERTY()
	FVector SourceShapeSize = FVector::ZeroVector;
	
	// Requested Voxel edge length used for the Bake.
	UPROPERTY()
	float RequestedVoxelSize = 0.f;
	
	// Maximum requested Voxel count per Static Mesh for the Bake.
	UPROPERTY()
	int32 RequestedMaxVoxelCount = 0;
	
	// Content hash of the original Static Mesh Package at Bake time.
	UPROPERTY()
	FString SourceAssetHash;
	
	// Computation-rule version of the Voxel Baker that created the Asset.
	UPROPERTY()
	int32 BakerVersion = 0;
	
	// Bake-result hash computed from the final Voxel size and center array.
	UPROPERTY()
	FString BakeResultHash;
	
	// Identifier used to find an identical Bake result in the Asset Registry.
	/**
	 * AssetRegistrySearchable allows this value to be queried without loading the Asset into memory.
	 * Saving the Asset with this option enabled records the lookup data in the Asset Registry.
	 * The Baker can therefore find an Asset with the same BakeKey without loading every UWPVoxelData.
	 */
	UPROPERTY(AssetRegistrySearchable)
	FString BakeKey;

	/**
	 * @brief Tests whether this Voxel Data Asset can be used at Runtime with the specified original Static Mesh.
	 * @param Mesh Original Static Mesh to validate.
	 * @return True if the Runtime Voxel data is valid.
	 */
	bool IsValidFor(const UStaticMesh* Mesh) const;
	
	/**
 	 * @brief Tests whether this Voxel Data can be used with the specified Primitive.
 	 * @param Primitive Static Mesh, Box, Sphere, or Capsule Component to validate.
 	 * @return True if the source identity and Runtime Voxel data are valid.
 	 */
	bool IsValidFor(const UPrimitiveComponent* Primitive) const;

	/**
 	 * @brief Tests whether an existing Asset exactly matches the current Primitive Bake result.
 	 * @param Primitive Primitive used as Bake input.
 	 * @param InBakeKey Identifier computed from the current Bake conditions.
 	 * @param InBakeResultHash Hash computed from the current Voxel operation result.
 	 * @return True if the existing Asset can be reused.
 	 */
	bool MatchesBake(const UPrimitiveComponent* Primitive, const FString& InBakeKey, const FString& InBakeResultHash) const;
	
private:
	friend class FWPVoxelBaker;
	
	// Tests whether the stored collision-only Voxel Body is usable.
	bool HasValidVoxelBody() const;

	// Extracts the supported Shape category and unscaled size from a Primitive.
	static bool GetShapeSignature(const UPrimitiveComponent* Primitive, UClass*& OutShapeClass, FVector& OutShapeSize);
};
