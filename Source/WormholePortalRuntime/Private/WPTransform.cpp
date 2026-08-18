// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPTransform.h"

#include "WormholePortalActor.h"
#include "Transit/WPTransitTypes.h"

bool FWPTransform::Build(const AWormholePortalActor* Source, const AWormholePortalActor* Dest, FWPTransform& OutMapping)
{
	if (!IsValid(Source) || !IsValid(Dest) || Source == Dest
		|| Source->GetWorld() != Dest->GetWorld())
	{
		return false;
	}
	
	// 입력 포탈 쌍에서 변환에 반복 사용되는 값만 미리 캐싱한다.
	// 이후 위치/방향/회전 변환 함수는 Actor 참조 없이 이 구조체 값만 사용한다.
	FWPTransform Mapping;
	Mapping.SourceCenter = Source->GetActorLocation();
	Mapping.DestCenter = Dest->GetActorLocation();
	// 포탈 Actor는 기본 사용 계약상 unit scale로 가정하므로 Scale 보정 없이 제작 반지름을 seam 반지름으로 사용한다.
	Mapping.SourceCoreRadius = Source->GetPortalRadius();
	Mapping.DestCoreRadius = Dest->GetPortalRadius();
	
	Mapping.SourceRotation = Source->GetActorQuat().GetNormalized();
	const FQuat DestRotation = Dest->GetActorQuat().GetNormalized();
	
	// Quat: C = A * B 연산은 B를 먼저 적용하고, A를 그 다음에 적용한다.
	// 주의: FTransform은 C = A * B 이면 A->B 순서 계산.
	Mapping.TransportRotation = DestRotation * Mapping.SourceRotation.Inverse();
	Mapping.TransportRotation.Normalize();
	OutMapping = Mapping;
	
	return true;
}

FVector FWPTransform::MapPoint(const FVector& Point) const
{
	return DestCenter - TransportRotation.RotateVector(Point - SourceCenter);
}

FVector FWPTransform::MapRayOrigin(const FVector& RayOrigin) const
{
	return DestCenter + TransportRotation.RotateVector(RayOrigin - SourceCenter);
}

FVector FWPTransform::UnmapPoint(const FVector& Point) const
{
	return SourceCenter - TransportRotation.Inverse().RotateVector(Point - DestCenter);
}

FVector FWPTransform::MapDir(const FVector& Dir) const
{
	return TransportRotation.RotateVector(Dir);
}

FVector FWPTransform::UnmapDir(const FVector& Dir) const
{
	return TransportRotation.Inverse().RotateVector(Dir);
}

FQuat FWPTransform::MapRot(const FQuat& Rotation) const
{
	FQuat MappedRotation = TransportRotation * Rotation;
	MappedRotation.Normalize();
	return MappedRotation;
}

FRotator FWPTransform::MapControlRotation(const FRotator& ControlRotation) const
{
	// 시선 방향에는 Portal의 전체 회전 차이를 적용한다.
	FRotator MappedControlRotation = MapRot(ControlRotation.Quaternion()).Rotator();
	MappedControlRotation.Roll = ControlRotation.Roll;
	
	return MappedControlRotation;
}

FVector FWPTransform::MapExit(const FVector& EntryPoint, EWPTransitPlane SelectedPlane) const
{
	// Source 표면의 진입 방향을 Source Portal Local 공간으로 이동
	FVector LocalExitDir = SourceRotation.UnrotateVector(EntryPoint - SourceCenter).GetSafeNormal();
	// Fallback으로 Dest Center값 반환.
	if (LocalExitDir.IsNearlyZero()) return DestCenter;
	
	switch (SelectedPlane)
	{
	case EWPTransitPlane::YZ: LocalExitDir.X *= -1.0; break;
	case EWPTransitPlane::XZ: LocalExitDir.Y *= -1.0; break;
	case EWPTransitPlane::XY: LocalExitDir.Z *= -1.0; break;
	default: return DestCenter;
	}
	
	// 반사 결과 -> Source World 공간으로 되돌린 뒤 Portal 사이 회전 차이 적용
	const FVector SourceExitDir = SourceRotation.RotateVector(LocalExitDir);
	const FVector DestExitDir = MapDir(SourceExitDir).GetSafeNormal();
	
	return DestCenter + DestExitDir * DestCoreRadius;
}

FTransform FWPTransform::MapTransform(const FTransform& Source, const FVector& EntryPoint, EWPTransitPlane SelectedPlane) const
{
	const FVector DestSurface = MapExit(EntryPoint, SelectedPlane);
	FTransform Result;
	
	// 선택된 평면은 Dest 출구 기준점에만 적용한다. Actor 상대 위치와 회전은 Portal 사이 회전 차이만 적용해 계층 구조 유지
	Result.SetLocation(DestSurface + MapDir(Source.GetLocation() - EntryPoint));
	Result.SetRotation(MapRot(Source.GetRotation()));
	Result.SetScale3D(Source.GetScale3D());
	
	return Result;
}
