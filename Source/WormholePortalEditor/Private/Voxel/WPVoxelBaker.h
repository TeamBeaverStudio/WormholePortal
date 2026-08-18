// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBodySetup;
class UStaticMesh;
class UWPVoxelData;
class UWPTransitComponent;
class UPrimitiveComponent;

/**
 * @brief Bakes Simple Collision from supported Primitive Components into Voxel Data Assets.
 *
 * Voxel Data Assets follow these policies:
 * - Reuse a valid asset when its bake inputs and result match.
 * - When the result differs or an existing asset is invalid, create a new asset without
 *   modifying or deleting the existing one.
 * - When a name is already in use, generate a unique name to protect the existing asset.
 * - Change the Transit Component's references only after every source has been processed
 *   and all new assets have been saved.
 * - On failure, discard unsaved assets created by the current operation while
 *   preserving existing Component references and existing assets.
 *
 * Supported sources are Static Mesh, Box, Sphere, and Capsule Components.
 * The Runtime Module consumes UWPVoxelData and its embedded collision-only BodySetup.
 */
class FWPVoxelBaker
{
public:
	/**
 	 * @brief Bakes Simple Collision from supported Primitive Components associated with a Transit Component.
 	 * @param TransitComp Transit Component that receives the baked Data Asset references.
 	 * @param OutMessage Message describing the operation result.
 	 * @return true when every supported Primitive is baked or reused, all new assets are saved,
 	 *         and the Component references are updated.
 	 */
	static bool Bake(UWPTransitComponent* TransitComp, OUT FText& OutMessage);
	
private:
	/**
	 * @brief Current version of the voxel-generation rules.
	 *
	 * Important: Increment this value whenever BuildVoxels() behavior or the meaning of
	 * persisted data changes.
	 */
	static constexpr int32 CurrentBakeVersion = 2;

	/**
	 * @brief Collects unique Primitives from collision-enabled UStaticMeshComponents
	 *        associated with a Transit Component.
	 * @param TransitComp Transit Component from which bake candidates are discovered.
	 * @param OutPrimitives Collected unique Primitives.
	 */
	static void GatherPrimitives(UWPTransitComponent* TransitComp, OUT TArray<UPrimitiveComponent*>& OutPrimitives);

	/**
 	 * @brief Removes unsaved Voxel Data Assets created by the current Bake operation.
 	 *
 	 * Assets already written to disk are preserved so a partial save failure never
 	 * deletes user data.
 	 *
 	 * @param CreatedData Voxel Data Assets created by the current Bake operation.
 	 */
	static void RollbackData(const TArray<UWPVoxelData*>& CreatedData);
	
	/**
	 * @brief Reuses or creates the Voxel Data Asset for one supported source Primitive.
	 * @param SourcePrimitive Source Primitive to bake.
	 * @param GeneratedRootPath Validated project package path for generated Voxel Assets.
	 * @param RequestedSize Requested voxel side length.
 	 * @param MaxVoxelCount Maximum number of voxels that may be generated.
 	 * @param OutCreatedData Data Assets created by the current Bake operation.
 	 * @param OutMessage Message describing the operation result.
 	 * @return Reused or newly created UWPVoxelData, or nullptr on failure.
 	 */
	static UWPVoxelData* BakePrimitive(UPrimitiveComponent* SourcePrimitive, const FString& GeneratedRootPath,
		float RequestedSize, int32 MaxVoxelCount,
		OUT TArray<UWPVoxelData*>& OutCreatedData, OUT FText& OutMessage);
	
	static UBodySetup* CreateVoxelBodySetup(UWPVoxelData* Data, const UBodySetup* SourceBodySetup, const TArray<FVector>& Centers, float VoxelSize);

	/**
	 * @brief Computes voxel centers that fill Simple Collision and a final size that
	 *        satisfies the count limit.
	 * @param SourceBodySetup Simple Collision BodySetup used for the calculation.
	 * @param RequestedSize Requested voxel side length.
	 * @param MaxVoxelCount Maximum number of voxels that may be generated.
	 * @param OutCenters Computed voxel centers.
	 * @param OutVoxelSize Final voxel size adjusted to satisfy MaxVoxelCount.
	 * @param OutMessage Message describing the operation result.
	 * @return true when valid voxel centers and a final size are computed.
	 */
	static bool BuildVoxels(UBodySetup* SourceBodySetup, float RequestedSize, int32 MaxVoxelCount, OUT TArray<FVector>& OutCenters, OUT float& OutVoxelSize, OUT FText& OutMessage);
	
	/**
	 * @brief Retrieves the content hash of the saved source Static Mesh package.
	 * @param SourceMesh Source Static Mesh.
	 * @param OutSourceAssetHash Content hash of the source package.
	 * @param OutMessage Message describing the failure reason.
	 * @return true when the content hash is retrieved.
	 */
	static bool TryGetSourceAssetHash(const UStaticMesh* SourceMesh, OUT FString& OutSourceAssetHash, OUT FText& OutMessage);

	/**
	 * @brief Builds a result hash from the final voxel size and center array.
	 * @param Centers Computed voxel centers.
	 * @param FinalVoxelSize Computed final voxel size.
	 * @return Hash identifying the voxel result.
	 */
	static FString MakeResultHash(const TArray<FVector>& Centers, float FinalVoxelSize);

	/**
 	 * @brief Builds a Bake Key from the source identity, Bake settings, version, and result hash.
 	 * @param SourcePrimitive Source Primitive whose current configuration identifies the Bake source.
 	 * @param SourceAssetHash Content hash used only when the source is a Static Mesh.
 	 * @param RequestedSize Requested voxel size.
 	 * @param MaxVoxelCount Maximum number of voxels that may be generated.
 	 * @param BakeResultHash Hash of the computed voxel result.
 	 * @return Identifier used to find an equivalent Bake result.
 	 */
	static FString MakeKey(const UPrimitiveComponent* SourcePrimitive, const FString& SourceAssetHash, float RequestedSize, 
		int32 MaxVoxelCount, const FString& BakeResultHash);

	/**
	 * @brief Finds an existing reusable Voxel Data Asset matching the source Primitive and Bake result.
	 * @param SourcePrimitive Source Primitive to match.
	 * @param GeneratedRootPath Validated project package path to search recursively.
	 * @param BakeKey Bake Key to search for.
 	 * @param BakeResultHash Voxel-result hash to match.
 	 * @return Reusable UWPVoxelData, or nullptr when none is available.
 	 */
	static UWPVoxelData* FindReusableData(UPrimitiveComponent* SourcePrimitive, const FString& GeneratedRootPath,
		const FString& BakeKey, const FString& BakeResultHash);

	/**
 	 * @brief Generates a nonconflicting object and package name for the Voxel Data Asset.
 	 * @param RootPath Path in which the generated Asset will be stored.
 	 * @param BaseDataName Base name for the Voxel Data Asset.
 	 * @param OutDataName Object name to use for the Voxel Data Asset.
 	 * @param OutDataPackageName Package name to use for the Voxel Data Asset.
 	 */
	static void MakeUniqueDataAssetNames(const FString& RootPath, const FString& BaseDataName, FString& OutDataName, FString& OutDataPackageName);
};
