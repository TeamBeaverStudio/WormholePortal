// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Physics/WPContactModifierCallback.h"

#include "Chaos/ContactModification.h"
#include "Chaos/ParticleHandle.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PBDRigidsSolver.h"

#include "WPTransform.h"
#include "Transit/WPTransitTypes.h"

FWormholeTransitJointCallback::FWormholeTransitJointCallback(const TArray<FWPTransitParticlePair>& InParticlePairs,
	const FWPTransform& InMapping, const FVector& InEntryPoint, EWPTransitPlane InSelectedPlane)
		: ParticlePairs(InParticlePairs)
		, SourceCenter(InMapping.SourceCenter)
		, DestCenter(InMapping.DestCenter)
		, SourceRadius(InMapping.SourceCoreRadius)
		, DestRadius(InMapping.DestCoreRadius)
		, TransportRotation(InMapping.TransportRotation)
{
	TransportRotation.Normalize();
	
	SourceSurface = InEntryPoint;
	DestSurface = InMapping.MapExit(InEntryPoint, InSelectedPlane);
	
	SourceNormal = (SourceSurface - InMapping.SourceCenter).GetSafeNormal();
	DestNormal = (DestSurface - InMapping.DestCenter).GetSafeNormal();
}

void FWormholeTransitJointCallback::OnPreSimulate_Internal()
{
	for (FWPTransitParticlePair& Pair : ParticlePairs)
	{
		Pair.MasterParticle = Pair.MasterProxy != nullptr ? Pair.MasterProxy->GetHandle_LowLevel() : nullptr;
		Pair.TwinParticle = Pair.TwinProxy != nullptr ? Pair.TwinProxy->GetHandle_LowLevel() : nullptr;
	}
}

void FWormholeTransitJointCallback::OnContactModification_Internal(Chaos::FCollisionContactModifier& Modifier)
{
	for (Chaos::FContactPairModifier& ContactPair : Modifier)
	{
		const Chaos::TVec2<Chaos::FGeometryParticleHandle*> Particles = ContactPair.GetParticlePair();
		const bool bFirstMaster = IsMasterParticle(Particles[0]);
		const bool bSecondMaster = IsMasterParticle(Particles[1]);
		const bool bFirstTwin = IsTwinParticle(Particles[0]);
		const bool bSecondTwin = IsTwinParticle(Particles[1]);
		const bool bHasMaster = bFirstMaster || bSecondMaster;
		const bool bHasTwin = bFirstTwin || bSecondTwin;
		
		if (!bHasMaster && !bHasTwin) continue;
		
		const bool bFirstTransit = bFirstMaster || bFirstTwin;
		const bool bSecondTransit = bSecondMaster || bSecondTwin;
		const bool bExternal = bFirstTransit != bSecondTransit;
		
		const int32 ContactCount = ContactPair.GetNumContacts();
		
		for (int32 ContactIndex = 0; ContactIndex < ContactCount; ++ContactIndex)
		{
			const FVector ContactPoint = ToFVector(ContactPair.GetWorldContactLocation(ContactIndex));
			
			// A tangent plane alone represents an infinite half-space.
			// Restrict the test to the seam sphere so unrelated contacts,
			// such as floor contacts outside the Portal, remain active.
			const bool bMasterPastSurface = FVector::DotProduct(ContactPoint - SourceSurface, SourceNormal) <= 0.f;
			const bool bTwinPastSurface = FVector::DotProduct(ContactPoint - DestSurface, DestNormal) <= 0.f;
			
			const bool bMasterInCore = FVector::DistSquared(ContactPoint, SourceCenter) <= FMath::Square(SourceRadius);
			const bool bTwinInCore = FVector::DistSquared(ContactPoint, DestCenter) <= FMath::Square(DestRadius);
			
			const bool bMasterInside = bHasMaster && bMasterPastSurface && bMasterInCore;
			const bool bTwinInside = bHasTwin && bTwinPastSurface && bTwinInCore;
			
			if (bMasterInside || bTwinInside)
			{
				// Disable only contacts owned by the portion currently inside the Portal seam.
				ContactPair.SetContactPointDisabled(ContactIndex);
				++DisabledContactCount;
				continue;
			}
			
			if (!bExternal) continue;
			
			for (FWPTransitParticlePair& Pair : ParticlePairs)
			{
				if (Pair.MasterParticle == Particles[0] || Pair.MasterParticle == Particles[1])
				{
					Pair.bMasterContact = true;
				}
				
				if (Pair.TwinParticle == Particles[0] || Pair.TwinParticle == Particles[1])
				{
					Pair.bTwinContact = true;
				}
			}
		}
	}
}

