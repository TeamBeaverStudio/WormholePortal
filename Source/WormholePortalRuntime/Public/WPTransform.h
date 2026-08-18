// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AWormholePortalActor;
enum class EWPTransitPlane : uint8;

/**
 * Provides coordinate transforms between a Source Portal and a Destination Portal.
 *
 * Lightweight data structure that precomputes and stores the reference Transform and rotation delta needed to 
 * map positions, directions, and rotations from the Source Portal to the Destination Portal.
 * Used by paths that repeatedly transform the same Source/Destination pair, including Actor traversal,
 * camera rendering, and Trace continuation.
 */
struct WORMHOLEPORTALRUNTIME_API FWPTransform
{
	/** Source Portal rotation used to transform a Source Portal Local plane into World space. */
	FQuat SourceRotation = FQuat::Identity;
	
	/** Rotation that maps Source-space direction vectors and rotations into Destination space. */
	FQuat TransportRotation = FQuat::Identity;
	
	/** World-space center of each Portal. */
	FVector SourceCenter = FVector::ZeroVector;
	FVector DestCenter = FVector::ZeroVector;
	
	/** Effective traversal radius of each Portal. */
	float SourceCoreRadius = 0.0f;
	float DestCoreRadius = 0.0f;
	
	/**
	 * @brief Attempts to build a Mapping from a Source Portal to a Destination Portal.
	 * @param SourcePortal Source Portal used as the entry.
	 * @param DestPortal Destination Portal used as the exit.
	 * @param OutMapping Receives the generated Mapping.
	 * @return true if the Mapping was built successfully.
	 */
	static bool Build(const AWormholePortalActor* SourcePortal, const AWormholePortalActor* DestPortal, OUT FWPTransform& OutMapping);
	
	/**
	 * @brief Maps a World-space position from Source space into Destination space.
	 * @param Point World-space position relative to the Source Portal.
	 * @return World-space position mapped relative to the Destination Portal.
	 */
	FVector MapPoint(const FVector& Point) const;

	/**
	 * @brief Maps a ray origin from Source space into Destination Portal space.
	 *
	 * MapPoint reverses the relative position for antipodal object traversal. A ray must instead continue
	 * from the corresponding local coordinate in Destination space along the direction preserved by
	 * TransformRayThroughPortal. Use this function for the virtual origin of a Point or SpotLight.
	 *
	 * @param RayOrigin Ray origin in Source space.
	 * @return Virtual ray origin in Destination space.
	 */
	FVector MapRayOrigin(const FVector& RayOrigin) const;

	/**
	 * @brief Maps a World-space position from Destination space back into Source space.
	 * @param Point World-space position in Destination space.
	 * @return World-space position mapped back relative to the Source Portal.
	 */
	FVector UnmapPoint(const FVector& Point) const;
	
	/**
	 * @brief Maps a direction vector from Source space into Destination space.
	 * @param Dir Direction in Source space.
	 * @return Direction mapped into Destination space.
	 */
	FVector MapDir(const FVector& Dir) const;

	/**
	 * @brief Maps a direction vector from Destination space back into Source space.
	 * @param Dir Direction in Destination space.
	 * @return Direction mapped back into Source Portal space.
	 */
	FVector UnmapDir(const FVector& Dir) const;

	/**
	 * @brief Maps a rotation from Source space into Destination space.
	 * @param Rotation Rotation in Source space.
	 * @return Rotation mapped into Destination space.
	 */
	FQuat MapRot(const FQuat& Rotation) const;
	
	/**
	 * @brief Maps a Control Rotation into Destination space while preserving its original Roll.
	 *
	 * Applies the Portal rotation delta to the view direction, but retains the Roll from the Source
	 * Control Rotation so the camera does not tilt sideways.
	 * @param ControlRotation Control Rotation in Source space.
	 * @return Control Rotation facing the mapped Destination direction while preserving the original Roll.
	 */
	FRotator MapControlRotation(const FRotator& ControlRotation) const;

	/**
	 * @brief Computes the Destination exit point using the selected Source Portal Local plane.
	 * @param EntryPoint Entry point on the Source Portal surface.
	 * @param SelectedPlane Plane used to reflect the position in Source Portal Local space.
	 * @return Corresponding exit point on the Destination Portal surface.
	 */
	FVector MapExit(const FVector& EntryPoint, EWPTransitPlane SelectedPlane) const;

	/**
	 * @brief Maps a World Transform into Destination space using the selected position-reflection plane.
	 * @param Source World Transform to map.
	 * @param EntryPoint Entry point on the Source Portal surface.
	 * @param SelectedPlane Plane used to reflect the position in Source Portal Local space.
	 * @return World Transform mapped relative to the Destination exit point.
	 */
	FTransform MapTransform(const FTransform& Source, const FVector& EntryPoint, EWPTransitPlane SelectedPlane) const;
};
