// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "WPTraceLibrary.generated.h"

class AWormholePortalActor;

UENUM(BlueprintType)
enum class EWPPortalTraceStatus : uint8
{
	/** Trace computation completed successfully. Use the function return value to determine whether a Blocking Hit occurred. */
	Completed,

	/** The World Context, Trace Channel, or Trace length is invalid. */
	InvalidInput,

	/** Another Portal required traversal after the allowed traversal count was exhausted. */
	MaxPortalDepthReached,

	/** Portal transformation failed because of a missing link, invalid entry direction, or a similar error. */
	PortalTransformFailed
};

UENUM(BlueprintType)
enum class EWPPortalTracePortalOutcome : uint8
{
	/** Successfully traversed the linked Portal. */
	Traversed,
	
	/** This Portal required traversal, but the MaxPortalDepth budget was exhausted. */
	MaxDepthReached,
	
	/** Transformation failed because of a missing link, invalid entry direction, or a similar error. */
	TransformFailed,
};

/**
 * Represents a Hit detected by a Scene Trace together with additional Portal-path
 * information.
 *
 * Hit stores information relative to the current Trace segment.
 * LogicalDistance and SegmentIndex store information relative to the complete logical
 * path.
 */
USTRUCT(BlueprintType)
struct WORMHOLEPORTALRUNTIME_API FWPPortalTraceHit
{
	GENERATED_BODY()
	
	/** Original HitResult returned by the underlying Scene Trace. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	FHitResult Hit;
	
	/**
	 * Logical distance consumed from the initial Trace start to this Hit.
	 * Excludes distance inside Portals and PortalExitOffset.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	float LogicalDistance = 0.0f;
	
	/** Zero-based index of the segment containing this Hit. It equals the number of Portals successfully traversed before the Hit. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	int32 SegmentIndex = 0;
};

/**
 * Represents a Portal actually reached along the logical Trace path and the result of
 * processing it.
 *
 * A Portal that was not reached on the actual path, such as one behind a Scene Blocking
 * Hit,
 * does not produce an event. If Outcome is not Traversed, the complete Portal Trace
 * terminates at this event.
 */
USTRUCT(BlueprintType)
struct WORMHOLEPORTALRUNTIME_API FWPPortalTracePortalEvent
{
	GENERATED_BODY()
	
	/**
	 * Original HitResult returned by the WPPortalTrace channel.
	 * bBlockingHit is only a physical result of the Portal-detection query; it does not mean that the complete Portal Trace was blocked.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	FHitResult DetectionHit;
	
	/** Portal entered by the Trace. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	TObjectPtr<AWormholePortalActor> EntryPortal = nullptr;

	/**
	 * Exit Portal linked to EntryPortal.
	 * May be nullptr when transformation fails because the link is missing.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	TObjectPtr<AWormholePortalActor> ExitPortal = nullptr;
	
	/** World-space direction when the Trace reaches the Entry Portal. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	FVector EntryDirection = FVector::ZeroVector;
	
	/** Start point of the next Trace segment after applying PortalExitOffset. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	FVector ExitTraceStart = FVector::ZeroVector;

	/**
	 * Normalized World-space direction continuing from the Exit Portal.
	 * May be ZeroVector for an event whose transformation did not succeed.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	FVector ExitDirection = FVector::ZeroVector;
	
	/** Logical distance from the initial Start to the Entry Portal surface. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	float LogicalDistance = 0.0f;
	
	/**
	 * Zero-based traversal depth when this Portal is reached.
	 * The first encountered Portal has PortalDepth 0.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	int32 PortalDepth = 0;
	
	/** Result of processing this Portal after it was reached. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	EWPPortalTracePortalOutcome Outcome = EWPPortalTracePortalOutcome::TransformFailed;
};

/**
 * Complete result of a Line Trace that can traverse Portals.
 *
 * @par SceneHits storage order
 * SceneHits are stored in logical Trace-path order beginning at the initial Start.
 * Hits within each Trace segment are stored nearest first.
 * After a Portal traversal, Hits from the next segment are appended to the array.
 *
 * If a Blocking Hit exists, it is the final SceneHits element and later segments are
 * not tested.
 * If only Overlap Hits exist, SceneHits may be non-empty while bBlockingHit remains
 * false.
 *
 * FHitResult::Distance is measured from the start of each post-Portal segment and
 * therefore may not
 * increase monotonically across SceneHits. Use FWPPortalTraceHit::LogicalDistance for
 * path distance
 * measured from the initial Start.
 *
 * @par PortalEvents storage order
 * PortalEvents are stored in the order in which Portals are actually reached along the
 * logical Trace path.
 * A Portal behind a Scene Blocking Hit or removed from the path by another Portal is
 * not stored.
 *
 * Successfully traversed Portal events have a Traversed Outcome.
 * If a MaxDepthReached or TransformFailed event exists, it is the final PortalEvents
 * element and Trace processing stops.
 *
 * PortalTraversalCount is the number of events whose Outcome is Traversed, not the
 * total PortalEvents count.
 *
 * @par Comparing order across arrays
 * SceneHits and PortalEvents are each sorted in path order but are independent arrays.
 * Compare LogicalDistance, not array indices, to determine relative occurrence across
 * the two arrays.
 *
 * If a Scene Hit and Portal are detected at the same location, the Portal takes
 * precedence and the Scene Hit
 * is omitted from SceneHits.
 *
 * @par Distance semantics
 * LogicalDistance and ProcessedDistance are logical Trace distances that exclude
 * distance inside Portals
 * and the self-hit-avoidance PortalExitOffset.
 */
USTRUCT(BlueprintType)
struct WORMHOLEPORTALRUNTIME_API FWPPortalTraceResult
{
	GENERATED_BODY()
	
