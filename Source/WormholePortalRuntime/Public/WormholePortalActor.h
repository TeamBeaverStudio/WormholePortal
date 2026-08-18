// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Rendering/WPRenderTypes.h"
#include "Rendering/LUT/WPLUTTypes.h"
#include "GameFramework/Actor.h"
#include "WormholePortalActor.generated.h"

class USceneComponent; 
class UPrimitiveComponent;
class UStaticMeshComponent;
class USphereComponent;
class UWPPortalDebugComponent;
class USplineComponent;
class UTextRenderComponent;
class UVolumeTexture;
class UTextureRenderTargetCube;
class UWorldPartitionStreamingSourceComponent;
class UWPLUTAsset;
class UWPLUTEndpointManager;
class UWPRuntimeSubsystem;
class UWPPortalStreamingSubsystem;
class AWormholePortalActor;

enum class EWPPortalChangeType : uint8;

/**
 * @brief Replicated state that delivers the runtime settings owned by a Portal Actor to clients, including late-joining clients.
 * 
 * Unreal Engine's Replicated Movement handles the Actor Transform separately.
 * This struct delivers the Link, physical Metric, and render-only Visual Scale values that are not
 * included in Movement Replication.
 */
USTRUCT()
struct WORMHOLEPORTALRUNTIME_API FWPPortalRepState
{
	GENERATED_BODY()

	/** @brief Counterpart Portal used as the destination for traversal and render mapping. */
	UPROPERTY()
	TObjectPtr<AWormholePortalActor> LinkedPortal;

	/** @brief Portal radius used for physical traversal tests and the l=0 seam. */
	UPROPERTY()
	float PortalRadius = 0.0f;

	/** @brief Metric distance from the seam to the mouth. */
	UPROPERTY()
	float ThroatHalfLength = 0.0f;

	/** @brief Metric distance from the mouth to the start of the flat-space region. */
	UPROPERTY()
	float TransitionLength = 0.0f;

	/** @brief Render-only uniform scale. It does not resize collision, bounds, or capture resources. */
	UPROPERTY()
	float PortalVisualScale = 1.0f;
};

/**
 * Actor representing the authoring metric, traversal boundaries, link relationship, and
 * non-raster bounds proxy for one end of a wormhole.
 * World-scoped runtime and capture managers own the runtime cube capture component and
 * target, pair capture state,
 * and actual CaptureScene submissions. This Actor does not tick.
 *
 * Coordinate and radius terms are defined as follows:
 * - PortalRadius (rho): The local radius of the l=0 seam and the sphere used for
 *   physical traversal tests.
 * - MouthRadius: PortalRadius + ThroatHalfLength (a).
 * - TransitionRadius: MouthRadius + TransitionLength (T), the outer boundary where the
 *   transition to flat space ends.
 * - Actor Scale: The default usage contract assumes a Portal Scale of (1,1,1). Other
 *   Scale values can be set,
 *   but the code neither changes nor rejects them, and does not apply Actor Scale to
 *   the metric radius calculations above.
 *   Changing Scale can therefore make the UE component visuals and collision Transform
 *   disagree with PortalRadius-based
 *   Portal math. The Scale of an object traversing the Portal is governed by a separate
 *   Transit policy and is not affected
 *   by this restriction.
 *
 * The World Runtime Subsystem and SceneViewExtension own the actual render resources
 * and compositing. OuterProxy is a
 * non-raster component retained only for serialization compatibility and analytic
 * bounds. The Actor does not own materials,
 * MIDs, cube capture state, or LUT request state.
 */
UCLASS()
class WORMHOLEPORTALRUNTIME_API AWormholePortalActor : public AActor
{
	GENERATED_BODY()
	
public:	
	/** Creates the authoring, overlap, streaming, and proxy components with safe initial values. */
	AWormholePortalActor();

	/** Registers the Portal RepState with Unreal Engine Property Replication. */
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** Prepares the authoring, proxy, and overlap state, then registers the endpoint with the Registry. */
	virtual void BeginPlay() override;
	/** Unregisters from the Registry so the World managers are cleaned up before releasing the overlap delegate. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** Recomputes the metric traversal boundaries and proxy size during editor updates or Spawn. */
	virtual void OnConstruction(const FTransform& Transform) override;

public:
	virtual void Destroyed() override;
	
public:
	/** Synchronizes the reciprocal link and rho/a/T metric for both Portals. A Portal cannot link to itself. */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal")
	void SetLinkedPortal(AWormholePortalActor* NewLinkedPortal);
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal")
	void ClearLinkedPortal();
	
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal")
	AWormholePortalActor* GetLinkedPortal() const;
	
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal")
	bool HasLinkedPortal() const;

