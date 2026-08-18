// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Gimmick/WPGrabComponent.h"

#include "DrawDebugHelpers.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Subsystem/WPTransitSubsystem.h"
#include "Transit/WPTransitTypes.h"

UWPGrabComponent::UWPGrabComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UWPGrabComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (UWPTransitSubsystem* TransitSubsystem = World->GetSubsystem<UWPTransitSubsystem>())
		{
			TransitStartedHandle = TransitSubsystem->OnTransitStarted().AddUObject(
				this, &UWPGrabComponent::HandleTransitStarted);
		}
	}
}

bool UWPGrabComponent::GrabActor()
{
	if (IsValid(GetGrabbedComponent())) return false;

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	FVector ViewLocation;
	FRotator ViewRotation;
	if (World == nullptr || !IsValid(Owner) || !GetView(ViewLocation, ViewRotation)) return false;

	FCollisionObjectQueryParams ObjectQuery;
	ObjectQuery.AddObjectTypesToQuery(ECC_PhysicsBody);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WPGrab), false, Owner);
	FHitResult Hit;
	const FVector TraceEnd = ViewLocation + ViewRotation.Vector() * GrabRange;
	const bool bHit = World->LineTraceSingleByObjectType(Hit, ViewLocation, TraceEnd, ObjectQuery, QueryParams);
	DrawDebugLine(World, ViewLocation, TraceEnd, FColor::Green, false, 5.0f);
	if (!bHit) return false;

	UPrimitiveComponent* HitComponent = Hit.GetComponent();
	if (!IsValid(HitComponent) || !HitComponent->IsSimulatingPhysics(Hit.BoneName)) return false;

	GrabComponentAtLocationWithRotation(
		HitComponent,
		Hit.BoneName,
		HitComponent->GetComponentLocation(),
		HitComponent->GetComponentRotation());

	SetTargetLocation(ViewLocation + ViewRotation.Vector() * HoldDistance);
	return true;
}

void UWPGrabComponent::DropActor()
{
	ReleaseComponent();
}

void UWPGrabComponent::HandleTransitStarted(const FWPTransitEvent& Event)
{
	UPrimitiveComponent* HeldComponent = GetGrabbedComponent();
	if (!IsValid(HeldComponent) || HeldComponent->GetOwner() != Event.Actor.Get()) return;

	const auto StopHorizontalMovement = [](UPrimitiveComponent* Primitive)
	{
		if (!IsValid(Primitive) || !Primitive->IsSimulatingPhysics()) return;

		const FVector Velocity = Primitive->GetPhysicsLinearVelocity();
		Primitive->SetPhysicsLinearVelocity(
			FVector(0.0, 0.0, Velocity.Z),
			false);
	};

	ReleaseComponent();
	StopHorizontalMovement(HeldComponent);

	AActor* TwinActor = Event.TwinActor.Get();
	if (!IsValid(TwinActor)) return;

	TInlineComponentArray<UPrimitiveComponent*> TwinPrimitives(TwinActor);
	for (UPrimitiveComponent* TwinPrimitive : TwinPrimitives)
	{
		if (!IsValid(TwinPrimitive) || TwinPrimitive->GetFName() != HeldComponent->GetFName() || TwinPrimitive->GetClass() != HeldComponent->GetClass())
		{
			continue;
		}

		StopHorizontalMovement(TwinPrimitive);
		break;
	}
}

void UWPGrabComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (IsValid(GetGrabbedComponent()))
	{
		FVector ViewLocation;
		FRotator ViewRotation;
		if (GetView(ViewLocation, ViewRotation))
		{
			SetTargetLocation(ViewLocation + ViewRotation.Vector() * HoldDistance);
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UWPGrabComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TransitStartedHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			if (UWPTransitSubsystem* TransitSubsystem = World->GetSubsystem<UWPTransitSubsystem>())
			{
				TransitSubsystem->OnTransitStarted().Remove(TransitStartedHandle);
			}
		}
		TransitStartedHandle.Reset();
	}

	DropActor();
	Super::EndPlay(EndPlayReason);
}

bool UWPGrabComponent::GetView(FVector& OutLocation, FRotator& OutRotation) const
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner)) return false;

	if (const APawn* Pawn = Cast<APawn>(Owner))
	{
		if (const APlayerController* PlayerController = Cast<APlayerController>(Pawn->GetController()))
		{
			PlayerController->GetPlayerViewPoint(OutLocation, OutRotation);
			return true;
		}
	}

	Owner->GetActorEyesViewPoint(OutLocation, OutRotation);
	return true;
}