	/** Reason the Trace terminated. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	EWPPortalTraceStatus Status = EWPPortalTraceStatus::InvalidInput;
	
	/** Indicates whether the final SceneHits element is a Blocking Hit. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	bool bBlockingHit = false;
	
	/** Originally requested Start-to-End distance. */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	float RequestedDistance = 0.0f;
	
	/**
	 * Logical distance for which processing completed.
	 * For a Blocking Hit, this is the distance to that Hit.
	 * For a successful Miss, this is RequestedDistance.
	 * For a Portal failure, this is the distance to the failed Entry Portal.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	float ProcessedDistance = 0.0f;

	/**
	 * Number of Portals successfully traversed.
	 * Excludes a final failed PortalEvent.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	int32 PortalTraversalCount = 0;

	/**
	 * Hits detected by Scene Trace.
	 * The Single API stores at most one final Blocking Hit.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	TArray<FWPPortalTraceHit> SceneHits;

	/**
	 * Portal events actually reached on the logical path.
	 * The final event may represent a failed traversal attempt.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wormhole Portal|Trace")
	TArray<FWPPortalTracePortalEvent> PortalEvents;
};

class AActor;

/**
 * Provides Trace operations that can traverse Portals.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API UWPTraceLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	
public:
	// Blueprint API
	
	/**
	 * Blueprint convenience function for a Single Line Trace that can traverse Portals.
	 *
	 * Returns only the final Scene Blocking Hit and termination status.
	 * Use PortalLineTraceSingleDetailed when the complete path is required, including
	 * traversed Portals
	 * and logical distance.
	 *
	 * @return True if a final Scene Blocking Hit exists.
	 */
	UFUNCTION(
		BlueprintCallable, 
		Category = "Wormhole Portal|Trace", 
		meta = (
			WorldContext = "WorldContextObject",
			DisplayName = "Portal Line Trace By Channel",
			AutoCreateRefTerm = "ActorsToIgnore",
			AdvancedDisplay = "TraceColor,TraceHitColor,DrawTime,MaxPortalDepth,PortalExitOffset",
			Keywords = "raycast line trace portal wormhole"
			))
	static bool PortalLineTraceSingle(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		FHitResult& OutHit,
		EWPPortalTraceStatus& OutStatus,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f);

