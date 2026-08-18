// Copyright 2026 Team Beaver. All Rights Reserved.
#include "WormholePortalActor.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/World.h"
#include "Rendering/WPCaptureManager.h"
#include "Rendering/LUT/WPLUTEndpointManager.h"
#include "Subsystem/WPRuntimeSubsystem.h"
#include "Subsystem/WPRegistrySubsystem.h"
#include "Subsystem/WPTransitSubsystem.h"
#include "WPSettings.h"
#include "Debug/WPPortalDebugComponent.h"
#include "Transit/WPTransitComponent.h"
#include "WPLog.h"
#include "WormholePortalStats.h"
#include "Rendering/LUT/WPLUTAsset.h"
#include "Rendering/LUT/WPLUTCatalog.h"
#include "Rendering/LUT/WPLUTTypes.h"

namespace
{
	constexpr float DefaultPortalRadius = 50.f;
	constexpr float BasePortalMeshRadius = 50.f;
	constexpr float MinPortalRadius = 1.f;

	const FName PortalOuterProxyTag(TEXT("WormholePortal.OuterProxy"));
	/**
	 * 기존 Blueprint/Map의 serialized component 구조와 analytic bounds를 보존한다.
	 * Component 자체는 유지하지만 어떤 raster path에도 참가시키지 않으며 production
	 * mask/composite는 SceneViewExtension만 소유한다.
	 */
	void ConfigurePortalProxy(UStaticMeshComponent* ProxyComponent, const FName RoleTag)
	{
		if (!ProxyComponent)
		{
			return;
		}

		ProxyComponent->ComponentTags.AddUnique(RoleTag);
		ProxyComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ProxyComponent->SetCollisionResponseToAllChannels(ECR_Ignore);
		ProxyComponent->SetGenerateOverlapEvents(false);
		ProxyComponent->SetCanEverAffectNavigation(false);
		ProxyComponent->SetCastShadow(false);
		ProxyComponent->SetReceivesDecals(false);
		ProxyComponent->SetRenderInMainPass(false);
		ProxyComponent->SetRenderInDepthPass(false);
		ProxyComponent->SetRenderCustomDepth(false);
		ProxyComponent->SetVisibleInRayTracing(false);
		ProxyComponent->SetVisibility(false);
		ProxyComponent->SetHiddenInGame(true);
	}

}

AWormholePortalActor::AWormholePortalActor()
{
	// Actor에는 capture/debug/streaming 상시 Tick 권위가 없다.
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// Portal 참조와 Transform을 Client 및 Late Join Client가 복원할 수 있게 합니다.
	bReplicates = true;
	AActor::SetReplicateMovement(true);
	
	PortalRadius = DefaultPortalRadius;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	
	OuterProxy = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("OuterProxy"));
	OuterProxy->SetupAttachment(SceneRoot);
	ConfigurePortalProxy(OuterProxy, PortalOuterProxyTag);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultProxyMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (DefaultProxyMesh.Succeeded())
	{
		OuterProxy->SetStaticMesh(DefaultProxyMesh.Object);
	}
	TriggerVolume = CreateDefaultSubobject<USphereComponent>(TEXT("TriggerVolume"));
	TriggerVolume->SetupAttachment(SceneRoot);
	
	TraceVolume = CreateDefaultSubobject<USphereComponent>(TEXT("TraceVolume"));
	TraceVolume->SetupAttachment(SceneRoot);

	PortalDebugVisualization = CreateDefaultSubobject<UWPPortalDebugComponent>(TEXT("PortalDebugVisualization"));
	PortalDebugVisualization->SetupAttachment(SceneRoot);
	
	PortalAreaStreamingSource = CreateDefaultSubobject<UWorldPartitionStreamingSourceComponent>(TEXT("PortalAreaStreamingSource"));
	PortalAreaStreamingSource->DisableStreamingSource();
	PortalAreaStreamingSource->TargetBehavior = EStreamingSourceTargetBehavior::Include;
	PortalAreaStreamingSource->TargetState = EStreamingSourceTargetState::Activated;
	PortalAreaStreamingSource->Priority = EStreamingSourcePriority::High;
	PortalAreaStreamingSource->TargetGrids.Reset();
	PortalAreaStreamingSource->Shapes.Reset();
	
	ApplyPortalSize();
	ApplyPortalCollisionSettings();
	ApplyOuterProxyNonRasterPolicy();
	
#if	WITH_EDITORONLY_DATA
	EditorLinkSpline = CreateEditorOnlyDefaultSubobject<USplineComponent>(TEXT("EditorLinkSpline"));
	if (EditorLinkSpline)
	{
		EditorLinkSpline->SetupAttachment(SceneRoot);
		EditorLinkSpline->SetHiddenInGame(true);
		EditorLinkSpline->SetVisibility(true);
		EditorLinkSpline->SetRelativeLocation(FVector::ZeroVector);
	}
	
	EditorLabel = CreateEditorOnlyDefaultSubobject<UTextRenderComponent>(TEXT("EditorLabel"));
	if (EditorLabel)
	{
		EditorLabel->SetupAttachment(SceneRoot);
		EditorLabel->SetHiddenInGame(true);
		EditorLabel->SetHorizontalAlignment(EHTA_Center);
		EditorLabel->SetVerticalAlignment(EVRTA_TextCenter);
		EditorLabel->SetTextRenderColor(FColor::Cyan);
		EditorLabel->SetWorldSize(24.f);
	}
#endif
}

void AWormholePortalActor::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AWormholePortalActor, PortalRepState);
}

void AWormholePortalActor::BeginPlay()
{
	// 로그 전용: BeginPlay 및 proxy setup의 CPU 시간을 측정합니다.
	const double BeginPlayStartSeconds = FPlatformTime::Seconds();
	Super::BeginPlay();

#if WITH_EDITOR
	if (const UWorld* World = GetWorld(); World && World->WorldType == EWorldType::PIE)
	{
		// 로그 전용: PIE debug 정책 적용 시간과 변경 전 상태를 출력합니다.
		#if !UE_BUILD_SHIPPING
		const double PieDebugPolicyStartSeconds = FPlatformTime::Seconds();
		const bool bWasDebugEnabled = bDrawPortalDebug;
		#endif
		bDrawPortalDebug = false;
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Render][ProxyDebug] PIE policy applied. Actor=%s WorldType=PIE PreviousEnabled=%d Enabled=0 EditorDefaultEnabled=1 Reason=ExcludeDebugDrawFromPIE CpuMs=%.3f"),
			*GetName(), bWasDebugEnabled ? 1 : 0,
			(FPlatformTime::Seconds() - PieDebugPolicyStartSeconds) * 1000.0);
		#endif
	}
#endif
	ApplyPortalSize();
	RefreshPortalDebugVisualization();
	RefreshPortalRepState();
	
	if (TriggerVolume)
	{
		TriggerVolume->OnComponentBeginOverlap.AddUniqueDynamic(this, &AWormholePortalActor::OnPortalTriggerBeginOverlap);
		TriggerVolume->OnComponentEndOverlap.AddUniqueDynamic(this, &AWormholePortalActor::OnPortalTriggerEndOverlap);
	}

	ApplyOuterProxyNonRasterPolicy();
	if (UWorld* World = GetWorld())
	{
		if (UWPRegistrySubsystem* RegistrySubsystem = World->GetSubsystem<UWPRegistrySubsystem>())
		{
			RegistrySubsystem->RegisterPortal(this);
		}
	}

	LogPortalProxyState(TEXT("BeginPlay"), BeginPlayStartSeconds);
	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[ActorLifetime] BeginPlay completed. Actor=%s RegistryPublishesEndpointLifecycle=1 RuntimeManagersOwnRenderResources=1 OuterProxyNonRaster=1 ActorTickEnabled=0 ActorCaptureExecution=0 ActorLUTRequests=0 ActorMIDBindings=0 ActorStreamingRuntimeState=0 CpuMs=%.4f"),
		*GetName(), (FPlatformTime::Seconds() - BeginPlayStartSeconds) * 1000.0);
	#endif
}

void AWormholePortalActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 로그 전용: EndPlay handoff 시간과 Registry 해제 결과를 출력합니다.
	#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	bool bRegistryUnregistered = false;
	#endif
	if (UWorld* World = GetWorld())
	{
		if (UWPRegistrySubsystem* RegistrySubsystem = World->GetSubsystem<UWPRegistrySubsystem>())
		{
			RegistrySubsystem->UnregisterPortal(this);
			#if !UE_BUILD_SHIPPING
			bRegistryUnregistered = true;
			#endif
		}
	}

	if (TriggerVolume)
	{
		TriggerVolume->OnComponentBeginOverlap.RemoveDynamic(this, &AWormholePortalActor::OnPortalTriggerBeginOverlap);
		TriggerVolume->OnComponentEndOverlap.RemoveDynamic(this, &AWormholePortalActor::OnPortalTriggerEndOverlap);
	}

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, Verbose,
		TEXT("[ActorLifetime] EndPlay ownership handoff completed. Actor=%s EndPlayReason=%d RegistryUnregisteredFirst=%d RuntimePairCleanupBeforeEndpointRelease=1 StreamingReleaseAuthority=StreamingSubsystem ActorRenderResourceRelease=0 ActorTickEnabled=0 CpuMs=%.4f"),
		*GetName(), static_cast<int32>(EndPlayReason), bRegistryUnregistered ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif
	Super::EndPlay(EndPlayReason);
}

void AWormholePortalActor::OnConstruction(const FTransform& Transform)
{
	// Details 패널, Spawn, Actor Transform 변경이 모두 이 경로로 들어온다. metric 보정과 컴포넌트 크기
	// 적용 시간을 함께 재서 값 이상과 에디터 Construction 과호출을 로그에서 구분할 수 있게 한다.
	// 로그 전용: construction metric 적용 CPU 시간을 측정합니다.
	const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
	Super::OnConstruction(Transform);

	const float PreviousThroatHalfLength = ThroatHalfLength;
	const float PreviousTransitionLength = TransitionLength;
	SanitizeMetricLengths();
	SanitizeStreamingSettings();
	if (!FMath::IsNearlyEqual(PreviousThroatHalfLength, ThroatHalfLength) || !FMath::IsNearlyEqual(PreviousTransitionLength, TransitionLength))
	{
		WP_LOG(this, Warning,
			TEXT("[Metric] Invalid length was clamped. Actor=%s ThroatHalfLength(a)=%.3f TransitionLength(T)=%.3f CpuMs=%.4f"),
			*GetName(), ThroatHalfLength, TransitionLength,
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
	}
	
	ApplyPortalSize();
	ApplyPortalCollisionSettings();
	ApplyOuterProxyNonRasterPolicy();

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[Metric] Construction applied. Actor=%s PortalScalePolicy=AssumedUnit PortalRadius=%.3f MouthRadius=%.3f TransitionRadius=%.3f CpuMs=%.3f"),
		*GetName(), GetPortalRadius(), GetMouthRadius(), GetTransitionRadius(),
		(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
	#endif
}

void AWormholePortalActor::RefreshPortalRepState()
{
	if (!HasAuthority()) return;

	PortalRepState.LinkedPortal = GetLinkedPortal();
	PortalRepState.PortalRadius = PortalRadius;
	PortalRepState.ThroatHalfLength = ThroatHalfLength;
	PortalRepState.TransitionLength = TransitionLength;
	PortalRepState.PortalVisualScale = PortalVisualScale;
}

void AWormholePortalActor::OnRep_PortalRepState()
{
	AWormholePortalActor* NewLinkedPortal = PortalRepState.LinkedPortal;
	if (!IsValid(NewLinkedPortal) || NewLinkedPortal == this || NewLinkedPortal->GetWorld() != GetWorld())
	{
		NewLinkedPortal = nullptr;
	}

	const bool bLinkChanged = LinkedPortal.Get() != NewLinkedPortal;
	LinkedPortal = NewLinkedPortal;

	SetMetricParametersInternal(PortalRepState.PortalRadius, PortalRepState.ThroatHalfLength, PortalRepState.TransitionLength, false);
	SetPortalVisualScaleInternal(PortalRepState.PortalVisualScale, false);
	// Replication applies authoritative state through the internal path. Once received,
	// public client-side runtime calls must obey the same uniform-scale-only contract.
	if (!bRuntimeMetricShapeInitialized)
	{
		CaptureRuntimeMetricShape(false);
	}

#if WITH_EDITOR
	if (bLinkChanged)
	{
		UpdateEditorVisualization();
	}
#endif

	if (bLinkChanged)
	{
		NotifyRegistryPortalChanged(EWPPortalChangeType::Link);
	}
}

void AWormholePortalActor::Destroyed()
{
	ClearLinkedPortal();
	
	Super::Destroyed();
}

void AWormholePortalActor::SetLinkedPortal(AWormholePortalActor* NewLinkedPortal)
{
	SetLinkedPortalInternal(NewLinkedPortal, true);
	
	if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal())
	{
		CurrentLinkedPortal->SetPortalRadiusInternal(PortalRadius, false);
		CurrentLinkedPortal->SetThroatHalfLengthInternal(ThroatHalfLength, false);
		CurrentLinkedPortal->SetTransitionLengthInternal(TransitionLength, false);
		CurrentLinkedPortal->SetPortalVisualScaleInternal(PortalVisualScale, false);
	}
}

void AWormholePortalActor::ClearLinkedPortal()
{
	SetLinkedPortal(nullptr);
}

AWormholePortalActor* AWormholePortalActor::GetLinkedPortal() const
{
	AWormholePortalActor* Portal = LinkedPortal.Get();
	return IsValid(Portal) && Portal != this ? Portal : nullptr;
}

bool AWormholePortalActor::HasLinkedPortal() const
{
	AWormholePortalActor* Portal = LinkedPortal.Get();
	return IsValid(Portal) && Portal != this;
}

bool AWormholePortalActor::IsLinkedPortalAreaReady() const
{
	const UWorld* World = GetWorld();
	if (!World || !World->IsPartitionedWorld())
	{
		return true;
	}

	const AWormholePortalActor* Destination = GetLinkedPortal();
	if (!Destination || !Destination->PortalAreaStreamingSource
		|| !Destination->PortalAreaStreamingSource->IsStreamingSourceEnabled())
	{
		return false;
	}

	return Destination->PortalAreaStreamingSource->IsStreamingCompleted();
}

UTextureRenderTargetCube* AWormholePortalActor::GetPortalCubeRenderTarget() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPCaptureEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetCaptureEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.RenderTarget.Get();
			}
		}
	}
	return nullptr;
}

FWPCubeContract AWormholePortalActor::GetPortalCubeContract() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPCaptureEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetCaptureEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.CubeContract;
			}
		}
	}
	return FWPCubeContract();
}

UVolumeTexture* AWormholePortalActor::GetLUTTexture() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPLUTEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetLUTEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.VolumeTexture.Get();
			}
		}
	}
	return nullptr;
}

float AWormholePortalActor::GetMetricOuterRadius() const
{
	const float SafePortalRadius = FMath::Max(GetPortalRadius(), MinPortalRadius);
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPLUTEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetLUTEndpointSnapshot(this, Snapshot)
				&& FMath::IsFinite(Snapshot.MetricOuterRadiusCm)
				&& Snapshot.MetricOuterRadiusCm >= SafePortalRadius)
			{
				return Snapshot.MetricOuterRadiusCm;
			}
		}
	}
	return SafePortalRadius;
}

FWPRayLUTContract AWormholePortalActor::GetLUTContract() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPLUTEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetLUTEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.Contract;
			}
		}
	}
	return FWPRayLUTContract();
}

float AWormholePortalActor::GetLUTZ() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPLUTEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetLUTEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.RatioCoordinate01;
			}
		}
	}
	return 0.0f;
}

uint32 AWormholePortalActor::GetLUTRevision() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPLUTEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetLUTEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.ResourceRevision;
			}
		}
	}
	return 0;
}
uint32 AWormholePortalActor::GetPortalCaptureGeneration() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UWPRuntimeSubsystem* RuntimeSubsystem =
			World->GetSubsystem<UWPRuntimeSubsystem>())
		{
			FWPCaptureEndpointSnapshot Snapshot;
			if (RuntimeSubsystem->GetCaptureEndpointSnapshot(this, Snapshot))
			{
				return Snapshot.CaptureGeneration;
			}
		}
	}
	return 0;
}

FName AWormholePortalActor::GetStableSelectorName() const
{
	return ResolveStableSelectorName();
}

FName AWormholePortalActor::ResolveStableSelectorName() const
{
	return GetFName();
}

void AWormholePortalActor::SetPortalRadius(float NewRadius)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (!ValidateRuntimeMetricMutation(
		NewRadius, ThroatHalfLength, TransitionLength, false,
		TEXT("SetPortalRadius"), StartSeconds))
	{
		return;
	}
	SetPortalRadiusInternal(NewRadius, true);
}

