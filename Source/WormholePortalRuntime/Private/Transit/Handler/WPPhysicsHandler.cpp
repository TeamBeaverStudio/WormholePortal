// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPPhysicsHandler.h"

#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Physics/WPContactModifierCallback.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Engine/StaticMesh.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/ShapeComponent.h"

#include "WPLog.h"
#include "WPTransform.h"
#include "Transit/WPTransitRun.h"
#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitShape.h"

bool FWPPhysicsHandler::SupportsPart(const UPrimitiveComponent* Part)
{
	if (!IsValid(Part) || Part->IsA<UInstancedStaticMeshComponent>()) return false;
	
	if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Part))
	{
		return IsValid(Mesh->GetStaticMesh());
	}
	return Part->IsA<UShapeComponent>();
}

void FWPPhysicsHandler::PrepPair(FWPTransitComponentPair& Pair)
{
	UPrimitiveComponent* Master = Pair.Master.Get();
	UPrimitiveComponent* Twin = Pair.Twin.Get();
	
	if (!IsValid(Master) || !IsValid(Twin)) return;
	
	// Cancel 또는 Commit 시 복원할 Master의 기존 물리 상태를 저장합니다.
	Pair.OldMass = Master->GetMass();
	Pair.OldMassScale = Master->GetMassScale();
	Pair.bHadGravity = Master->IsGravityEnabled();
	Pair.bWasSim = Master->IsSimulatingPhysics();
	Pair.OldCollision = Master->GetCollisionEnabled();
	
	if (const FBodyInstance* MasterBody = Master->GetBodyInstance())
	{
		Pair.OldMassOverride = MasterBody->GetMassOverride();
		Pair.bHadMassOverride = MasterBody->bOverrideMass;
	}
	
	Pair.bMassChanged = Pair.OldMass > KINDA_SMALL_NUMBER;
	
	// Crossing 중 master와 Twin이 전체 질량 중복해서 가지지 않게 나눔
	if (Pair.bMassChanged)
	{
		const float SplitMass = FMath::Max(Pair.OldMass * 0.5f, KINDA_SMALL_NUMBER);
		Master->SetMassOverrideInKg(NAME_None, SplitMass, true);
		Twin->SetMassOverrideInKg(NAME_None, SplitMass, true);
	}
	
	// twin에는 중력 중복 적용X, Chaos Callback으로 master하고 동기화
	Twin->SetEnableGravity(false);
	
	// TODO: Crossing 중에는 Master와 Twin을 깨운다.
	// Cancel은 Master의 기존 수면 상태를 복원하고,
	// Commit은 Twin의 최종 수면 상태를 Master에 적용해야 한다.
	Master->WakeAllRigidBodies();
	Twin->WakeAllRigidBodies();
}

void FWPPhysicsHandler::ResetPair(FWPTransitComponentPair& Pair)
{
	UPrimitiveComponent* Master = Pair.Master.Get();
	if (!IsValid(Master)) return;
	
	if (Pair.bMassChanged)
	{
		Master->SetMassScale(NAME_None, Pair.OldMassScale);
		Master->SetMassOverrideInKg(NAME_None, Pair.OldMassOverride, Pair.bHadMassOverride);
		Pair.bMassChanged = false;
	}
	
	Master->SetEnableGravity(Pair.bHadGravity);
	Master->SetSimulatePhysics(Pair.bWasSim);
	Master->SetCollisionEnabled(Pair.OldCollision);
	
	if (Master->IsSimulatingPhysics())
	{
		Master->WakeAllRigidBodies();
	}
}

bool FWPPhysicsHandler::ReadBody(UPrimitiveComponent* Part, FTransform& OutTransform, FVector& OutLinear, FVector& OutAngular)
{
	if (!IsValid(Part)) return false;
	
	FBodyInstance* Body = Part->GetBodyInstance();
	
	if (Body == nullptr || !Body->IsValidBodyInstance()) return false;
	
	OutTransform = Body->GetUnrealWorldTransform(false, true);
	OutLinear = Body->GetUnrealWorldVelocity();
	OutAngular = Body->GetUnrealWorldAngularVelocityInRadians();
	
	return true;
}

