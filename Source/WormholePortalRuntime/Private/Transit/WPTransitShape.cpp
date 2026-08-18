// Copyright 2026 Team Beaver. All Rights Reserved.


#include "WPTransitShape.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

#include "Transit/WPTransitComponent.h"
#include "Transit/WPTransitRun.h"
#include "WPSettings.h"
#include "Transit/WPTransitTypes.h"

bool FWPTransitShape::FitsGate(const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Parts, const FVector& GateCenter,
	const FVector& MoveDir, float SourceRadius)
{
	const FVector& SafeDir = MoveDir.GetSafeNormal();
	
	if (Parts.IsEmpty() || SafeDir.IsNearlyZero() || SourceRadius <= 0.0f)
	{
		return false;
	}
	
	const double RadiusSq = FMath::Square( static_cast<double>(SourceRadius) + UE_DOUBLE_KINDA_SMALL_NUMBER);
	bool bHasShape = false;
	
	TArray<FVector> Points;
	Points.Reserve(8);
	
	for (const TWeakObjectPtr<UPrimitiveComponent>& PartPtr : Parts)
	{
		const UPrimitiveComponent* Part = PartPtr.Get();
		
		// Capsule
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Part))
		{
			if(!CapsuleFitsGate(Capsule, GateCenter, SafeDir, SourceRadius)) return false;
			
			bHasShape = true;
			continue;
		}
		
		// Sphere
		if (const USphereComponent* Sphere = Cast<USphereComponent>(Part))
		{
			if (!SphereFitsGate(Sphere, GateCenter, SafeDir, SourceRadius)) return false;
			
			bHasShape = true;
			continue;
		}
		
		if (!GetPoints(PartPtr.Get(), OUT Points))
		{
			return false;
		}
		
		for (const FVector& Point : Points)
		{
			const FVector Offset = Point - GateCenter;
			const float AxisDist = FVector::DotProduct(Offset, SafeDir);
			const FVector Radial = Offset - SafeDir * AxisDist;
			
			if (Radial.SizeSquared() > RadiusSq)
			{
				return false;
			}
			bHasShape = true;
		}
	}
	return bHasShape;
}

bool FWPTransitShape::GetMasterCenter(const AActor* Master, const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Parts, EWPTransitCenter CenterMode, FVector& OutCenter)
{
	OutCenter = FVector::ZeroVector;
	if (!IsValid(Master)) return false;
	
	switch (CenterMode)
	{
	case EWPTransitCenter::RootLocation:
		{
			const USceneComponent* Root = Master->GetRootComponent();
			if (!IsValid(Root)) return false;
			
			OutCenter = Root->GetComponentLocation();
		}
		break;
		
	case EWPTransitCenter::ActorOBBCenter:
		{
			const FTransform ActorToWorld = Master->GetActorTransform();
			FBox LocalBounds(ForceInit);
			
			for (const TWeakObjectPtr<UPrimitiveComponent>& PartPtr : Parts)
			{
				const UPrimitiveComponent* Part = PartPtr.Get();
				if (!IsValid(Part)) continue;
				
				const FTransform ComponentToActor = Part->GetComponentTransform().GetRelativeTransform(ActorToWorld);
				LocalBounds += Part->CalcBounds(ComponentToActor).GetBox();
			}
			
			if (LocalBounds.IsValid == 0) return false;
			
			OutCenter = ActorToWorld.TransformPosition(LocalBounds.GetCenter());
		}
		break;
		
	default:
		return false;
	}
	return !OutCenter.ContainsNaN();
}