	/**
	 * Detailed Blueprint Single Line Trace that can traverse Portals.
	 *
	 * OutResult contains the final Scene Hit, successful and failed Portal-traversal
	 * events,
	 * total logical distance, and Portal traversal count.
	 * PortalEvents recorded before the failure point are preserved even when the status is
	 * a failure.
	 *
	 * @param OutResult Detailed Trace result reset on every call.
	 * @return True if a final Scene Blocking Hit exists.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Wormhole Portal|Trace",
		meta = (
			WorldContext = "WorldContextObject",
			DisplayName = "Portal Line Trace Detailed By Channel",
			AutoCreateRefTerm = "ActorsToIgnore",
			AdvancedDisplay = "TraceColor,TraceHitColor,DrawTime,MaxPortalDepth,PortalExitOffset",
			Keywords = "raycast line trace portal wormhole detailed"
		))
	static bool PortalLineTraceSingleDetailed(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		FWPPortalTraceResult& OutResult,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f);

	/**
	 * Blueprint convenience function for a Multi Line Trace that can traverse Portals.
	 *
	 * Returns Scene Overlap Hits and the first Blocking Hit in logical path order.
	 * If a Blocking Hit exists, it is the final OutHits element.
	 *
	 * FHitResult::Distance is relative to the segment start after each Portal traversal.
	 * Use PortalLineTraceMultiDetailed when total logical distance and Portal events are
	 * required.
	 *
	 * OutHits collected before the failure point are preserved even when the status is a
	 * failure.
	 *
	 * @param OutHits Scene Hits stored in logical path order.
	 * @param OutStatus Reason the Trace terminated.
	 * @return True if a Scene Blocking Hit exists.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Wormhole Portal|Trace",
		meta = (
			WorldContext = "WorldContextObject",
			DisplayName = "Portal Line Trace Multi By Channel",
			AutoCreateRefTerm = "ActorsToIgnore",
			AdvancedDisplay = "TraceColor,TraceHitColor,DrawTime,MaxPortalDepth,PortalExitOffset",
			Keywords = "raycast line trace multi portal wormhole"
		))
	static bool PortalLineTraceMulti(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		TArray<FHitResult>& OutHits,
		EWPPortalTraceStatus& OutStatus,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f);

	/**
	 * Detailed Blueprint Multi Line Trace that can traverse Portals.
	 *
	 * Returns Scene Hits, each Hit's logical distance and segment index, successful and
	 * failed
	 * Portal-traversal events, and the traversal count.
	 * SceneHits and PortalEvents collected before the failure point are preserved when the
	 * status is a failure.
	 *
	 * @param OutResult Detailed Trace result reset on every call.
	 * @return True if a Scene Blocking Hit exists.
	 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Wormhole Portal|Trace",
		meta = (
			WorldContext = "WorldContextObject",
			DisplayName = "Portal Line Trace Multi Detailed By Channel",
			AutoCreateRefTerm = "ActorsToIgnore",
			AdvancedDisplay = "TraceColor,TraceHitColor,DrawTime,MaxPortalDepth,PortalExitOffset",
			Keywords = "raycast line trace multi portal wormhole detailed"
		))
	static bool PortalLineTraceMultiDetailed(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		FWPPortalTraceResult& OutResult,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f);
	
public:
	// C++ API
	
	/**
	 * Tests for a Blocking Hit on the specified Collision Channel while traversing Portals.
	 *
	 * @note Portal detection uses LineTraceSingleByChannel because the Portal distance and
	 *       hit position are required.
	 *       General World collision testing uses LineTraceTestByChannel.
	 *
	 * @param WorldContextObject Context Object used to obtain the World.
	 * @param Start Initial Trace start position.
	 * @param End Initial Trace end position.
	 * @param TraceChannel Collision Channel used for general World collision. Must not be
	 *                     PortalTraceChannel.
	 * @param QueryParams Query Parameters used for general World collision and Portal
	 *                    detection.
	 * @param ResponseParams Collision Response Parameters applied to general World
	 *                       collision.
	 * @param MaxPortalDepth Maximum allowed Portal traversal count.
	 * @param PortalExitOffset Distance used to push the next Trace start away from the Exit
	 *                         Portal.
	 *
	 * @return True if the Trace path contains a general World Blocking Hit.
	 *         False if there is no collision or the Trace cannot be computed to completion.
	 */
	static bool PortalLineTraceTestByChannel(
		const UObject* WorldContextObject,
		const FVector& Start,
		const FVector& End,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams = FCollisionQueryParams::DefaultQueryParam,
		const FCollisionResponseParams& ResponseParams = FCollisionResponseParams::DefaultResponseParam,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f);
	
