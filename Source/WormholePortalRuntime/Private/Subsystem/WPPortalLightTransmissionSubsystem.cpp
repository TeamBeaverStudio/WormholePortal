// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPPortalLightTransmissionSubsystem.h"

#include "Subsystem/WPPortalLightCollectionSubsystem.h"
#include "Lighting/WPPortalLightShadowMath.h"
#include "Subsystem/WPRegistrySubsystem.h"

#include "WPLog.h"
#include "Lighting/WPPortalLightTags.h"
#include "WPSettings.h"
#include "WPTransform.h"
#include "Transit/WPTransitTags.h"
#include "WormholePortalActor.h"

#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "PixelFormat.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

namespace WPPortalLightTransmissionPrivate
{
	const FName ProxyHostBaseName(TEXT("WPPortalLightProxyHost"));
	const FName ProxyRootBaseName(TEXT("WPPortalLightProxyRoot"));
	const FName ProxyLightBaseName(TEXT("WPPortalLightProxy"));
	const FName SourceShadowCaptureBaseName(TEXT("WPPortalSourceShadowCapture"));
	const FName SourceShadowDepthBaseName(TEXT("WPPortalSourceShadowDepth"));

	const TCHAR* const LocalLightMaskMaterialPath =
		TEXT("/WormholePortal/WormholePortal/Materials/LightFunctions/M_WPPortalSphericalGate.M_WPPortalSphericalGate");

	const FName PortalCenterParameter(TEXT("WP_PortalCenter"));
	const FName LightPositionParameter(TEXT("WP_LightPosition"));
	const FName PortalRadiusParameter(TEXT("WP_PortalRadius"));
	const FName ExitFeatherParameter(TEXT("WP_ExitFeather"));

	const FName SourceShadowEnabledParameter(TEXT("WP_SourceShadowEnabled"));
	const FName SourceShadowDepthParameter(TEXT("WP_SourceShadowDepth"));
	const FName SourceShadowForwardParameter(TEXT("WP_SourceShadowForward"));
	const FName SourceShadowRightParameter(TEXT("WP_SourceShadowRight"));
	const FName SourceShadowUpParameter(TEXT("WP_SourceShadowUp"));
	const FName SourceShadowTanHalfFovParameter(TEXT("WP_SourceShadowTanHalfFov"));
	const FName SourceShadowBiasParameter(TEXT("WP_SourceShadowBias"));
	const FName SourceShadowInvResolutionParameter(TEXT("WP_SourceShadowInvResolution"));
	const FName SourceShadowPortalRadiusParameter(TEXT("WP_SourceShadowPortalRadius"));
	const FName SourceShadowPCFRadiusParameter(TEXT("WP_SourceShadowPCFRadius"));

	constexpr double ShadowCasterReconcileIntervalSeconds = 1.0;
	constexpr double ResolutionHysteresisSeconds = 0.5;
	constexpr double OffscreenCaptureRateHz = 5.0;
	constexpr float CaptureFovMarginDegrees = 1.0f;
	constexpr float CaptureMaxFovDegrees = 170.0f;
	constexpr float CaptureFarPlaneMarginCm = 10.0f;
	constexpr float MatchingPortalRadiusToleranceCm = 0.1f;

	int32 QuantizeBaseResolution(const int32 RequestedResolution)
	{
		if (RequestedResolution < 192)
		{
			return 128;
		}

		return RequestedResolution < 384 ? 256 : 512;
	}

	void DisableMegaLightsBeforeRegistration(ULightComponent& LightComponent)
	{
		/** UE 5.8에는 bAllowMegaLights의 public setter가 없으므로 등록 전에 reflected property를 설정합니다. */
		if (FBoolProperty* AllowMegaLightsProperty = FindFProperty<FBoolProperty>(
			ULightComponent::StaticClass(),
			TEXT("bAllowMegaLights")))
		{
			AllowMegaLightsProperty->SetPropertyValue_InContainer(&LightComponent, false);
		}
	}
}

bool UWPPortalLightTransmissionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	
	const UWorld* World = Cast<UWorld>(Outer);
	
	return IsValid(World)
	&& World->IsGameWorld()
	&& World->GetNetMode() != NM_DedicatedServer
	&& FApp::CanEverRender();
}

void UWPPortalLightTransmissionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	check(IsInGameThread());
	
	Super::Initialize(Collection);
	
	bDeinitializing = false;
	bPairTopologyDirty = true;
	
	ActivePairs.Reset();
	RouteStates.Reset();
	ProxyHostActor.Reset();
	ShadowCasters.Reset();
	ShadowCasterKeys.Reset();
	PendingSpawnedActors.Reset();
	NextFullShadowCasterReconcileRealSeconds = 0.0;
	
	RegistrySubsystem = Collection.InitializeDependency<UWPRegistrySubsystem>();
	LightCollectionSubsystem = Collection.InitializeDependency<UWPPortalLightCollectionSubsystem>();

	PortalLocalLightMaskMaterial = LoadObject<UMaterialInterface>(
		nullptr,
		WPPortalLightTransmissionPrivate::LocalLightMaskMaterialPath);
	SourceShadowFallbackTexture = nullptr;

	if (!IsValid(PortalLocalLightMaskMaterial.Get()))
	{
		WP_LOG(this, Error,
			TEXT("Portal local-light mask material could not be loaded. Path=%s"),
			WPPortalLightTransmissionPrivate::LocalLightMaskMaterialPath);
	}
	else
	{
		UTexture* DefaultSourceShadowTexture = nullptr;
		if (PortalLocalLightMaskMaterial->GetTextureParameterDefaultValue(
			FHashedMaterialParameterInfo(WPPortalLightTransmissionPrivate::SourceShadowDepthParameter),
			DefaultSourceShadowTexture))
		{
			SourceShadowFallbackTexture = DefaultSourceShadowTexture;
		}
	}
	
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		/**
		 * Registry 계약에 맞춰 delegate를 먼저 연결한 다음
		 * 현재 Pair 전체 목록을 bootstrap합니다.
		 */
		PortalPairAddedHandle = Registry->OnPortalPairAdded().AddUObject(this, &UWPPortalLightTransmissionSubsystem::HandlePortalPairAdded);
		PortalPairRemovedHandle = Registry->OnPortalPairRemoved().AddUObject(this, &UWPPortalLightTransmissionSubsystem::HandlePortalPairRemoved);
	}
	
	RefreshPairTopology();

	UWorld* World = GetWorld();
	check(World);

	const FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateUObject(
		this,
		&UWPPortalLightTransmissionSubsystem::HandleActorSpawned);
	ActorSpawnedHandle = World->AddOnActorSpawnedHandler(ActorSpawnedDelegate);

	DiscoverAllShadowCasters();
	NextFullShadowCasterReconcileRealSeconds = FPlatformTime::Seconds()
		+ WPPortalLightTransmissionPrivate::ShadowCasterReconcileIntervalSeconds;
	
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UWPPortalLightTransmissionSubsystem::HandleWorldPostActorTick);
}

void UWPPortalLightTransmissionSubsystem::Deinitialize()
{
	check(IsInGameThread());
	
	bDeinitializing = true;
	
	if (WorldPostActorTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
		WorldPostActorTickHandle.Reset();
	}

	if (UWorld* World = GetWorld())
	{
		if (ActorSpawnedHandle.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
		}
	}
	ActorSpawnedHandle.Reset();
	
	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		if (PortalPairAddedHandle.IsValid())
		{
			Registry->OnPortalPairAdded().Remove(PortalPairAddedHandle);
		}
		
		if (PortalPairRemovedHandle.IsValid())
		{
			Registry->OnPortalPairRemoved().Remove(PortalPairRemovedHandle);
		}
	}
	
	PortalPairAddedHandle.Reset();
	PortalPairRemovedHandle.Reset();
	
	DestroyAllRouteStates();
	SourceShadowFallbackTexture = nullptr;
	PortalLocalLightMaskMaterial = nullptr;
	
	if (AActor* ProxyHost = ProxyHostActor.Get())
	{
		UWorld* World = GetWorld();
		
		if (World && !World->bIsTearingDown)
		{
			ProxyHost->Destroy();
		}
	}
	
	ProxyHostActor.Reset();
	ActivePairs.Reset();
	PendingSpawnedActors.Reset();
	ShadowCasterKeys.Reset();
	ShadowCasters.Reset();
	NextFullShadowCasterReconcileRealSeconds = 0.0;
	
	LightCollectionSubsystem.Reset();
	RegistrySubsystem.Reset();
	
	bPairTopologyDirty = true;
	
	Super::Deinitialize();
}

