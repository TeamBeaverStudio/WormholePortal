// Copyright 2026 Team Beaver. All Rights Reserved.
#include "Trace/WPTraceLibrary.h"

#include "WormholePortalActor.h"
#include "WPSettings.h"
#include "Engine/Engine.h"
#include "KismetTraceUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "PhysicsEngine/PhysicsSettings.h"

bool UWPTraceLibrary::PortalLineTraceSingle(const UObject* WorldContextObject, FVector Start, FVector End,
                                            ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore,
                                            EDrawDebugTrace::Type DrawDebugType, FHitResult& OutHit, EWPPortalTraceStatus& OutStatus, bool bIgnoreSelf,
                                            FLinearColor TraceColor, FLinearColor TraceHitColor, float DrawTime, int32 MaxPortalDepth, float PortalExitOffset)
{
	static const FName TraceTag(TEXT("PortalLineTraceSingle"));
	
	const FCollisionQueryParams QueryParams = MakePortalTraceQueryParams(TraceTag, bTraceComplex, ActorsToIgnore, bIgnoreSelf, WorldContextObject);
	
	const ECollisionChannel CollisionChannel = UEngineTypes::ConvertToCollisionChannel(TraceChannel);
	
	FWPPortalTraceResult TraceResult;
	
	const bool bHit = PortalLineTraceSingleByChannel(
		WorldContextObject,
		TraceResult,
		Start,
		End,
		CollisionChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam,
		MaxPortalDepth,
		PortalExitOffset,
		DrawDebugType,
		TraceColor,
		TraceHitColor,
		DrawTime);
	
	OutStatus = TraceResult.Status;
	OutHit = bHit && !TraceResult.SceneHits.IsEmpty() ? TraceResult.SceneHits.Last().Hit : FHitResult{};
	
	return bHit;
}

bool UWPTraceLibrary::PortalLineTraceSingleDetailed(const UObject* WorldContextObject, FVector Start, FVector End,
	ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore,
	EDrawDebugTrace::Type DrawDebugType, FWPPortalTraceResult& OutResult, bool bIgnoreSelf, FLinearColor TraceColor,
	FLinearColor TraceHitColor, float DrawTime, int32 MaxPortalDepth, float PortalExitOffset)
{
	static const FName TraceTag(TEXT("PortalLineTraceSingleDetailed"));
	
	const FCollisionQueryParams QueryParams = MakePortalTraceQueryParams(TraceTag, bTraceComplex, ActorsToIgnore, bIgnoreSelf, WorldContextObject);
	
	const ECollisionChannel CollisionChannel = UEngineTypes::ConvertToCollisionChannel(TraceChannel);
	
	return PortalLineTraceSingleByChannel(
		WorldContextObject,
		OutResult,
		Start,
		End,
		CollisionChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam,
		MaxPortalDepth,
		PortalExitOffset,
		DrawDebugType,
		TraceColor,
		TraceHitColor,
		DrawTime);
}

bool UWPTraceLibrary::PortalLineTraceMulti(const UObject* WorldContextObject, FVector Start, FVector End,
	ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore,
	EDrawDebugTrace::Type DrawDebugType, TArray<FHitResult>& OutHits, EWPPortalTraceStatus& OutStatus, bool bIgnoreSelf,
	FLinearColor TraceColor, FLinearColor TraceHitColor, float DrawTime, int32 MaxPortalDepth, float PortalExitOffset)
{
	static const FName TraceTag(TEXT("PortalLineTraceMulti"));
	
	const FCollisionQueryParams QueryParams = MakePortalTraceQueryParams(TraceTag, bTraceComplex, ActorsToIgnore, bIgnoreSelf, WorldContextObject);
	
	const ECollisionChannel CollisionChannel = UEngineTypes::ConvertToCollisionChannel(TraceChannel);
	
	FWPPortalTraceResult TraceResult;
	
	const bool bHit = PortalLineTraceMultiByChannel(
		WorldContextObject,
		TraceResult,
		Start,
		End,
		CollisionChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam,
		MaxPortalDepth,
		PortalExitOffset,
		DrawDebugType,
		TraceColor,
		TraceHitColor,
		DrawTime);
	
	OutStatus = TraceResult.Status;
	
	OutHits.Reset();
	OutHits.Reserve(TraceResult.SceneHits.Num());
	
	for (const FWPPortalTraceHit& TraceHit : TraceResult.SceneHits)
	{
		OutHits.Add(TraceHit.Hit);
	}
	
	return bHit;
}