	/**
	 * Performs a Line Trace on the specified Collision Channel.
	 *
	 * General World collision is tested on TraceChannel.
	 * Portals are tested separately on the PortalTraceChannel configured in UWPSettings.
	 * If a Portal is closer than general collision, the Trace continues from the linked
	 * Portal's opposite side
	 * for the remaining distance.
	 *
	 * @param WorldContextObject Context Object used to obtain the World.
	 * @param OutResult Detailed result reset on every call.
	 *                  May contain PortalEvents up to the failure point even when the call
	 *                  fails.
	 * @param Start Initial Trace start position.
	 * @param End Initial Trace end position.
	 * @param TraceChannel Collision Channel used for general World collision. Must not be
	 *                     PortalTraceChannel.
	 * @param QueryParams Query Parameters used for general collision and Portal detection.
	 * @param ResponseParams Collision Response Parameters applied to general World
	 *                       collision.
	 * @param MaxPortalDepth Maximum allowed Portal traversal count.
	 * @param PortalExitOffset Distance used to push the next Trace start away from the Exit
	 *                         Portal surface.
	 *
	 * @return True if a final Blocking Hit exists.
	 */
	static bool PortalLineTraceSingleByChannel(
		const UObject* WorldContextObject,
		FWPPortalTraceResult& OutResult,
		const FVector& Start,
		const FVector& End,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams = FCollisionQueryParams::DefaultQueryParam,
		const FCollisionResponseParams& ResponseParams = FCollisionResponseParams::DefaultResponseParam,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f,
		EDrawDebugTrace::Type DrawDebugType = EDrawDebugTrace::None,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f);

	/**
	 * Returns Scene Overlap Hits and the first Blocking Hit while traversing Portals.
	 *
	 * Hits from each segment are appended to OutResult.SceneHits in logical path order.
	 * If a Blocking Hit exists, it is the final SceneHits element.
	 * SceneHits and PortalEvents collected before the failure point are preserved when the
	 * call fails.
	 *
	 * @param OutResult Detailed Trace result reset on every call.
	 * @return True if a Scene Blocking Hit exists.
	 */
	static bool PortalLineTraceMultiByChannel(
		const UObject* WorldContextObject,
		FWPPortalTraceResult& OutResult,
		const FVector& Start,
		const FVector& End,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams = FCollisionQueryParams::DefaultQueryParam,
		const FCollisionResponseParams& ResponseParams = FCollisionResponseParams::DefaultResponseParam,
		int32 MaxPortalDepth = 4,
		float PortalExitOffset = 2.0f,
		EDrawDebugTrace::Type DrawDebugType = EDrawDebugTrace::None,
		FLinearColor TraceColor = FLinearColor::Red,
		FLinearColor TraceHitColor = FLinearColor::Green,
		float DrawTime = 5.0f);
	
	// TODO : PortalLineTraceTestByObjectType()
	// TODO : PortalLineTraceSingleByObjectType()
	// TODO : PortalLineTraceMultiByObjectType()
	
	// TODO : PortalLineTraceTestByProfile()
	// TODO : PortalLineTraceSingleByProfile()
	// TODO : PortalLineTraceMultiByProfile()
	
	// TODO: Implement the Portal trace variants below when schedule and priority permit.
	// TODO : PortalSweepSingleByChannel, ObjectType, Profile
	
	// TODO: Finalize the distance semantics of PortalExitOffset.
	// PortalExitOffset is currently not subtracted from the remaining Trace distance.
	// Example: after consuming 50 cm to the Entry and advancing 2 cm from the Exit surface,
	// the Trace still uses the full remaining 50 cm.
	// Each Portal traversal therefore moves the reachable World-space endpoint forward by
	// PortalExitOffset,
	// and collision between the Exit surface and ExitTraceStart is not tested.
	// Decide whether PortalExitOffset is exclusively for self-hit avoidance or should
	// consume the logical
	// or physical Trace budget.
	
private:
	static FCollisionQueryParams MakePortalTraceQueryParams(
		FName TraceTag, 
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		bool bIgnoreSelf,
		const UObject* WorldContextObject);
};