bool FWPTransitShape::SelectPlane(const FQuat& SourceRotation, const FVector& TangentPlaneNormal, const FVector& MoveDir, EWPTransitPlane& OutPlane)
{
	// 기본값 초기화
	OutPlane = EWPTransitPlane::YZ;
	// Source Portal 중심 -> Master Actor 중심으로 향하는 방향
	const FVector SafeTangentPlaneNormal = TangentPlaneNormal.GetSafeNormal();
	if (SafeTangentPlaneNormal.IsNearlyZero() || SourceRotation.ContainsNaN() || SourceRotation.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false;
	}
	
	// YZ, XZ, XY 후보는 Source Portal 로컬 축 기준으로 정의 -> World 방향 접평면 법선과 이동 방향을 Source Portal 로컬 공간으로 이동
	const FQuat SafeSourceRotation = SourceRotation.GetNormalized();
	const FVector LocalTangentPlaneNormal = SafeSourceRotation.UnrotateVector(SafeTangentPlaneNormal).GetSafeNormal();
	const FVector LocalMoveDir = SafeSourceRotation.UnrotateVector(MoveDir.GetSafeNormal()).GetSafeNormal();
	if (LocalTangentPlaneNormal.IsNearlyZero()) return false;
	
	// 접평면 선택 정책은 프로젝트 설정에서 직접 읽는다.
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings) return false;
	
	constexpr uint8 PlaneCount = static_cast<uint8>(EWPTransitPlane::MAX);
	constexpr uint8 PriorityCount = static_cast<uint8>(EWPPlanePriority::MAX);
	
	// 후보 평면 선택할 때 필요한 계산 결과. 이 함수에서만 사용하기 때문에 구조체를 여기에 선언한다.
	struct FPlaneCandidate
	{
		EWPTransitPlane Plane;
		FVector PlaneNormalLocal;
		// 진입 접평면과 후보 평면이 이루는 각도. 값이 작을수록 진입 접평면에 더 가까운 후보
		double PlaneAngleDegrees = 90.f;
		// 이동 방향과 해당 후보의 출구 법선이 이루는 각도. 값이 작을수록 물체가 출구 바깥쪽을 향하게 됨
		double VelocityAngleDegrees = 180.f;
	};
	
	// 각 후보 평면의 Source Portal Local 법선
	FPlaneCandidate Candidates[PlaneCount] = {
		{EWPTransitPlane::YZ, FVector(1.0, 0.0, 0.0)},
		{EWPTransitPlane::XZ, FVector(0.0, 1.0, 0.0)},
		{EWPTransitPlane::XY, FVector(0.0, 0.0, 1.0)}
	};
	
	double ClosestPlaneAngleDegrees = 90.0;
	const bool bHasMoveDirection = !LocalMoveDir.IsNearlyZero();
	
	for (FPlaneCandidate& Candidate : Candidates)
	{
		// 접평면 각도 -> 평면 법선의 내적으로 계산
		const double PlaneCosine = FMath::Clamp(FMath::Abs(FVector::DotProduct(LocalTangentPlaneNormal, Candidate.PlaneNormalLocal)), 0.0, 1.0);
		Candidate.PlaneAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(PlaneCosine));
		ClosestPlaneAngleDegrees = FMath::Min(ClosestPlaneAngleDegrees, Candidate.PlaneAngleDegrees);
		
		if (!bHasMoveDirection)
		{
			// 이동 방향이 없으면 Velocity 각도 계산X -> 선택 단계에서 전역 평면 우선순위 사용
			continue;
		}
		
		// 벡터 V를 법선 N을 가진 평면으로 반사한다. Reflected = V - 2 * Dot(V, N) * N
		const double NormalComponent = FVector::DotProduct(LocalTangentPlaneNormal, Candidate.PlaneNormalLocal);
		const FVector ExitNormalLocal = LocalTangentPlaneNormal - 2.0 * NormalComponent * Candidate.PlaneNormalLocal;
		
		// 이동 방향과 후보 출구 법선 사이 각도 계산
		const double VelocityCosine = FMath::Clamp(FVector::DotProduct(LocalMoveDir, ExitNormalLocal), -1.0, 1.0);
		Candidate.VelocityAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(VelocityCosine));
	}
	
	//Config 파일은 직접 수정될 수 있음. Editor Clamp에 의존X
	const double SafeTieAngleDegree = FMath::IsFinite(Settings->PlaneTieAngleDegrees) ? FMath::Clamp(static_cast<double>(Settings->PlaneTieAngleDegrees), 0.0, 90.0) : 0.0;
	
	// EWPPlanePriority 각 Enum값에 대응하는 실제 평면 검사 순서
	static constexpr EWPTransitPlane PriorityOrders[PriorityCount][PlaneCount] = 
	{
		{EWPTransitPlane::YZ, EWPTransitPlane::XZ, EWPTransitPlane::XY},
		{EWPTransitPlane::YZ, EWPTransitPlane::XY, EWPTransitPlane::XZ},
		{EWPTransitPlane::XZ, EWPTransitPlane::YZ, EWPTransitPlane::XY},
		{EWPTransitPlane::XZ, EWPTransitPlane::XY, EWPTransitPlane::YZ},
		{EWPTransitPlane::XY, EWPTransitPlane::YZ, EWPTransitPlane::XZ},
		{EWPTransitPlane::XY, EWPTransitPlane::XZ, EWPTransitPlane::YZ},
	};
	
	// Config를 벗어난 값이 들어오면 기본값인 YZ > XZ > XY 순서를 사용
	const uint8 PriorityIndex = static_cast<uint8>(Settings->PlanePriority);
	const uint8 SafePriorityIndex = PriorityIndex < PriorityCount ? PriorityIndex : 0;
	
	const FPlaneCandidate* SelectedCandidate = nullptr;
	// 높은 우선순위의 평면부터 검사
	for (const EWPTransitPlane& Plane : PriorityOrders[SafePriorityIndex])
	{
		const uint8 CandidateIndex = static_cast<uint8>(Plane);
		if (CandidateIndex >= PlaneCount) return false;
		
		const FPlaneCandidate& Candidate = Candidates[CandidateIndex];
		// 가장 가까운 접평면과의 각도 차이가 설정값 이하인 후보만
		// 이동 방향 각도 비교에 포함
		const bool bIsPlaneTie = Candidate.PlaneAngleDegrees <= ClosestPlaneAngleDegrees + SafeTieAngleDegree;
		if (!bIsPlaneTie) continue;
		
		// 처음 발견한 유효 후보 기본 선택으로 저장
		if (!SelectedCandidate)
		{
			SelectedCandidate = &Candidate;
			continue;
		}
		
		// 이동 방향이 없으면 후보를 더 비교할 기준이 없음.
		if (!bHasMoveDirection) continue;
		
		// 이동 방향과 출구 법선 사이 각도가 기존 후보보다 확실히 작은 경우만 교체.
		if (Candidate.VelocityAngleDegrees < SelectedCandidate->VelocityAngleDegrees - UE_DOUBLE_SMALL_NUMBER)
		{
			SelectedCandidate = &Candidate;
		}
	}
	
	if (!SelectedCandidate) return false;
	
	OutPlane = SelectedCandidate->Plane;	
	return true;
}
bool FWPTransitShape::MasterOutside(const FWPTransitRun& Run, const FVector& Surface, const FVector& Normal)
{
	const UWPTransitComponent* TransitComponent = Run.TransitComponent.Get();
	if (!IsValid(TransitComponent) || TransitComponent->GetTransitPrimitives().IsEmpty()) return false;
	
	for (const UPrimitiveComponent* Part : TransitComponent->GetTransitPrimitives())
	{
		if (!IsOutside(Part, Surface, Normal)) return false;
	}
	
	return true;
}

