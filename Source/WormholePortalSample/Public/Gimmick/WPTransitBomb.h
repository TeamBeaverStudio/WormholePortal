// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "WPTransitBomb.generated.h"

class UBoxComponent;
class UDamageType;
class UStaticMeshComponent;
class UWPTransitComponent;

/**
 * @brief Demo physics bomb that applies an explosion at both its own and its valid Counterpart's locations while crossing a portal.
 *
 * It explodes after ExplosionDelay. Outside the Crossing phase, or when no valid Counterpart
 * exists, it explodes only at its current location.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Transit Bomb"))
class WORMHOLEPORTALSAMPLE_API AWPTransitBomb : public AActor
{
	GENERATED_BODY()

public:
	AWPTransitBomb();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	

	/**
	 * @brief Called at each explosion location so Blueprints can add custom effects.
	 * @param ExplosionLocation World-space location of the explosion.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wormhole Portal|Sample|Gimmick", meta = (DisplayName = "On Exploded"))
	void OnExploded(FVector ExplosionLocation);

private:
	void Explode();
	void ExplodeAt(const FTransform& ExplosionTransform);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> BombMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UWPTransitComponent> TransitComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "s"))
	float ExplosionDelay = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true"))
	TSubclassOf<AActor> ExplosionEffectClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float ExplosionRadius = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0"))
	float Damage = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0"))
	float PhysicsImpulse = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Gimmick", meta = (AllowPrivateAccess = "true"))
	TSubclassOf<UDamageType> DamageType;

	FTimerHandle ExplosionTimer;
	bool bExploded = false;
};