	/** 
	 * Returns true in a regular Level. In World Partition, returns whether the linked destination 
	 * Portal's PortalAreaStreamingSource is enabled and has completed streaming.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Streaming")
	bool IsLinkedPortalAreaReady() const;

	/**
	 * Returns a read-only borrowed reference to the Cube Render Target owned by the runtime
	 * capture manager.
	 * Consumers must not call Init, ResizeTarget, UpdateResource, UpdateResourceImmediate,
	 * or ReleaseResource through
	 * this pointer. Cube allocation changes are permitted only through the runtime capture
	 * manager path, which also
	 * increments ResourceGeneration and notifies the Registry.
	 */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Render")
	UTextureRenderTargetCube* GetPortalCubeRenderTarget() const;

	/** Returns the layout and stable resource generation for the runtime capture manager endpoint. */
	FWPCubeContract GetPortalCubeContract() const;

	/** Returns a borrowed reference to the Volume LUT prepared by the runtime LUT endpoint manager. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Render")
	UVolumeTexture* GetLUTTexture() const;

	/** Returns the logical Z coordinate in the Volume LUT for the current Actor's T/rho value. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Render")
	float GetLUTZ() const;

	/** Returns the metric outer radius from the runtime LUT endpoint snapshot. */
	float GetMetricOuterRadius() const;

	/** Returns the current LUT layout, content revision, resource generation, and expected texture format. */
	FWPRayLUTContract GetLUTContract() const;

	/** Returns the cache revision used by the Renderer to detect LUT resource replacement. */
	uint32 GetLUTRevision() const;

	/** Returns the number of successful Cube CaptureScene submissions made by the runtime capture manager. */
	uint32 GetPortalCaptureGeneration() const;

	/**
	* Returns the stable selector identity for the Pair endpoint.
	* Uses a UObject FName that remains stable across PIE replication rather than the
	* mutable Editor Actor Label.
	 */
	FName GetStableSelectorName() const;
	
	/**
	 * Sets the authored seam radius. Runtime callers must use SetMetricParameters so rho,
	 * a, and T cannot drift independently on Tick.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Metric")
	void SetPortalRadius(float NewRadius);

	/** Sets the metric length a during authoring. Independent runtime mutation is rejected after BeginPlay. */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Metric")
	void SetThroatHalfLength(float NewHalfLength);

	/** Sets the metric length T during authoring. Independent runtime mutation is rejected after BeginPlay. */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Metric")
	void SetTransitionLength(float NewTransitionLength);

	/**
	 * Establishes the final physical Metric shape as one atomic operation. Call this once before
	 * runtime animation so collision, analytic bounds, LUT selection, and capture resolution all
	 * agree on the same rho/a/T values. Later calls may only preserve the original a/rho and T/rho
	 * ratios. For inexpensive growth animation, use SetPortalVisualScale instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Physical Metric")
	void InitializePhysicalMetric(
		float NewPortalRadius,
		float NewThroatHalfLength,
		float NewTransitionLength);

	/**
	 * Backward-compatible name for InitializePhysicalMetric. New code and Blueprints should use the
	 * explicit Physical Metric API so this operation is not confused with render-only Visual Scale.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Physical Metric",
		meta = (DeprecatedFunction, DeprecationMessage = "Use InitializePhysicalMetric. Use SetPortalVisualScale for low-cost visual growth."))
	void SetMetricParameters(
		float NewPortalRadius,
		float NewThroatHalfLength,
		float NewTransitionLength);

	/**
	 * Uniformly resizes the physical Metric relative to the shape captured by
	 * InitializePhysicalMetric. This changes collision, bounds, visibility queries, and dynamic
	 * capture resolution. It is intentionally more expensive than SetPortalVisualScale.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Physical Metric")
	void SetUniformPhysicalMetricScale(float NewPhysicalMetricScale);

	/** Returns the current physical Metric scale relative to the initialized Metric shape. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Physical Metric")
	float GetUniformPhysicalMetricScale() const;

	/**
	 * Changes only the rendered wormhole size. The final physical Metric, collision, analytic bounds,
	 * LUT identity, ownership/warmup state, capture cadence, and dynamic capture resolution remain
	 * unchanged. This is the recommended API for per-frame spawn or growth effects.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Visual")
	void SetPortalVisualScale(float NewVisualScale);

	/** Returns the render-only scale applied by the compositor. */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Visual")
	float GetPortalVisualScale() const;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Metric")
	float GetPortalRadius() const;