int32 UWPPortalLightTransmissionSubsystem::GetProxyLightCount() const
{
	check(IsInGameThread());
	
	int32 ValidProxyCount = 0;
	
	for (const TPair<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>& Entry : RouteStates)
	{
		if (Entry.Value.ProxyLight.IsValid())
		{
			++ValidProxyCount;
		}
	}
	
	return ValidProxyCount;
}

void UWPPortalLightTransmissionSubsystem::RefreshPairTopology()
{
	check(IsInGameThread());
	
	bPairTopologyDirty = false;
	
	TMap<FGuid, FWPPortalLightActivePairState> NewActivePairs;
	
	UWPRegistrySubsystem* Registry = RegistrySubsystem.Get();
	UWorld* World = GetWorld();
	
	if (!Registry || !World)
	{
		ActivePairs.Reset();
		return;
	}
	
	TArray<FWPPortalPairSnapshot> PairSnapshots;
	Registry->GetRegisteredPortalPairs(PairSnapshots);
	
	NewActivePairs.Reserve(PairSnapshots.Num());
	
	for (const FWPPortalPairSnapshot& PairSnapshot : PairSnapshots)
	{
		if (!PairSnapshot.IsStructurallyValid())
		{
			continue;
		}
		
		AWormholePortalActor* PortalA = PairSnapshot.PortalA.Get();
		AWormholePortalActor* PortalB = PairSnapshot.PortalB.Get();
		
		if (!IsValid(PortalA) || !IsValid(PortalB) || PortalA->GetWorld() != World || PortalB->GetWorld() != World)
		{
			continue;
		}
		
		FWPPortalLightActivePairState PairState;
		PairState.PortalA = PortalA;
		PairState.PortalB = PortalB;
		
		NewActivePairs.Add(PairSnapshot.PairId, MoveTemp(PairState));
	}
	
	ActivePairs = MoveTemp(NewActivePairs);
}

void UWPPortalLightTransmissionSubsystem::ReconcileAllRoutes()
{
	check(IsInGameThread());
	
	TSet<FWPPortalLightRouteKey> DesiredRoutes;
	DesiredRoutes.Reserve(RouteStates.Num() + 8); // Why sum 8??
	
	for (const TPair<FGuid, FWPPortalLightActivePairState>& PairEntry : ActivePairs)
	{
		AWormholePortalActor* PortalA = PairEntry.Value.PortalA.Get();
		AWormholePortalActor* PortalB = PairEntry.Value.PortalB.Get();
		
		if (!IsValid(PortalA) || !IsValid(PortalB))
		{
			continue;
		}
		
		/**
		 * Registry의 PortalA/B는 canonical order일 뿐 방향 의미가 없으므로
		 * 반드시 양방향 route를 각각 만듭니다.
		 */
		ReconcileDirection(PairEntry.Key, PortalA, PortalB, DesiredRoutes);
		ReconcileDirection(PairEntry.Key, PortalB, PortalA, DesiredRoutes);
	}
	
	for (TMap<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>::TIterator It(RouteStates); It; ++It)
	{
		if (DesiredRoutes.Contains(It.Key()))
		{
			continue;
		}
		
		DestroyRouteState(It.Value());
		It.RemoveCurrent();
	}
}

void UWPPortalLightTransmissionSubsystem::ReconcileDirection(const FGuid& PairId, AWormholePortalActor* EntryPortal,
	AWormholePortalActor* ExitPortal, TSet<FWPPortalLightRouteKey>& OutDesiredRoutes)
{
	check(IsInGameThread());
	
	UWorld* World = GetWorld();
	UWPPortalLightCollectionSubsystem* Collection = LightCollectionSubsystem.Get();
	
	// 왜 매번 이런 검사를 할까.
	if (!World || !Collection || !PairId.IsValid() || !IsValid(EntryPortal) || !IsValid(ExitPortal) || EntryPortal == ExitPortal || EntryPortal->GetWorld() != World || ExitPortal->GetWorld() != World)
	{
		return;
	}
	
	FWPTransform Mapping;
	
	if (!FWPTransform::Build(EntryPortal,ExitPortal,Mapping))
	{
		return;
	}
	
	FWPPortalAffectingLightSnapshot CollectionSnapshot;
	
	if (!Collection->GetPortalLightSnapshot(EntryPortal, CollectionSnapshot))
	{
		return;
	}
	
	for (const TWeakObjectPtr<ULightComponent>& WeakSourceLight : CollectionSnapshot.AffectingLights)
	{
		ULightComponent* SourceLight = WeakSourceLight.Get();
		
		if (!IsValid(SourceLight) || SourceLight->GetWorld() != World || !IsSupportedProxySourceType(*SourceLight))
		{
			continue;
		}
		
		/**
		 * Collection에서도 제외하지만, 전달 단계에서도 한 번 더 막아
		 * Generated Proxy 재귀 전달을 방지합니다.
		 */
		if (SourceLight->ComponentHasTag(WPPortalLightTags::Generated))
		{
			continue;
		}
		
		if (const AActor* SourceOwner = SourceLight->GetOwner())
		{
			if (SourceOwner->ActorHasTag(WPPortalLightTags::Generated))
			{
				continue;
			}
		}
		
		const FWPPortalLightRouteKey RouteKey {
			PairId,
			TObjectKey<AWormholePortalActor>(EntryPortal),
			TObjectKey<ULightComponent>(SourceLight)
		};
		
		OutDesiredRoutes.Add(RouteKey);
		
		FWPPortalLightTransmissionState* RouteState = RouteStates.Find(RouteKey);
		
		if (RouteState)
		{
			ULightComponent* ExistingProxy = RouteState->ProxyLight.Get();
			
			if (!IsValid(ExistingProxy) || !DoesProxyTypeMatchSource(*ExistingProxy, *SourceLight))
			{
				DestroyRouteState(*RouteState);
				RouteStates.Remove(RouteKey);
				RouteState = nullptr;
			}
		}
		
		if (!RouteState)
		{
			ULightComponent* NewProxy = CreateProxyLight(*SourceLight, Mapping, *ExitPortal);
			
			if (!IsValid(NewProxy))
			{
				continue;
			}
			
			FWPPortalLightTransmissionState NewState;
			NewState.EntryPortal = EntryPortal;
			NewState.ExitPortal = ExitPortal;
			NewState.SourceLight = SourceLight;
			NewState.ProxyLight = NewProxy;
			
			RouteStates.Add(RouteKey, MoveTemp(NewState));
			
			continue;
		}
		
		RouteState->EntryPortal = EntryPortal;
		RouteState->ExitPortal = ExitPortal;
		RouteState->SourceLight = SourceLight;
		
		if (ULightComponent* ProxyLight = RouteState->ProxyLight.Get())
		{
			UpdateProxyLight(*ProxyLight, *SourceLight, Mapping, *ExitPortal);
		}
	}
}

