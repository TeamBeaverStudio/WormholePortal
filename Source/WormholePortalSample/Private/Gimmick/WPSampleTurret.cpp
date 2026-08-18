// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Gimmick/WPSampleTurret.h"

#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "Gimmick/WPBeamTarget.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"
#include "Trace/WPTraceLibrary.h"

AWPSampleTurret::AWPSampleTurret()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	TurretMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TurretMesh"));
	TurretMesh->SetupAttachment(SceneRoot);
	TurretMesh->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);

	Muzzle = CreateDefaultSubobject<UArrowComponent>(TEXT("Muzzle"));
	Muzzle->SetupAttachment(TurretMesh);
	Muzzle->SetArrowColor(FColor::Red);
}

void AWPSampleTurret::BeginPlay()
{
	Super::BeginPlay();

	if (const UWorld* World = GetWorld())
	{
		LastBeamUpdateSeconds = World->GetTimeSeconds();
	}

	UpdateBeam();
	GetWorldTimerManager().SetTimer(
		BeamUpdateTimer,
		this,
		&AWPSampleTurret::UpdateBeam,
		BeamUpdateInterval,
		true);
}

void AWPSampleTurret::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(BeamUpdateTimer);
	HideUnusedBeamSegments(0);
	Super::EndPlay(EndPlayReason);
}

void AWPSampleTurret::UpdateBeam()
{
	UWorld* World = GetWorld();
	if (!IsValid(World) || !IsValid(Muzzle))
	{
		HideUnusedBeamSegments(0);
		ContinuousHitSeconds = 0.0f;
		return;
	}

	const double CurrentSeconds = World->GetTimeSeconds();
	const float ElapsedSeconds = static_cast<float>(FMath::Max(CurrentSeconds - LastBeamUpdateSeconds, 0.0));
	LastBeamUpdateSeconds = CurrentSeconds;

	const FVector TraceStart = Muzzle->GetComponentLocation();
	const FVector InitialDirection = Muzzle->GetForwardVector();
	const FVector TraceEnd = TraceStart + InitialDirection * TraceDistance;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WPSampleTurret), false, this);
	FWPPortalTraceResult TraceResult;
	const bool bBlockingHit = UWPTraceLibrary::PortalLineTraceSingleByChannel(
		this,
		TraceResult,
		TraceStart,
		TraceEnd,
		TraceChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam,
		MaxPortalDepth,
		PortalExitOffset);

	if (TraceResult.Status == EWPPortalTraceStatus::InvalidInput)
	{
		HideUnusedBeamSegments(0);
		UpdateTargetDamage(nullptr, ElapsedSeconds);
		return;
	}

	const bool bRenderBeam = IsValid(BeamTemplate);
	int32 UsedSegmentCount = 0;
	FVector SegmentStart = TraceStart;
	FVector SegmentDirection = InitialDirection;
	float SegmentLogicalStart = 0.0f;

	for (const FWPPortalTracePortalEvent& PortalEvent : TraceResult.PortalEvents)
	{
		if (bRenderBeam)
		{
			SetBeamSegment(UsedSegmentCount++, SegmentStart, PortalEvent.DetectionHit.ImpactPoint);
		}

		if (PortalEvent.Outcome != EWPPortalTracePortalOutcome::Traversed)
		{
			HideUnusedBeamSegments(UsedSegmentCount);
			UpdateTargetDamage(nullptr, ElapsedSeconds);
			return;
		}

		SegmentStart = PortalEvent.ExitTraceStart;
		SegmentDirection = PortalEvent.ExitDirection;
		SegmentLogicalStart = PortalEvent.LogicalDistance;
	}

	AActor* HitActor = nullptr;
	if (TraceResult.Status == EWPPortalTraceStatus::Completed)
	{
		FVector SegmentEnd;
		if (bBlockingHit && !TraceResult.SceneHits.IsEmpty())
		{
			const FHitResult& FinalHit = TraceResult.SceneHits.Last().Hit;
			SegmentEnd = FinalHit.ImpactPoint;
			HitActor = FinalHit.GetActor();
		}
		else
		{
			const float RemainingDistance = FMath::Max(TraceDistance - SegmentLogicalStart, 0.0f);
			SegmentEnd = SegmentStart + SegmentDirection * RemainingDistance;
		}

		if (bRenderBeam)
		{
			SetBeamSegment(UsedSegmentCount++, SegmentStart, SegmentEnd);
		}
	}

	HideUnusedBeamSegments(UsedSegmentCount);
	UpdateTargetDamage(HitActor, ElapsedSeconds);
}

void AWPSampleTurret::SetBeamSegment(const int32 SegmentIndex, const FVector& Start, const FVector& End)
{
	UParticleSystemComponent* BeamComponent = GetOrCreateBeamSegment(SegmentIndex);
	if (!IsValid(BeamComponent)) return;

	BeamComponent->SetWorldLocation(Start);
	BeamComponent->SetVisibility(true);
	if (!BeamComponent->IsActive())
	{
		BeamComponent->ActivateSystem(true);
	}

	BeamComponent->SetBeamSourcePoint(0, Start, 0);
	BeamComponent->SetBeamTargetPoint(0, End, 0);
}

void AWPSampleTurret::HideUnusedBeamSegments(const int32 UsedSegmentCount)
{
	for (int32 SegmentIndex = UsedSegmentCount; SegmentIndex < BeamSegments.Num(); ++SegmentIndex)
	{
		UParticleSystemComponent* BeamComponent = BeamSegments[SegmentIndex];
		if (!IsValid(BeamComponent)) continue;

		BeamComponent->SetVisibility(false);
		BeamComponent->DeactivateSystem();
	}
}

UParticleSystemComponent* AWPSampleTurret::GetOrCreateBeamSegment(const int32 SegmentIndex)
{
	if (SegmentIndex < 0 || !IsValid(BeamTemplate)) return nullptr;

	while (BeamSegments.Num() <= SegmentIndex)
	{
		UParticleSystemComponent* BeamComponent = NewObject<UParticleSystemComponent>(this);
		if (!IsValid(BeamComponent)) return nullptr;

		BeamComponent->SetupAttachment(SceneRoot);
		BeamComponent->SetTemplate(BeamTemplate);
		BeamComponent->SetAutoActivate(false);
		AddInstanceComponent(BeamComponent);
		BeamComponent->RegisterComponent();
		BeamSegments.Add(BeamComponent);
	}

	return BeamSegments[SegmentIndex];
}

void AWPSampleTurret::UpdateTargetDamage(AActor* HitActor, const float ElapsedSeconds)
{
	if (!IsValid(DamageTarget) || HitActor != DamageTarget || DamagePerSecond <= 0.0f)
	{
		ContinuousHitSeconds = 0.0f;
		return;
	}

	ContinuousHitSeconds += ElapsedSeconds;
	while (ContinuousHitSeconds >= DamageInterval && IsValid(DamageTarget))
	{
		ContinuousHitSeconds -= DamageInterval;
		UGameplayStatics::ApplyDamage(
			DamageTarget,
			DamagePerSecond,
			GetInstigatorController(),
			this,
			nullptr);
	}
}