void AWormholePortalActor::SetThroatHalfLength(float NewHalfLength)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (!ValidateRuntimeMetricMutation(
		PortalRadius, NewHalfLength, TransitionLength, false,
		TEXT("SetThroatHalfLength"), StartSeconds))
	{
		return;
	}
	SetThroatHalfLengthInternal(NewHalfLength, true);
}

void AWormholePortalActor::SetTransitionLength(float NewTransitionLength)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (!ValidateRuntimeMetricMutation(
		PortalRadius, ThroatHalfLength, NewTransitionLength, false,
		TEXT("SetTransitionLength"), StartSeconds))
	{
		return;
	}
	SetTransitionLengthInternal(NewTransitionLength, true);
}

void AWormholePortalActor::InitializePhysicalMetric(
	const float NewPortalRadius,
	const float NewThroatHalfLength,
	const float NewTransitionLength)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (!ValidateRuntimeMetricMutation(
		NewPortalRadius, NewThroatHalfLength, NewTransitionLength, true,
		TEXT("InitializePhysicalMetric"), StartSeconds))
	{
		return;
	}

	const bool bWasRuntimeMetricShapeInitialized = bRuntimeMetricShapeInitialized;
	SetMetricParametersInternal(
		NewPortalRadius,
		NewThroatHalfLength,
		NewTransitionLength,
		true);

	const UWorld* World = GetWorld();
	if (World && World->IsGameWorld() && HasActorBegunPlay())
	{
		if (!bWasRuntimeMetricShapeInitialized)
		{
			CaptureRuntimeMetricShape(true);
			#if !UE_BUILD_SHIPPING
			WP_LOG(this, Verbose,
				TEXT("[Metric][RuntimeMutationGuard] Initial atomic metric shape established. Actor=%s PortalRadius=%.3f ThroatHalfLength=%.3f TransitionLength=%.3f ThroatRatio=%.6f TransitionRatio=%.6f SubsequentPolicy=UniformScaleOnly IndependentSettersAllowed=0 CpuMs=%.4f"),
				*GetName(), PortalRadius, ThroatHalfLength, TransitionLength,
				ThroatHalfLength / FMath::Max(PortalRadius, MinPortalRadius),
				TransitionLength / FMath::Max(PortalRadius, MinPortalRadius),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
			#endif
		}
		else if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal();
			IsValid(CurrentLinkedPortal) && !CurrentLinkedPortal->bRuntimeMetricShapeInitialized)
		{
			CurrentLinkedPortal->CaptureRuntimeMetricShape(false);
		}
	}
}

void AWormholePortalActor::SetMetricParameters(
	const float NewPortalRadius,
	const float NewThroatHalfLength,
	const float NewTransitionLength)
{
	InitializePhysicalMetric(NewPortalRadius, NewThroatHalfLength, NewTransitionLength);
}

void AWormholePortalActor::SetUniformPhysicalMetricScale(const float NewPhysicalMetricScale)
{
	#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	#endif
	if (!bRuntimeMetricShapeInitialized)
	{
		CaptureRuntimeMetricShape(true);
	}

	const float SafeScale = FMath::IsFinite(NewPhysicalMetricScale)
		? FMath::Max(NewPhysicalMetricScale, KINDA_SMALL_NUMBER)
		: 1.0f;
	InitializePhysicalMetric(
		RuntimeMetricBasePortalRadius * SafeScale,
		RuntimeMetricBaseThroatHalfLength * SafeScale,
		RuntimeMetricBaseTransitionLength * SafeScale);
	#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[PhysicalMetricScale] Scale applied. Actor=%s RequestedScale=%.6f AppliedScale=%.6f BaseMetric=(R=%.3f,A=%.3f,T=%.3f) CurrentMetric=(R=%.3f,A=%.3f,T=%.3f) ChangesCollisionBoundsCaptureResolution=1 CpuMs=%.4f"),
		*GetName(), NewPhysicalMetricScale, SafeScale,
		RuntimeMetricBasePortalRadius, RuntimeMetricBaseThroatHalfLength,
		RuntimeMetricBaseTransitionLength, PortalRadius, ThroatHalfLength, TransitionLength,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif
}

float AWormholePortalActor::GetUniformPhysicalMetricScale() const
{
	return bRuntimeMetricShapeInitialized && RuntimeMetricBasePortalRadius > KINDA_SMALL_NUMBER
		? PortalRadius / RuntimeMetricBasePortalRadius
		: 1.0f;
}

void AWormholePortalActor::SetPortalVisualScale(const float NewVisualScale)
{
	SetPortalVisualScaleInternal(NewVisualScale, true);
}

float AWormholePortalActor::GetPortalVisualScale() const
{
	return PortalVisualScale;
}

bool AWormholePortalActor::ValidateRuntimeMetricMutation(
	float NewPortalRadius,
	float NewThroatHalfLength,
	float NewTransitionLength,
	const bool bAtomicMetricUpdate,
	const TCHAR* ApiName,
	const double StartSeconds)
{
	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !HasActorBegunPlay())
	{
		return true;
	}

	if (!bAtomicMetricUpdate)
	{
		LogRejectedRuntimeMetricMutation(
			ApiName,
			bRuntimeMetricShapeInitialized
				? TEXT("IndependentRuntimeMetricMutation")
				: TEXT("AtomicInitialSetupRequired"),
			NewPortalRadius, NewThroatHalfLength, NewTransitionLength, StartSeconds);
		return false;
	}

	// The first atomic runtime call establishes the authored dimensionless shape. It may
	// choose arbitrary valid rho, a, and T values. Every later call must preserve that
	// shape and may only change one common scale.
	if (!bRuntimeMetricShapeInitialized)
	{
		return true;
	}

	NewPortalRadius = FMath::IsFinite(NewPortalRadius)
		? FMath::Max(NewPortalRadius, MinPortalRadius)
		: MinPortalRadius;
	NewThroatHalfLength = FMath::IsFinite(NewThroatHalfLength)
		? FMath::Max(NewThroatHalfLength, 0.0f)
		: 0.0f;
	bool bTransitionClamped = false;
	NewTransitionLength = ClampTransitionLengthToLUTDomain(
		NewTransitionLength, NewPortalRadius, bTransitionClamped);

	const float UniformScale = NewPortalRadius / FMath::Max(PortalRadius, MinPortalRadius);
	const float ExpectedThroatHalfLength = RuntimeMetricThroatRatio * NewPortalRadius;
	const float ExpectedTransitionLength = RuntimeMetricTransitionRatio * NewPortalRadius;
	const float ThroatTolerance = FMath::Max(
		1.0e-4f,
		FMath::Max(FMath::Abs(ExpectedThroatHalfLength), FMath::Abs(NewThroatHalfLength)) * 1.0e-4f);
	const float TransitionTolerance = FMath::Max(
		1.0e-4f,
		FMath::Max(FMath::Abs(ExpectedTransitionLength), FMath::Abs(NewTransitionLength)) * 1.0e-4f);
	const bool bUniformScaleOnly = FMath::IsFinite(UniformScale)
		&& UniformScale > 0.0f
		&& FMath::IsNearlyEqual(NewThroatHalfLength, ExpectedThroatHalfLength, ThroatTolerance)
		&& FMath::IsNearlyEqual(NewTransitionLength, ExpectedTransitionLength, TransitionTolerance);
	if (bUniformScaleOnly)
	{
		return true;
	}

	LogRejectedRuntimeMetricMutation(
		ApiName, TEXT("DimensionlessMetricRatioChanged"),
		NewPortalRadius, NewThroatHalfLength, NewTransitionLength, StartSeconds);
	return false;
}

void AWormholePortalActor::CaptureRuntimeMetricShape(const bool bIncludeLinkedPortal)
{
	const float SafePortalRadius = FMath::Max(PortalRadius, MinPortalRadius);
	RuntimeMetricBasePortalRadius = SafePortalRadius;
	RuntimeMetricBaseThroatHalfLength = ThroatHalfLength;
	RuntimeMetricBaseTransitionLength = TransitionLength;
	RuntimeMetricThroatRatio = ThroatHalfLength / SafePortalRadius;
	RuntimeMetricTransitionRatio = TransitionLength / SafePortalRadius;
	bRuntimeMetricShapeInitialized = true;

	if (bIncludeLinkedPortal)
	{
		if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal();
			IsValid(CurrentLinkedPortal) && CurrentLinkedPortal != this)
		{
			CurrentLinkedPortal->CaptureRuntimeMetricShape(false);
		}
	}
}