AActor* UWPPortalLightTransmissionSubsystem::EnsureProxyHost()
{
	// 의문. Light Proxy Host 전용 Actor를 C++ 클래스로 하나 만들어서 쓰면 안되나?
	// 왜 여기서 처리해줄까..?
	check(IsInGameThread());
	
	UWorld* World = GetWorld();
	
	if (!World || bDeinitializing)
	{
		return nullptr;
	}
	
	if (AActor* ExistingHost = ProxyHostActor.Get())
	{
		if (ExistingHost->GetWorld() == World)
		{
			return ExistingHost;
		}
	}
	
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(World, AActor::StaticClass(), WPPortalLightTransmissionPrivate::ProxyHostBaseName);
	
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	
	AActor* NewHost = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	
	if (!IsValid(NewHost))
	{
		return nullptr;
	}
	
	NewHost->Tags.AddUnique(WPPortalLightTags::Generated);
	NewHost->SetActorEnableCollision(false);
	NewHost->SetActorTickEnabled(false);
	NewHost->SetReplicates(false);
	
	const FName RootName = MakeUniqueObjectName(
		NewHost,
		USceneComponent::StaticClass(),
		WPPortalLightTransmissionPrivate::ProxyRootBaseName);
	
	USceneComponent* RootComponent = NewObject<USceneComponent>(NewHost, RootName, RF_Transient);
	
	if (!RootComponent)
	{
		NewHost->Destroy();
		return nullptr;
	}
	
	NewHost->AddInstanceComponent(RootComponent);
	NewHost->SetRootComponent(RootComponent);
	
	RootComponent->OnComponentCreated();
	RootComponent->SetMobility(EComponentMobility::Movable);
	RootComponent->RegisterComponent();
	
	if (!RootComponent->IsRegistered())
	{
		NewHost->Destroy();
		return nullptr;
	}
	
	ProxyHostActor = NewHost;
	
	return NewHost;
}

ULightComponent* UWPPortalLightTransmissionSubsystem::CreateProxyLight(const ULightComponent& SourceLight,
	const FWPTransform& Mapping, const AWormholePortalActor& ExitPortal)
{
	// 의문. Light Proxy Host 전용 Actor를 C++ 클래스로 하나 만들어서 CDO로 만들어주면 안되나...?
	// 왜 여기서 처리해줄까..?
	check(IsInGameThread());

	if (!IsSupportedProxySourceType(SourceLight))
	{
		return nullptr;
	}
	
	AActor* ProxyHost = EnsureProxyHost();
	
	if (!ProxyHost)
	{
		return nullptr;
	}
	
	UClass* ProxyClass = nullptr;
	
	switch (SourceLight.GetLightType())
	{
	case LightType_Point:
		ProxyClass = UPointLightComponent::StaticClass();
		break;
		
	case LightType_Spot:
		ProxyClass = USpotLightComponent::StaticClass();
		break;
		
	default:
		return nullptr;
	}
	
	const FName ProxyName = MakeUniqueObjectName(ProxyHost, ProxyClass, WPPortalLightTransmissionPrivate::ProxyLightBaseName);
	
	ULightComponent* ProxyLight = NewObject<ULightComponent>(
		ProxyHost,
		ProxyClass,
		ProxyName,
		RF_Transient);
	
	if (!ProxyLight)
	{
		return nullptr;
	}
	
	ProxyHost->AddInstanceComponent(ProxyLight);
	
	ProxyLight->OnComponentCreated();
	ProxyLight->ComponentTags.AddUnique(WPPortalLightTags::Generated);
	
	if (USceneComponent* HostRoot = ProxyHost->GetRootComponent())
	{
		ProxyLight->SetupAttachment(HostRoot);
	}
	
	ProxyLight->SetMobility(EComponentMobility::Movable);
	
	/**
	 * bAffectsWorld는 runtime registration 이후 변경을 전제로 하지 않으므로
	 * 반드시 RegisterComponent 이전에 지정합니다.
	 */
	ProxyLight->bAffectsWorld = true;
	WPPortalLightTransmissionPrivate::DisableMegaLightsBeforeRegistration(*ProxyLight);

	/**
	 * Point와 Spot은 동일한 구면 Portal gate를 사용합니다.
	 * Proxy별 MID가 각 route의 출구 중심, 반지름, 가상 광원 위치를 소유합니다.
	 */
	UMaterialInterface* PortalMaskParent = PortalLocalLightMaskMaterial.Get();

	if (!IsValid(PortalMaskParent))
	{
		ProxyLight->DestroyComponent();
		return nullptr;
	}

	UMaterialInstanceDynamic* PortalMask = UMaterialInstanceDynamic::Create(
		PortalMaskParent,
		ProxyLight);

	if (!IsValid(PortalMask))
	{
		ProxyLight->DestroyComponent();
		return nullptr;
	}

	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowEnabledParameter,
		0.0f);

	ProxyLight->SetLightFunctionMaterial(PortalMask);
	ProxyLight->SetLightFunctionScale(FVector::OneVector);
	ProxyLight->SetLightFunctionFadeDistance(HALF_WORLD_MAX);
	ProxyLight->SetLightFunctionDisabledBrightness(0.0f);
	
	UpdateProxyLight(
		*ProxyLight,
		SourceLight,
		Mapping,
		ExitPortal);
	
	ProxyLight->RegisterComponent();
	
	if (!ProxyLight->IsRegistered())
	{
		ProxyLight->DestroyComponent();
		return nullptr;
	}
	
	return ProxyLight;
}

