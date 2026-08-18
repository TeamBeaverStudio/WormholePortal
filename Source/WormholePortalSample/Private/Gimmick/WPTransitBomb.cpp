// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Gimmick/WPTransitBomb.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"
#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitTags.h"
#include "Transit/WPTransitTypes.h"

AWPTransitBomb::AWPTransitBomb()
{
	PrimaryActorTick.bCanEverTick = false;

	BombMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BombMesh"));
	SetRootComponent(BombMesh);
	BombMesh->SetRelativeScale3D(FVector(0.1f));
	BombMesh->SetMobility(EComponentMobility::Movable);
	BombMesh->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	BombMesh->SetGenerateOverlapEvents(true);
	BombMesh->SetSimulatePhysics(true);

	TransitComponent = CreateDefaultSubobject<UWPTransitComponent>(TEXT("TransitComponent"));
	TransitComponent->SetTransitType(EWPTransitType::Physics);

	DamageType = UDamageType::StaticClass();
}

void AWPTransitBomb::BeginPlay()
{
	Super::BeginPlay();

	if (ActorHasTag(WPTransitTags::Generated)) return;

	BombMesh->SetSimulatePhysics(true);

	if (ExplosionDelay <= 0.0f)
	{
		ExplosionTimer = GetWorldTimerManager().SetTimerForNextTick(this, &AWPTransitBomb::Explode);
		return;
	}

	GetWorldTimerManager().SetTimer(ExplosionTimer, this, &AWPTransitBomb::Explode, ExplosionDelay, false);
}

void AWPTransitBomb::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ExplosionTimer);
	Super::EndPlay(EndPlayReason);
}

void AWPTransitBomb::Explode()
{
	if (bExploded || ActorHasTag(WPTransitTags::Generated)) return;
	bExploded = true;

	TArray<FTransform, TInlineAllocator<2>> ExplosionTransforms;
	ExplosionTransforms.Add(GetActorTransform());

	if (IsValid(TransitComponent) && TransitComponent->GetPhase() == EWPTransitPhase::Crossing)
	{
		if (const AActor* Counterpart = TransitComponent->GetCounterPartActor(); IsValid(Counterpart))
		{
			ExplosionTransforms.Add(Counterpart->GetActorTransform());
		}
	}

	for (const FTransform& ExplosionTransform : ExplosionTransforms)
	{
		ExplodeAt(ExplosionTransform);
	}
	
	if (IsValid(TransitComponent))
	{
		TransitComponent->CancelTransit();
	}

	Destroy();
}

void AWPTransitBomb::ExplodeAt(const FTransform& ExplosionTransform)
{
	UWorld* World = GetWorld();
	if (!IsValid(World)) return;

	const FVector ExplosionLocation = ExplosionTransform.GetLocation();

	if (ExplosionEffectClass)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Owner = GetOwner();
		SpawnParameters.Instigator = GetInstigator();
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<AActor>(ExplosionEffectClass, ExplosionTransform, SpawnParameters);
	}

	if (ExplosionRadius > 0.0f)
	{
		FCollisionObjectQueryParams ObjectQueryParams;
		ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);
		ObjectQueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WPTransitBombExplosion), false, this);
		if (const AActor* Counterpart = TransitComponent->GetCounterPartActor(); IsValid(Counterpart))
		{
			QueryParams.AddIgnoredActor(Counterpart);
		}

		TArray<FOverlapResult> Overlaps;
		World->OverlapMultiByObjectType(
			Overlaps,
			ExplosionLocation,
			FQuat::Identity,
			ObjectQueryParams,
			FCollisionShape::MakeSphere(ExplosionRadius),
			QueryParams);

		TSet<AActor*> DamagedActors;
		for (const FOverlapResult& Overlap : Overlaps)
		{
			UPrimitiveComponent* HitComponent = Overlap.GetComponent();
			if (!IsValid(HitComponent)) continue;

			if (PhysicsImpulse > 0.0f && HitComponent->IsSimulatingPhysics())
			{
				HitComponent->AddRadialImpulse(
					ExplosionLocation,
					ExplosionRadius,
					PhysicsImpulse,
					ERadialImpulseFalloff::RIF_Constant,
					true);
			}

			AActor* HitActor = Overlap.GetActor();
			if (Damage <= 0.0f || !IsValid(HitActor) || DamagedActors.Contains(HitActor)) continue;

			DamagedActors.Add(HitActor);
			UGameplayStatics::ApplyDamage(
				HitActor,
				Damage,
				GetInstigatorController(),
				this,
				DamageType);
		}
	}

	OnExploded(ExplosionLocation);
}
