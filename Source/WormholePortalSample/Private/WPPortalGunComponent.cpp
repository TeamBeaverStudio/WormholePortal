// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPPortalGunComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Math/RotationMatrix.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "WPSettings.h"
#include "WormholePortalRuntime/Public/Trace/WPTraceLibrary.h"

UWPPortalGunComponent::UWPPortalGunComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	
	PortalClass = AWormholePortalActor::StaticClass();
}

void UWPPortalGunComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateGrowthAnimation(DeltaTime);
}

void UWPPortalGunComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearPortals();
	
	Super::EndPlay(EndPlayReason);
}

bool UWPPortalGunComponent::FirePortalA(FVector TargetPosition)
{
	if (!bCanSpawnPortalA) return false;
	return FirePortal(TargetPosition, PortalA, bOwnsPortalA);
}

bool UWPPortalGunComponent::FirePortalB(FVector TargetPosition)
{
	if (!bCanSpawnPortalB) return false;
	return FirePortal(TargetPosition, PortalB, bOwnsPortalB);
}

bool UWPPortalGunComponent::SetPortalA(AWormholePortalActor* NewPortal)
{
	AWormholePortalActor* Other = IsValid(PortalB.Get()) ? PortalB.Get() : nullptr;
	return AssignPortal(NewPortal, PortalA, bOwnsPortalA, Other);
}

bool UWPPortalGunComponent::SetPortalB(AWormholePortalActor* NewPortal)
{
	AWormholePortalActor* Other = IsValid(PortalA.Get()) ? PortalA.Get() : nullptr;
	return AssignPortal(NewPortal, PortalB, bOwnsPortalB, Other);
}

bool UWPPortalGunComponent::HasPortalA() const
{
	return IsValid(PortalA.Get());
}

bool UWPPortalGunComponent::HasPortalB() const
{
	return IsValid(PortalB.Get());
}

void UWPPortalGunComponent::CyclePortalSpawnDirection(const float WheelDelta)
{
	if (FMath::IsNearlyZero(WheelDelta)) return;
	
	const int32 Count = static_cast<int32>(EWPPortalSpawnDirection::Count);
	const int32 Index = FMath::Clamp(static_cast<int32>(PortalSpawnDirection), 0, Count - 1);
	const int32 Step = WheelDelta > 0.0f ? 1 : -1;
	PortalSpawnDirection = static_cast<EWPPortalSpawnDirection>((Index + Step + Count) % Count);
}

FText UWPPortalGunComponent::GetPortalSpawnDirectionText() const
{
	return UEnum::GetDisplayValueAsText(PortalSpawnDirection);
}

void UWPPortalGunComponent::ClearPortals()
{
	ReleasePortal(PortalA, bOwnsPortalA);
	ReleasePortal(PortalB, bOwnsPortalB);
	GrowthElapsedSeconds = 0.0f;
	SetComponentTickEnabled(false);
}

bool UWPPortalGunComponent::FirePortal(const FVector& TargetPosition, TObjectPtr<AWormholePortalActor>& Slot, bool& bOwned)
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	
	if (!IsValid(Owner) || World == nullptr || PortalClass == nullptr) return false;
	
	const FVector TraceStart = Owner->GetActorLocation();
	const FVector ToTarget = TargetPosition - TraceStart;
	const float Distance = ToTarget.Size();
	if (Distance <= KINDA_SMALL_NUMBER) return false;
	
	const float TraceDistance = FMath::Min(Distance, FMath::Max(MaxPlacementDistance, 1.0f));
	const FVector TraceDirection = ToTarget / Distance;
	const FVector TraceEnd = TraceStart + TraceDirection * TraceDistance;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WPPortalGunPlacement), false, Owner);
	
	if (AActor* OwnerOfOwner = Owner->GetOwner())
	{
		QueryParams.AddIgnoredActor(OwnerOfOwner);
	}
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	
	FHitResult HitResult;
	const bool bHit = World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, QueryParams);
	DrawDebugLine(World, TraceStart, TraceEnd, FColor::Red, false, 5.0f);
	if (!bHit) return false;
	
	const FVector SurfaceNormal = HitResult.ImpactNormal.GetSafeNormal();
	if (SurfaceNormal.IsNearlyZero()) return false;
	
	const FVector SpawnLocation = HitResult.ImpactPoint + SurfaceNormal * PortalRadius / 2.f;
	const FVector SpawnDirection = ToWorldVector(PortalSpawnDirection);
	const FRotator SpawnRotation = FRotationMatrix::MakeFromX(SpawnDirection).Rotator();
	
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Owner;
	SpawnParams.Instigator = Owner->GetInstigator();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWormholePortalActor* NewPortal = World->SpawnActor<AWormholePortalActor>(PortalClass.Get(), SpawnLocation, SpawnRotation, SpawnParams);
	
	if (!IsValid(NewPortal)) return false;
	
	NewPortal->SetActorScale3D(FVector::OneVector);
	
	InitializeFinalPhysicalMetric(NewPortal);
	
	ReleasePortal(Slot, bOwned);
	Slot = NewPortal;
	bOwned = true;
	
	LinkAndStartGrowth();
	return true;
}

bool UWPPortalGunComponent::AssignPortal(AWormholePortalActor* NewPortal, TObjectPtr<AWormholePortalActor>& Slot, bool& bOwned, AWormholePortalActor* Other)
{
	if (NewPortal != nullptr && !IsValid(NewPortal)) return false;
	if (NewPortal == Slot.Get()) return true;
	if (NewPortal != nullptr && (NewPortal == Other || NewPortal->GetWorld() != GetWorld())) return false;
	
	ReleasePortal(Slot, bOwned);
	if (IsValid(NewPortal) && NewPortal->GetLinkedPortal() != Other) NewPortal->ClearLinkedPortal();
	
	Slot = NewPortal;
	bOwned = false;
	LinkAndStartGrowth();
	return true;
}

