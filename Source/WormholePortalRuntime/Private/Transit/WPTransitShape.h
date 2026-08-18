// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UPrimitiveComponent;
class UCapsuleComponent;
class USphereComponent;

struct FWPTransitRun;

enum class EWPTransitCenter : uint8;
enum class EWPTransitPlane : uint8;
/**
 * @brief Performs shape tests required for Portal Transit.
 *
 * Tests only the geometry of the supplied Primitives.
 */
class FWPTransitShape final
{
public:
	// Returns whether the Primitives' approximate projected cross-sections fit within the Gate radius.
	static bool FitsGate(const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Parts, const FVector& GateCenter, const FVector& MoveDir, float GateRadius);
	
	// Computes the Master center at Transit start according to the configured CenterMode.
	static bool GetMasterCenter(const AActor* Master, const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Parts, EWPTransitCenter CenterMode, OUT FVector& OutCenter);

	/**
	 * @brief Selects the Transit reflection plane from the entry tangent plane and movement direction.
	 * @param SourceRotation World-space rotation of the Source Portal.
	 * @param TangentPlaneNormal World-space normal of the Source entry tangent plane.
	 * @param MoveDir World-space movement direction at Transit start.
	 * @param OutPlane Selected EWPTransitPlane.
	 * @return true if a valid plane was selected.
	 */
	static bool SelectPlane(const FQuat& SourceRotation, const FVector& TangentPlaneNormal, const FVector& MoveDir, OUT EWPTransitPlane& OutPlane);
	
	// Returns whether the entire Master shape has moved back outside the Source entry surface.
	static bool MasterOutside(const FWPTransitRun& Run, const FVector& Surface, const FVector& Normal);
	
	// Returns whether the entire Master collision shape has crossed inside the Source surface.
	static bool MasterInside(const FWPTransitRun& Run, const FVector& Surface, const FVector& Normal);

	/**
	 * @brief Tests whether one Primitive lies completely in the half-space pointed to by the surface Normal.
	 * @param Part Primitive to test.
	 * @param Surface World-space point on the test plane.
	 * @param Normal World-space direction pointing toward the required half-space.
	 * @return true if the entire Primitive lies outside the plane.
	 */
	static bool IsOutside(const UPrimitiveComponent* Part, const FVector& Surface, const FVector& Normal);
	
private:
	// Returns the eight World-space bounding-box corners that approximate the Primitive shape.
	static bool GetPoints(const UPrimitiveComponent* Part, OUT TArray<FVector>& OutPoints);
	
	// Transforms the Box corners and appends them to the World-space point array.
	static void AddPoints(const FBox& Bounds, const FTransform& Transform, OUT TArray<FVector>& OutPoints);
	
	// Tests whether the projected capsule axis segment plus its scaled radius fits within the Gate radius.
	static bool CapsuleFitsGate(const UCapsuleComponent* Capsule, const FVector& GateCenter, const FVector& MoveDir, float SourceRadius);
	
	// Tests whether the projected Sphere fits within the Gate radius.
	static bool SphereFitsGate(const USphereComponent* Sphere, const FVector& GateCenter, const FVector& MoveDir, float SourceRadius);
	
	// Tests whether the entire Capsule lies in the half-space pointed to by the specified plane Normal.
	static bool IsCapsuleOutside(const UCapsuleComponent* Capsule, const FVector& Surface, const FVector& Normal);
	
	// Tests whether the entire Sphere lies in the required half-space.
	static bool IsSphereOutside(const USphereComponent* Sphere, const FVector& Surface, const FVector& Normal);
};