void FWPPhysicsHandler::BindChaos(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping)
{
	if (Run.PhysicsState.Callback != nullptr || World == nullptr) return;
	
	// 로그 전용: Chaos 경고에 Actor context를 남기기 위한 조회입니다.
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	AActor* MasterActor = IsValid(TransitComp) ? TransitComp->GetOwner() : nullptr;
	FPhysScene_Chaos* ChaosScene = static_cast<FPhysScene_Chaos*>(World->GetPhysicsScene());
	
	// Chaos Scene가 없는 경우 Early-Return
	if (ChaosScene == nullptr)
	{
		if (!Run.PhysicsState.bChaosWarned)
		{
			WP_LOG(World, Warning, TEXT("Physics Callback Registration failed. Actor=%s, Reason=No Chaos Scene"), *GetNameSafe(MasterActor));
			
			Run.PhysicsState.bChaosWarned = true;
		}
		return;
	}
	
	// Chaos Solver가 없는 경우 Early-Return
	Run.PhysicsState.Solver = ChaosScene->GetSolver();
	if (Run.PhysicsState.Solver == nullptr)
	{
		if (!Run.PhysicsState.bChaosWarned)
		{
			WP_LOG(World, Warning, TEXT("Physics callback registration failed. Actor=%s Reason=No Chaos solver."), *GetNameSafe(MasterActor));
			
			Run.PhysicsState.bChaosWarned = true;
		}
		return;
	}
	
	TArray<FWPTransitParticlePair> ParticlePairs;
	for (const FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		UPrimitiveComponent* Master = Pair.Master.Get();
		UPrimitiveComponent* Twin = Pair.Twin.Get();
		
		if (!IsValid(Master) || !IsValid(Twin)) continue;
		
		if (FBodyInstance* MasterBody = Master->GetBodyInstance())
		{
			MasterBody->SetContactModification(true);
		}
		if (FBodyInstance* TwinBody = Twin->GetBodyInstance())
		{
			TwinBody->SetContactModification(true);
		}
		
		// Chaos 콜백에는 Component가 아니라 각 Body의 Physics Proxy를 전달합니다. Twin 생성 직후
		// Body가 아직 만들어지지 않은 Pair는 이번 등록에서 제외하고 다음 Update에서 다시 시도합니다.
		FWPTransitParticlePair& ParticlePair = ParticlePairs.AddDefaulted_GetRef();
		ParticlePair.MasterProxy = GetProxy(Master);
		ParticlePair.TwinProxy = GetProxy(Twin);
		
		// GameThread -> Physics Proxy까지 검사.
		// Physics Thread Particle은 OnPreSimulate_Internal에서 가져온다.
		if (ParticlePair.MasterProxy == nullptr || ParticlePair.TwinProxy == nullptr)
		{
			ParticlePairs.Pop();
		}
	}
	
	// Solver에서 보정할 Pair가 없으면 Early-Return
	if (ParticlePairs.IsEmpty())
	{
		if (!Run.PhysicsState.bChaosWarned)
		{
			WP_LOG(World, Warning, TEXT("Physics callback registration failed. Actor=%s Reason=No valid physics proxy pair"), *GetNameSafe(MasterActor));

			Run.PhysicsState.bChaosWarned = true;
		}

		// Solver 포인터와 Contact Modification 설정을 함께 원복해야 다음 Update에서 깨끗하게 재시도합니다.
		UnbindChaos(Run);
		return;
	}
	
	Run.PhysicsState.Callback = Run.PhysicsState.Solver->CreateAndRegisterSimCallbackObject_External<FWormholeTransitJointCallback>(
		ParticlePairs,
		Mapping,
		Run.EntryPoint,
		Run.SelectedPlane);
	
	if (Run.PhysicsState.Callback == nullptr && !Run.PhysicsState.bChaosWarned)
	{
		WP_LOG(World, Warning, TEXT("Physics callback registration failed. Actor=%s Reason=CreateAndRegister returned null."), *GetNameSafe(MasterActor));

		Run.PhysicsState.bChaosWarned = true;
	}
}

