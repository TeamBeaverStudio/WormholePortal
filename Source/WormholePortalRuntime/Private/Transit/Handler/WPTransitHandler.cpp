// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Transit/Handler/WPTransitHandler.h"

#include "GameFramework/Actor.h"
#include "Transit/Handler/WPCharacterHandler.h"
#include "Transit/Handler/WPPawnHandler.h"
#include "Transit/Handler/WPPhysicsHandler.h"
#include "Transit/Handler/WPProjectileHandler.h"
#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitShape.h"
#include "Transit/WPTransitComponent.h"
#include "WPTransform.h"

bool IWPTransitHandler::Begin(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	// MovementComponent 기반 Handler는 Twin 생성 뒤 추가 Runtime 상태를 만들지 않습니다.
	return true;
}

bool IWPTransitHandler::Update(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	UWPTransitComponent* TransitComponent = Run.TransitComponent.Get();
	AActor* Master = IsValid(TransitComponent) ? TransitComponent->GetOwner() : nullptr;
	AActor* Twin = Run.TwinActor;
	if (!IsValid(Master) || !IsValid(Twin)) return false;

	// MovementComponent 기반 종류는 Master의 현재 Actor Transform을 매 프레임 출구 공간으로 옮깁니다.
	// Physics 종류는 독립된 물리 Body를 사용하므로 FWPPhysicsHandler::Update에서 이 기본 동작을 대체합니다.
	Twin->SetActorTransform(Mapping.MapTransform(Master->GetActorTransform(), Run.EntryPoint,
		Run.SelectedPlane), false, nullptr, ETeleportType::TeleportPhysics);
	Twin->ForceNetUpdate();
	return true;
}

bool IWPTransitHandler::HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
	const FVector& EntryNormal, const FVector&, const FVector&) const
{
	// 기본 Handler는 Visual상태와 상관없이 Source의 Transit Primitive가 완전히 통과했는지 판정한다.
	return FWPTransitShape::MasterInside(Run, SourceSurface, EntryNormal);
}

void IWPTransitHandler::Cancel(FWPTransitRun& Run) const
{
	// 공통 MovementComponent 경로는 Crossing 중 Master 상태를 바꾸지 않으므로 복원할 값이 없습니다.
}

const IWPTransitHandler* FWPTransitHandlers::Get(EWPTransitType TransitType)
{
	// Handler는 Actor별 상태를 보관하지 않습니다. 전이 상태는 FWPTransitRun에 있으므로 종류별
	// 공유 인스턴스 하나를 모든 TransitComponent가 안전하게 재사용합니다.
	static const FWPPhysicsHandler PhysicsHandler;
	static const FWPCharacterHandler CharacterHandler;
	static const FWPProjectileHandler ProjectileHandler;
	static const FWPPawnHandler PawnHandler;

	switch (TransitType)
	{
	case EWPTransitType::Physics:	return &PhysicsHandler;
	case EWPTransitType::Character:	return &CharacterHandler;
	case EWPTransitType::Projectile:return &ProjectileHandler;
	case EWPTransitType::Pawn:		return &PawnHandler;
	default:						return nullptr;
	}
}