void FWormholeTransitJointCallback::OnPreSolve_Internal()
{
	// Master, Twin의 목표 속도, 회전 계산
	// SetV(Velocity), W(Angular Velocity), (필요하면 P(Position), Q(Quaternion))  계산

	const float DeltaSeconds = FMath::Max(static_cast<float>(GetDeltaTime_Internal()), KINDA_SMALL_NUMBER);

	for (FWPTransitParticlePair& Pair : ParticlePairs)
	{
		// 위치/속도와 회전/각속도 오차를 각각 보정합니다.
		if (SyncSleep(Pair))
		{
			FixLinear(Pair, DeltaSeconds);
			FixAngular(Pair, DeltaSeconds);
		}
		
		Pair.bMasterContact = false;
		Pair.bTwinContact = false;
	}

	// 끝난 후 Chaos Solver에서 남아있는 Contact, Friction, Constraint 해결
}

void FWormholeTransitJointCallback::OnParticleUnregistered_Internal(
	TArray<TTuple<Chaos::FUniqueIdx, Chaos::FSingleParticlePhysicsProxy*>>& UnregisteredProxies)
{
	for (const auto& Entry : UnregisteredProxies)
	{
		const Chaos::FSingleParticlePhysicsProxy* Proxy = Entry.Get<1>();
		ParticlePairs.RemoveAllSwap([Proxy](const FWPTransitParticlePair& Pair)
		{
			return Pair.MasterProxy == Proxy || Pair.TwinProxy == Proxy;
		});
	}
}

FVector FWormholeTransitJointCallback::ToFVector(const Chaos::FVec3& Vector)
{
	return FVector(Vector.X, Vector.Y, Vector.Z);
}

Chaos::FVec3 FWormholeTransitJointCallback::ToChaosVector(const FVector& Vector)
{
	return Chaos::FVec3(Vector.X, Vector.Y, Vector.Z);
}