void FWPPhysicsHandler::UnbindChaos(FWPTransitRun& Run)
{
	if (Run.PhysicsState.Solver != nullptr && Run.PhysicsState.Callback != nullptr)
	{
		Run.PhysicsState.Solver->UnregisterAndFreeSimCallbackObject_External(Run.PhysicsState.Callback);
	}
	
	Run.PhysicsState.Callback = nullptr;
	Run.PhysicsState.Solver = nullptr;
	
	for (const FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		if (IsValid(Pair.Master))
		{
			if (FBodyInstance* MasterBody = Pair.Master->GetBodyInstance())
			{
				MasterBody->SetContactModification(false);
			}
		}
		
		if (IsValid(Pair.Twin))
		{
			if (FBodyInstance* TwinBody = Pair.Twin->GetBodyInstance())
			{
				TwinBody->SetContactModification(false);
			}
		}
	}
}

bool FWPPhysicsHandler::Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	if (Run.PhysicsState.Pairs.IsEmpty()) return false;
	
	// 모든 Twin 상태를 먼저 확보한 뒤 Master에 적용합니다. 읽는 도중 한 Pair가 무효해져도 일부
	// Master만 목적지로 이동하는 일이 없도록 수집과 적용을 두 단계로 분리합니다.
	// 해당 함수 내부에서만 사용하기 때문에 구조체를 함수 안에 선언해둡니다.
	struct FWPBodyState
	{
		UPrimitiveComponent* Master = nullptr;
		FTransform Transform = FTransform::Identity;
		FVector Linear = FVector::ZeroVector;
		FVector Angular = FVector::ZeroVector;
	};
	
	TArray<FWPBodyState> States;
	States.Reserve(Run.PhysicsState.Pairs.Num());
	
	for (const FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		UPrimitiveComponent* Master = Pair.Master.Get();
		UPrimitiveComponent* Twin = Pair.Twin.Get();
		
		if (!IsValid(Master) || !IsValid(Twin)) return false;
		
		FWPBodyState& State = States.AddDefaulted_GetRef();
		State.Master = Master;
		
		if (!ReadBody(Twin, State.Transform, State.Linear, State.Angular))
		{
			// Fallback 경로 처리
			State.Transform = Twin->GetComponentTransform();
			State.Linear = Twin->GetPhysicsLinearVelocity();
			State.Angular = Twin->GetPhysicsAngularVelocityInRadians();
		}
		// Chaos Transform에는 Component Scale이 없으므로 Master의 현재 World Scale을 보존합니다.
		State.Transform.SetScale3D(Master->GetComponentScale());
	}
	
	// Twin 상태 확보 후 Callback과 임시 물리 설정 정리
	UnbindChaos(Run);
	
	for (FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		ResetPair(Pair);
	}
	
	for (const FWPBodyState& State : States)
	{
		if (!IsValid(State.Master)) continue;
		
		State.Master->SetWorldTransform(State.Transform, false, nullptr, ETeleportType::TeleportPhysics);
		
		if (State.Master->IsSimulatingPhysics())
		{
			State.Master->SetPhysicsLinearVelocity(State.Linear);
			State.Master->SetPhysicsAngularVelocityInRadians(State.Angular);
			// TODO: 강제로 기상하는 문제 해결해야함.
			State.Master->WakeAllRigidBodies();
		}
	}
	return true;
}

void FWPPhysicsHandler::Cancel(FWPTransitRun& Run) const
{
	UnbindChaos(Run);
	for (FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		ResetPair(Pair);
	}
}

FVector FWPPhysicsHandler::GetVelocity(const UWPTransitComponent* TransitComponent) const
{
	if (!IsValid(TransitComponent)) return FVector::ZeroVector;
	
	for (const UPrimitiveComponent* Part : TransitComponent->GetTransitPrimitives())
	{
		if (!SupportsPart(Part) || !Part->IsSimulatingPhysics())
		{
			continue;
		}
		
		const FVector Velocity = Part->GetPhysicsLinearVelocity();
		
		if (!Velocity.IsNearlyZero())
		{
			return Velocity;
		}
	}
	return FVector::ZeroVector;
}