bool FWPTransitShape::MasterInside(const FWPTransitRun& Run, const FVector& Surface, const FVector& Normal)
{
	// Source 외부 방향 반대로 뒤집어서 Source 내부 공간 판정
	return MasterOutside(Run, Surface, -Normal);
}

bool FWPTransitShape::IsOutside(const UPrimitiveComponent* Part, const FVector& Surface, const FVector& Normal)
{
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (!IsValid(Part) || SafeNormal.IsNearlyZero()) return false;
	
	// Capsule
	if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Part))
	{
		return IsCapsuleOutside(Capsule, Surface, SafeNormal);
	}
	
	// Sphere
	if (const USphereComponent* Sphere = Cast<USphereComponent>(Part))
	{
		return IsSphereOutside(Sphere, Surface, SafeNormal);
	}
	
	const UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Part);
	const UStaticMesh* Mesh = IsValid(MeshComp) ? MeshComp->GetStaticMesh() : nullptr;
	
	// BoxComp & Static Mesh (OBB)
	if (Part->IsA<UBoxComponent>() || IsValid(Mesh))
	{
		TArray<FVector> Points;
		Points.Reserve(8);
		
		if (!GetPoints(Part, OUT Points)) return false;
		
		for (const FVector& Point : Points)
		{
			if (FVector::DotProduct(Point - Surface, SafeNormal) <= 0.0f)
			{
				return false;
			}
		}
		return true;
	}
	
	// fallback (AABB)
	const FBox WorldBounds = Part->Bounds.GetBox();
	if (!WorldBounds.IsValid) return false;
	
	const float ProjectedExtent = FVector::DotProduct(WorldBounds.GetExtent(), SafeNormal.GetAbs());
	const float CenterDist = FVector::DotProduct(WorldBounds.GetCenter() - Surface, SafeNormal);
	return CenterDist - ProjectedExtent > 0.0f;
}

