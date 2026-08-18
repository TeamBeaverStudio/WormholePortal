// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPCharacterHandler.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitShape.h"
#include "Transit/WPTransitComponent.h"
#include "WPTransform.h"

FVector FWPCharacterHandler::GetVelocity(const UWPTransitComponent* TransitComp) const
{
	const UCharacterMovementComponent* CMC = GetMove(TransitComp);
	return IsValid(CMC) ? CMC->Velocity : FVector::ZeroVector; 
}

bool FWPCharacterHandler::HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
	const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const
{
	return FWPTransitShape::MasterInside(Run, SourceSurface, EntryNormal);
}

bool FWPCharacterHandler::Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	ACharacter* Master = GetCharacter(TransitComp);
	UCharacterMovementComponent* MoveComp = GetMove(TransitComp);
	UCapsuleComponent* MoveRoot = GetRoot(TransitComp, MoveComp);
	AController* Controller = IsValid(Master) ? Master->GetController() : nullptr;
	if (!IsValid(TransitComp) || !IsValid(Master) || !IsValid(MoveComp) || !IsValid(MoveRoot))
	{
		return false;
	}
	
	const FTransform Target = Mapping.MapTransform(MoveRoot->GetComponentTransform(), Run.EntryPoint, Run.SelectedPlane);
	const FRotator DestRotation = Target.GetRotation().Rotator();
	const FRotator DestControl = IsValid(Controller) ? Mapping.MapControlRotation(Controller->GetControlRotation()) : FRotator::ZeroRotator;
	
	const FVector OldVelocity = MoveComp->Velocity;
	MoveComp->Velocity = Mapping.MapDir(OldVelocity);
	
	// Character 위치는 Capsule을 직접 이동하지 않음. Pawn의 공식 Teleport 경로 사용
	// bNoCheck를 사용해 Portal Mapping으로 계산한 정확한 위치가 다른 위치로 보정되지 않게 함.
	if (!Master->TeleportTo(Target.GetLocation(), DestRotation, false, true))
	{
		MoveComp->Velocity = OldVelocity;
		MoveComp->UpdateComponentVelocity();
		return false;
	}
	
	MoveComp->UpdateComponentVelocity();
	
	// Controller is optional.
	if (IsValid(Controller))
	{
		Controller->SetControlRotation(DestControl);
	}
	
	ResetArm(FindActiveSpringArm(Master));
	
	// 원격 PlayerController가 Possess한 Character에도 위치, 속도와 Camera방향을 전달한다
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (IsValid(PC) && !PC->IsLocalController())
	{
		TransitComp->ClientSyncChar(Target.GetLocation(), DestRotation, MoveComp->Velocity, DestControl);
	}
	
	Master->ForceNetUpdate();
	return true;
}

void FWPCharacterHandler::SyncClient(UWPTransitComponent* TransitComp, const FVector& DestLocation,
	const FRotator& DestRotation, const FVector& DestVelocity, const FRotator& DestControl)
{
	ACharacter* Master = GetCharacter(TransitComp);
	UCharacterMovementComponent* MoveComp = GetMove(TransitComp);
	UCapsuleComponent* MoveRoot = GetRoot(TransitComp, MoveComp);
	APlayerController* PC = IsValid(Master) ? Cast<APlayerController>(Master->GetController()) : nullptr;
	
	if (!IsValid(Master) || !IsValid(MoveComp) || !IsValid(MoveRoot) || !IsValid(PC) || !PC->IsLocalController())
	{
		return;
	}
	
	const FVector OldVelocity = MoveComp->Velocity;
	MoveComp->Velocity = DestVelocity;
	
	if (!Master->TeleportTo(DestLocation, DestRotation, false, true))
	{
		MoveComp->Velocity = OldVelocity;
		MoveComp->UpdateComponentVelocity();
		return;
	}
	
	MoveComp->UpdateComponentVelocity();
	PC->SetControlRotation(DestControl);
	ResetArm(FindActiveSpringArm(Master));
}

ACharacter* FWPCharacterHandler::GetCharacter(const UWPTransitComponent* TransitComp)
{
	return IsValid(TransitComp) ? Cast<ACharacter>(TransitComp->GetOwner()) : nullptr;
}

UCharacterMovementComponent* FWPCharacterHandler::GetMove(const UWPTransitComponent* TransitComp)
{
	return IsValid(TransitComp) ? Cast<UCharacterMovementComponent>(TransitComp->GetMovementComponent()) : nullptr;
}

UCapsuleComponent* FWPCharacterHandler::GetRoot(const UWPTransitComponent* TransitComp, const UCharacterMovementComponent* MoveComp)
{
	if (!IsValid(TransitComp) || !IsValid(MoveComp)) return nullptr;
	
	return Cast<UCapsuleComponent>(MoveComp->UpdatedComponent);
}

USpringArmComponent* FWPCharacterHandler::FindActiveSpringArm(ACharacter* Master)
{
	if (!IsValid(Master)) return nullptr;
	
	// Camera가 여러 개일 때 활성 Camera를 찾기 위함
	TArray<UCameraComponent*> Cameras;
	Master->GetComponents<UCameraComponent>(Cameras);
	for (UCameraComponent* Camera : Cameras)
	{
		// Camera가 아니라 Spring Arm이 회전을 담당하도록 제한
		if (!IsValid(Camera) || !Camera->IsActive() || Camera->bUsePawnControlRotation)
		{
			continue;
		}
		
		USpringArmComponent* SpringArm = Cast<USpringArmComponent>(Camera->GetAttachParent());
		// 현재 구현이 Controller 회전 전체를 변환하기 때문에 Inherit 검사
		if (!IsValid(SpringArm) || SpringArm->GetOwner() != Master || Camera->GetAttachSocketName() != USpringArmComponent::SocketName
			|| !SpringArm->bUsePawnControlRotation || !SpringArm->bInheritPitch || !SpringArm->bInheritYaw || !SpringArm->bInheritRoll)
		{
			continue;
		}
		
		return SpringArm;
	}
	
	return nullptr;
}

void FWPCharacterHandler::ResetArm(USpringArmComponent* SpringArm)
{
	if (!IsValid(SpringArm)) return;
	
	const FVector ArmOrigin = SpringArm->GetComponentLocation() + SpringArm->TargetOffset;
	
	SpringArm->PreviousArmOrigin = ArmOrigin;
	SpringArm->PreviousDesiredLoc = ArmOrigin;
	SpringArm->PreviousDesiredRot = SpringArm->GetTargetRotation();
}