void AWormholePortalActor::LogRejectedRuntimeMetricMutation(
	const TCHAR* ApiName,
	const TCHAR* Reason,
	const float RequestedPortalRadius,
	const float RequestedThroatHalfLength,
	const float RequestedTransitionLength,
	const double StartSeconds)
{
	++RejectedRuntimeMetricMutationCount;
	const double NowSeconds = FPlatformTime::Seconds();
	constexpr double RejectionLogIntervalSeconds = 1.0;
	if (LastRuntimeMetricMutationRejectLogSeconds >= 0.0
		&& NowSeconds - LastRuntimeMetricMutationRejectLogSeconds < RejectionLogIntervalSeconds)
	{
		return;
	}

	const uint64 SuppressedSincePreviousLog = RejectedRuntimeMetricMutationCount
		- LastLoggedRuntimeMetricMutationRejectCount - 1;
	LastRuntimeMetricMutationRejectLogSeconds = NowSeconds;
	LastLoggedRuntimeMetricMutationRejectCount = RejectedRuntimeMetricMutationCount;
	WP_LOG(this, Warning,
		TEXT("[Metric][RuntimeMutationGuard] Runtime metric mutation rejected. Actor=%s Api=%s Reason=%s Requested=(R=%.3f,A=%.3f,T=%.3f) Current=(R=%.3f,A=%.3f,T=%.3f) CurrentRatios=(AOverR=%.6f,TOverR=%.6f) ShapeInitialized=%d Policy=FirstAtomicSetupThenUniformScaleOnly IndependentSettersAllowed=0 RejectedCount=%llu SuppressedSincePreviousLog=%llu LogThrottleSeconds=%.1f CpuMs=%.4f"),
		*GetName(), ApiName ? ApiName : TEXT("Unknown"), Reason ? Reason : TEXT("Unknown"),
		RequestedPortalRadius, RequestedThroatHalfLength, RequestedTransitionLength,
		PortalRadius, ThroatHalfLength, TransitionLength,
		ThroatHalfLength / FMath::Max(PortalRadius, MinPortalRadius),
		TransitionLength / FMath::Max(PortalRadius, MinPortalRadius),
		bRuntimeMetricShapeInitialized ? 1 : 0,
		static_cast<unsigned long long>(RejectedRuntimeMetricMutationCount),
		static_cast<unsigned long long>(SuppressedSincePreviousLog),
		RejectionLogIntervalSeconds,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

float AWormholePortalActor::GetPortalRadius() const
{
	return PortalRadius;
}

float AWormholePortalActor::GetMouthRadius() const
{
	return GetPortalRadius() + FMath::Max(ThroatHalfLength, 0.0f);
}

float AWormholePortalActor::GetThroatHalfLength() const
{
	return ThroatHalfLength;
}

float AWormholePortalActor::GetTransitionLength() const
{
	return TransitionLength;
}

float AWormholePortalActor::GetTransitionRadius() const
{
	return GetMouthRadius() + FMath::Max(TransitionLength, 0.0f);
}

FWPLUTDescriptor AWormholePortalActor::GetEffectiveLUTDescriptor() const
{
	/** 1. Actor's Override LUT Asset */
	if (!LUTAssetOverride.IsNull())
	{
		if (const UWPLUTAsset* Asset = LUTAssetOverride.LoadSynchronous())
		{
			FString Error;
			if (Asset->Descriptor.IsValid(&Error))
			{
				return Asset->Descriptor.GetSanitized();
			}
		}
	}
	
	/** 2. Catalog's Active LUT Asset */
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	FString GeneratedRootPath;
	FString CatalogObjectPath;
	FString PathError;
	if (IsValid(Settings)
		&& Settings->TryResolveGeneratedLUTPaths(GeneratedRootPath, CatalogObjectPath, PathError))
	{
		if (const UWPLUTCatalog* Catalog = LoadObject<UWPLUTCatalog>(nullptr, *CatalogObjectPath))
		{
			FWPLUTDescriptor ActiveDescriptor;
			const TSoftObjectPtr<UWPLUTAsset> ActiveAsset = Catalog->GetActiveLUTAsset(&ActiveDescriptor);

			if (!ActiveAsset.IsNull())
			{
				return ActiveDescriptor.GetSanitized();
			}
		}
	}
	/** 3. Default : Balanced/Standard [0.5, 8.0] */
	return FWPLUTDescriptor::MakeDefault().GetSanitized();
}

void AWormholePortalActor::NotifyRegistryPortalChanged(const EWPPortalChangeType ChangeType)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_RegistryChangeDispatch);
	RefreshPortalRepState();
	if (HasAuthority() && GetIsReplicated())
	{
		// Link and physical Metric changes are infrequent contract boundaries and request an
		// immediate network update. Per-frame Visual Scale uses the Actor's normal replication
		// cadence so the recommended animation path does not force one network update per Tick.
		// 서로 다른 Actor Channel 사이의 도착 순서는 보장하지 않으므로 Transit은 별도의 Mapping snapshot을 사용합니다.
		if (ChangeType != EWPPortalChangeType::Visual)
		{
			ForceNetUpdate();
		}
	}

	if (UWorld* World = GetWorld())
	{
		if (UWPRegistrySubsystem* RegistrySubsystem = World->GetSubsystem<UWPRegistrySubsystem>())
		{
			INC_DWORD_STAT(STAT_WP_RegistryChangeEvents);
			RegistrySubsystem->NotifyPortalChanged(const_cast<AWormholePortalActor*>(this), ChangeType);
		}
	}
}

void AWormholePortalActor::SetDrawPortalDebug(const bool bEnabled)
{
	bDrawPortalDebug = bEnabled;
	RefreshPortalDebugVisualization();
}

bool AWormholePortalActor::IsPortalDebugEnabled() const
{
	return bDrawPortalDebug;
}