	/** Returns the metric radius of the mouth boundary: PortalRadius + ThroatHalfLength (a). */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Metric")
	float GetMouthRadius() const;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Metric")
	float GetThroatHalfLength() const;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Metric")
	float GetTransitionLength() const;

	/** Returns the boundary radius where the flat-space region begins: MouthRadius + TransitionLength (T). */
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Metric")
	float GetTransitionRadius() const;
	
	/** Returns the effective LUT descriptor used by this Actor. */
	FWPLUTDescriptor GetEffectiveLUTDescriptor() const;

	/** Immediately enables or disables Editor Scene Proxy boundary rendering. The BeginPlay policy always disables it in PIE. */
	UFUNCTION(BlueprintCallable, BlueprintSetter, Category = "Wormhole Portal|Debug")
	void SetDrawPortalDebug(bool bEnabled);

	UFUNCTION(BlueprintPure, BlueprintGetter, Category = "Wormhole Portal|Debug")
	bool IsPortalDebugEnabled() const;
	
	/**
	 * Transforms an entry surface point and direction into the linked Portal's space.
	 * Does not perform a Trace. Currently accepts only rays traveling from outside the
	 * Portal sphere toward its interior.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Trace")
	bool TransformRayThroughPortal(const FVector& EntrySurfacePoint, const FVector& EntryWorldDirection,
		FVector& OutExitStart, FVector& OutExitDirection, float ExitSurfaceOffset = 2.f) const;

private:
	friend class UWPLUTEndpointManager;
	friend class UWPRuntimeSubsystem;
	friend class UWPPortalStreamingSubsystem;

	/** Internal implementation that establishes or clears the bidirectional link without recursion. */
	void SetLinkedPortalInternal(AWormholePortalActor* NewLinkedPortal, bool bUpdateOtherPortal);

	/** Handles minimum-radius clamping, component repositioning, link propagation, and timing logs through a single path. */
	void SetPortalRadiusInternal(float NewRadius, bool bUpdateLinkedPortal);
	void SetThroatHalfLengthInternal(float NewHalfLength, bool bUpdateLinkedPortal);
	void SetTransitionLengthInternal(float NewTransitionLength, bool bUpdateLinkedPortal);

	/** Applies a complete metric state without exposing invalid intermediate T/rho ratios. */
	void SetMetricParametersInternal(
		float NewPortalRadius,
		float NewThroatHalfLength,
		float NewTransitionLength,
		bool bUpdateLinkedPortal);
	/** Applies render-only scale and optionally mirrors it to the linked endpoint. */
	void SetPortalVisualScaleInternal(float NewVisualScale, bool bUpdateLinkedPortal);
	/** Returns true outside runtime play, for the first atomic setup, or for a uniform scale-only update. */
	bool ValidateRuntimeMetricMutation(
		float NewPortalRadius,
		float NewThroatHalfLength,
		float NewTransitionLength,
		bool bAtomicMetricUpdate,
		const TCHAR* ApiName,
		double StartSeconds);
	/** Emits a throttled warning so an erroneous per-Tick caller does not create log-induced load. */
	void LogRejectedRuntimeMetricMutation(
		const TCHAR* ApiName,
		const TCHAR* Reason,
		float RequestedPortalRadius,
		float RequestedThroatHalfLength,
		float RequestedTransitionLength,
		double StartSeconds);
	/** Captures the current physical values and their a/rho and T/rho ratios as the runtime scale contract. */
	void CaptureRuntimeMetricShape(bool bIncludeLinkedPortal);
	/** Clamps the metric lengths a and T to nonnegative values. */
	void SanitizeMetricLengths();
	
