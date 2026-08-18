// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Gimmick/WPSampleActorSpawner.h"

#include "Components/ArrowComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"

AWPSampleActorSpawner::AWPSampleActorSpawner()
{
	SceneRootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRootComponent);

	Direction = CreateDefaultSubobject<UArrowComponent>(TEXT("Direction"));
	Direction->SetRelativeRotation(FRotator(30.0f, 0.0f, 0.0f));
	Direction->SetArrowColor(FColor::Red);
	Direction->SetArrowSize(2.5f);
	Direction->SetupAttachment(GetRootComponent());
}

void AWPSampleActorSpawner::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		GetWorld()->GetTimerManager().SetTimer(
			SpawnTimerHandle,
			this,
			&AWPSampleActorSpawner::SpawnActor,
			RepeatTime,
			bRepeat);
	}
}

void AWPSampleActorSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	if (HasAuthority())
	{
		GetWorld()->GetTimerManager().ClearTimer(SpawnTimerHandle);
	}
}

void AWPSampleActorSpawner::SpawnActor()
{
	if (!HasAuthority()) return;

	if (SpawnCount < 0)
	{
		GetWorld()->GetTimerManager().ClearTimer(SpawnTimerHandle);
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Actor = GetWorld()->SpawnActor<AActor>(
		SpawnActorClass,
		GetActorLocation(),
		Direction->GetComponentRotation(),
		Params);

	if (!IsValid(Actor)) return;

	if (bReplicated)
	{
		Actor->SetReplicates(true);
		Actor->SetReplicateMovement(true);
		Actor->ForceNetUpdate();
	}

	ApplyLaunchToActor(Actor);

	if (!bRepeat)
	{
		SpawnCount--;
	}
}

void AWPSampleActorSpawner::ApplyLaunchToActor(AActor* SpawnedActor) const
{
	if (!HasAuthority() || !IsValid(SpawnedActor)) return;

	const FVector LaunchVector = IsValid(Direction)
		? Direction->GetForwardVector() * Impulse
		: GetActorForwardVector();

	if (UPrimitiveComponent* LaunchPrimitive = Cast<UPrimitiveComponent>(SpawnedActor->GetRootComponent()))
	{
		if (bReplicated)
		{
			LaunchPrimitive->SetIsReplicated(true);
			LaunchPrimitive->bReplicatePhysicsToAutonomousProxy = true;
		}

		LaunchPrimitive->SetPhysicsLinearVelocity(LaunchVector);
		LaunchPrimitive->SetPhysicsAngularVelocityInRadians(LaunchVector.GetSafeNormal());
		LaunchPrimitive->WakeAllRigidBodies();
	}
}
