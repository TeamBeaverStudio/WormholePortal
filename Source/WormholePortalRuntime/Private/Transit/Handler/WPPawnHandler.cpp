// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPPawnHandler.h"

#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitShape.h"
#include "Transit/WPTransitComponent.h"
#include "WPTransform.h"

#include "components/SceneComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"

FVector FWPPawnHandler::GetVelocity(const UWPTransitComponent* TransitComp) const
{
	const UPawnMovementComponent* MoveComp = GetMove(TransitComp);
	return IsValid(MoveComp) ? MoveComp->Velocity : FVector::ZeroVector;
}

bool FWPPawnHandler::HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
	const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const
{
	// Pawn 속도는 Portal 사이 회전 차이만 적용하여 사용자 이동 의도를 유지합니다.
	// 구형 Portal의 축 평면 경계에서는 이 속도가 Destination 출구의 접선이 될 수 있으므로, Twin이 출구
	// 바깥으로 이동하기만 기다리지 않고 Source 형상이 완전히 통과한 시점에 Commit합니다.
	return FWPTransitShape::MasterInside(Run, SourceSurface, EntryNormal);
}

bool FWPPawnHandler::Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	
	APawn* Master = GetPawn(TransitComp);
	UPawnMovementComponent* MoveComp = GetMove(TransitComp);
	USceneComponent* MoveRoot = GetRoot(TransitComp, MoveComp);
	AController* Controller = IsValid(Master) ? Master->GetController() : nullptr;
	
	if (!IsValid(Master) || !IsValid(MoveComp) || !IsValid(MoveRoot)) return false;
	
	const FTransform& Target = Mapping.MapTransform(MoveRoot->GetComponentTransform(), Run.EntryPoint, Run.SelectedPlane);
	const FRotator TargetControl = IsValid(Controller) ? Mapping.MapControlRotation(Controller->GetControlRotation()) : FRotator::ZeroRotator;
	
	MoveRoot->SetWorldTransform(Target, false, nullptr, ETeleportType::TeleportPhysics);
	
	MoveComp->Velocity = Mapping.MapDir(MoveComp->Velocity);
	MoveComp->UpdateComponentVelocity();
	MoveComp->OnTeleported();
	
	// Controller가 있으면 회전을 갱신하고, PlayerController일 때만 소유 Client를 보정합니다.
	if (IsValid(Controller))
	{
		Controller->SetControlRotation(TargetControl);
		if (APlayerController* PC = Cast<APlayerController>(Controller))
		{
			PC->ClientSetLocation(Target.GetLocation(), TargetControl);
		}
	}
	
	Master->ForceNetUpdate();
	return true;
}

APawn* FWPPawnHandler::GetPawn(const UWPTransitComponent* TransitComp)
{
	return IsValid(TransitComp) ? Cast<APawn>(TransitComp->GetOwner()) : nullptr;
}

UPawnMovementComponent* FWPPawnHandler::GetMove(const UWPTransitComponent* TransitComp)
{
	return IsValid(TransitComp) ? Cast<UPawnMovementComponent>(TransitComp->GetMovementComponent()) : nullptr;
}

USceneComponent* FWPPawnHandler::GetRoot(const UWPTransitComponent* TransitComp, const UPawnMovementComponent* MoveComp)
{
	if (!IsValid(TransitComp) || !IsValid(MoveComp)) return nullptr;
	
	return MoveComp->UpdatedComponent;
}