void UWPPortalLightTransmissionSubsystem::UpdateProxyLight(ULightComponent& ProxyLight,
	const ULightComponent& SourceLight, const FWPTransform& Mapping, const AWormholePortalActor& ExitPortal) const
{
	check(IsInGameThread());

	const ULocalLightComponent* SourceLocal = CastChecked<ULocalLightComponent>(&SourceLight);
	ULocalLightComponent* ProxyLocal = CastChecked<ULocalLightComponent>(&ProxyLight);

	ProxyLocal->SetAttenuationRadius(SourceLocal->AttenuationRadius);
	ProxyLocal->SetIntensityUnits(SourceLocal->IntensityUnits);

	/** Spot은 PointLightComponent를 상속하므로 Point/Spot 공통 속성입니다. */
	if (const UPointLightComponent* SourcePoint = Cast<UPointLightComponent>(&SourceLight))
	{
		UPointLightComponent* ProxyPoint = CastChecked<UPointLightComponent>(&ProxyLight);
		
		ProxyPoint->SetUseInverseSquaredFalloff(SourcePoint->bUseInverseSquaredFalloff);
		ProxyPoint->SetLightFalloffExponent(SourcePoint->LightFalloffExponent);
		ProxyPoint->SetInverseExposureBlend(SourcePoint->InverseExposureBlend);
		ProxyPoint->SetSourceRadius(SourcePoint->SourceRadius);
		ProxyPoint->SetSoftSourceRadius(SourcePoint->SoftSourceRadius);
		ProxyPoint->SetSourceLength(SourcePoint->SourceLength);
	}
	
	if (const USpotLightComponent* SourceSpot = Cast<USpotLightComponent>(&SourceLight))
	{
		USpotLightComponent* ProxySpot = CastChecked<USpotLightComponent>(&ProxyLight);
		ProxySpot->SetInnerConeAngle(SourceSpot->InnerConeAngle);
		ProxySpot->SetOuterConeAngle(SourceSpot->OuterConeAngle);
	}
	
	// 원본의 직접광 속성
	ProxyLight.SetIntensity(SourceLight.Intensity);
	ProxyLight.SetLightFColor(SourceLight.LightColor);
	ProxyLight.SetUseTemperature(SourceLight.bUseTemperature);
	ProxyLight.SetTemperature(SourceLight.Temperature);
	ProxyLight.SetDiffuseScale(SourceLight.DiffuseScale);
	ProxyLight.SetSpecularScale(SourceLight.SpecularScale);
	ProxyLight.SetLightingChannels(
		SourceLight.LightingChannels.bChannel0,
		SourceLight.LightingChannels.bChannel1,
		SourceLight.LightingChannels.bChannel2);

	ProxyLight.SetMaxDrawDistance(SourceLight.MaxDrawDistance);
	ProxyLight.SetMaxDistanceFadeRange(SourceLight.MaxDistanceFadeRange);
	
	/** IES는 Portal Light Function과 별도 경로이므로 그대로 전달합니다. */
	ProxyLight.SetIESTexture(SourceLight.IESTexture.Get());
	ProxyLight.SetUseIESBrightness(SourceLight.bUseIESBrightness);
	ProxyLight.SetIESBrightnessScale(SourceLight.IESBrightnessScale);

	const FQuat ProxyRotation = Mapping.MapRot(SourceLight.GetComponentQuat());
	/**
	 * MapPoint는 통과 오브젝트의 antipodal 위치용입니다.
	 * 광선의 연속성은 Destination의 같은 로컬 좌표인 MapRayOrigin을 사용합니다.
	 */
	const FVector ProxyLocation = Mapping.MapRayOrigin(SourceLight.GetComponentLocation());

	UMaterialInstanceDynamic* PortalMask = Cast<UMaterialInstanceDynamic>(
		ProxyLight.LightFunctionMaterial.Get());

	if (!IsValid(PortalMask))
	{
		/** Mask 없는 Local Proxy는 출구 공간 전체에 빛을 누출하므로 fail-closed 처리합니다. */
		ProxyLight.SetVisibility(false, false);
		return;
	}

	const FVector ExitCenter = ExitPortal.GetActorLocation();
	const float ExitRadius = FMath::Max(ExitPortal.GetPortalRadius(), 1.0f);
	const float ExitFeather = FMath::Clamp(ExitRadius * 0.01f, 0.5f, 5.0f);

	PortalMask->SetVectorParameterValue(
		WPPortalLightTransmissionPrivate::PortalCenterParameter,
		FLinearColor(ExitCenter.X, ExitCenter.Y, ExitCenter.Z, 0.0f));

	PortalMask->SetVectorParameterValue(
		WPPortalLightTransmissionPrivate::LightPositionParameter,
		FLinearColor(ProxyLocation.X, ProxyLocation.Y, ProxyLocation.Z, 0.0f));

	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::PortalRadiusParameter,
		ExitRadius);

	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::ExitFeatherParameter,
		ExitFeather);

	ProxyLight.SetLightFunctionScale(FVector::OneVector);
	ProxyLight.SetLightFunctionFadeDistance(HALF_WORLD_MAX);
	ProxyLight.SetLightFunctionDisabledBrightness(0.0f);
	
	/**
	 * Proxy의 native shadow는 Exit 공간의 occluder를 처리합니다.
	 * Entry 공간 visibility는 route별 SceneDepth를 Light Function에 곱합니다.
	 *
	 * GI, Reflection, Volumetric을 켜면 포탈 마스크가 없는 상태에서
	 * 출구 공간 전체에 간접효과가 누출되므로 명시적으로 끕니다.
	 */
	ProxyLight.SetCastShadows(SourceLight.CastShadows != 0);
	ProxyLight.SetShadowBias(SourceLight.ShadowBias);
	ProxyLight.SetShadowSlopeBias(SourceLight.ShadowSlopeBias);

	ProxyLight.SetCastVolumetricShadow(false);
	ProxyLight.SetCastDeepShadow(false);
	ProxyLight.SetUseRayTracedDistanceFieldShadows(false);
	
	ProxyLight.SetAffectTranslucentLighting(false);
	ProxyLight.SetAffectReflection(false);
	
	ProxyLight.SetIndirectLightingIntensity(0.0f);
	ProxyLight.SetVolumetricScatteringIntensity(0.0f);
	
	ProxyLight.SetEnableLightShaftBloom(false);
	
	ProxyLight.SetWorldLocationAndRotation(ProxyLocation, ProxyRotation, false, nullptr, ETeleportType::TeleportPhysics);
	
	ProxyLight.SetHiddenInGame(false, false);
	ProxyLight.SetVisibility(true, false);
}

bool UWPPortalLightTransmissionSubsystem::IsSupportedProxySourceType(const ULightComponent& SourceLight)
{
	switch (SourceLight.GetLightType())
	{
	case LightType_Point:
	case LightType_Spot:
		return true;
		
	default:
		return false;
	}
}

bool UWPPortalLightTransmissionSubsystem::DoesProxyTypeMatchSource(const ULightComponent& ProxyLight,
	const ULightComponent& SourceLight)
{
	return ProxyLight.GetLightType() == SourceLight.GetLightType();
}

bool UWPPortalLightTransmissionSubsystem::IsSourceShadowRouteSupported(
	const FWPPortalLightTransmissionState& RouteState) const
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const ULightComponent* SourceLight = RouteState.SourceLight.Get();
	const ULightComponent* ProxyLight = RouteState.ProxyLight.Get();
	const AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	const AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();

	if (!Settings
		|| !Settings->bEnablePortalSourceShadows
		|| !IsValid(SourceLight)
		|| !IsValid(ProxyLight)
		|| !IsValid(EntryPortal)
		|| !IsValid(ExitPortal)
		|| !IsSupportedProxySourceType(*SourceLight)
		|| SourceLight->CastShadows == 0
		|| SourceLight->CastDynamicShadows == 0)
	{
		return false;
	}

	if (!IsValid(Cast<UMaterialInstanceDynamic>(ProxyLight->LightFunctionMaterial.Get())))
	{
		return false;
	}

	const float EntryRadius = EntryPortal->GetPortalRadius();
	const float ExitRadius = ExitPortal->GetPortalRadius();
	if (EntryRadius <= KINDA_SMALL_NUMBER
		|| ExitRadius <= KINDA_SMALL_NUMBER
		|| !FMath::IsNearlyEqual(
			EntryRadius,
			ExitRadius,
			WPPortalLightTransmissionPrivate::MatchingPortalRadiusToleranceCm))
	{
		return false;
	}

	const float SourceToEntryDistance = FVector::Distance(
		SourceLight->GetComponentLocation(),
		EntryPortal->GetActorLocation());

	/** 2D perspective capture는 광원이 portal 구체 내부/표면에 있을 때 정의되지 않습니다. */
	if (!FMath::IsFinite(SourceToEntryDistance)
		|| SourceToEntryDistance <= EntryRadius + KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const float RadiusRatio = FMath::Clamp(EntryRadius / SourceToEntryDistance, 0.0f, 0.9999f);
	const float RequiredFovDegrees = FMath::RadiansToDegrees(FMath::Asin(RadiusRatio) * 2.0f)
		+ WPPortalLightTransmissionPrivate::CaptureFovMarginDegrees;

	/** SceneCapture의 안정 범위를 넘는 near-surface route는 부분 shadow 대신 전체 fail-open합니다. */
	return FMath::IsFinite(RequiredFovDegrees)
		&& RequiredFovDegrees <= WPPortalLightTransmissionPrivate::CaptureMaxFovDegrees;
}