FQuat FWormholeTransitJointCallback::ToFQuat(const Chaos::FRotation3& Rotation)
{
	return FQuat(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
}

FVector FWormholeTransitJointCallback::MapPosition(const FVector& SourcePosition) const
{
	return DestSurface + TransportRotation.RotateVector(SourcePosition - SourceSurface);
}

FVector FWormholeTransitJointCallback::MapDirection(const FVector& SourceDirection) const
{
	return TransportRotation.RotateVector(SourceDirection);
}

FVector FWormholeTransitJointCallback::UnmapDirection(const FVector& DestinationDirection) const
{
	return TransportRotation.Inverse().RotateVector(DestinationDirection);
}

void FWormholeTransitJointCallback::FixLinear(const FWPTransitParticlePair& Pair, float DeltaSeconds)
{
	Chaos::FPBDRigidParticleHandle* MasterRigid = Pair.MasterParticle != nullptr ? Pair.MasterParticle->CastToRigidParticle() : nullptr;
	Chaos::FPBDRigidParticleHandle* TwinRigid = Pair.TwinParticle != nullptr ? Pair.TwinParticle->CastToRigidParticle() : nullptr;
	if (MasterRigid == nullptr || TwinRigid == nullptr) return;
	
	const FVector MasterPosition = ToFVector(MasterRigid->GetP());
	const FVector TwinPosition = ToFVector(TwinRigid->GetP());
	const FVector MasterVelocity = ToFVector(MasterRigid->GetV());
	const FVector TwinVelocity = ToFVector(TwinRigid->GetV());
	
	const FVector TargetPosition = MapPosition(MasterPosition);
	const FVector TargetVelocity = MapDirection(MasterVelocity);
	
	const FVector PositionError = TargetPosition - TwinPosition;
	const FVector VelocityError = TargetVelocity - TwinVelocity;
	
	// 속도 차이 제거 -> 다음 Step에서 위치 오차 닫히게 함.
	const FVector VelocityDelta = VelocityError + PositionError / DeltaSeconds;
	
	float MasterWeight = 0.f, TwinWeight = 0.f;
	GetMoveWeight(Pair, MasterWeight, TwinWeight);
	if (TwinWeight > 0.0f) TwinRigid->SetV(ToChaosVector(TwinVelocity + VelocityDelta * TwinWeight));
	if (MasterWeight > 0.0f) MasterRigid->SetV(ToChaosVector(MasterVelocity - UnmapDirection(VelocityDelta) * MasterWeight));	
	
}

void FWormholeTransitJointCallback::FixAngular(const FWPTransitParticlePair& Pair, float DeltaSeconds)
{
	Chaos::FPBDRigidParticleHandle* MasterRigid = Pair.MasterParticle != nullptr ? Pair.MasterParticle->CastToRigidParticle() : nullptr;
	Chaos::FPBDRigidParticleHandle* TwinRigid = Pair.TwinParticle != nullptr ? Pair.TwinParticle->CastToRigidParticle() : nullptr;
	
	if (MasterRigid == nullptr || TwinRigid == nullptr)
	{
		return;
	}
	
	const FQuat MasterRotation = ToFQuat(MasterRigid->GetQ()).GetNormalized();
	const FQuat TwinRotation = ToFQuat(TwinRigid->GetQ()).GetNormalized();
	
	const FVector MasterSpin = ToFVector(MasterRigid->GetW());
	const FVector TwinSpin = ToFVector(TwinRigid->GetW());
	
	FQuat TargetRotation = TransportRotation * MasterRotation;
	
	TargetRotation.Normalize();
	
	FQuat RotationError = TargetRotation * TwinRotation.Inverse();
	RotationError.Normalize();
	
	// 같은 회전을 나타내는 Quaternion 중 짧은 회전 경로를 선택합니다.
	if (RotationError.W < 0.0f)
	{
		RotationError.X *= -1.0f;
		RotationError.Y *= -1.0f;
		RotationError.Z *= -1.0f;
		RotationError.W *= -1.0f;
	}
	
	const float ErrorAngle = FMath::Clamp(RotationError.GetAngle(), 0.0f, PI);
	const FVector ErrorAxis = ErrorAngle > KINDA_SMALL_NUMBER ? RotationError.GetRotationAxis() : FVector::ZeroVector;
	
	const FVector AngularError = ErrorAxis * ErrorAngle;
	const FVector TargetSpin = MapDirection(MasterSpin);
	const FVector SpinError = TargetSpin - TwinSpin;
	
	// 현재 각속도 차이 제거 -> 다음 Step에서 회전 오차가 닫히게함.
	const FVector SpinDelta = SpinError + AngularError / DeltaSeconds;
	float MasterWeight = 0.f, TwinWeight = 0.f;
	GetMoveWeight(Pair, MasterWeight, TwinWeight);
	if (TwinWeight > 0.0f) TwinRigid->SetW(ToChaosVector(TwinSpin + SpinDelta * TwinWeight));
	if (MasterWeight > 0.0f) MasterRigid->SetW(ToChaosVector(MasterSpin - UnmapDirection(SpinDelta) * MasterWeight));
}

void FWormholeTransitJointCallback::GetMoveWeight(const FWPTransitParticlePair& Pair, float& OutMasterWeight,
                                                  float& OutTwinWeight)
{
	OutMasterWeight = 0.5f;
	OutTwinWeight = 0.5f;
	
	if (Pair.bMasterContact == Pair.bTwinContact) return;
	
	if (Pair.bMasterContact)
	{
		OutMasterWeight = 0.f;
		OutTwinWeight = 1.f;
		return;
	}
	OutMasterWeight = 1.f;
	OutTwinWeight = 0.f;
}

bool FWormholeTransitJointCallback::IsMasterParticle(const Chaos::FGeometryParticleHandle* Particle) const
{
	for (const FWPTransitParticlePair& Pair : ParticlePairs)
	{
		if (Pair.MasterParticle == Particle)
		{
			return true;
		}
	}
	return false;
}

bool FWormholeTransitJointCallback::IsTwinParticle(const Chaos::FGeometryParticleHandle* Particle) const
{
	for (const FWPTransitParticlePair& Pair : ParticlePairs)
	{
		if (Pair.TwinParticle == Particle)
		{
			return true;
		}
	}
	return false;
}

bool FWormholeTransitJointCallback::SyncSleep(const FWPTransitParticlePair& Pair)
{
	Chaos::FPBDRigidParticleHandle* MasterRigid = Pair.MasterParticle != nullptr ? Pair.MasterParticle->CastToRigidParticle() : nullptr;
	Chaos::FPBDRigidParticleHandle* TwinRigid = Pair.TwinParticle != nullptr ? Pair.TwinParticle->CastToRigidParticle() : nullptr;
	
	if (MasterRigid == nullptr || TwinRigid == nullptr) return false;
	
	const bool bMasterSleep = MasterRigid->IsSleeping();
	const bool bTwinSleep = TwinRigid->IsSleeping();
	
	// 논리적 물체 전체가 정지한 상태이므로 보정하지 않음.
	if (bMasterSleep && bTwinSleep)
	{
		return false;
	}
	
	// 두 Particle이 모두 Awake면 보정 가능.
	if (!bMasterSleep && !bTwinSleep)
	{
		return true;
	}
	
	Chaos::FPBDRigidsSolver* JointSolver = static_cast<Chaos::FPBDRigidsSolver*>(GetSolver());
	if (JointSolver == nullptr || JointSolver->GetEvolution() == nullptr)
	{
		return false;
	}
	
	// 한쪽에서 발생한 wake를 반대편 공간의 Particle에도 전달
	Chaos::FPBDRigidParticleHandle* SleepingRigid = bMasterSleep ? MasterRigid : TwinRigid;
	JointSolver->GetEvolution()->WakeParticle(SleepingRigid);
	return true;
}