void UWPPortalGunComponent::LinkAndStartGrowth()
{
	#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	#endif
	AWormholePortalActor* A = PortalA.Get();
	AWormholePortalActor* B = PortalB.Get();
	
	if (!IsValid(A) || !IsValid(B))
	{
		GrowthElapsedSeconds = 0.0f;
		SetComponentTickEnabled(false);
		return;
	}
	
	InitializeFinalPhysicalMetric(A);
	InitializeFinalPhysicalMetric(B);
	if (A->GetLinkedPortal() != B) A->SetLinkedPortal(B);

	const float InitialScale = FMath::Clamp(InitialGrowthScale, 0.01f, 1.0f);
	const bool bShouldGrow = GrowthDuration > KINDA_SMALL_NUMBER && InitialScale < 1.0f;
	const float StartScale = bShouldGrow ? InitialScale : 1.0f;
	ApplyGrowthScale(A, StartScale);
	
	GrowthElapsedSeconds = 0.0f;
	SetComponentTickEnabled(bShouldGrow);
	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[PortalGun][Growth] Growth initialized. PortalA=%s PortalB=%s Mode=%s InitialScale=%.4f DurationSeconds=%.3f TickEnabled=%d FinalMetric=(R=%.3f,A=%.3f,T=%.3f) CpuMs=%.4f"),
		*GetNameSafe(A), *GetNameSafe(B),
		GrowthMode == EWPPortalGrowthMode::VisualOnly ? TEXT("VisualOnly") : TEXT("PhysicalMetric"),
		StartScale, GrowthDuration, bShouldGrow ? 1 : 0,
		PortalRadius, ThroatHalfLength, TransitionLength,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif
}

void UWPPortalGunComponent::UpdateGrowthAnimation(const float DeltaTime)
{
	AWormholePortalActor* A = PortalA.Get();
	AWormholePortalActor* B = PortalB.Get();
	
	if (!IsValid(A) || !IsValid(B) || A->GetLinkedPortal() != B)
	{
		GrowthElapsedSeconds = 0.0f;
		SetComponentTickEnabled(false);
		return;
	}
	
	GrowthElapsedSeconds += FMath::Max(DeltaTime, 0.0f);
	
	const float LinearAlpha = FMath::Clamp(GrowthElapsedSeconds / FMath::Max(GrowthDuration, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
	const float EasedAlpha = LinearAlpha >= 1.0f ? 1.0f : FMath::Clamp(EaseGrowth(LinearAlpha), 0.0f, 1.0f);
	const float StartScale = FMath::Clamp(InitialGrowthScale, 0.01f, 1.0f);
	ApplyGrowthScale(A, FMath::Lerp(StartScale, 1.0f, EasedAlpha));
	
	if (LinearAlpha >= 1.0f) SetComponentTickEnabled(false);
}

float UWPPortalGunComponent::EaseGrowth(const float LinearAlpha)
{
	const float X = FMath::Clamp(LinearAlpha, 0.0f, 1.0f);
	return 1.0f - FMath::Pow(1.0f - X, 5.0f);
}

FVector UWPPortalGunComponent::ToWorldVector(const EWPPortalSpawnDirection Direction)
{
	switch (Direction)
	{
	case EWPPortalSpawnDirection::PositiveX: return FVector::ForwardVector;
	case EWPPortalSpawnDirection::NegativeX: return FVector::BackwardVector;
	case EWPPortalSpawnDirection::PositiveY: return FVector::RightVector;
	case EWPPortalSpawnDirection::NegativeY: return FVector::LeftVector;
	case EWPPortalSpawnDirection::PositiveZ: return FVector::UpVector;
	case EWPPortalSpawnDirection::NegativeZ: return FVector::DownVector;
	default: return FVector::ForwardVector;
	}
}

void UWPPortalGunComponent::InitializeFinalPhysicalMetric(
	AWormholePortalActor* Portal) const
{
	if (!IsValid(Portal)) return;
	Portal->InitializePhysicalMetric(PortalRadius, ThroatHalfLength, TransitionLength);
}

void UWPPortalGunComponent::ApplyGrowthScale(
	AWormholePortalActor* Portal,
	const float Scale) const
{
	if (!IsValid(Portal)) return;
	
	const float SafeScale = FMath::Clamp(Scale, 0.01f, 1.0f);
	if (GrowthMode == EWPPortalGrowthMode::VisualOnly)
	{
		// Recommended path: the final physical Metric is already established, so this
		// per-frame call changes only compositor parameters and render-packet publication.
		Portal->SetPortalVisualScale(SafeScale);
		return;
	}

	// Explicit higher-cost path: collision, bounds, occlusion-query eligibility, and
	// dynamic capture resolution follow the animated physical Metric.
	Portal->SetPortalVisualScale(1.0f);
	Portal->SetUniformPhysicalMetricScale(SafeScale);
}

void UWPPortalGunComponent::ReleasePortal(TObjectPtr<AWormholePortalActor>& Portal, bool& bOwned)
{
	if (IsValid(Portal.Get()))
	{
		if (bOwned) Portal->Destroy();
		else Portal->ClearLinkedPortal();
	}
	
	Portal = nullptr;
	bOwned = false;
}