bool AWormholePortalActor::TransformRayThroughPortal(const FVector& EntrySurfacePoint,
	const FVector& EntryWorldDirection, FVector& OutExitStart, FVector& OutExitDirection, float ExitSurfaceOffset) const
{
	
	
	OutExitStart = FVector::ZeroVector;
	OutExitDirection = FVector::ZeroVector;
	
	AWormholePortalActor* ExitPortal = GetLinkedPortal();
	if (!ExitPortal)
	{
		return false;
	}
	
	const FVector SafeWorldDirection = EntryWorldDirection.GetSafeNormal();
	if (SafeWorldDirection.IsNearlyZero())
	{
		return false;
	}
	
	const FTransform EntryTransform = GetActorTransform();
	const FTransform ExitTransform = ExitPortal->GetActorTransform();
	
	FVector LocalEntryPoint = EntryTransform.InverseTransformPositionNoScale(EntrySurfacePoint);
	if (LocalEntryPoint.IsNearlyZero())
	{
		return false;
	}
	
	const FVector LocalSurfaceNormal = LocalEntryPoint.GetSafeNormal();
	
	const FVector LocalDirection = EntryTransform.InverseTransformVectorNoScale(SafeWorldDirection).GetSafeNormal();
	
	if (LocalDirection.IsNearlyZero())
	{
		return false;
	}
	
	const float SurfaceCrossingDot = FVector::DotProduct(LocalSurfaceNormal, LocalDirection);
	if (SurfaceCrossingDot >= -1.e-3f)
	{
		return false;
	}
	
	const float EntryRadius = GetPortalRadius();
	const float ExitRadius = ExitPortal->GetPortalRadius();
	
	LocalEntryPoint = LocalSurfaceNormal * EntryRadius;
	
	const float DistanceToExitSurface = -2.f * FVector::DotProduct(LocalEntryPoint, LocalDirection);
	if (DistanceToExitSurface <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	
	FVector LocalExitPoint = LocalEntryPoint + LocalDirection * DistanceToExitSurface;
	if (LocalExitPoint.IsNearlyZero())
	{
		return false;
	}
	
	LocalExitPoint = LocalExitPoint.GetSafeNormal() * ExitRadius;
	
	OutExitDirection = ExitTransform.TransformVectorNoScale(LocalDirection).GetSafeNormal();
	if (OutExitDirection.IsNearlyZero())
	{
		return false;
	}
	
	OutExitStart = ExitTransform.TransformPositionNoScale(LocalExitPoint) + OutExitDirection * FMath::Max(ExitSurfaceOffset, 0.f);
	
	return true;
	
}

void AWormholePortalActor::SetLinkedPortalInternal(AWormholePortalActor* NewLinkedPortal, bool bUpdateOtherPortal)
{
	// 로그 전용: link 요청 처리 CPU 시간을 측정합니다.
	const double LinkStartSeconds = FPlatformTime::Seconds();
	if (NewLinkedPortal == this)
	{
		WP_LOG(this, Warning,
			TEXT("[PortalLink] Self-link rejected. Actor=%s RequestedPortal=%s UpdateOtherPortal=%d CpuMs=%.4f"),
			*GetPathName(), *GetPathNameSafe(NewLinkedPortal), bUpdateOtherPortal ? 1 : 0,
			(FPlatformTime::Seconds() - LinkStartSeconds) * 1000.0);
		NewLinkedPortal = nullptr;
	}
	
	if (!IsValid(NewLinkedPortal))
	{
		NewLinkedPortal = nullptr;
	}

	if (NewLinkedPortal && NewLinkedPortal->GetWorld() != GetWorld())
	{
		WP_LOG(this, Warning,
			TEXT("[PortalLink] Cross-world link rejected before topology mutation. Actor=%s ActorWorld=%s RequestedPortal=%s RequestedWorld=%s ExistingPortal=%s UpdateOtherPortal=%d RegistryNotification=0 CounterpartMutation=0 CpuMs=%.4f"),
			*GetPathName(), *GetPathNameSafe(GetWorld()),
			*GetPathNameSafe(NewLinkedPortal), *GetPathNameSafe(NewLinkedPortal->GetWorld()),
			*GetPathNameSafe(LinkedPortal.Get()), bUpdateOtherPortal ? 1 : 0,
			(FPlatformTime::Seconds() - LinkStartSeconds) * 1000.0);
		return;
	}
	
	if (LinkedPortal.Get() == NewLinkedPortal)
	{
		if (bUpdateOtherPortal && IsValid(NewLinkedPortal) && NewLinkedPortal->LinkedPortal.Get() != this)
		{
			NewLinkedPortal->SetLinkedPortalInternal(this, true);
		}
		
		return;
	}
	
	AWormholePortalActor* PreviousLinkedPortal = LinkedPortal.Get();
	
#if WITH_EDITOR
	ModifyForLinkedPortalChange();
#endif
	
	LinkedPortal = NewLinkedPortal;
	
#if WITH_EDITOR
	UpdateEditorVisualization();
	
	if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal())
	{
		CurrentLinkedPortal->UpdateEditorVisualization();
	}
#endif
	
	if (!bUpdateOtherPortal)
	{
		NotifyRegistryPortalChanged(EWPPortalChangeType::Link);
		return;
	}
	
	if (IsValid(PreviousLinkedPortal) && PreviousLinkedPortal->LinkedPortal.Get() == this)
	{
		PreviousLinkedPortal->SetLinkedPortalInternal(nullptr, false);
	}
	
	if (IsValid(NewLinkedPortal) && NewLinkedPortal->LinkedPortal.Get() != this)
	{
		NewLinkedPortal->SetLinkedPortalInternal(this, true);
	}

	NotifyRegistryPortalChanged(EWPPortalChangeType::Link);
}

void AWormholePortalActor::SetMetricParametersInternal(
	float NewPortalRadius,
	float NewThroatHalfLength,
	float NewTransitionLength,
	const bool bUpdateLinkedPortal)
{
	#if !UE_BUILD_SHIPPING
	const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
	#endif
	const float RequestedRadius = NewPortalRadius;
	const float RequestedThroatHalfLength = NewThroatHalfLength;
	const float RequestedTransitionLength = NewTransitionLength;

	NewPortalRadius = FMath::IsFinite(NewPortalRadius)
		? FMath::Max(NewPortalRadius, MinPortalRadius)
		: MinPortalRadius;
	NewThroatHalfLength = FMath::IsFinite(NewThroatHalfLength)
		? FMath::Max(NewThroatHalfLength, 0.0f)
		: 0.0f;

	bool bTransitionClamped = false;
	NewTransitionLength = ClampTransitionLengthToLUTDomain(
		NewTransitionLength,
		NewPortalRadius,
		bTransitionClamped);

	const bool bRadiusClamped = !FMath::IsFinite(RequestedRadius)
		|| !FMath::IsNearlyEqual(RequestedRadius, NewPortalRadius);
	const bool bThroatClamped = !FMath::IsFinite(RequestedThroatHalfLength)
		|| !FMath::IsNearlyEqual(RequestedThroatHalfLength, NewThroatHalfLength);
	const bool bMetricChanged =
		!FMath::IsNearlyEqual(PortalRadius, NewPortalRadius)
		|| !FMath::IsNearlyEqual(ThroatHalfLength, NewThroatHalfLength)
		|| !FMath::IsNearlyEqual(TransitionLength, NewTransitionLength);

#if WITH_EDITOR
	if (bMetricChanged)
	{
		ModifyForLinkedPortalChange();
	}
#endif

	PortalRadius = NewPortalRadius;
	ThroatHalfLength = NewThroatHalfLength;
	TransitionLength = NewTransitionLength;

	ApplyPortalSize();

#if WITH_EDITOR
	UpdateEditorVisualization();
#endif

	if (bUpdateLinkedPortal && IsValid(LinkedPortal.Get()))
	{
		LinkedPortal->SetMetricParametersInternal(
			PortalRadius,
			ThroatHalfLength,
			TransitionLength,
			false);
	}

	if (bRadiusClamped || bThroatClamped || bTransitionClamped)
	{
		const FWPLUTDescriptor Descriptor = GetEffectiveLUTDescriptor();
		WP_LOG(this, Warning,
			TEXT("[LUTDomain] Atomic metric parameters clamped. Actor=%s Requested=(R=%.3f,A=%.3f,T=%.3f) Applied=(R=%.3f,A=%.3f,T=%.3f) AppliedRatio=%.6f Domain=[%.6f, %.6f]"),
			*GetPathName(),
			RequestedRadius, RequestedThroatHalfLength, RequestedTransitionLength,
			PortalRadius, ThroatHalfLength, TransitionLength,
			TransitionLength > KINDA_SMALL_NUMBER ? TransitionLength / PortalRadius : 0.0f,
			Descriptor.TransitionRatioMin, Descriptor.TransitionRatioMax);
	}

	if (bMetricChanged)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, VeryVerbose,
			TEXT("[Metric] Atomic parameters changed. Actor=%s PortalRadius=%.3f ThroatHalfLength=%.3f TransitionLength=%.3f LinkedPropagation=%d CpuMs=%.3f"),
			*GetName(), PortalRadius, ThroatHalfLength, TransitionLength, bUpdateLinkedPortal ? 1 : 0,
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
		#endif
		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
	}
}

void AWormholePortalActor::SetPortalVisualScaleInternal(
	const float NewVisualScale,
	const bool bUpdateLinkedPortal)
{
	const float SafeVisualScale = FMath::IsFinite(NewVisualScale)
		? FMath::Max(NewVisualScale, KINDA_SMALL_NUMBER)
		: 1.0f;
	if (FMath::IsNearlyEqual(PortalVisualScale, SafeVisualScale))
	{
		return;
	}

	PortalVisualScale = SafeVisualScale;
	if (bUpdateLinkedPortal && IsValid(LinkedPortal.Get()))
	{
		LinkedPortal->SetPortalVisualScaleInternal(SafeVisualScale, false);
	}

	NotifyRegistryPortalChanged(EWPPortalChangeType::Visual);
}

