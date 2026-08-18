// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Transit/WPTransitTypeResolver.h"

#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitTags.h"
#include "Transit/Handler/WPPhysicsHandler.h"

FWPTransitResolveResult FWPTransitTypeResolver::Resolve(const AActor* Actor, const UWPTransitComponent* TransitComponent, EWPTransitType RequestedType)
{
	if (!IsValid(Actor))
	{
		return MakeResult(RequestedType, EWPTransitResolveStatus::NotSupported, EWPTransitResolveFailReason::UnsupportedActor);
	}
	if (Actor->ActorHasTag(WPTransitTags::Ignore))
	{
		return MakeResult(RequestedType, EWPTransitResolveStatus::NotSupported, EWPTransitResolveFailReason::UnsupportedActor);
	}
	if (Actor->ActorHasTag(WPTransitTags::Generated))
	{
		return MakeResult(RequestedType, EWPTransitResolveStatus::NotSupported, EWPTransitResolveFailReason::UnsupportedActor);
	}

	const EWPTransitType TransitType = (RequestedType == EWPTransitType::Auto ? DetectType(Actor) : RequestedType);

	// Auto는 Actor의 현재 구성으로 Type을 판정, 명시적으로 지정한 종류는 다른 종류로 대체하지 않는다. 
	// 잘못 지정된 값은 해당 종류의 NeedsSetup 결과로 그대로 보고.
	FWPTransitResolveResult Result;
	switch (TransitType)
	{
	case EWPTransitType::Character:
		Result = ResolveCharacter(Actor, TransitComponent);
		break;
	case EWPTransitType::Projectile:
		Result = ResolveProjectile(Actor, TransitComponent);
		break;
	case EWPTransitType::Pawn:
		Result = ResolvePawn(Actor, TransitComponent);
		break;
	case EWPTransitType::Physics:
		Result = ResolvePhysics(Actor, TransitComponent);
		break;
	default:
		Result = MakeResult(EWPTransitType::Auto, EWPTransitResolveStatus::NotSupported, EWPTransitResolveFailReason::UnsupportedActor);
		break;
	}
	
	if (Result.IsPassed())
	{
		bool bHasOverlapPart = false;
		for (UPrimitiveComponent* Part : Result.TransitPrimitives)
		{
			if (CanPortalOverlap(Part))
			{
				bHasOverlapPart = true;
				break;
			}
		}
		
		if (!bHasOverlapPart)
		{
			Result.Status = EWPTransitResolveStatus::NeedsSetup;
			Result.FailReason = EWPTransitResolveFailReason::NoOverlapPart;
			Result.FailedComponents.Reset();
			
			for (UPrimitiveComponent* Part : Result.TransitPrimitives)
			{
				if (IsValid(Part))
				{
					Result.FailedComponents.Add(Part);
				}
			}
		}
	}
	
	if (Result.IsPassed() && IsValid(TransitComponent) && !TransitComponent->GetTransitEnabled())
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::TransitDisabled;
	}
	return Result;
}

EWPTransitType FWPTransitTypeResolver::DetectType(const AActor* Actor)
{
	// 상위 개념부터 검사하여 Character가 Pawn으로, Projectile이 일반 Actor로 판정되는 것을 막습니다.
	if (Actor->IsA<ACharacter>()) return EWPTransitType::Character;
	
	if (Actor->FindComponentByClass<UProjectileMovementComponent>() != nullptr)
	{
		return EWPTransitType::Projectile;
	}
	if (Actor->IsA<APawn>()) return EWPTransitType::Pawn;

	TInlineComponentArray<UPrimitiveComponent*> Parts(Actor);
	for (UPrimitiveComponent* Part : Parts)
	{
		if (CanUsePart(Actor, Part) && IsPhysicsBody(Part))
		{
			return EWPTransitType::Physics;
		}
	}

	return EWPTransitType::Auto;
}