bool UWPPortalLightTransmissionSubsystem::EnsureSourceShadowResources(
	FWPPortalLightTransmissionState& RouteState,
	const int32 Resolution)
{
	check(IsInGameThread());

	if (!IsSourceShadowRouteSupported(RouteState) || Resolution <= 0)
	{
		return false;
	}

	AActor* ProxyHost = EnsureProxyHost();
	if (!ProxyHost)
	{
		return false;
	}

	USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get();
	if (!IsValid(Capture))
	{
		const FName CaptureName = MakeUniqueObjectName(
			ProxyHost,
			USceneCaptureComponent2D::StaticClass(),
			WPPortalLightTransmissionPrivate::SourceShadowCaptureBaseName);

		Capture = NewObject<USceneCaptureComponent2D>(
			ProxyHost,
			USceneCaptureComponent2D::StaticClass(),
			CaptureName,
			RF_Transient);

		if (!Capture)
		{
			return false;
		}

		ProxyHost->AddInstanceComponent(Capture);
		Capture->OnComponentCreated();
		Capture->ComponentTags.AddUnique(WPPortalLightTags::Generated);

		if (USceneComponent* HostRoot = ProxyHost->GetRootComponent())
		{
			Capture->SetupAttachment(HostRoot);
		}

		Capture->SetMobility(EComponentMobility::Movable);
		Capture->ProjectionType = ECameraProjectionMode::Perspective;
		Capture->CaptureSource = SCS_SceneDepth;
		Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = false;
		Capture->bExcludeFromSceneTextureExtents = true;
		Capture->bFiniteFarPlane = true;
		Capture->bUseRayTracingIfEnabled = false;
		Capture->RegisterComponent();

		if (!Capture->IsRegistered())
		{
			Capture->DestroyComponent();
			return false;
		}

		RouteState.SourceShadowCapture = Capture;
	}

	UTextureRenderTarget2D* DepthTarget = RouteState.SourceShadowDepthTarget.Get();
	if (!IsValid(DepthTarget))
	{
		const FName TargetName = MakeUniqueObjectName(
			Capture,
			UTextureRenderTarget2D::StaticClass(),
			WPPortalLightTransmissionPrivate::SourceShadowDepthBaseName);

		DepthTarget = NewObject<UTextureRenderTarget2D>(
			Capture,
			UTextureRenderTarget2D::StaticClass(),
			TargetName,
			RF_Transient);

		if (!DepthTarget)
		{
			return false;
		}

		DepthTarget->ClearColor = FLinearColor(HALF_WORLD_MAX, 0.0f, 0.0f, 0.0f);
		DepthTarget->AddressX = TA_Clamp;
		DepthTarget->AddressY = TA_Clamp;
		DepthTarget->Filter = TF_Nearest;
		DepthTarget->bAutoGenerateMips = false;
		DepthTarget->InitCustomFormat(Resolution, Resolution, PF_R32_FLOAT, true);

		RouteState.SourceShadowDepthTarget = DepthTarget;
		RouteState.CurrentSourceShadowResolution = Resolution;
		RouteState.bSourceShadowValid = false;
	}
	else if (DepthTarget->SizeX != Resolution || DepthTarget->SizeY != Resolution)
	{
		SetSourceShadowMaterialEnabled(RouteState, false);
		DepthTarget->ResizeTarget(Resolution, Resolution);
		RouteState.CurrentSourceShadowResolution = Resolution;
		RouteState.bSourceShadowValid = false;
	}

	Capture->TextureTarget = DepthTarget;
	RouteState.bSourceShadowActive = true;
	return true;
}

bool UWPPortalLightTransmissionSubsystem::ConfigureSourceShadowCapture(
	FWPPortalLightTransmissionState& RouteState,
	const int32 Resolution)
{
	if (!EnsureSourceShadowResources(RouteState, Resolution))
	{
		return false;
	}

	ULightComponent* SourceLight = RouteState.SourceLight.Get();
	AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get();
	if (!IsValid(SourceLight) || !IsValid(EntryPortal) || !IsValid(Capture))
	{
		return false;
	}

	const FVector SourceLocation = SourceLight->GetComponentLocation();
	const FVector ToEntryPortal = EntryPortal->GetActorLocation() - SourceLocation;
	const float DistanceToEntryPortal = ToEntryPortal.Length();
	const float EntryRadius = EntryPortal->GetPortalRadius();

	if (!FMath::IsFinite(DistanceToEntryPortal)
		|| DistanceToEntryPortal <= EntryRadius + KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const FVector CaptureForward = ToEntryPortal / DistanceToEntryPortal;
	const float RadiusRatio = FMath::Clamp(EntryRadius / DistanceToEntryPortal, 0.0f, 0.9999f);
	const float HalfFovRadians = FMath::Asin(RadiusRatio);
	const float FovDegrees = FMath::RadiansToDegrees(HalfFovRadians * 2.0f)
		+ WPPortalLightTransmissionPrivate::CaptureFovMarginDegrees;
	if (!FMath::IsFinite(FovDegrees)
		|| FovDegrees > WPPortalLightTransmissionPrivate::CaptureMaxFovDegrees)
	{
		return false;
	}

	Capture->FOVAngle = FovDegrees;
	Capture->MaxViewDistanceOverride = DistanceToEntryPortal
		+ EntryRadius
		+ WPPortalLightTransmissionPrivate::CaptureFarPlaneMarginCm;
	Capture->SetWorldLocationAndRotation(
		SourceLocation,
		FRotationMatrix::MakeFromX(CaptureForward).Rotator(),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);

	RebuildSourceShadowShowOnlyList(RouteState);
	return true;
}

void UWPPortalLightTransmissionSubsystem::RebuildSourceShadowShowOnlyList(
	FWPPortalLightTransmissionState& RouteState) const
{
	USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get();
	const ULightComponent* SourceLight = RouteState.SourceLight.Get();
	const AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	if (!IsValid(Capture) || !IsValid(SourceLight) || !IsValid(EntryPortal))
	{
		return;
	}

	Capture->ClearShowOnlyComponents();

	const FVector SourceLocation = SourceLight->GetComponentLocation();
	const FVector ToEntryPortal = EntryPortal->GetActorLocation() - SourceLocation;
	const float DistanceToEntryPortal = ToEntryPortal.Length();
	if (DistanceToEntryPortal <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector CaptureForward = ToEntryPortal / DistanceToEntryPortal;
	const float TanHalfFov = FMath::Tan(FMath::DegreesToRadians(Capture->FOVAngle * 0.5f));
	const float MaxDistance = DistanceToEntryPortal
		+ EntryPortal->GetPortalRadius()
		+ WPPortalLightTransmissionPrivate::CaptureFarPlaneMarginCm;

	for (const FWPPortalShadowCasterRecord& Record : ShadowCasters)
	{
		UPrimitiveComponent* PrimitiveComponent = Record.PrimitiveComponent.Get();
		if (!IsEligibleShadowCaster(PrimitiveComponent))
		{
			continue;
		}

		if (DoesBoundsIntersectSourceShadowCone(
			PrimitiveComponent->Bounds,
			SourceLocation,
			CaptureForward,
			TanHalfFov,
			MaxDistance))
		{
			Capture->ShowOnlyComponent(PrimitiveComponent);
		}
	}
}

void UWPPortalLightTransmissionSubsystem::UpdateSourceShadowMaterialParameters(
	FWPPortalLightTransmissionState& RouteState) const
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const ULightComponent* SourceLight = RouteState.SourceLight.Get();
	ULightComponent* ProxyLight = RouteState.ProxyLight.Get();
	const AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	const AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();
	const USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get();
	UTextureRenderTarget2D* DepthTarget = RouteState.SourceShadowDepthTarget.Get();
	if (!Settings
		|| !IsValid(SourceLight)
		|| !IsValid(ProxyLight)
		|| !IsValid(EntryPortal)
		|| !IsValid(ExitPortal)
		|| !IsValid(Capture)
		|| !IsValid(DepthTarget))
	{
		return;
	}

	UMaterialInstanceDynamic* PortalMask = Cast<UMaterialInstanceDynamic>(
		ProxyLight->LightFunctionMaterial.Get());
	if (!IsValid(PortalMask))
	{
		return;
	}

	FWPTransform Mapping;
	if (!FWPTransform::Build(EntryPortal, ExitPortal, Mapping))
	{
		return;
	}

	const FQuat CaptureRotation = Capture->GetComponentQuat();
	const FVector MappedForward = Mapping.MapDir(CaptureRotation.GetAxisX()).GetSafeNormal();
	const FVector MappedRight = Mapping.MapDir(CaptureRotation.GetAxisY()).GetSafeNormal();
	const FVector MappedUp = Mapping.MapDir(CaptureRotation.GetAxisZ()).GetSafeNormal();
	const float TanHalfFov = FMath::Tan(FMath::DegreesToRadians(Capture->FOVAngle * 0.5f));
	const int32 SafeResolution = FMath::Max(RouteState.CurrentSourceShadowResolution, 1);

	PortalMask->SetTextureParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowDepthParameter,
		DepthTarget);
	PortalMask->SetVectorParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowForwardParameter,
		FLinearColor(MappedForward.X, MappedForward.Y, MappedForward.Z, 0.0f));
	PortalMask->SetVectorParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowRightParameter,
		FLinearColor(MappedRight.X, MappedRight.Y, MappedRight.Z, 0.0f));
	PortalMask->SetVectorParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowUpParameter,
		FLinearColor(MappedUp.X, MappedUp.Y, MappedUp.Z, 0.0f));
	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowTanHalfFovParameter,
		TanHalfFov);
	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowBiasParameter,
		FMath::Max(Settings->PortalSourceShadowDepthBiasCm, 0.0f));
	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowInvResolutionParameter,
		1.0f / static_cast<float>(SafeResolution));
	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowPortalRadiusParameter,
		EntryPortal->GetPortalRadius());
	PortalMask->SetScalarParameterValue(
		WPPortalLightTransmissionPrivate::SourceShadowPCFRadiusParameter,
		FMath::Max(Settings->PortalSourceShadowPCFRadiusTexels, 0.0f));
}