void AWormholePortalActor::SetPortalRadiusInternal(float NewRadius, bool bUpdateLinkedPortal)
{
	// 포탈 Actor Scale은 기본 사용 계약상 1로 가정하므로 PortalRadius를 로컬/월드 seam 반지름으로 그대로 사용한다.
	// 로그 전용: radius 변경 처리 CPU 시간을 측정합니다.
	#if !UE_BUILD_SHIPPING
	const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
	#endif
	const float RequestedRadius = NewRadius;
	NewRadius = FMath::IsFinite(NewRadius)
		? FMath::Max(NewRadius, MinPortalRadius)
		: MinPortalRadius;

	const FWPLUTDescriptor Descriptor = GetEffectiveLUTDescriptor();
	if (TransitionLength > KINDA_SMALL_NUMBER)
	{
		const float MinRadiusForLUT = TransitionLength / Descriptor.TransitionRatioMax;
		const float MaxRadiusForLUT = TransitionLength / Descriptor.TransitionRatioMin;
		NewRadius = FMath::Clamp(
			NewRadius,
			FMath::Max(MinPortalRadius, MinRadiusForLUT),
			FMath::Max(MinPortalRadius, MaxRadiusForLUT));
	}

	const bool bRadiusClamped = !FMath::IsFinite(RequestedRadius)
		|| !FMath::IsNearlyEqual(RequestedRadius, NewRadius);
	const bool bRadiusChanged = !FMath::IsNearlyEqual(PortalRadius, NewRadius);

#if WITH_EDITOR
	if (bRadiusChanged)
	{
		ModifyForLinkedPortalChange();	
	}
#endif
	// Details 패널 변경 경로에서는 PortalRadius가 이미 바뀐 상태로 들어올 수 있다.
	// 값이 같아 보여도 컴포넌트 크기와 링크된 포탈 크기는 다시 적용한다.
	PortalRadius = NewRadius;
	ApplyPortalSize();

	if (bRadiusClamped)
	{
		WP_LOG(this, Warning,
			TEXT("[LUTDomain] PortalRadius clamped. Actor=%s RequestedRadius=%.3f ClampedRadius=%.3f TransitionLength=%.3f ClampedRatio=%.6f Domain=[%.6f, %.6f]"),
			*GetPathName(), RequestedRadius, PortalRadius, TransitionLength,
			TransitionLength > KINDA_SMALL_NUMBER ? TransitionLength / PortalRadius : 0.0f,
			Descriptor.TransitionRatioMin, Descriptor.TransitionRatioMax);
	}
	
#if WITH_EDITOR
	UpdateEditorVisualization();
#endif
	// 링크된 포탈은 같은 반지름을 가져야 하므로 한 방향으로만 크기를 전파한다.
	if (bUpdateLinkedPortal && IsValid(LinkedPortal.Get()))
	{
		LinkedPortal->SetPortalRadiusInternal(PortalRadius, false);
	}

	if (bRadiusChanged)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Metric] Portal radius changed. Actor=%s PortalScalePolicy=AssumedUnit PortalRadius=%.3f MouthRadius=%.3f TransitionRadius=%.3f LinkedPropagation=%d CpuMs=%.3f"),
			*GetName(), GetPortalRadius(), GetMouthRadius(), GetTransitionRadius(),
			bUpdateLinkedPortal ? 1 : 0,
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
		#endif

		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
	}
}

void AWormholePortalActor::SetThroatHalfLengthInternal(float NewHalfLength, const bool bUpdateLinkedPortal)
{
	// 로그 전용: throat 길이 변경 처리 CPU 시간을 측정합니다.
	#if !UE_BUILD_SHIPPING
	const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
	#endif
	NewHalfLength = FMath::Max(NewHalfLength, 0.0f);
	const bool bLengthChanged = !FMath::IsNearlyEqual(ThroatHalfLength, NewHalfLength);

#if WITH_EDITOR
	if (bLengthChanged)
	{
		ModifyForLinkedPortalChange();
	}
#endif

	ThroatHalfLength = NewHalfLength;
	ApplyPortalSize();

#if WITH_EDITOR
	UpdateEditorVisualization();
#endif

	if (bUpdateLinkedPortal && IsValid(LinkedPortal.Get()))
	{
		LinkedPortal->SetThroatHalfLengthInternal(ThroatHalfLength, false);
	}

	if (bLengthChanged)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Metric] Throat half length changed. Actor=%s PortalRadius=%.3f ThroatHalfLength=%.3f TransitionLength=%.3f LinkedPropagation=%d CpuMs=%.3f"),
			*GetName(), GetPortalRadius(), ThroatHalfLength, TransitionLength,
			bUpdateLinkedPortal ? 1 : 0,
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
		#endif
		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
	}
}

void AWormholePortalActor::SetTransitionLengthInternal(float NewTransitionLength, const bool bUpdateLinkedPortal)
{
	// 로그 전용: transition 길이 변경 처리 CPU 시간을 측정합니다.
	#if !UE_BUILD_SHIPPING
	const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
	#endif
	const float RequestedTransitionLength = NewTransitionLength;
	
	bool bRatioClamped = false;
	NewTransitionLength = ClampTransitionLengthToLUTDomain(
		NewTransitionLength,
		PortalRadius,
		bRatioClamped);
	
	if (bRatioClamped)
	{
		const FWPLUTDescriptor Descriptor = GetEffectiveLUTDescriptor();
		
		WP_LOG(this, Warning,
			TEXT("[LUTDomain] TransitionLength clamped. "
				 "Actor=%s Radius=%.3f RequestedLength=%.3f "
				 "RequestedRatio=%.6f ClampedLength=%.3f "
				 "ClampedRatio=%.6f Domain=[%.6f, %.6f]"),
			 *GetPathName(),
			 PortalRadius,
			 RequestedTransitionLength,
			 RequestedTransitionLength / FMath::Max(PortalRadius, MinPortalRadius),
			 NewTransitionLength,
			 NewTransitionLength / FMath::Max(PortalRadius, MinPortalRadius),
			 Descriptor.TransitionRatioMin,
			 Descriptor.TransitionRatioMax);
	}

	const bool bLengthChanged = !FMath::IsNearlyEqual(TransitionLength, NewTransitionLength);

#if WITH_EDITOR
	if (bLengthChanged)
	{
		ModifyForLinkedPortalChange();
	}
#endif

	TransitionLength = NewTransitionLength;
	ApplyPortalSize();

#if WITH_EDITOR
	UpdateEditorVisualization();
#endif

	if (bUpdateLinkedPortal && IsValid(LinkedPortal.Get()))
	{
		LinkedPortal->SetTransitionLengthInternal(TransitionLength, false);
	}

	if (bLengthChanged)
	{
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Metric] Transition length changed. Actor=%s PortalRadius=%.3f ThroatHalfLength=%.3f TransitionLength=%.3f LinkedPropagation=%d CpuMs=%.3f"),
			*GetName(), GetPortalRadius(), ThroatHalfLength, TransitionLength,
			bUpdateLinkedPortal ? 1 : 0,
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
		#endif
		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
	}
}

void AWormholePortalActor::SanitizeMetricLengths()
{
	ThroatHalfLength = FMath::Max(ThroatHalfLength, 0.0f);
	
	bool bClamped = false;
	TransitionLength = ClampTransitionLengthToLUTDomain(
		TransitionLength,
		PortalRadius,
		bClamped);

	if (bClamped)
	{
		WP_LOG(this, Warning,
			TEXT("[LUTDomain] Serialized metric was outside "
				 "the active LUT domain and was clamped. Actor=%s"),
			*GetPathName());
	}
}

float AWormholePortalActor::ClampTransitionLengthToLUTDomain(float RequestedTransitionLength, float Radius,
	bool& bOutClamped) const
{
	const float SafeRadius = FMath::Max(Radius, MinPortalRadius);
	const float SafeLength = FMath::IsFinite(RequestedTransitionLength)
	? FMath::Max(RequestedTransitionLength, 0.0f)
	: 0.0f;
	
	/** T==0 */
	if (SafeLength <= KINDA_SMALL_NUMBER)
	{
		bOutClamped = !FMath::IsNearlyZero(RequestedTransitionLength);
		return 0.0f;
	}
	
	/** T!=0 */
	const FWPLUTDescriptor Descriptor = GetEffectiveLUTDescriptor();
	
	const float RequestRatio = SafeLength / SafeRadius;
	const float ClampedRatio = Descriptor.ClampTransitionRatio(RequestRatio);
	
	bOutClamped = !FMath::IsNearlyEqual(
		RequestRatio,
		ClampedRatio,
		FMath::Max(1.0e-5f, RequestRatio * 1.0e-5f));
	
	return ClampedRatio * SafeRadius;
}

void AWormholePortalActor::SanitizeStreamingSettings()
{
	StreamingPreloadDistance = FMath::Max(StreamingPreloadDistance, 0.0f);
	StreamingReleaseDistance = FMath::Max(StreamingReleaseDistance, StreamingPreloadDistance);
	StreamingQueryInterval = FMath::Max(StreamingQueryInterval, 0.01f);
}