	/** Clamps TransitionLength (T) to the Domain of the current LUT Asset. */
	float ClampTransitionLengthToLUTDomain(
		float RequestedTransitionLength,
		float Radius,
		bool& bOutClamped) const;

	/** Clamps the streaming distances and query interval to valid ranges. */
	void SanitizeStreamingSettings();

	/** Applies the metric radii directly to the Trigger and Trace spheres and applies TransitionRadius to the non-raster bounds size. */
	void ApplyPortalSize() const;

	/** Logs the non-raster bounds proxy, unit-scale seam radius, and initialization CPU time for diagnostics. */
	/** Logging only. Outputs proxy state and CPU time for the call interval without modifying Actor state. */
	void LogPortalProxyState(const TCHAR* Phase, double StartSeconds) const;

	/** Updates the persistent debug scene proxy data when an event occurs. Does not use Actor Tick or DrawDebug. */
	void RefreshPortalDebugVisualization() const;

	/** Retains the serialized bounds component while permanently excluding it from visible, raster, and ray-tracing paths. */
	void ApplyOuterProxyNonRasterPolicy() const;

	/** Excludes the render proxy from collision and assigns purpose-specific Query responses only to the Trigger and Trace spheres. */
	void ApplyPortalCollisionSettings() const;

	/** Refreshes the authoritative Portal RepState, then notifies the Registry of a Link or Metric change. */
	void NotifyRegistryPortalChanged(EWPPortalChangeType ChangeType);

	/** Writes the current Link and Metric values to the authoritative Portal RepState. */
	void RefreshPortalRepState();

	/** Applies the replicated Link and Metric values received by the client to the actual Portal state and components. */
	UFUNCTION()
	void OnRep_PortalRepState();

	/** Returns a stable UObject FName that does not depend on mutable labels or authoring tags. */
	FName ResolveStableSelectorName() const;

	void ApplyPortalTraceChannelResponses() const;

	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Transit")
	void OnPortalTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Transit")
	void OnPortalTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
	/** Local coordinate origin shared by all runtime components. */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Wormhole Portal|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;
	
	/**
	 * Non-raster proxy that provides the TransitionRadius bounds.
 	 * Preserves the serialized component name and structure used by existing Blueprints and
 	 * Maps, but does not participate
 	 * in the main, depth, or custom-depth passes and does not create, inspect, bind, or
 	 * update materials.
	 */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Wormhole Portal|Proxy", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> OuterProxy;