bool FWPPhysicsHandler::HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface, const FVector& EntryNormal,
	const FVector& DestSurface, const FVector& ExitNormal) const
{
	if (Run.PhysicsState.Pairs.IsEmpty()) return false;
	
	for (const FWPTransitComponentPair& Pair : Run.PhysicsState.Pairs)
	{
		if (!FWPTransitShape::IsOutside(Pair.Twin.Get(), DestSurface, ExitNormal))
		{
			return false;
		}
	}
	return true;
}

bool FWPPhysicsHandler::Begin(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	UWPTransitComponent* TransitComp = Run.TransitComponent.Get();
	AActor* TwinActor = Run.TwinActor;
	if (!IsValid(TransitComp) || !IsValid(TwinActor)) return false;
	
	/**
	 * Unreal이 제공하는 Component 수집용 TArray
	 * 아래와 똑같음
	 * - TArray<UPrimitiveComponent*> TwinParts;
	 * - TwinActor->GetComponents<UPrimitiveComponent>(TwinParts);
	 */
	TInlineComponentArray<UPrimitiveComponent*> TwinParts(TwinActor);
	TMap<FName, UPrimitiveComponent*> TwinByName;
	for (UPrimitiveComponent* TwinPart : TwinParts)
	{
		if (SupportsPart(TwinPart))
		{
			TwinByName.Add(TwinPart->GetFName(), TwinPart);
		}
	}
	
	// Resolver가 전이 대상으로 선택한 Primitive 중 Physics 기능이 실제로 사용하는 Component만 준비합니다.
	for (UPrimitiveComponent* Master  : TransitComp->GetTransitPrimitives())
	{
		if (!SupportsPart(Master) || !Master->IsSimulatingPhysics()) continue;
		
		UPrimitiveComponent* Twin = TwinByName.FindRef(Master->GetFName());
		if (!IsValid(Twin)) continue;
		
		Twin->SetCollisionEnabled(Master->GetCollisionEnabled());
		Twin->SetCollisionObjectType(Master->GetCollisionObjectType());
		Twin->SetCollisionResponseToChannels(Master->GetCollisionResponseToChannels());
		Twin->SetGenerateOverlapEvents(Master->GetGenerateOverlapEvents());
		Twin->SetSimulatePhysics(Master->IsSimulatingPhysics());
		
		Twin->SetWorldTransform(Mapping.MapTransform(Master->GetComponentTransform(), Run.EntryPoint, Run.SelectedPlane),
			false, nullptr, ETeleportType::TeleportPhysics);
		Twin->SetPhysicsLinearVelocity(Mapping.MapDir(Master->GetPhysicsLinearVelocity()));
		Twin->SetPhysicsAngularVelocityInRadians(Mapping.MapDir(Master->GetPhysicsAngularVelocityInRadians()));
		
		FWPTransitComponentPair& Pair = Run.PhysicsState.Pairs.AddDefaulted_GetRef();
		Pair.Master = Master;
		Pair.Twin = Twin;
		
		PrepPair(Pair);
	}
	
	return !Run.PhysicsState.Pairs.IsEmpty();
}

bool FWPPhysicsHandler::Update(UWorld* World, FWPTransitRun& Run, const FWPTransform& Mapping) const
{
	if (Run.PhysicsState.Callback == nullptr)
	{
		BindChaos(World, Run, Mapping);
	}
	return true;
}

Chaos::FSingleParticlePhysicsProxy* FWPPhysicsHandler::GetProxy(UPrimitiveComponent* Part)
{
	if (!IsValid(Part)) return nullptr;
	
	FBodyInstance* Body = Part->GetBodyInstance();
	if (Body == nullptr || Body->GetPhysicsActor() == nullptr) return nullptr;
	
	return Body->GetPhysicsActor();
}