void AWormholePortalActor::ApplyPortalSize() const
{
	// TriggerVolume이 이동과 렌더 계산 모두에서 사용하는 seam의 단일 기준이다. TraceVolume은 같은
	// PortalRadius를 공유하고, Mouth와 Transition은 실제 이동 판정을 넓히지 않는 렌더 metric 경계다.
	TriggerVolume->SetSphereRadius(GetPortalRadius());
	TraceVolume->SetSphereRadius(GetPortalRadius());
	
	// PortalRadius는 cm 단위 반지름이고, BasePortalMeshRadius는 Scale 1 메시의 기준 반지름이다.
	// Actor Scale은 기본 사용 계약상 1로 가정한다. OuterProxy의 Relative Scale은 Actor Scale 보정이 아니라
	// TransitionRadius를 기준 구체 Mesh 크기로 표현하는 내부 값이다.
	const float TransitionVisualScale = GetTransitionRadius() / BasePortalMeshRadius;
	OuterProxy->SetRelativeScale3D(FVector(TransitionVisualScale));
	RefreshPortalDebugVisualization();
}

void AWormholePortalActor::LogPortalProxyState(const TCHAR* Phase, const double StartSeconds) const
{
	// Phase와 StartSeconds는 이 로그의 context/CPU 시간 출력에만 사용합니다.
	const UStaticMesh* SourceMesh = OuterProxy ? OuterProxy->GetStaticMesh() : nullptr;
	const bool bNonRasterBoundsPolicyApplied = OuterProxy
		&& !OuterProxy->IsVisible()
		&& OuterProxy->bHiddenInGame
		&& !OuterProxy->bRenderInMainPass
		&& !OuterProxy->bRenderInDepthPass
		&& !OuterProxy->bRenderCustomDepth;
	const bool bSetupValid = OuterProxy && SourceMesh && TriggerVolume
		&& bNonRasterBoundsPolicyApplied;

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[Render][Proxy] State. Actor=%s Phase=%s Setup=%s BoundsProxy=OuterProxy SeamSource=TriggerVolume SourceMesh=%s MaterialInspection=0 MaterialMutation=0 RasterPolicy=AlwaysDisabled PortalScalePolicy=AssumedUnit TransitionRadius=%.3f MouthRadius=%.3f SeamRadius=%.3f OuterRelativeScale=%s TransitionFormula=PortalRadius+ThroatHalfLength+TransitionLength MouthFormula=PortalRadius+ThroatHalfLength SeamFormula=PortalRadius OuterRole=SerializedAnalyticBounds TriggerRole=MovementAndSeam InnerProxyPresent=0 ProxyVisible=%d HiddenInGame=%d ProxyMainPass=%d ProxyDepthPass=%d CustomDepth=%d NonRasterPolicyApplied=%d Collision=NoCollision OuterTag=%s DebugEnabled=%d CpuMs=%.3f"),
		*GetName(), Phase, bSetupValid ? TEXT("Ready") : TEXT("Invalid"), *GetNameSafe(SourceMesh),
		GetTransitionRadius(), GetMouthRadius(), GetPortalRadius(),
		OuterProxy ? *OuterProxy->GetRelativeScale3D().ToCompactString() : TEXT("Invalid"),
		OuterProxy && OuterProxy->IsVisible() ? 1 : 0,
		OuterProxy && OuterProxy->bHiddenInGame ? 1 : 0,
		OuterProxy && OuterProxy->bRenderInMainPass ? 1 : 0,
		OuterProxy && OuterProxy->bRenderInDepthPass ? 1 : 0,
		OuterProxy && OuterProxy->bRenderCustomDepth ? 1 : 0,
		bNonRasterBoundsPolicyApplied ? 1 : 0,
		*PortalOuterProxyTag.ToString(), bDrawPortalDebug ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif

	if (!bSetupValid)
	{
		WP_LOG(this, Error,
			TEXT("[Render][Proxy] Setup validation failed. Actor=%s Phase=%s SourceMeshValid=%d OuterValid=%d TriggerValid=%d NonRasterPolicyApplied=%d MainPass=%d DepthPass=%d CustomDepth=%d InnerProxyExpected=0 CpuMs=%.3f"),
			*GetName(), Phase, SourceMesh ? 1 : 0, OuterProxy ? 1 : 0, TriggerVolume ? 1 : 0,
			bNonRasterBoundsPolicyApplied ? 1 : 0,
			OuterProxy && OuterProxy->bRenderInMainPass ? 1 : 0,
			OuterProxy && OuterProxy->bRenderInDepthPass ? 1 : 0,
			OuterProxy && OuterProxy->bRenderCustomDepth ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
}

void AWormholePortalActor::ApplyOuterProxyNonRasterPolicy() const
{
	// 로그 전용: non-raster proxy 정책 적용 CPU 시간을 측정합니다.
	#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	#endif
	if (!OuterProxy)
	{
		return;
	}

	// Keep the serialized component and authored bounds without creating any visible output.
	// Material inspection/mutation is intentionally absent; content migration removed overrides.
	OuterProxy->SetCanEverAffectNavigation(false);
	OuterProxy->SetCastShadow(false);
	OuterProxy->SetReceivesDecals(false);
	OuterProxy->SetVisibleInRayTracing(false);
	OuterProxy->SetVisibility(false, true);
	OuterProxy->SetHiddenInGame(true);
	OuterProxy->SetRenderInMainPass(false);
	OuterProxy->SetRenderInDepthPass(false);
	OuterProxy->SetRenderCustomDepth(false);

	#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[ActorProxy] Non-raster bounds policy applied. Actor=%s Registered=%d Visible=%d HiddenInGame=%d MainPass=%d DepthPass=%d CustomDepth=%d SerializedComponentPreserved=1 MaterialInspection=0 MaterialMutation=0 RasterFallbackAvailable=0 CpuMs=%.4f"),
		*GetName(), OuterProxy->IsRegistered() ? 1 : 0,
		OuterProxy->IsVisible() ? 1 : 0, OuterProxy->bHiddenInGame ? 1 : 0,
		OuterProxy->bRenderInMainPass ? 1 : 0,
		OuterProxy->bRenderInDepthPass ? 1 : 0,
		OuterProxy->bRenderCustomDepth ? 1 : 0,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif
}
void AWormholePortalActor::RefreshPortalDebugVisualization() const
{
	// 로그 전용: editor debug scene-proxy 갱신 CPU 시간을 측정합니다.
	#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
	#endif
	if (!PortalDebugVisualization)
	{
		return;
	}

	FTransform SeamRelativeTransform = TriggerVolume
		? TriggerVolume->GetRelativeTransform()
		: FTransform::Identity;
	SeamRelativeTransform.SetScale3D(FVector::OneVector);
	PortalDebugVisualization->SetPortalDebugData(
		bDrawPortalDebug, GetPortalRadius(), GetMouthRadius(), GetTransitionRadius(), SeamRelativeTransform);
	#if !UE_BUILD_SHIPPING
	WP_LOG(this, VeryVerbose,
		TEXT("[Render][ProxyDebug] Persistent scene-proxy data refreshed. Actor=%s Enabled=%d TransitionRadius=%.3f MouthRadius=%.3f SeamRadius=%.3f ActorTickEnabled=0 DrawDebugCalls=0 PersistentPrimitiveComponent=1 CpuMs=%.4f"),
		*GetName(), bDrawPortalDebug ? 1 : 0, GetTransitionRadius(), GetMouthRadius(),
		GetPortalRadius(), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	#endif
}

void AWormholePortalActor::ApplyPortalCollisionSettings() const
{
	OuterProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OuterProxy->SetCollisionResponseToAllChannels(ECR_Ignore);
	OuterProxy->SetGenerateOverlapEvents(false);
	
	TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TriggerVolume->SetCollisionObjectType(ECC_WorldDynamic);
	TriggerVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	TriggerVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	TriggerVolume->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Overlap);
	TriggerVolume->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
	TriggerVolume->SetGenerateOverlapEvents(true);
	
	TraceVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TraceVolume->SetCollisionObjectType(ECC_WorldDynamic);
	TraceVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	TraceVolume->SetGenerateOverlapEvents(false);
	
	ApplyPortalTraceChannelResponses();
}

void AWormholePortalActor::ApplyPortalTraceChannelResponses() const
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const ECollisionChannel PortalTraceChannel = Settings->PortalTraceChannel;
	
	TraceVolume->SetCollisionResponseToChannel(PortalTraceChannel, ECR_Block);
}

void AWormholePortalActor::OnPortalTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	(void)OverlappedComponent;
	(void)OtherBodyIndex;
	(void)bFromSweep;
	(void)SweepResult;

	if (!HasAuthority() || !IsValid(OtherActor) || OtherActor == this)
	{
		return;
	}

	UWPTransitComponent* TransitComponent = OtherActor->FindComponentByClass<UWPTransitComponent>();
	if (!IsValid(TransitComponent) || !TransitComponent->GetTransitEnabled())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (UWPTransitSubsystem* TransitSubsystem = World->GetSubsystem<UWPTransitSubsystem>())
		{
			TransitSubsystem->TryStart(TransitComponent, this, OtherComp);
		}
	}
}