FWPTransitResolveResult FWPTransitTypeResolver::ResolveCharacter(const AActor* Actor, const UWPTransitComponent* TransitComponent)
{
	const ACharacter* Character = Cast<ACharacter>(Actor);
	if (!IsValid(Character))
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::TypeMismatch);
	}
	
	UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	
	// Check Failure Status
	// Invalid Movement Component
	if (!IsValid(MoveComp))
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::MissingMovement);
	}
	
	// Invalid Part
	const EWPTransitResolveFailReason PartFail = GetPartFail(Actor, Capsule);
	if (PartFail != EWPTransitResolveFailReason::None)
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, PartFail, Capsule);
	}
	
	// A visual Mesh is optional, but Ragdoll cannot use the Character Movement Transit path.
	if (IsValid(Mesh) && Mesh->IsSimulatingPhysics())
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::SimPhysics, Mesh);
	}
	
	// MoveComp's Owner isn't Character 
	if (MoveComp->GetCharacterOwner() != Character)
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidMoveOwner, MoveComp);
	}
	
	// MoveComp's Update Component isn't Capsule
	if (MoveComp->UpdatedComponent != Capsule)
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidUpdatedPart, MoveComp);
	}
	
	// InValid Character Root Component
	if (Character->GetRootComponent() != Capsule)
	{
		return MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidRootPart, Character->GetRootComponent());
	}

	FWPTransitResolveResult Result = MakeResult(EWPTransitType::Character, EWPTransitResolveStatus::Passed, EWPTransitResolveFailReason::None);
	Result.TransitPrimitives.Add(Capsule);
	GatherTransitStaticMeshes(Actor, OUT Result.TransitPrimitives);
	Result.MovementComponent = MoveComp;
	
	if (!HasVoxelData(TransitComponent, Result.TransitPrimitives, OUT Result.FailedComponents))
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::MissingVoxelData;
	}
	
	return Result;
}

FWPTransitResolveResult FWPTransitTypeResolver::ResolveProjectile(const AActor* Actor, const UWPTransitComponent* TransitComponent)
{
	// Resolver는 이전 캐시를 재사용하지 않고 현재 Actor 구성을 검사
	UProjectileMovementComponent* MoveComp = Actor->FindComponentByClass<UProjectileMovementComponent>();
	
	// Projectile Transit requires a valid Projectile Movement Component.
	if (!IsValid(MoveComp))
	{
		return MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::MissingMovement);
	}
	
	// Validate the Movement Component before inspecting the Primitive referenced by it.
	if (MoveComp->GetOwner() != Actor)
	{
		return MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidMoveOwner, MoveComp);
	}
	
	// Updated Component Invalid Check
	UPrimitiveComponent* MoveRoot = Cast<UPrimitiveComponent>(MoveComp->UpdatedComponent);
	if (!IsValid(MoveRoot))
	{
		return MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidUpdatedPart, MoveComp);
	}
	
	// Failed Part Check
	const EWPTransitResolveFailReason PartFail = GetPartFail(Actor, MoveRoot);
	if (PartFail != EWPTransitResolveFailReason::None)
	{
		return MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::NeedsSetup, PartFail, MoveRoot);
	}
	
	// Root Simulating Physics
	if (MoveRoot->IsSimulatingPhysics())
	{
		return MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::SimPhysics, MoveRoot);
	}

	FWPTransitResolveResult Result = MakeResult(EWPTransitType::Projectile, EWPTransitResolveStatus::Passed, EWPTransitResolveFailReason::None);
	Result.TransitPrimitives.Add(MoveRoot);
	GatherTransitStaticMeshes(Actor, OUT Result.TransitPrimitives);
	Result.MovementComponent = MoveComp;
	
	if (!HasVoxelData(TransitComponent, Result.TransitPrimitives, OUT Result.FailedComponents))
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::MissingVoxelData;
	}
	return Result;
}