	/** Metric-boundary visualization component that creates a persistent Scene Proxy in an Editor World. */
	UPROPERTY(VisibleDefaultsOnly, Transient, Category = "Wormhole Portal|Debug", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UWPPortalDebugComponent> PortalDebugVisualization;

	/**
 	 * Displays the seam/Trigger, mouth, and transition boundaries together in an Editor
 	 * World.
 	 * BeginPlay forcibly disables the visualization in PIE to avoid contaminating
 	 * performance measurements.
 	 * The visualization is also disabled in Standalone and packaged builds.
 	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintGetter = IsPortalDebugEnabled,
		BlueprintSetter = SetDrawPortalDebug, Category = "Wormhole Portal|Debug",
		meta = (AllowPrivateAccess = "true", DisplayName = "Draw Portal Debug"))
	bool bDrawPortalDebug = true;


	/** Streaming Source that loads Runtime Grid Cells around this Portal. Enabled only when the Portal at the other end of the link must be loaded. */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Wormhole Portal|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UWorldPartitionStreamingSourceComponent> PortalAreaStreamingSource;
	
	/** Per-instance override for special cases that must use this asset instead of the global catalog. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Render|LUT",
		meta = (AllowPrivateAccess = "true"))
	TSoftObjectPtr<UWPLUTAsset> LUTAssetOverride;

	/** Approach distance at which destination preloading begins while the source is inactive. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Streaming",
		meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float StreamingPreloadDistance = 15000.0f;

	/** Outer distance used to keep streaming active. Clamped so it is not less than PreloadDistance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Streaming",
		meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float StreamingReleaseDistance = 20000.0f;

	/** Interval at which proximity demand from cameras and Transit Actors is reevaluated. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Streaming",
		meta = (AllowPrivateAccess = "true", ClampMin = "0.01", UIMin = "0.01", Units = "s"))
	float StreamingQueryInterval = 0.2f;

	/** Single source of truth for the Overlap that starts physical traversal and for the l=0 seam. Assumes an Actor Scale of 1 and uses SphereRadius directly. */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Wormhole Portal|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USphereComponent> TriggerVolume;
	
	/** Query-only seam sphere that blocks only the Portal Trace Channel. Does not generate traversal Overlaps. */
	UPROPERTY()
	TObjectPtr<USphereComponent> TraceVolume;
	
	/** Counterpart Portal used as the destination for traversal and render-camera mapping. SetLinkedPortal guarantees the reciprocal link. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<AWormholePortalActor> LinkedPortal;
	
	/**
	 * Authored radius rho, in centimeters, of the l=0 physical-traversal seam sphere.
	 * The default usage contract assumes a Portal Actor Scale of 1, making this value the
	 * single source of truth
	 * for both the local-space and World-space traversal radius.
	 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Metric",
		meta = (AllowPrivateAccess = "true", DisplayName = "Portal Radius (rho)", ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float PortalRadius;

	/** Metric length a, in centimeters, from the seam to the mouth. This is an additive distance used to compute MouthRadius, not an independent radius. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Metric",
		meta = (AllowPrivateAccess = "true", DisplayName = "Throat Half Length (a)", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float ThroatHalfLength = 100.0f;

	/** Metric length T, in centimeters, over which distortion transitions from the mouth to flat space. Used to compute TransitionRadius. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Metric",
		meta = (AllowPrivateAccess = "true", DisplayName = "Transition Length (T)", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float TransitionLength = 200.0f;

	/** Render-only uniform scale published to the compositor; it never changes physical Metric state. */
	float PortalVisualScale = 1.0f;

	/**
	 * Runtime-only latch set by the first successful atomic SetMetricParameters call.
	 * Once set, public runtime APIs accept only uniform scale-only metric updates.
	 * Editor authoring, replication application, and LUT binding correction use internal paths.
	 */
	bool bRuntimeMetricShapeInitialized = false;
	/** Physical seam radius captured when the runtime Metric shape is initialized. */
	float RuntimeMetricBasePortalRadius = 0.0f;
	/** Physical throat half-length captured when the runtime Metric shape is initialized. */
	float RuntimeMetricBaseThroatHalfLength = 0.0f;
	/** Physical transition length captured when the runtime Metric shape is initialized. */
	float RuntimeMetricBaseTransitionLength = 0.0f;
	/** Immutable a/rho ratio captured by the first successful atomic runtime setup. */
	float RuntimeMetricThroatRatio = 0.0f;
	/** Immutable T/rho ratio captured by the first successful atomic runtime setup. */
	float RuntimeMetricTransitionRatio = 0.0f;
	/** Total rejected public runtime metric mutations; used only for diagnostics. */
	uint64 RejectedRuntimeMetricMutationCount = 0;
	/** Rejection count included in the most recent throttled diagnostic log. */
	uint64 LastLoggedRuntimeMetricMutationRejectCount = 0;
	/** World time of the last rejection log. Negative means no log has been emitted. */
	double LastRuntimeMetricMutationRejectLogSeconds = -1.0;

	/** Latest authoritative state for the Portal Link and Metric values not included in Movement Replication. */
	UPROPERTY(ReplicatedUsing = OnRep_PortalRepState)
	FWPPortalRepState PortalRepState;
	
#if	WITH_EDITORONLY_DATA
	// Editor-only visualization components for inspecting the link relationship.
	// Do not affect runtime Portal rendering.
	UPROPERTY()
	TObjectPtr<USplineComponent> EditorLinkSpline;
	
	UPROPERTY()
	TObjectPtr<UTextRenderComponent> EditorLabel;
#endif
	
#if WITH_EDITOR
public:
	/** Saves the previous link before a Details change so the bidirectional link and Undo state can be restored. */
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	/** Applies post-processing for radius, length, link, and debug-toggle changes made by Details while bypassing their setters. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** Recomputes both editor link lines after the Portal moves. */
	virtual void PostEditMove(bool bFinished) override;
	
private:
	void UpdateEditorVisualization() const;
	
   	void ModifyForLinkedPortalChange();
	
private:
	TWeakObjectPtr<AWormholePortalActor> LinkedPortalBeforeEdit;
#endif
};