void UWPPortalLightTransmissionSubsystem::SetSourceShadowMaterialEnabled(
	FWPPortalLightTransmissionState& RouteState,
	const bool bEnabled) const
{
	if (ULightComponent* ProxyLight = RouteState.ProxyLight.Get())
	{
		if (UMaterialInstanceDynamic* PortalMask = Cast<UMaterialInstanceDynamic>(
			ProxyLight->LightFunctionMaterial.Get()))
		{
			PortalMask->SetScalarParameterValue(
				WPPortalLightTransmissionPrivate::SourceShadowEnabledParameter,
				bEnabled ? 1.0f : 0.0f);
		}
	}
}

void UWPPortalLightTransmissionSubsystem::DestroySourceShadowResources(
	FWPPortalLightTransmissionState& RouteState) const
{
	check(IsInGameThread());

	SetSourceShadowMaterialEnabled(RouteState, false);

	if (ULightComponent* ProxyLight = RouteState.ProxyLight.Get())
	{
		if (UMaterialInstanceDynamic* PortalMask = Cast<UMaterialInstanceDynamic>(
			ProxyLight->LightFunctionMaterial.Get()))
		{
			if (UTexture* FallbackTexture = SourceShadowFallbackTexture.Get())
			{
				/** MID는 null texture override를 무시하므로 valid parent texture로 교체해야 RT 참조가 해제됩니다. */
				PortalMask->SetTextureParameterValue(
					WPPortalLightTransmissionPrivate::SourceShadowDepthParameter,
					FallbackTexture);
			}
		}
	}

	if (USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get())
	{
		Capture->TextureTarget = nullptr;
		Capture->ClearShowOnlyComponents();
		Capture->DestroyComponent();
	}

	RouteState.SourceShadowCapture.Reset();
	RouteState.SourceShadowDepthTarget.Reset();
	RouteState.CurrentSourceShadowResolution = 0;
	RouteState.ResolutionCandidate = 0;
	RouteState.ResolutionCandidateSinceRealSeconds = 0.0;
	RouteState.bSourceShadowValid = false;
	RouteState.bSourceShadowDirty = true;
	RouteState.bSourceShadowActive = false;
}

bool UWPPortalLightTransmissionSubsystem::GetExitPortalScreenMetrics(
	const AWormholePortalActor& ExitPortal,
	float& OutDiameterPixels) const
{
	OutDiameterPixels = 0.0f;

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	bool bVisibleInAnyLocalView = false;
	const FVector PortalCenter = ExitPortal.GetActorLocation();
	const float PortalRadius = FMath::Max(ExitPortal.GetPortalRadius(), 1.0f);

	for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator)
	{
		const APlayerController* PlayerController = Iterator->Get();
		if (!IsValid(PlayerController) || !PlayerController->IsLocalController())
		{
			continue;
		}

		int32 ViewportWidth = 0;
		int32 ViewportHeight = 0;
		PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
		if (ViewportWidth <= 0 || ViewportHeight <= 0)
		{
			continue;
		}

		FVector ViewLocation;
		FRotator ViewRotation;
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);

		if (FVector::DistSquared(ViewLocation, PortalCenter) <= FMath::Square(PortalRadius))
		{
			bVisibleInAnyLocalView = true;
			OutDiameterPixels = FMath::Max(
				OutDiameterPixels,
				static_cast<float>(FMath::Max(ViewportWidth, ViewportHeight)));
			continue;
		}

		FVector2D CenterScreen;
		FVector2D RadiusScreen;
		const FVector ViewRight = FRotationMatrix(ViewRotation).GetUnitAxis(EAxis::Y);
		if (!PlayerController->ProjectWorldLocationToScreen(PortalCenter, CenterScreen, true)
			|| !PlayerController->ProjectWorldLocationToScreen(
				PortalCenter + ViewRight * PortalRadius,
				RadiusScreen,
				true))
		{
			continue;
		}

		const float RadiusPixels = FVector2D::Distance(CenterScreen, RadiusScreen);
		if (!FMath::IsFinite(RadiusPixels) || RadiusPixels <= 0.0f)
		{
			continue;
		}

		const bool bIntersectsViewport = CenterScreen.X + RadiusPixels >= 0.0f
			&& CenterScreen.Y + RadiusPixels >= 0.0f
			&& CenterScreen.X - RadiusPixels <= static_cast<float>(ViewportWidth)
			&& CenterScreen.Y - RadiusPixels <= static_cast<float>(ViewportHeight);

		if (bIntersectsViewport)
		{
			bVisibleInAnyLocalView = true;
			OutDiameterPixels = FMath::Max(OutDiameterPixels, RadiusPixels * 2.0f);
		}
	}

	return bVisibleInAnyLocalView;
}

bool UWPPortalLightTransmissionSubsystem::DoesBoundsIntersectSourceShadowCone(
	const FBoxSphereBounds& Bounds,
	const FVector& SourceLocation,
	const FVector& CaptureForward,
	const float TanHalfFov,
	const float MaxDistance)
{
	if (!FMath::IsFinite(Bounds.SphereRadius)
		|| Bounds.SphereRadius < 0.0f
		|| !FMath::IsFinite(TanHalfFov)
		|| TanHalfFov <= 0.0f
		|| !FMath::IsFinite(MaxDistance)
		|| MaxDistance <= 0.0f)
	{
		return false;
	}

	const FVector ToBounds = Bounds.Origin - SourceLocation;
	const float AxialDistance = FVector::DotProduct(ToBounds, CaptureForward);
	const float SphereRadius = Bounds.SphereRadius;
	if (AxialDistance + SphereRadius < 0.0f
		|| AxialDistance - SphereRadius > MaxDistance)
	{
		return false;
	}

	const float RadialDistanceSquared = FMath::Max(
		ToBounds.SizeSquared() - FMath::Square(AxialDistance),
		0.0f);
	const float ConeRadius = FMath::Max(AxialDistance, 0.0f) * TanHalfFov + SphereRadius;
	return RadialDistanceSquared <= FMath::Square(ConeRadius);
}

void UWPPortalLightTransmissionSubsystem::DiscoverAllShadowCasters()
{
	check(IsInGameThread());

	bool bRegistryChanged = false;
	for (TObjectIterator<UPrimitiveComponent> Iterator; Iterator; ++Iterator)
	{
		bRegistryChanged |= TrackShadowCaster(*Iterator);
	}

	bRegistryChanged |= CompactShadowCasters();
	if (bRegistryChanged)
	{
		MarkAllSourceShadowsDirty();
	}
}

void UWPPortalLightTransmissionSubsystem::DiscoverShadowCastersOnActor(AActor* Actor)
{
	check(IsInGameThread());

	if (bDeinitializing || !IsValid(Actor) || Actor->GetWorld() != GetWorld())
	{
		return;
	}

	bool bRegistryChanged = false;
	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Actor->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		bRegistryChanged |= TrackShadowCaster(PrimitiveComponent);
	}

	if (bRegistryChanged)
	{
		MarkAllSourceShadowsDirty();
	}
}

void UWPPortalLightTransmissionSubsystem::ProcessPendingSpawnedActors()
{
	check(IsInGameThread());

	TArray<TWeakObjectPtr<AActor>> PendingActors;
	Swap(PendingActors, PendingSpawnedActors);
	for (const TWeakObjectPtr<AActor>& WeakActor : PendingActors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			DiscoverShadowCastersOnActor(Actor);
		}
	}
}