FWPTransitResolveResult FWPTransitTypeResolver::ResolvePawn(const AActor* Actor, const UWPTransitComponent* TransitComponent)
{
	const APawn* Pawn = Cast<APawn>(Actor);
	if (!IsValid(Pawn) || Pawn->IsA<ACharacter>())
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::TypeMismatch);
	}
	// Resolver는 이전 캐시를 재사용하지 않고 현재 Pawn 구성을 검사합니다.
	UPawnMovementComponent* MoveComp = Pawn->GetMovementComponent();
	
	// Pawn Transit requires a valid non-Character Pawn Movement Component.
	if (!IsValid(MoveComp))
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::MissingMovement);
	}
	
	// Character Movement is handled only by the Character Handler.
	if (MoveComp->IsA<UCharacterMovementComponent>())
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidMoveType, MoveComp);
	}

	// The Pawn Movement must belong to the Pawn being resolved.
	if (MoveComp->GetPawnOwner() != Pawn)
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidMoveOwner, MoveComp);
	}

	// Invalid UpdatedComponent
	UPrimitiveComponent* MoveRoot = Cast<UPrimitiveComponent>(MoveComp->UpdatedComponent);
	if (!IsValid(MoveRoot))
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidUpdatedPart, MoveComp);
	}
	
	// Failed Part
	const EWPTransitResolveFailReason PartFail = GetPartFail(Actor, MoveRoot);
	if (PartFail != EWPTransitResolveFailReason::None)
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, PartFail, MoveRoot);
	}
	
	// Updated Component isn't Pawn Root Component
	if (MoveRoot != Pawn->GetRootComponent())
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::InvalidRootPart, Pawn->GetRootComponent());
	}

	// Pawn Simulating Physics
	if (MoveRoot->IsSimulatingPhysics())
	{
		return MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::NeedsSetup, EWPTransitResolveFailReason::SimPhysics, MoveRoot);
	}
	
	FWPTransitResolveResult Result = MakeResult(EWPTransitType::Pawn, EWPTransitResolveStatus::Passed, EWPTransitResolveFailReason::None);
	Result.TransitPrimitives.Add(MoveRoot);
	GatherTransitStaticMeshes(Actor, OUT Result.TransitPrimitives);
	Result.MovementComponent = MoveComp;
	
	if (!HasVoxelData(TransitComponent, Result.TransitPrimitives, OUT Result.FailedComponents))
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::MissingVoxelData;
	}
	return Result;
}

FWPTransitResolveResult FWPTransitTypeResolver::ResolvePhysics(const AActor* Actor, const UWPTransitComponent* TransitComponent)
{
	FWPTransitResolveResult Result = MakeResult(EWPTransitType::Physics, EWPTransitResolveStatus::Passed, EWPTransitResolveFailReason::None);
	
	TInlineComponentArray<UPrimitiveComponent*> Parts(Actor);
	bool bHasPhysicsBody = false;
	
	for (UPrimitiveComponent* Part : Parts)
	{
		if (!CanUsePart(Actor, Part)) continue;
		
		Result.TransitPrimitives.Add(Part);
		
		if (IsPhysicsBody(Part))
		{
			bHasPhysicsBody = true;
		}
	}
	if (Result.TransitPrimitives.IsEmpty())
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::MissingPrimitives;
	}
	else if (!bHasPhysicsBody)
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::NoPhysicsBody;
		
		for (UPrimitiveComponent* Part : Result.TransitPrimitives)
		{
			if (IsValid(Part))
			{
				Result.FailedComponents.Add(Part);
			}
		}
	}
	else if (!HasVoxelData(TransitComponent, Result.TransitPrimitives, OUT Result.FailedComponents))
	{
		Result.Status = EWPTransitResolveStatus::NeedsSetup;
		Result.FailReason = EWPTransitResolveFailReason::MissingVoxelData;
	}
	return Result;
}

FWPTransitResolveResult FWPTransitTypeResolver::MakeResult(EWPTransitType TransitType, EWPTransitResolveStatus Status, 
	EWPTransitResolveFailReason FailReason, UActorComponent* FailedComponent)
{
	FWPTransitResolveResult Result;
	Result.TransitType = TransitType;
	Result.Status = Status;
	Result.FailReason = FailReason;
	if (IsValid(FailedComponent)) Result.FailedComponents.Add(FailedComponent);
	return Result;
}