bool UWPTraceLibrary::PortalLineTraceMultiDetailed(const UObject* WorldContextObject, FVector Start, FVector End,
	ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore,
	EDrawDebugTrace::Type DrawDebugType, FWPPortalTraceResult& OutResult, bool bIgnoreSelf, FLinearColor TraceColor,
	FLinearColor TraceHitColor, float DrawTime, int32 MaxPortalDepth, float PortalExitOffset)
{
	static const FName TraceTag(TEXT("PortalLineTraceMultiDetailed"));
	
	const FCollisionQueryParams QueryParams = MakePortalTraceQueryParams(TraceTag, bTraceComplex, ActorsToIgnore, bIgnoreSelf, WorldContextObject);
	
	const ECollisionChannel CollisionChannel = UEngineTypes::ConvertToCollisionChannel(TraceChannel);
	
	return PortalLineTraceMultiByChannel(
		WorldContextObject,
		OutResult,
		Start,
		End,
		CollisionChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam,
		MaxPortalDepth,
		PortalExitOffset,
		DrawDebugType,
		TraceColor,
		TraceHitColor,
		DrawTime);
}

bool UWPTraceLibrary::PortalLineTraceTestByChannel(const UObject* WorldContextObject, const FVector& Start,
                                                   const FVector& End, ECollisionChannel TraceChannel, const FCollisionQueryParams& QueryParams,
                                                   const FCollisionResponseParams& ResponseParams, int32 MaxPortalDepth, float PortalExitOffset)
{
	if (!GEngine || !WorldContextObject)
	{
		return false;
	}
	
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	
	if (!World)
	{
		return false;
	}
	
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings)
	{
		return false;
	}
	
	const ECollisionChannel PortalTraceChannel = Settings->PortalTraceChannel.GetValue();
	
	// WPPortalTrace는 내부 포탈 탐색 전용 채널이다.
	if (!ensureMsgf(TraceChannel != PortalTraceChannel, 
		TEXT("PortalLineTraceTestByChannel requires a scene trace channel, "
			"not the configured WPPortalTrace channel")))
	{
		return false;
	}
	
	const FVector InitialDelta = End - Start;
	const float InitialDistance = InitialDelta.Size();
	
	if (InitialDistance <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	FVector CurrentStart = Start;
	FVector CurrentDirection = InitialDelta / InitialDistance;
	float RemainingDistance = InitialDistance;
	
	const int32 SafeMaxPortalDepth = FMath::Max(MaxPortalDepth, 0);
	const float SafePortalExitOffset = FMath::Max(PortalExitOffset, 0.0f);
	
	for (int32 PortalDepth = 0; PortalDepth <= SafeMaxPortalDepth; ++PortalDepth)
	{
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		
		const FVector CurrentEnd = CurrentStart + CurrentDirection * RemainingDistance;
		
		// 가장 가까운 포탈을 먼저 찾는다.
		FHitResult PortalHit;
		const bool bHitPortalVolume = World->LineTraceSingleByChannel(
			PortalHit,
			CurrentStart,
			CurrentEnd,
			PortalTraceChannel,
			QueryParams);
		
		AWormholePortalActor* HitPortal = bHitPortalVolume ? Cast<AWormholePortalActor>(PortalHit.GetActor()) : nullptr;
		
		const bool bHitValidPortal = IsValid(HitPortal);
		
		// 유효한 포탈이 없다면 전체 구간에 월드 충돌이 있는지만 검사한다.
		if (!bHitValidPortal)
		{
			return World->LineTraceTestByChannel(
				CurrentStart,
				CurrentEnd,
				TraceChannel,
				QueryParams,
				ResponseParams);
		}
		
		// 포탈과 같은 거리에 있는 월드 충돌은 제외한다.
		// Single 구현과 동일하게 거리가 같으면 포탈을 우선시 한다.
		const float SceneTestDistance = FMath::Max(PortalHit.Distance - KINDA_SMALL_NUMBER, 0.0f);
		
		if (SceneTestDistance > KINDA_SMALL_NUMBER)
		{
			const FVector SceneTestEnd = CurrentStart + CurrentDirection * SceneTestDistance;
			
			const bool bSceneHitBeforePortal =
				World->LineTraceTestByChannel(
					CurrentStart,
					SceneTestEnd,
					TraceChannel,
					QueryParams,
					ResponseParams);
			
			if (bSceneHitBeforePortal)
			{
				return true;
			}
		}
		
		// 최대 통과 횟수에 도달했다면 false를 반환함.
		if (PortalDepth >= SafeMaxPortalDepth)
		{
			return false;
		}
		
		const float DistanceToPortal = FMath::Max(PortalHit.Distance, 0.0f);
		RemainingDistance -= DistanceToPortal;
		
		// Trace가 포탈 표면에서 정확히 끝난 경우에는 더 진행할 거리가 없음.
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		
		FVector ExitStart = FVector::ZeroVector;
		FVector ExitDirection = FVector::ZeroVector;
		
		const bool bTransformed = HitPortal->TransformRayThroughPortal(
			PortalHit.ImpactPoint,
			CurrentDirection,
			ExitStart,
			ExitDirection,
			SafePortalExitOffset);
		
		// 링크가 없거나 진입 방향이 잘못되어 통과할 수 없다면 false를 반환한다.
		if (!bTransformed)
		{
			return false;
		}
		
		ExitDirection = ExitDirection.GetSafeNormal();
		if (ExitDirection.IsNearlyZero())
		{
			return false;
		}
		
		// 포탈 반대편에서 남은 거리만큼 다음 구간을 검사한다.
		CurrentStart = ExitStart;
		CurrentDirection = ExitDirection;
	}
	
	return false;
}