bool UWPPortalLightTransmissionSubsystem::TrackShadowCaster(UPrimitiveComponent* PrimitiveComponent)
{
	check(IsInGameThread());

	if (!IsEligibleShadowCaster(PrimitiveComponent))
	{
		return false;
	}

	const TObjectKey<UPrimitiveComponent> ObjectKey(PrimitiveComponent);
	if (ShadowCasterKeys.Contains(ObjectKey))
	{
		return false;
	}

	FWPPortalShadowCasterRecord Record;
	Record.ObjectKey = ObjectKey;
	Record.PrimitiveComponent = PrimitiveComponent;
	ShadowCasterKeys.Add(ObjectKey);
	ShadowCasters.Add(MoveTemp(Record));
	return true;
}

bool UWPPortalLightTransmissionSubsystem::CompactShadowCasters()
{
	check(IsInGameThread());

	bool bRegistryChanged = false;
	for (int32 Index = ShadowCasters.Num() - 1; Index >= 0; --Index)
	{
		const FWPPortalShadowCasterRecord& Record = ShadowCasters[Index];
		if (IsEligibleShadowCaster(Record.PrimitiveComponent.Get()))
		{
			continue;
		}

		ShadowCasterKeys.Remove(Record.ObjectKey);
		ShadowCasters.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		bRegistryChanged = true;
	}

	return bRegistryChanged;
}

bool UWPPortalLightTransmissionSubsystem::IsEligibleShadowCaster(
	const UPrimitiveComponent* PrimitiveComponent) const
{
	if (!IsValid(PrimitiveComponent)
		|| PrimitiveComponent->GetWorld() != GetWorld()
		|| !PrimitiveComponent->IsRegistered()
		|| PrimitiveComponent->CastShadow == 0
		|| PrimitiveComponent->bCastDynamicShadow == 0
		|| PrimitiveComponent->ComponentHasTag(WPPortalLightTags::Generated)
		|| PrimitiveComponent->ComponentHasTag(WPTransitTags::Generated))
	{
		return false;
	}

	const AActor* Owner = PrimitiveComponent->GetOwner();
	return IsValid(Owner)
		&& !Owner->IsA<AWormholePortalActor>()
		&& !Owner->ActorHasTag(WPPortalLightTags::Generated)
		&& !Owner->ActorHasTag(WPTransitTags::Generated);
}

void UWPPortalLightTransmissionSubsystem::MarkAllSourceShadowsDirty()
{
	for (TPair<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>& RouteEntry : RouteStates)
	{
		RouteEntry.Value.bSourceShadowDirty = true;
	}
}

void UWPPortalLightTransmissionSubsystem::UpdateSourceShadowRoutes()
{
	check(IsInGameThread());

	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings || !Settings->bEnablePortalSourceShadows)
	{
		for (TPair<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>& RouteEntry : RouteStates)
		{
			DestroySourceShadowResources(RouteEntry.Value);
		}
		return;
	}

	struct FRouteCandidate
	{
		FWPPortalLightTransmissionState* RouteState = nullptr;
		uint32 StableHash = 0;
		int32 DesiredResolution = 256;
		bool bTransformDirty = false;
		bool bResolutionChangeReady = false;
	};

	const double NowSeconds = FPlatformTime::Seconds();
	const int32 BaseResolution = WPPortalLightTransmissionPrivate::QuantizeBaseResolution(
		Settings->PortalSourceShadowBaseResolution);
	TArray<FRouteCandidate> RouteCandidates;
	RouteCandidates.Reserve(RouteStates.Num());

	for (TPair<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>& RouteEntry : RouteStates)
	{
		FWPPortalLightTransmissionState& RouteState = RouteEntry.Value;
		RouteState.bSourceShadowActive = false;

		if (!IsSourceShadowRouteSupported(RouteState))
		{
			DestroySourceShadowResources(RouteState);
			continue;
		}

		AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();
		ULightComponent* SourceLight = RouteState.SourceLight.Get();
		AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
		check(ExitPortal && SourceLight && EntryPortal);

		RouteState.bExitPortalOnScreen = GetExitPortalScreenMetrics(
			*ExitPortal,
			RouteState.ExitPortalScreenDiameterPixels);

		const int32 AdaptiveResolution = RouteState.bExitPortalOnScreen
			? WPPortalLightShadowMath::SelectAdaptiveResolution(
				RouteState.ExitPortalScreenDiameterPixels)
			: (RouteState.CurrentSourceShadowResolution > 0
				? RouteState.CurrentSourceShadowResolution
				: BaseResolution);

		bool bResolutionChangeReady = false;
		if (RouteState.CurrentSourceShadowResolution <= 0)
		{
			RouteState.ResolutionCandidate = AdaptiveResolution;
			RouteState.ResolutionCandidateSinceRealSeconds = NowSeconds;
			bResolutionChangeReady = true;
		}
		else if (AdaptiveResolution == RouteState.CurrentSourceShadowResolution)
		{
			RouteState.ResolutionCandidate = AdaptiveResolution;
			RouteState.ResolutionCandidateSinceRealSeconds = NowSeconds;
		}
		else
		{
			if (RouteState.ResolutionCandidate != AdaptiveResolution)
			{
				RouteState.ResolutionCandidate = AdaptiveResolution;
				RouteState.ResolutionCandidateSinceRealSeconds = NowSeconds;
			}

			bResolutionChangeReady = NowSeconds - RouteState.ResolutionCandidateSinceRealSeconds
				>= WPPortalLightTransmissionPrivate::ResolutionHysteresisSeconds;
		}

		const bool bTransformDirty = !RouteState.bSourceShadowValid
			|| !RouteState.LastCapturedSourceTransform.Equals(SourceLight->GetComponentTransform(), 0.01f)
			|| !RouteState.LastCapturedEntryTransform.Equals(EntryPortal->GetActorTransform(), 0.01f)
			|| !FMath::IsNearlyEqual(
				RouteState.LastCapturedEntryRadius,
				EntryPortal->GetPortalRadius(),
				KINDA_SMALL_NUMBER);

		FRouteCandidate& Candidate = RouteCandidates.AddDefaulted_GetRef();
		Candidate.RouteState = &RouteState;
		Candidate.StableHash = GetTypeHash(RouteEntry.Key);
		Candidate.DesiredResolution = RouteState.CurrentSourceShadowResolution <= 0
			? AdaptiveResolution
			: (bResolutionChangeReady
				? AdaptiveResolution
				: RouteState.CurrentSourceShadowResolution);
		Candidate.bTransformDirty = bTransformDirty;
		Candidate.bResolutionChangeReady = bResolutionChangeReady;
	}

	RouteCandidates.Sort([](const FRouteCandidate& A, const FRouteCandidate& B)
	{
		const FWPPortalLightTransmissionState& AState = *A.RouteState;
		const FWPPortalLightTransmissionState& BState = *B.RouteState;
		if (AState.bExitPortalOnScreen != BState.bExitPortalOnScreen)
		{
			return AState.bExitPortalOnScreen;
		}
		if (!FMath::IsNearlyEqual(
			AState.ExitPortalScreenDiameterPixels,
			BState.ExitPortalScreenDiameterPixels))
		{
			return AState.ExitPortalScreenDiameterPixels > BState.ExitPortalScreenDiameterPixels;
		}
		if (AState.bSourceShadowValid != BState.bSourceShadowValid)
		{
			return !AState.bSourceShadowValid;
		}
		if (A.bTransformDirty != B.bTransformDirty)
		{
			return A.bTransformDirty;
		}
		if (!FMath::IsNearlyEqual(
			AState.LastSourceShadowCaptureRealSeconds,
			BState.LastSourceShadowCaptureRealSeconds))
		{
			return AState.LastSourceShadowCaptureRealSeconds
				< BState.LastSourceShadowCaptureRealSeconds;
		}
		return A.StableHash < B.StableHash;
	});

	const int32 ActiveRouteCount = FMath::Clamp(
		Settings->MaxActivePortalSourceShadowRoutes,
		0,
		RouteCandidates.Num());
	for (int32 CandidateIndex = ActiveRouteCount; CandidateIndex < RouteCandidates.Num(); ++CandidateIndex)
	{
		DestroySourceShadowResources(*RouteCandidates[CandidateIndex].RouteState);
	}

	TArray<WPPortalLightShadowMath::FCaptureCandidate> CaptureCandidates;
	CaptureCandidates.Reserve(ActiveRouteCount);
	for (int32 CandidateIndex = 0; CandidateIndex < ActiveRouteCount; ++CandidateIndex)
	{
		FRouteCandidate& Candidate = RouteCandidates[CandidateIndex];
		FWPPortalLightTransmissionState& RouteState = *Candidate.RouteState;
		RouteState.bSourceShadowActive = true;

		/** Exit 이동은 source depth를 무효화하지 않으므로 mapped basis만 현재 mapping으로 갱신합니다. */
		if (RouteState.bSourceShadowValid)
		{
			UpdateSourceShadowMaterialParameters(RouteState);
			SetSourceShadowMaterialEnabled(RouteState, true);
		}

		const double CaptureRateHz = RouteState.bExitPortalOnScreen
			? FMath::Max(static_cast<double>(Settings->PortalSourceShadowUpdateRateHz), 1.0)
			: WPPortalLightTransmissionPrivate::OffscreenCaptureRateHz;
		const double CaptureInterval = 1.0 / CaptureRateHz;
		const bool bPeriodicCaptureDue = RouteState.LastSourceShadowCaptureRealSeconds < 0.0
			|| NowSeconds - RouteState.LastSourceShadowCaptureRealSeconds >= CaptureInterval;

		WPPortalLightShadowMath::FCaptureCandidate& CaptureCandidate = CaptureCandidates.AddDefaulted_GetRef();
		CaptureCandidate.bCaptureDue = !RouteState.bSourceShadowValid
			|| RouteState.bSourceShadowDirty
			|| Candidate.bTransformDirty
			|| Candidate.bResolutionChangeReady
			|| bPeriodicCaptureDue;
		CaptureCandidate.bNeedsInitialCapture = !RouteState.bSourceShadowValid;
		CaptureCandidate.bTransformDirty = Candidate.bTransformDirty;
		CaptureCandidate.ScreenDiameterPixels = RouteState.ExitPortalScreenDiameterPixels;
		CaptureCandidate.LastCaptureTimeSeconds = RouteState.LastSourceShadowCaptureRealSeconds;
	}

	TArray<int32> SelectedCaptureIndices;
	WPPortalLightShadowMath::SelectCaptureCandidateIndices(
		CaptureCandidates,
		FMath::Max(Settings->MaxPortalSourceShadowCapturesPerFrame, 0),
		SelectedCaptureIndices);

	for (const int32 SelectedIndex : SelectedCaptureIndices)
	{
		if (!RouteCandidates.IsValidIndex(SelectedIndex))
		{
			continue;
		}

		FRouteCandidate& Candidate = RouteCandidates[SelectedIndex];
		FWPPortalLightTransmissionState& RouteState = *Candidate.RouteState;
		if (!ConfigureSourceShadowCapture(RouteState, Candidate.DesiredResolution))
		{
			SetSourceShadowMaterialEnabled(RouteState, false);
			RouteState.bSourceShadowValid = false;
			continue;
		}

		USceneCaptureComponent2D* Capture = RouteState.SourceShadowCapture.Get();
		ULightComponent* SourceLight = RouteState.SourceLight.Get();
		AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
		AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();
		if (!IsValid(Capture) || !IsValid(SourceLight) || !IsValid(EntryPortal) || !IsValid(ExitPortal))
		{
			SetSourceShadowMaterialEnabled(RouteState, false);
			RouteState.bSourceShadowValid = false;
			continue;
		}

		Capture->CaptureSceneDeferred();

		RouteState.LastSourceShadowCaptureRealSeconds = NowSeconds;
		RouteState.LastCapturedSourceTransform = SourceLight->GetComponentTransform();
		RouteState.LastCapturedEntryTransform = EntryPortal->GetActorTransform();
		RouteState.LastCapturedEntryRadius = EntryPortal->GetPortalRadius();
		RouteState.ResolutionCandidate = RouteState.CurrentSourceShadowResolution;
		RouteState.ResolutionCandidateSinceRealSeconds = NowSeconds;
		RouteState.bSourceShadowValid = true;
		RouteState.bSourceShadowDirty = false;

		UpdateSourceShadowMaterialParameters(RouteState);
		SetSourceShadowMaterialEnabled(RouteState, true);
	}
}

