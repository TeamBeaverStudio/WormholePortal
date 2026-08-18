// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPProjectileHandler.h"

#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitShape.h"
#include "Transit/WPTransitComponent.h"
#include "WPTransform.h"

#include "Components/SceneComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

FVector FWPProjectileHandler::GetVelocity(const UWPTransitComponent* TransitComp) const
{
	const UProjectileMovementComponent* MoveComp = GetMove(TransitComp);
	return IsValid(MoveComp) ? MoveComp->Velocity : FVector::ZeroVector;
}

bool FWPProjectileHandler::HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
	const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const
{
	// Projectile 속도는 Portal 사이 회전 차이만 적용하여 발사 의도를 유지합니다. 구형 Portal의
	// 축 평면 경계에서는 이 속도가 Destination 출구의 접선이 될 수 있으므로, Twin이 출구
	// 바깥으로 이동하기만 기다리지 않고 Source 형상이 완전히 통과한 시점에 Commit합니다.
	return FWPTransitShape::MasterInside(Run, SourceSurface, EntryNormal);
}

bool FWPProjectileHandler::Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	UProjectileMovementComponent* MoveComp = GetMove(TransitComp);
	USceneComponent* MoveRoot = GetRoot(TransitComp, MoveComp);
	
	if (!IsValid(MoveComp) || !IsValid(MoveRoot)) return false;
	
	const FTransform Target = Mapping.MapTransform(MoveRoot->GetComponentTransform(), Run.EntryPoint, Run.SelectedPlane);
	
	MoveRoot->SetWorldTransform(Target,false, nullptr, ETeleportType::TeleportPhysics);
	MoveComp->Velocity = Mapping.MapDir(MoveComp->Velocity);
	MoveComp->UpdateComponentVelocity();
	
	return true;
}

UProjectileMovementComponent* FWPProjectileHandler::GetMove(const UWPTransitComponent* TransitComp)
{
	return IsValid(TransitComp) ? Cast<UProjectileMovementComponent>(TransitComp->GetMovementComponent()) : nullptr;
}

USceneComponent* FWPProjectileHandler::GetRoot(const UWPTransitComponent* TransitComp, const UProjectileMovementComponent* MoveComp)
{
	if (!IsValid(TransitComp) || !IsValid(MoveComp)) return nullptr;
	
	return MoveComp->UpdatedComponent;
}
