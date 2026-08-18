// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WPSampleTurret.generated.h"

class AWPBeamTarget;
class UArrowComponent;
class UParticleSystem;
class UParticleSystemComponent;
class USceneComponent;
class UStaticMeshComponent;

/**
 * @brief A sample turret that performs Portal-aware Line Traces and renders each Trace segment as a Beam.
 *
 * Applies DamagePerSecond once per second while continuously hitting the assigned DamageTarget.
 * The Cascade Beam Source and Target are updated to match the actual Trace segment in each space.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Sample Turret"))
class WORMHOLEPORTALSAMPLE_API AWPSampleTurret : public AActor
{
	GENERATED_BODY()

public:
	AWPSampleTurret();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void UpdateBeam();
	void SetBeamSegment(int32 SegmentIndex, const FVector& Start, const FVector& End);
	void HideUnusedBeamSegments(int32 UsedSegmentCount);
	UParticleSystemComponent* GetOrCreateBeamSegment(int32 SegmentIndex);
	void UpdateTargetDamage(AActor* HitActor, float ElapsedSeconds);

	/** @brief Provides the reference Transform for the Turret Components. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	/** @brief Provides the Turret visuals and World collision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> TurretMesh;

	/** @brief Defines the start position and direction of the Portal Line Trace and Beam. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArrowComponent> Muzzle;

	/** @brief Maximum logical distance of the Portal Line Trace. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true", ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float TraceDistance = 3000.0f;

	/** @brief Scene Trace Channel used for Beam collision checks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;

	/** @brief Cascade Beam Particle System used to render each Trace segment. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UParticleSystem> BeamTemplate;

	/** @brief Stage-specific Target that can receive Beam damage. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<AWPBeamTarget> DamageTarget;

	/** @brief Damage applied after continuously hitting DamageTarget for one second. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Turret", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0"))
	float DamagePerSecond = 20.0f;

	/** @brief Pool of Cascade Beam Components created to render the current Trace path. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UParticleSystemComponent>> BeamSegments;

	FTimerHandle BeamUpdateTimer;
	float ContinuousHitSeconds = 0.0f;
	double LastBeamUpdateSeconds = 0.0;

	static constexpr float BeamUpdateInterval = 0.05f;
	static constexpr float DamageInterval = 1.0f;
	static constexpr int32 MaxPortalDepth = 4;
	static constexpr float PortalExitOffset = 2.0f;
};
