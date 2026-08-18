// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WPSampleActorSpawner.generated.h"

class UArrowComponent;
class USceneComponent;

/**
 * @brief Demo Spawner that repeatedly spawns and launches the specified Actor.
 *
 * Actors are spawned only on the server. The Spawner Actor provides the spawn location,
 * while the Direction Component provides the spawn rotation and launch direction.
 */
UCLASS(BlueprintType, Blueprintable, Placeable, meta = (DisplayName = "WP Sample Actor Spawner", PrioritizeCategories = "Spawner"))
class WORMHOLEPORTALSAMPLE_API AWPSampleActorSpawner : public AActor
{
	GENERATED_BODY()

public:
	AWPSampleActorSpawner();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	/** @brief Root Component that defines the Spawner's reference Transform. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spawner")
	TObjectPtr<USceneComponent> SceneRootComponent;

	/** @brief Specifies the rotation of spawned Actors and their launch direction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Spawner")
	TObjectPtr<UArrowComponent> Direction;

	/** @brief Actor class to spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	TSubclassOf<AActor> SpawnActorClass;

	/** @brief Linear velocity magnitude applied when the spawned Actor has a Primitive root Component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	float Impulse = 1200.0f;

	/** @brief Time interval between Actor spawns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	float RepeatTime = 1.0f;

	/** @brief Legacy spawn-count setting retained to match the original Spawner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner", meta = (ClampMin = "0"))
	int32 SpawnCount = 0;

	/** @brief Whether the timer repeatedly spawns Actors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	bool bRepeat = true;

	/** @brief Whether to enable Actor, movement, and root Primitive Component replication for spawned Actors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawner")
	bool bReplicated = false;

private:
	void SpawnActor();
	void ApplyLaunchToActor(AActor* SpawnedActor) const;

	FTimerHandle SpawnTimerHandle;
};