bool UWPTraceLibrary::PortalLineTraceSingleByChannel(const UObject* WorldContextObject, FWPPortalTraceResult& OutResult,
	const FVector& Start, const FVector& End, ECollisionChannel TraceChannel, const FCollisionQueryParams& QueryParams,
	const FCollisionResponseParams& ResponseParams, int32 MaxPortalDepth, float PortalExitOffset,
	EDrawDebugTrace::Type DrawDebugType, FLinearColor TraceColor, FLinearColor TraceHitColor, float DrawTime)
{
	OutResult = FWPPortalTraceResult{};
	
	const FVector InitialDelta = End - Start;
	const float InitialDistance = InitialDelta.Size();
	
	OutResult.RequestedDistance = InitialDistance;
	
	if (!GEngine || !WorldContextObject)
	{
		return false;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	
	if (!World)
	{
		return false;
	}
	
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings)
	{
		return false;
	}
	
	const ECollisionChannel PortalTraceChannel = Settings->PortalTraceChannel.GetValue();
	
	// WPPortalTrace는 내부 포탈 탐색 전용 채널이다.
	if (!ensureMsgf(TraceChannel != PortalTraceChannel, 
		TEXT("PortalLineTraceSingleByChannel requires a scene trace channel, "
			"not the configured WPPortalTrace channel")))
	{
		return false;
	}
	
	if (InitialDistance <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	OutResult.Status = EWPPortalTraceStatus::Completed;
	
	FVector CurrentStart = Start;
	FVector CurrentDirection = InitialDelta / InitialDistance;
	float RemainingDistance = InitialDistance;
	float AccumulatedLogicalDistance = 0.0f;
	
	const int32 SafeMaxPortalDepth = FMath::Max(MaxPortalDepth, 0);
	const float SafePortalExitOffset = FMath::Max(PortalExitOffset, 0.0f);
	
	for (int32 PortalDepth = 0; PortalDepth <= SafeMaxPortalDepth; ++PortalDepth)
	{
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		
		const FVector CurrentEnd = CurrentStart + CurrentDirection * RemainingDistance;
		
		// 실제 월드 오브젝트에 대한 Trace
		FHitResult SceneHit;
		const bool bHitScene = World->LineTraceSingleByChannel(
			SceneHit,
			CurrentStart,
			CurrentEnd,
			TraceChannel,
			QueryParams,
			ResponseParams);
		
		// 포탈 TraceVolume에 대한 전용 Trace.
		FHitResult PortalHit;
		const bool bHitPortalVolume = World->LineTraceSingleByChannel(
			PortalHit,
			CurrentStart,
			CurrentEnd,
			PortalTraceChannel,
			QueryParams);
		
		AWormholePortalActor* HitPortal = bHitPortalVolume ? Cast<AWormholePortalActor>(PortalHit.GetActor()) : nullptr;
		
		// 포탈 전용 채널에서 다른 액터가 검출된 경우에는 포탈로 취급하지 않는다.
		// WPPortalTrace 설정이 올바르다면 일반적으로 발생하지 않아야 한다.
		const bool bHitValidPortal = IsValid(HitPortal);

		// 유효한 포탈이 없거나 Scene Hit가 더 가까우면 현재 구간에서 Trace를 종료한다.
		const bool bPortalIsNearest = bHitValidPortal && (!bHitScene || PortalHit.Distance <= SceneHit.Distance + KINDA_SMALL_NUMBER);
		
		if (!bPortalIsNearest)
		{
#if ENABLE_DRAW_DEBUG
			DrawDebugLineTraceSingle(
				World,
				CurrentStart,
				CurrentEnd,
				DrawDebugType,
				bHitScene,
				SceneHit,
				TraceColor,
				TraceHitColor,
				DrawTime);
#endif
			
			if (bHitScene)
			{
				FWPPortalTraceHit TraceHit;
				TraceHit.Hit = SceneHit;
				TraceHit.LogicalDistance = AccumulatedLogicalDistance + SceneHit.Distance;
				TraceHit.SegmentIndex = PortalDepth;
				
				OutResult.SceneHits.Add(MoveTemp(TraceHit));
				OutResult.bBlockingHit = true;
				OutResult.ProcessedDistance = AccumulatedLogicalDistance + SceneHit.Distance;
				
				return true;
			}
			
			OutResult.ProcessedDistance = OutResult.RequestedDistance;
			return false;
		}
		
#if ENABLE_DRAW_DEBUG
		const FHitResult EmptyHit;
		
		DrawDebugLineTraceSingle(
			World,
			CurrentStart,
			PortalHit.ImpactPoint,
			DrawDebugType,
			false,
			EmptyHit,
			TraceColor,
			TraceHitColor,
			DrawTime);
#endif
		
		const float DistanceToPortal = FMath::Max(PortalHit.Distance, 0.0f);
		
		const float PortalLogicalDistance = AccumulatedLogicalDistance + DistanceToPortal;
		
		RemainingDistance -= DistanceToPortal;
		
		// 포탈 표면에서 Trace가 정상 종료됨.
		// 통과를 시도하지 않았으므로 PortalEvent를 만들지 않는다.
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			OutResult.ProcessedDistance = OutResult.RequestedDistance;
			return false;
		}
		
		FWPPortalTracePortalEvent PortalEvent;
		PortalEvent.DetectionHit = PortalHit;
		PortalEvent.EntryPortal = HitPortal;
		PortalEvent.ExitPortal = HitPortal->GetLinkedPortal();
		PortalEvent.EntryDirection = CurrentDirection;
		PortalEvent.LogicalDistance = PortalLogicalDistance;
		PortalEvent.PortalDepth = PortalDepth;
		
		// 포탈에는 도달했지만 통과 예산이 없으므로 실패 이벤트를 기록한다.
		if (PortalDepth >= SafeMaxPortalDepth)
		{
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::MaxDepthReached;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::MaxPortalDepthReached;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		FVector ExitStart = FVector::ZeroVector;
		FVector ExitDirection = FVector::ZeroVector;
		
		const bool bTransformed = HitPortal->TransformRayThroughPortal(
			PortalHit.ImpactPoint,
			CurrentDirection,
			ExitStart,
			ExitDirection,
			SafePortalExitOffset);
		
		// 변환에 실패한 포탈도 도달한 경로 정보로 보존한다.
		if (!bTransformed)
		{
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::TransformFailed;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::PortalTransformFailed;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		ExitDirection = ExitDirection.GetSafeNormal();
		if (ExitDirection.IsNearlyZero())
		{
			PortalEvent.ExitTraceStart = ExitStart;
			PortalEvent.ExitDirection = ExitDirection;
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::TransformFailed;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::PortalTransformFailed;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		PortalEvent.ExitTraceStart = ExitStart;
		PortalEvent.ExitDirection = ExitDirection;
		PortalEvent.Outcome = EWPPortalTracePortalOutcome::Traversed;
		
		OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
		++OutResult.PortalTraversalCount;
		
		AccumulatedLogicalDistance = PortalLogicalDistance;
				
		// 포탈 반대편에서 남은 거리만큼 다음 구간을 검사한다.		
		CurrentStart = ExitStart;
		CurrentDirection = ExitDirection;
	}
	
	return false;	
}

bool UWPTraceLibrary::PortalLineTraceMultiByChannel(const UObject* WorldContextObject, FWPPortalTraceResult& OutResult,
	const FVector& Start, const FVector& End, ECollisionChannel TraceChannel, const FCollisionQueryParams& QueryParams,
	const FCollisionResponseParams& ResponseParams, int32 MaxPortalDepth, float PortalExitOffset,
	EDrawDebugTrace::Type DrawDebugType, FLinearColor TraceColor, FLinearColor TraceHitColor, float DrawTime)
{
	OutResult = FWPPortalTraceResult{};
	
	const FVector InitialDelta = End - Start;
	const float InitialDistance = InitialDelta.Size();
	
	OutResult.RequestedDistance = InitialDistance;
	
	if (!GEngine || !WorldContextObject)
	{
		return false;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	
	if (!World)
	{
		return false;
	}
	
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings)
	{
		return false;
	}
	
	const ECollisionChannel PortalTraceChannel = Settings->PortalTraceChannel.GetValue();
	
	// WPPortalTrace는 내부 포탈 탐색 전용 채널이다.
	if (!ensureMsgf(TraceChannel != PortalTraceChannel, 
		TEXT("PortalLineTraceMultiByChannel requires a scene trace channel, "
			"not the configured WPPortalTrace channel")))
	{
		return false;
	}
	
	if (InitialDistance <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	OutResult.Status = EWPPortalTraceStatus::Completed;
	
	FVector CurrentStart = Start;
	FVector CurrentDirection = InitialDelta / InitialDistance;
	float RemainingDistance = InitialDistance;
	float AccumulatedLogicalDistance = 0.0f;
	
	const int32 SafeMaxPortalDepth = FMath::Max(MaxPortalDepth, 0);
	const float SafePortalExitOffset = FMath::Max(PortalExitOffset, 0.0f);
	
	for (int32 PortalDepth = 0; PortalDepth <= SafeMaxPortalDepth; ++PortalDepth)
	{
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		
		const FVector CurrentEnd = CurrentStart + CurrentDirection * RemainingDistance;
		
		// 현재 구간에서 가장 가까운 포탈을 찾는다.
		FHitResult PortalHit;
		const bool bHitPortalVolume = World->LineTraceSingleByChannel(
			PortalHit,
			CurrentStart,
			CurrentEnd,
			PortalTraceChannel,
			QueryParams);
		
		AWormholePortalActor* HitPortal = bHitPortalVolume ? Cast<AWormholePortalActor>(PortalHit.GetActor()) : nullptr;
		
		const bool bHitValidPortal = IsValid(HitPortal);
		
		// 포탈이 있으면 포탈 바로 앞까지만 Scene Multi Trace를 실행한다.
		// 같은 거리에 있는 Scene Hit는 제외하여 포탈을 우선한다.
		FVector SceneTraceEnd = CurrentEnd;
		bool bShouldTraceScene = true;
		
		if (bHitValidPortal)
		{
			const float SceneTraceDistance = FMath::Max(PortalHit.Distance - KINDA_SMALL_NUMBER, 0.0f);
			
			if (SceneTraceDistance <= KINDA_SMALL_NUMBER)
			{
				bShouldTraceScene = false;
			}

			else
			{
				SceneTraceEnd = CurrentStart + CurrentDirection * SceneTraceDistance;
			}
		}
		
		TArray<FHitResult> SegmentHits;
		bool bSceneBlockingHit = false;
		
		if (bShouldTraceScene)
		{
			bSceneBlockingHit = World->LineTraceMultiByChannel(
				SegmentHits,
				CurrentStart,
				SceneTraceEnd,
				TraceChannel,
				QueryParams,
				ResponseParams);
		}
		
#if ENABLE_DRAW_DEBUG
		// 포탈로 이어지는 구간은 ImpactPoint까지만 디버그 표시한다.
		const FVector DebugEnd = bHitValidPortal && !bSceneBlockingHit ? PortalHit.ImpactPoint : CurrentEnd;
		
		DrawDebugLineTraceMulti(
			World,
			CurrentStart,
			DebugEnd,
			DrawDebugType,
			bSceneBlockingHit,
			SegmentHits,
			TraceColor,
			TraceHitColor,
			DrawTime);
#endif
		
		// 구간별 정렬 상태를 유지한 채 논리적 경로 순서로 누적한다.
		for (const FHitResult& SegmentHit : SegmentHits)
		{
			FWPPortalTraceHit TraceHit;
			TraceHit.Hit = SegmentHit;
			TraceHit.LogicalDistance = AccumulatedLogicalDistance + SegmentHit.Distance;
			TraceHit.SegmentIndex = PortalDepth;
			
			OutResult.SceneHits.Add(MoveTemp(TraceHit));
		}
			
		// Multi Trace의 첫 Blocking Hit에서 전체 Trace 종료
		if (bSceneBlockingHit)
		{
			check(!SegmentHits.IsEmpty());
			check(SegmentHits.Last().bBlockingHit);
			
			OutResult.bBlockingHit = true;
			OutResult.ProcessedDistance = AccumulatedLogicalDistance + SegmentHits.Last().Distance;
			
			return true;
		}
		
		// 포탈이 없으면 현재 구간이 마지막 구간이다.
		if (!bHitValidPortal)
		{
			OutResult.ProcessedDistance = OutResult.RequestedDistance;
			
			return false;
		}
		
		const float DistanceToPortal = FMath::Max(PortalHit.Distance, 0.0f);
		
		const float PortalLogicalDistance = AccumulatedLogicalDistance + DistanceToPortal;
		
		RemainingDistance -= DistanceToPortal;
		
		// 포탈 표면에서 Trace가 정상 종료됐다.
		// 통과를 시도하지 않으므로 PortalEvent는 만들지 않는다.
		if (RemainingDistance <= KINDA_SMALL_NUMBER)
		{
			OutResult.ProcessedDistance = OutResult.RequestedDistance;
			
			return false;
		}
		
		FWPPortalTracePortalEvent PortalEvent;
		PortalEvent.DetectionHit = PortalHit;
		PortalEvent.EntryPortal = HitPortal;
		PortalEvent.ExitPortal = HitPortal->GetLinkedPortal();
		PortalEvent.EntryDirection = CurrentDirection;
		PortalEvent.LogicalDistance = PortalLogicalDistance;
		PortalEvent.PortalDepth = PortalDepth;
		
		if (PortalDepth >= SafeMaxPortalDepth)
		{
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::MaxDepthReached;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::MaxPortalDepthReached;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		FVector ExitStart = FVector::ZeroVector;
		FVector ExitDirection = FVector::ZeroVector;
		
		const bool bTransformed = HitPortal->TransformRayThroughPortal(
			PortalHit.ImpactPoint,
			CurrentDirection,
			ExitStart,
			ExitDirection,
			SafePortalExitOffset);
		
		if (!bTransformed)
		{
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::TransformFailed;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::PortalTransformFailed;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		ExitDirection = ExitDirection.GetSafeNormal();
		
		if (ExitDirection.IsNearlyZero())
		{
			PortalEvent.ExitTraceStart = ExitStart;
			PortalEvent.ExitDirection = ExitDirection;
			PortalEvent.Outcome = EWPPortalTracePortalOutcome::TransformFailed;
			
			OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
			OutResult.Status = EWPPortalTraceStatus::PortalTransformFailed;
			OutResult.ProcessedDistance = PortalLogicalDistance;
			
			return false;
		}
		
		PortalEvent.ExitTraceStart = ExitStart;
		PortalEvent.ExitDirection = ExitDirection;
		PortalEvent.Outcome = EWPPortalTracePortalOutcome::Traversed;
		
		OutResult.PortalEvents.Add(MoveTemp(PortalEvent));
		++OutResult.PortalTraversalCount;
		
		AccumulatedLogicalDistance = PortalLogicalDistance;
		
		CurrentStart = ExitStart;
		CurrentDirection = ExitDirection;
	}
	
	return false;
}

FCollisionQueryParams UWPTraceLibrary::MakePortalTraceQueryParams(FName TraceTag, bool bTraceComplex,
	const TArray<AActor*>& ActorsToIgnore, bool bIgnoreSelf, const UObject* WorldContextObject)
{
	FCollisionQueryParams QueryParams(TraceTag, SCENE_QUERY_STAT_ONLY(WPPortalTrace), bTraceComplex);
	
	// Blueprint의 기본 Trace와 동일한 추가 Hit 정보를 요청한다.
	QueryParams.bReturnPhysicalMaterial = true;
	QueryParams.bReturnFaceIndex = !UPhysicsSettings::Get()->bSuppressFaceRemapTable;
	
	QueryParams.AddIgnoredActors(ActorsToIgnore);
	
	if (bIgnoreSelf)
	{
		const AActor* IgnoreActor = Cast<AActor>(WorldContextObject);
		
		if (IgnoreActor)
		{
			QueryParams.AddIgnoredActor(IgnoreActor);
		}

		else
		{
			// Component 등 Actor 내부 객체가 World Context일 때
			// Outer 체인에서 소유 Actor를 찾는다.
			const UObject* CurrentObject = WorldContextObject;
			
			while (CurrentObject)
			{
				CurrentObject = CurrentObject->GetOuter();
				
				IgnoreActor = Cast<AActor>(CurrentObject);
				
				if (IgnoreActor)
				{
					QueryParams.AddIgnoredActor(IgnoreActor);
					break;
				}
			}
		}
	}
	
	return QueryParams;
}
