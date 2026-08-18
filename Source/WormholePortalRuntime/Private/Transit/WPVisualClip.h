// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;

/**
 * @brief Writes the Sphere Clip values consumed by the Transit Material to a Primitive.
 *
 * Uses four consecutive Custom Primitive Data values with the following layout:
 * XYZ: FP32 World coordinates of the Portal Sphere center used to display the Primitive
 * W[0..7]: Low 8 bits containing Octahedral Normal X
 * W[8..15]: Next 8 bits containing Octahedral Normal Y
 * W[16..31]: FP16 Portal Radius
 *
 * A Packed Data value of zero disables Visual Clip.
 */
class FWPVisualClip final
{
public:
	/**
	 * @brief Displays only entry-side pixels on the Master that lie outside the Source Sphere.
	 * @param Part Primitive Component that receives the clip values.
	 * @param SourceCenter World-space center of the Source Portal Sphere.
	 * @param EntryNormal World-space direction from the Source Portal center toward the entry point.
	 * @param Radius Radius of the Source Portal Sphere.
	 */
	static void SetSource(UPrimitiveComponent* Part, const FVector& SourceCenter, const FVector& EntryNormal, float Radius);

	/**
	 * @brief Displays only exit-side pixels on the Twin that lie outside the Destination Sphere.
	 * @param Part Primitive Component that receives the clip values.
	 * @param DestCenter World-space center of the Destination Portal Sphere.
	 * @param ExitNormal World-space direction from the Destination Portal Sphere center toward the exit point.
	 * @param Radius Radius of the Destination Portal Sphere.
	 */
	static void SetTwin(UPrimitiveComponent* Part, const FVector& DestCenter, const FVector& ExitNormal, float Radius);

	/**
	 * @brief Packs a Normal and Radius into the uint32 format consumed by the Material.
	 * @param Normal World-space direction to pack.
	 * @param Radius Portal radius to store as FP16.
	 * @return Value containing the Octahedral Normal in the low 16 bits and the FP16 Radius in the high
	 * 16 bits, or zero if the input is invalid.
	 */
	static uint32 PackData(const FVector& Normal, float Radius);

	/**
	 * @brief Writes prepacked Visual Clip values to a Primitive.
	 * @param Part Primitive Component that receives the clip values.
	 * @param Center FP32 World-space center of the Portal Sphere.
	 * @param PackedData Packed Normal and Radius.
	 */
	static void SetPacked(UPrimitiveComponent* Part, const FVector3f& Center, uint32 PackedData);
	
	/**
	 * @brief Disables the Sphere Clip values on a Primitive.
	 * @param Part Primitive Component whose clip values will be cleared.
	 */
	static void Clear(UPrimitiveComponent* Part);

private:
	/**
	 * @brief Converts a unit direction vector to two-dimensional coordinates using octahedral encoding.
	 * @param Normal Unit direction vector to encode.
	 * @return Octahedral coordinates with each component in the range [-1, 1].
	 */
	static FVector2f EncodeNormal(const FVector& Normal);
	
	/** @brief Writes the Sphere Center, Normal, and Radius to CPD. */
	static void SetData(UPrimitiveComponent* Part, const FVector& Center, const FVector& Normal, float Radius);
	
	// Returns the base index for Visual Clip CPD from the project settings.
	static int32 GetDataIndex();
};