EWPTransitResolveFailReason FWPTransitTypeResolver::GetPartFail(const AActor* Actor, const UPrimitiveComponent* Comp)
{
	if (!IsValid(Actor)) return EWPTransitResolveFailReason::UnsupportedActor;
	if (!IsValid(Comp)) return EWPTransitResolveFailReason::MissingPrimitives;
	if (Comp->GetOwner() != Actor) return EWPTransitResolveFailReason::InvalidPartOwner;
	if (Comp->GetMobility() != EComponentMobility::Movable) return EWPTransitResolveFailReason::PartNotMovable;
	if (Comp->GetCollisionEnabled() == ECollisionEnabled::NoCollision) return EWPTransitResolveFailReason::PartNoCollision;
	if (Comp->IsA<UInstancedStaticMeshComponent>()) return EWPTransitResolveFailReason::UnsupportedPart;
	if (Comp->ComponentHasTag(WPTransitTags::Generated) || Comp->ComponentHasTag(WPTransitTags::Ignore)) return EWPTransitResolveFailReason::ExcludedPart;
	
#if WITH_EDITORONLY_DATA
	if (Comp->IsEditorOnly() || Comp->IsVisualizationComponent())
	{
		return EWPTransitResolveFailReason::ExcludedPart;
	}
#endif
	return EWPTransitResolveFailReason::None;
}

bool FWPTransitTypeResolver::CanUsePart(const AActor* Actor, const UPrimitiveComponent* Component)
{
	return GetPartFail(Actor, Component) == EWPTransitResolveFailReason::None;
}

void FWPTransitTypeResolver::GatherTransitStaticMeshes(const AActor* Actor, TArray<TObjectPtr<UPrimitiveComponent>>& InOutParts)
{
	if (!IsValid(Actor)) return;

	TArray<UStaticMeshComponent*> StaticMeshes;
	Actor->GetComponents<UStaticMeshComponent>(StaticMeshes);
	for (UStaticMeshComponent* StaticMesh : StaticMeshes)
	{
		// Template로 생성된 Twin과 같은 이름의 Component를 연결 대상으로 사용합니다.
		if (!CanUsePart(Actor, StaticMesh) || !IsValid(StaticMesh->GetStaticMesh()))
		{
			continue;
		}
		InOutParts.AddUnique(StaticMesh);
	}
}

bool FWPTransitTypeResolver::HasVoxelData(const UWPTransitComponent* TransitComponent, const TArray<TObjectPtr<UPrimitiveComponent>>& Parts,
	TArray<TWeakObjectPtr<UActorComponent>>& OutFailedComponents)
{
	OutFailedComponents.Reset();
	
	// Editor가 Component를 부착하기 전에는 Bake 대상만 판정하므로 데이터 유무를 검사할 수 없습니다.
	if (!IsValid(TransitComponent) ||!TransitComponent->IsVoxelCollisionEnabled())
	{
		return true;
	}
	
	for (UPrimitiveComponent* Part : Parts)
	{
		if (!IsValid(Part)) continue;
		
		const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Part);
		const bool bSupportedStaticMesh = IsValid(StaticMesh) && !StaticMesh->IsA<UInstancedStaticMeshComponent>() && IsValid(StaticMesh->GetStaticMesh());
		
		const bool bSupportedShape = Part->IsA<UBoxComponent>() || Part->IsA<USphereComponent>() || Part->IsA<UCapsuleComponent>();
		
		if ((bSupportedStaticMesh || bSupportedShape) && TransitComponent->FindVoxelData(Part) == nullptr)
		{
			OutFailedComponents.Add(Part);
		}
		
	}
	return OutFailedComponents.IsEmpty();
}

bool FWPTransitTypeResolver::CanPortalOverlap(const UPrimitiveComponent* Component)
{
	// 검사기준은 현재 Portal Trigger 설정과 맞춤.
	if (!IsValid(Component) || !Component->IsQueryCollisionEnabled()
		|| !Component->GetGenerateOverlapEvents() || Component->GetCollisionResponseToChannel(ECC_WorldDynamic) == ECR_Ignore)
	{
		return false;
	}
	
	// Keep this list synchronized with AWormholePortalActor::ApplyPortalCollisionSettings().
	switch (Component->GetCollisionObjectType())
	{
	case ECC_Pawn:
	case ECC_PhysicsBody:
	case ECC_WorldDynamic:
		return true;

	default:
		return false;
	}
}

bool FWPTransitTypeResolver::IsPhysicsBody(const UPrimitiveComponent* Comp)
{
	return FWPPhysicsHandler::SupportsPart(Comp) && Comp->IsPhysicsCollisionEnabled() && Comp->IsSimulatingPhysics();
}
