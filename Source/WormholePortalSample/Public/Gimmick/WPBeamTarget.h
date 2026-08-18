// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WPBeamTarget.generated.h"

class UStaticMeshComponent;

/**
 * @brief A stage-specific Target destroyed by Beam damage from the Sample Turret.
 *
 * Implement destruction effects in OnExploded. External actors such as Guards should respond through the Actor destruction event.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Beam Target"))
class WORMHOLEPORTALSAMPLE_API AWPBeamTarget : public AActor
{
	GENERATED_BODY()

public:
	AWPBeamTarget();

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

protected:
	virtual void BeginPlay() override;

	/**
	 * @brief Updates UI and hit effects when the Health changes.
	 * @param NewHealth Current Health after applying the change.
	 * @param InMaxHealth Maximum Health of the Target.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wormhole Portal|Sample|Beam Target", meta = (DisplayName = "On Health Changed"))
	void OnHealthChanged(float NewHealth, float InMaxHealth);

	/** @brief Executes explosion effects before destruction when the Health is depleted. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wormhole Portal|Sample|Beam Target", meta = (DisplayName = "On Exploded"))
	void OnExploded();

private:
	/** @brief Provides the Target visuals and Beam collision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Beam Target", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> TargetMesh;

	/** @brief Maximum Health available to the Target. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Beam Target", meta = (AllowPrivateAccess = "true", ClampMin = "1.0", UIMin = "1.0"))
	float MaxHealth = 60.0f;

	/** @brief Current remaining Health. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Beam Target", meta = (AllowPrivateAccess = "true"))
	float CurrentHealth = 0.0f;

	bool bExploded = false;
};