bool FWPTransitShape::CapsuleFitsGate(const UCapsuleComponent* Capsule, const FVector& GateCenter,
	const FVector& MoveDir, float SourceRadius)
{
	const FVector SafeMoveDir = MoveDir.GetSafeNormal();
	if (!IsValid(Capsule) || SafeMoveDir.IsNearlyZero() || SourceRadius <= 0.0f) return false;
	
	const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	if (CapsuleRadius <= 0.0f || CapsuleHalfHeight < CapsuleRadius || 
		static_cast<double>(CapsuleRadius) > static_cast<double>(SourceRadius) + UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	const FVector CapsuleAxis = Capsule->GetUpVector().GetSafeNormal();
	if (CapsuleAxis.IsNearlyZero()) return false;
	
	const float SegmentHalfLength = FMath::Max(CapsuleHalfHeight - CapsuleRadius, 0.0f);
	const FVector CapsuleCenter = Capsule->GetComponentLocation();
	
	const FVector SegmentLength = CapsuleAxis * SegmentHalfLength;
	const FVector SegmentPoints[2] = {CapsuleCenter + SegmentLength, CapsuleCenter - SegmentLength};
	
	const double AvailableRadius = FMath::Max( static_cast<double>(SourceRadius) - static_cast<double>(CapsuleRadius), 0.0);
	
	for (const FVector& SegmentPoint : SegmentPoints)
	{
		const FVector Offset = SegmentPoint - GateCenter;
		const float AxisDistance = FVector::DotProduct(Offset, SafeMoveDir);
		const FVector RadialOffset = Offset - SafeMoveDir * AxisDistance;
		
		if (RadialOffset.SizeSquared() > FMath::Square(AvailableRadius + UE_DOUBLE_KINDA_SMALL_NUMBER))
		{
			return false;
		}
	}
	return true;
}

bool FWPTransitShape::SphereFitsGate(const USphereComponent* Sphere, const FVector& GateCenter, const FVector& MoveDir, float SourceRadius)
{
	const FVector SafeMoveDir = MoveDir.GetSafeNormal();
	if (!IsValid(Sphere) || SafeMoveDir.IsNearlyZero() || SourceRadius <= 0.0f) return false;
	
	const float SphereRadius = Sphere->GetScaledSphereRadius();

	if (SphereRadius <= 0.0f || static_cast<double>(SphereRadius) > static_cast<double>(SourceRadius) + UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	const FVector Offset = Sphere->GetComponentLocation() - GateCenter;
	const float AxisDistance = FVector::DotProduct(Offset, SafeMoveDir);
	const FVector RadialOffset = Offset - SafeMoveDir * AxisDistance;
	const double AvailableRadius = FMath::Max(static_cast<double>(SourceRadius) - static_cast<double>(SphereRadius), 0.0);

	return RadialOffset.SizeSquared() <= FMath::Square(AvailableRadius + UE_DOUBLE_KINDA_SMALL_NUMBER);
}

bool FWPTransitShape::IsCapsuleOutside(const UCapsuleComponent* Capsule, const FVector& Surface, const FVector& Normal)
{
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (!IsValid(Capsule) || SafeNormal.IsNearlyZero()) return false;
	
	const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector CapsuleAxis = Capsule->GetUpVector().GetSafeNormal();
	
	if (CapsuleRadius <= 0.0f || CapsuleHalfHeight < CapsuleRadius || CapsuleAxis.IsNearlyZero()) return false;
	
	const float SegHalfLength = FMath::Max(CapsuleHalfHeight - CapsuleRadius, 0.0f);
	const float ProjectedExtent = CapsuleRadius + SegHalfLength * FMath::Abs(FVector::DotProduct(CapsuleAxis, SafeNormal));
	
	const float CenterDistance = FVector::DotProduct(Capsule->GetComponentLocation() - Surface, SafeNormal);
	
	return CenterDistance > ProjectedExtent;
		
}

bool FWPTransitShape::IsSphereOutside(const USphereComponent* Sphere, const FVector& Surface, const FVector& Normal)
{
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (!IsValid(Sphere) || SafeNormal.IsNearlyZero()) return false;
	
	const float SphereRadius = Sphere->GetScaledSphereRadius();
	if (SphereRadius <= 0.0f) return false;
	
	const float CenterDistance = FVector::DotProduct( Sphere->GetComponentLocation() - Surface, SafeNormal);
	
	return CenterDistance > SphereRadius;
}

bool FWPTransitShape::GetPoints(const UPrimitiveComponent* Part, TArray<FVector>& OutPoints)
{
	OutPoints.Reset();
	
	if (!IsValid(Part)) return false;
	// Box Component
	if (const UBoxComponent* Box = Cast<UBoxComponent>(Part))
	{
		const FVector BoxExtent = Box->GetUnscaledBoxExtent();
		if (BoxExtent.ContainsNaN()) return false;
		
		const FBox LocalBounds(-BoxExtent, BoxExtent);
		AddPoints(LocalBounds, Box->GetComponentTransform(), OUT OutPoints);
		
		return OutPoints.Num() == 8;
	}
	
	// Static Mesh
	const UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Part);
	const UStaticMesh* Mesh = IsValid(MeshComp) ? MeshComp->GetStaticMesh() : nullptr;
	
	if (IsValid(Mesh))
	{
		const FBox LocalBounds = Mesh->GetBoundingBox();
		if (!LocalBounds.IsValid) return false;
		
		AddPoints(LocalBounds, MeshComp->GetComponentTransform(), OUT OutPoints);
		
		return OutPoints.Num() == 8;
	}
	
	// AABB (World Bounds)
	const FBox WorldBounds = Part->Bounds.GetBox();
	if (!WorldBounds.IsValid) return false;
	
	AddPoints(WorldBounds, FTransform::Identity, OUT OutPoints);
	
	return OutPoints.Num() == 8;
}

void FWPTransitShape::AddPoints(const FBox& Bounds, const FTransform& Transform, TArray<FVector>& OutPoints)
{
	for (int32 X = 0; X < 2; X++)
	{
		for (int32 Y = 0; Y < 2; Y++)
		{
			for (int32 Z = 0; Z < 2; Z++)
			{
				const FVector LocalPoint(
					X == 0 ? Bounds.Min.X : Bounds.Max.X,
					Y == 0 ? Bounds.Min.Y : Bounds.Max.Y,
					Z == 0 ? Bounds.Min.Z : Bounds.Max.Z
				);
				
				OutPoints.Add(Transform.TransformPosition(LocalPoint));
			}
		}
	}
}