void UWPPortalLightTransmissionSubsystem::DestroyRouteState(FWPPortalLightTransmissionState& RouteState)
{
	check(IsInGameThread());

	DestroySourceShadowResources(RouteState);
	
	if (ULightComponent* ProxyLight = RouteState.ProxyLight.Get())
	{
		ProxyLight->SetVisibility(false, false);
		ProxyLight->DestroyComponent();
	}
	
	RouteState.ProxyLight.Reset();
	RouteState.SourceLight.Reset();
	RouteState.EntryPortal.Reset();
	RouteState.ExitPortal.Reset();
}

void UWPPortalLightTransmissionSubsystem::DestroyAllRouteStates()
{
	check(IsInGameThread());
	
	for (TPair<FWPPortalLightRouteKey, FWPPortalLightTransmissionState>& Entry : RouteStates)
	{
		DestroyRouteState(Entry.Value);
	}
	
	RouteStates.Reset();
}

void UWPPortalLightTransmissionSubsystem::HandleActorSpawned(AActor* SpawnedActor)
{
	check(IsInGameThread());

	if (bDeinitializing || !IsValid(SpawnedActor) || SpawnedActor->GetWorld() != GetWorld())
	{
		return;
	}

	/** Construction/registration 완료 뒤 component를 읽도록 PostActorTick까지 지연합니다. */
	PendingSpawnedActors.AddUnique(SpawnedActor);
}

void UWPPortalLightTransmissionSubsystem::HandleWorldPostActorTick(UWorld* TickedWorld, ELevelTick TickType,
	float DeltaSeconds)
{
	check(IsInGameThread());
	
	(void)TickType;
	(void)DeltaSeconds;
	
	if (bDeinitializing || TickedWorld != GetWorld())
	{
		return;
	}

	ProcessPendingSpawnedActors();

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds >= NextFullShadowCasterReconcileRealSeconds)
	{
		DiscoverAllShadowCasters();
		NextFullShadowCasterReconcileRealSeconds = NowSeconds
			+ WPPortalLightTransmissionPrivate::ShadowCasterReconcileIntervalSeconds;
	}
	else if (CompactShadowCasters())
	{
		MarkAllSourceShadowsDirty();
	}
	
	if (bPairTopologyDirty)
	{
		RefreshPairTopology();
	}
	
	/**
	 * Collection subsystem을 InitializeDependency로 먼저 초기화했으므로
	 * Collection의 PostActorTick callback이 먼저 등록됩니다.
	 * 
	 * 따라서 일반적으로 현재 프레임의 최신 membership을 소비합니다.
	 */
	ReconcileAllRoutes();
	UpdateSourceShadowRoutes();
}

void UWPPortalLightTransmissionSubsystem::HandlePortalPairAdded(const FWPPortalPairSnapshot& PairSnapshot)
{
	check(IsInGameThread());
	
	(void)PairSnapshot;
	
	/**
	 * 이벤트 payload만으로 상태를 직접 구성하지 않고,
	 * 다음 PostActorTick에 Registry 전체 snapshot을 다시 읽습니다.
	 */
	bPairTopologyDirty = true;
}

void UWPPortalLightTransmissionSubsystem::HandlePortalPairRemoved(const FWPPortalPairSnapshot& PairSnapshot)
{
	check(IsInGameThread());
	
	/**
	 * Removed event의 endpoint는 이미 invalid일 수 있으므로
	 * Actor를 역참조하지 않습니다.
	 * 
	 * 다음 PostActorTick에 Registry snapshot을 다시 읽으면
	 * PairId가 사라지고 해당 route가 정리됩니다.
	 */
	(void)PairSnapshot;
	
	bPairTopologyDirty = true;
}
