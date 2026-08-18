// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Gimmick/WPBeamTarget.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"

AWPBeamTarget::AWPBeamTarget()
{
	PrimaryActorTick.bCanEverTick = false;

	TargetMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TargetMesh"));
	SetRootComponent(TargetMesh);
	TargetMesh->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);

	SetCanBeDamaged(true);
}

void AWPBeamTarget::BeginPlay()
{
	Super::BeginPlay();

	CurrentHealth = FMath::Max(MaxHealth, 1.0f);
	OnHealthChanged(CurrentHealth, MaxHealth);
}

float AWPBeamTarget::TakeDamage(
	const float DamageAmount,
	const FDamageEvent& DamageEvent,
	AController* EventInstigator,
	AActor* DamageCauser)
{
	if (bExploded || CurrentHealth <= 0.0f || DamageAmount <= 0.0f) return 0.0f;

	const float ActualDamage = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (ActualDamage <= 0.0f) return 0.0f;

	const float AppliedDamage = FMath::Min(CurrentHealth, ActualDamage);
	CurrentHealth -= AppliedDamage;
	OnHealthChanged(CurrentHealth, MaxHealth);

	if (CurrentHealth <= 0.0f)
	{
		bExploded = true;
		OnExploded();
		Destroy();
	}

	return AppliedDamage;
}