void AWormholePortalActor::OnPortalTriggerEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	(void)OverlappedComponent;
	(void)OtherActor;
	(void)OtherComp;
	(void)OtherBodyIndex;

}

#if WITH_EDITOR
void AWormholePortalActor::ModifyForLinkedPortalChange()
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	
	const UWorld* World = GetWorld();
	if (!World || World->IsGameWorld())
	{
		return;
	}
	Modify();
}

void AWormholePortalActor::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
	
	if (PropertyAboutToChange && PropertyAboutToChange->GetName() == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, LinkedPortal))
	{
		LinkedPortalBeforeEdit = LinkedPortal.Get();
	}
}

void AWormholePortalActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, LUTAssetOverride))
	{
		SanitizeMetricLengths();
		ApplyPortalSize();
		
		if (IsValid(LinkedPortal.Get()))
		{
			LinkedPortal->SetTransitionLengthInternal(TransitionLength, false);
		}
		
		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
		return;
	}
	
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, bDrawPortalDebug))
	{
		// 로그 전용: debug toggle 처리 CPU 시간을 측정합니다.
		#if !UE_BUILD_SHIPPING
		const double DebugToggleStartSeconds = FPlatformTime::Seconds();
		#endif
		SetDrawPortalDebug(bDrawPortalDebug);
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Render][ProxyDebug] Toggle changed. Actor=%s Enabled=%d Renderer=PersistentSceneProxy ActorTickEnabled=0 DrawDebugCalls=0 TransitionRadius=%.3f MouthRadius=%.3f SeamRadius=%.3f PortalScalePolicy=AssumedUnit CpuMs=%.4f"),
			*GetName(), bDrawPortalDebug ? 1 : 0,
			GetTransitionRadius(), GetMouthRadius(), GetPortalRadius(),
			(FPlatformTime::Seconds() - DebugToggleStartSeconds) * 1000.0);
		#endif
		return;
	}
	
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, PortalRadius))
	{
		const float RadiusBeforeSanitize = PortalRadius;
		SetPortalRadiusInternal(PortalRadius, true);
		// Details 패널은 UPROPERTY 값을 먼저 쓴 뒤 이 함수에 들어오므로, 정상 범위 값은
		// SetPortalRadiusInternal의 값 비교만으로 변경을 감지할 수 없다. Clamp가 내부 알림을
		// 발생시키지 않은 일반 편집 경로를 여기서 보완한다.
		if (FMath::IsNearlyEqual(RadiusBeforeSanitize, PortalRadius))
		{
			NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
		}
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, ThroatHalfLength)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, TransitionLength))
	{
		// 로그 전용: Details metric 변경 처리 CPU 시간을 측정합니다.
		#if !UE_BUILD_SHIPPING
		const double MetricUpdateStartSeconds = FPlatformTime::Seconds();
		#endif
		if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, ThroatHalfLength))
		{
			SetThroatHalfLengthInternal(ThroatHalfLength, true);
		}
		else
		{
			SetTransitionLengthInternal(TransitionLength, true);
		}
		#if !UE_BUILD_SHIPPING
		WP_LOG(this, Verbose,
			TEXT("[Metric] Length changed and non-raster analytic bounds resized. Actor=%s PortalScalePolicy=AssumedUnit PortalRadius=%.3f ThroatHalfLength(a)=%.3f MouthRadius=%.3f TransitionLength(T)=%.3f TransitionRadius=%.3f SeamSource=TriggerVolume InnerProxyPresent=0 CpuMs=%.3f"),
			*GetName(), GetPortalRadius(), ThroatHalfLength, GetMouthRadius(), TransitionLength, GetTransitionRadius(),
			(FPlatformTime::Seconds() - MetricUpdateStartSeconds) * 1000.0);
		#endif
		NotifyRegistryPortalChanged(EWPPortalChangeType::Metric);
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, StreamingPreloadDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, StreamingReleaseDistance)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, StreamingQueryInterval))
	{
		SanitizeStreamingSettings();
		return;
	}
	
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AWormholePortalActor, LinkedPortal))
	{
		// 로그 전용: Details link 변경 처리 CPU 시간을 측정합니다.
		const double LinkEditStartSeconds = FPlatformTime::Seconds();
		AWormholePortalActor* PreviousLinkedPortal = LinkedPortalBeforeEdit.Get();
		AWormholePortalActor* NewLinkedPortal = LinkedPortal.Get();
		
		if (NewLinkedPortal == this)
		{
			LinkedPortal = nullptr;
			NewLinkedPortal = nullptr;
		}
		if (IsValid(NewLinkedPortal) && NewLinkedPortal->GetWorld() != GetWorld())
		{
			// The details panel writes the property before PostEditChangeProperty. Restore the
			// pre-edit value before unlinking either endpoint or notifying the registry.
			LinkedPortal = PreviousLinkedPortal;
			LinkedPortalBeforeEdit.Reset();
			UpdateEditorVisualization();
			WP_LOG(this, Warning,
				TEXT("[PortalLink] Cross-world details edit rejected and restored. Actor=%s ActorWorld=%s RequestedPortal=%s RequestedWorld=%s RestoredPortal=%s RegistryNotification=0 CounterpartMutation=0 CpuMs=%.4f"),
				*GetPathName(), *GetPathNameSafe(GetWorld()),
				*GetPathNameSafe(NewLinkedPortal), *GetPathNameSafe(NewLinkedPortal->GetWorld()),
				*GetPathNameSafe(PreviousLinkedPortal),
				(FPlatformTime::Seconds() - LinkEditStartSeconds) * 1000.0);
			return;
		}

		LinkedPortalBeforeEdit.Reset();
		const bool bLinkChanged = PreviousLinkedPortal != NewLinkedPortal;
	
		if (IsValid(PreviousLinkedPortal) && PreviousLinkedPortal != NewLinkedPortal && PreviousLinkedPortal->LinkedPortal.Get() == this)
		{
			PreviousLinkedPortal->SetLinkedPortalInternal(nullptr, false);
		}
	
		SetLinkedPortalInternal(NewLinkedPortal, true);
		
		if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal())
		{
			CurrentLinkedPortal->SetPortalRadiusInternal(PortalRadius, false);
			CurrentLinkedPortal->SetThroatHalfLengthInternal(ThroatHalfLength, false);
			CurrentLinkedPortal->SetTransitionLengthInternal(TransitionLength, false);
		}

		// Details 패널 경로에서는 LinkedPortal이 이미 새 값이라 SetLinkedPortalInternal의
		// 동일 값 빠른 반환 경로를 탄다. 실제 토폴로지 변경은 명시적으로 전달한다.
		if (bLinkChanged)
		{
			NotifyRegistryPortalChanged(EWPPortalChangeType::Link);
		}
		
		return;
	}
}

void AWormholePortalActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	
	UpdateEditorVisualization();
	
	if (AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal())
	{
		CurrentLinkedPortal->UpdateEditorVisualization();
	}
}

void AWormholePortalActor::UpdateEditorVisualization() const
{
#if WITH_EDITORONLY_DATA
	if (!EditorLinkSpline || !EditorLabel)
	{
		return;
	}
	
	EditorLabel->SetRelativeLocation(FVector(0.f, 0.f, GetPortalRadius() + 30.f));
	
	if (const AWormholePortalActor* CurrentLinkedPortal = GetLinkedPortal())
	{
		EditorLabel->SetText(FText::FromString(FString::Printf(TEXT("Linked: %s"), *CurrentLinkedPortal->GetActorLabel())));
		
		EditorLinkSpline->SetVisibility(true);
		EditorLinkSpline->ClearSplinePoints(false);
		EditorLinkSpline->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::Local, false);
		EditorLinkSpline->AddSplinePoint(
			GetActorTransform().InverseTransformPosition(CurrentLinkedPortal->GetActorLocation()),
			ESplineCoordinateSpace::Local,
			true
		);
	}

	else
	{
		EditorLabel->SetText(FText::FromString(TEXT("Unlinked")));
		EditorLinkSpline->SetVisibility(false);
	}
#endif
}
#endif





