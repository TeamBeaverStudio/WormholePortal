// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Subsystem/WPPortalAudioSubsystem.h"

#include "Audio/WPPortalAudioMath.h"
#include "Subsystem/WPRegistrySubsystem.h"
#include "WormholePortalActor.h"
#include "WPLog.h"
#include "Audio/WPPortalAudioTags.h"
#include "WPSettings.h"
#include "WPTransform.h"
#include "Transit/WPTransitTags.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioThread.h"
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSourceBus.h"
#include "Sound/SoundSubmix.h"
#include "UObject/UObjectIterator.h"

namespace WPPortalAudioPrivate
{
	const FName ProxyHostBaseName(TEXT("WPPortalAudioProxyHost"));
	const FName ProxyRootBaseName(TEXT("WPPortalAudioProxyRoot"));
	const FName ProxyAudioBaseName(TEXT("WPPortalAudioProxy"));
	const FName SourceBusBaseName(TEXT("WPPortalAudioSourceBus"));

	constexpr int32 MaximumListenerProbeCount = 8;
	constexpr float PortalScaleTolerance = 1.0e-3f;
	constexpr float PortalRadiusToleranceCm = 1.0e-3f;

	bool IsUnitScalePortal(const AWormholePortalActor& Portal)
	{
		return Portal.GetActorScale3D().Equals(FVector::OneVector, PortalScaleTolerance);
	}

	FSoundAttenuationSettings MakePortalAttenuation(
		const FSoundAttenuationSettings& SourceAttenuation)
	{
		FSoundAttenuationSettings PortalAttenuation = SourceAttenuation;

		// Exit Portal은 모든 방향으로 재방사하므로 원본 shape의 최대 무감쇠 범위를
		// 유지한 구면 attenuation으로 바꿉니다.
		const float PortalInnerRadius = FMath::Max(
			SourceAttenuation.GetMaxDimension()
				- SourceAttenuation.GetMaxFalloffDistance(),
			0.0f);
		PortalAttenuation.AttenuationShape = EAttenuationShape::Sphere;
		PortalAttenuation.AttenuationShapeExtents = FVector(PortalInnerRadius, 0.0f, 0.0f);
		PortalAttenuation.ConeOffset = 0.0f;
		PortalAttenuation.ConeSphereRadius = 0.0f;
		PortalAttenuation.ConeSphereFalloffDistance = 0.0f;

		// 펼친 Proxy->Listener trace는 Entry 공간을 잘못 검사하므로 native occlusion을 끕니다.
		PortalAttenuation.bEnableOcclusion = false;
		return PortalAttenuation;
	}

	float ResolveSourcePickupGain(
		const UAudioComponent& SourceAudio,
		const FVector& EntrySurfacePoint)
	{
		const FSoundAttenuationSettings* SourceAttenuation =
			SourceAudio.GetAttenuationSettingsToApply();
		if (!SourceAttenuation || !SourceAttenuation->bAttenuate)
		{
			return 1.0f;
		}
		if (SourceAttenuation->AttenuationShape == EAttenuationShape::Sphere)
		{
			return 1.0f;
		}

		const FTransform SourceTransform = SourceAudio.GetComponentTransform();
		const float SourceShapeGain = SourceAttenuation->Evaluate(
			SourceTransform,
			EntrySurfacePoint);
		const float SphericalReferenceGain = MakePortalAttenuation(*SourceAttenuation).Evaluate(
			SourceTransform,
			EntrySurfacePoint);

		if (!FMath::IsFinite(SourceShapeGain)
			|| !FMath::IsFinite(SphericalReferenceGain)
			|| SourceShapeGain <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}

		// 거리 곡선은 logical path에 native attenuation으로 한 번 적용하므로,
		// 여기서는 원본 shape가 만든 입사 방향/형상 감쇠만 분리해 사용합니다.
		return SphericalReferenceGain > KINDA_SMALL_NUMBER
			? FMath::Clamp(SourceShapeGain / SphericalReferenceGain, 0.0f, 1.0f)
			: 1.0f;
	}

	bool MakeBiasedSegment(
		const FVector& Start,
		const FVector& End,
		const FVector& Direction,
		const float StartBias,
		const float EndBias,
		FVector& OutStart,
		FVector& OutEnd)
	{
		const float Length = FVector::Distance(Start, End);
		if (!FMath::IsFinite(Length) || Length <= 1.0f || Direction.IsNearlyZero())
		{
			return false;
		}

		const float MaximumBias = Length * 0.25f;
		OutStart = Start + Direction * FMath::Clamp(StartBias, 0.0f, MaximumBias);
		OutEnd = End - Direction * FMath::Clamp(EndBias, 0.0f, MaximumBias);
		return FVector::DistSquared(OutStart, OutEnd) > 1.0f;
	}
}

bool UWPPortalAudioSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	return IsValid(World)
		&& World->IsGameWorld()
		&& World->GetNetMode() != NM_DedicatedServer;
}

void UWPPortalAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	Super::Initialize(Collection);

	bDeinitializing = false;
	bPairTopologyDirty = true;

	TrackedSources.Reset();
	TrackedSourceKeys.Reset();
	PendingSpawnedActors.Reset();
	ActivePairs.Reset();
	RouteStates.Reset();
	ProxyHostActor.Reset();
	PortalBusSoundClass = nullptr;

	UWorld* World = GetWorld();
	check(World);

	PortalBusSoundClass = NewObject<USoundClass>(
		this,
		MakeUniqueObjectName(this, USoundClass::StaticClass(), TEXT("WPPortalAudioBusClass")),
		RF_Transient);
	if (PortalBusSoundClass)
	{
		// 원본 post-effect 신호에는 원본 SoundClass gain이 이미 포함됩니다.
		PortalBusSoundClass->Properties.bApplyAmbientVolumes = false;
		PortalBusSoundClass->Properties.bApplyEffects = false;
		PortalBusSoundClass->Properties.bReverb = false;
		if (GEngine && GEngine->GetAudioDeviceManager())
		{
			GEngine->GetAudioDeviceManager()->RegisterSoundClass(PortalBusSoundClass);
		}
	}

	RegistrySubsystem = Collection.InitializeDependency<UWPRegistrySubsystem>();

	if (UWPRegistrySubsystem* Registry = RegistrySubsystem.Get())
	{
		// Delegate를 먼저 연결한 뒤 authoritative pair snapshot을 bootstrap합니다.
		PortalPairAddedHandle = Registry->OnPortalPairAdded().AddUObject(
			this,
			&UWPPortalAudioSubsystem::HandlePortalPairAdded);
		PortalPairRemovedHandle = Registry->OnPortalPairRemoved().AddUObject(
			this,
			&UWPPortalAudioSubsystem::HandlePortalPairRemoved);
	}

	const FOnActorSpawned::FDelegate ActorSpawnedDelegate = FOnActorSpawned::FDelegate::CreateUObject(
		this,
		&UWPPortalAudioSubsystem::HandleActorSpawned);
	ActorSpawnedHandle = World->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this,
		&UWPPortalAudioSubsystem::HandleWorldPostActorTick);

	DiscoverAllAudioComponents();
	RefreshPairTopology();

	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const float ReconcileInterval = Settings
		? FMath::Max(Settings->PortalAudioSourceReconcileInterval, 0.05f)
		: 0.25f;
	NextFullSourceReconcileRealSeconds = FPlatformTime::Seconds() + ReconcileInterval;

#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalAudio][Initialize] World=%s TrackedSources=%d ActivePairs=%d CpuMs=%.3f"),
		*GetNameSafe(World),
		TrackedSources.Num(),
		ActivePairs.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void UWPPortalAudioSubsystem::Deinitialize()
{
	check(IsInGameThread());

#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
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

#if !UE_BUILD_SHIPPING
	const int32 ReleasedRouteCount = RouteStates.Num();
#endif
	DestroyAllRouteStates();

	for (FWPTrackedPortalAudioSource& SourceRecord : TrackedSources)
	{
		ReleaseSourceBusSend(SourceRecord);
	}

	if (AActor* ProxyHost = ProxyHostActor.Get())
	{
		UWorld* World = GetWorld();
		if (World && !World->bIsTearingDown)
		{
			ProxyHost->Destroy();
		}
	}

	ProxyHostActor.Reset();
	if (PortalBusSoundClass)
	{
		if (GEngine && GEngine->GetAudioDeviceManager())
		{
			GEngine->GetAudioDeviceManager()->UnregisterSoundClass(PortalBusSoundClass);
		}

		// 위의 proxy stop, bus send 해제, SoundClass 해제가 참조한 transient UObject를
		// TrackedSources/UPROPERTY에서 놓기 전에 audio thread가 모두 소비하게 합니다.
		FAudioCommandFence AudioCleanupFence;
		AudioCleanupFence.BeginFence();
		AudioCleanupFence.Wait();
		PortalBusSoundClass = nullptr;
	}
	ActivePairs.Reset();
	PendingSpawnedActors.Reset();
	TrackedSourceKeys.Reset();
	TrackedSources.Reset();
	RegistrySubsystem.Reset();

	NextFullSourceReconcileRealSeconds = 0.0;
	bPairTopologyDirty = true;

#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		Verbose,
		TEXT("[PortalAudio][Deinitialize] ReleasedRoutes=%d CpuMs=%.3f"),
		ReleasedRouteCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif

	Super::Deinitialize();
}

int32 UWPPortalAudioSubsystem::GetTrackedSourceCount() const
{
	check(IsInGameThread());
	return TrackedSources.Num();
}

int32 UWPPortalAudioSubsystem::GetProxyAudioCount() const
{
	check(IsInGameThread());

	int32 Count = 0;
	for (const TPair<FWPPortalAudioRouteKey, FWPPortalAudioRouteState>& Entry : RouteStates)
	{
		if (Entry.Value.ProxyAudio.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

void UWPPortalAudioSubsystem::DiscoverAllAudioComponents()
{
	check(IsInGameThread());

	if (bDeinitializing)
	{
		return;
	}

	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		UAudioComponent* AudioComponent = *It;
		if (!SilenceGeneratedTransitAudio(AudioComponent)
			&& IsSupportedSource(AudioComponent))
		{
			TrackAudioComponent(AudioComponent);
		}
	}
}

void UWPPortalAudioSubsystem::DiscoverAudioComponentsOnActor(AActor* Actor)
{
	check(IsInGameThread());

	if (bDeinitializing || !IsValid(Actor) || Actor->GetWorld() != GetWorld())
	{
		return;
	}

	TInlineComponentArray<UAudioComponent*> AudioComponents;
	Actor->GetComponents(AudioComponents);

	for (UAudioComponent* AudioComponent : AudioComponents)
	{
		if (!SilenceGeneratedTransitAudio(AudioComponent))
		{
			TrackAudioComponent(AudioComponent);
		}
	}
}

void UWPPortalAudioSubsystem::ProcessPendingSpawnedActors()
{
	check(IsInGameThread());

	TArray<TWeakObjectPtr<AActor>> PendingActors;
	Swap(PendingActors, PendingSpawnedActors);

	for (const TWeakObjectPtr<AActor>& WeakActor : PendingActors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			DiscoverAudioComponentsOnActor(Actor);
		}
	}
}

bool UWPPortalAudioSubsystem::SilenceGeneratedTransitAudio(
	UAudioComponent* AudioComponent) const
{
	if (!IsValid(AudioComponent)
		|| AudioComponent->IsTemplate()
		|| AudioComponent->GetWorld() != GetWorld())
	{
		return false;
	}

	const AActor* Owner = AudioComponent->GetOwner();
	if (!IsValid(Owner) || !Owner->ActorHasTag(WPTransitTags::Generated))
	{
		return false;
	}

	// Transit twin은 clipping을 위한 시각 복제체입니다. 원본과 Portal Audio proxy에
	// 더해 auto-activate된 세 번째 소리가 나지 않도록 등록 완료 직후 음소거합니다.
	AudioComponent->ComponentTags.AddUnique(WPTransitTags::Generated);
	AudioComponent->Stop();
	AudioComponent->SetVolumeMultiplier(0.0f);
	return true;
}

void UWPPortalAudioSubsystem::TrackAudioComponent(UAudioComponent* AudioComponent)
{
	check(IsInGameThread());

	if (!IsSupportedSource(AudioComponent))
	{
		return;
	}

	const TObjectKey<UAudioComponent> ObjectKey(AudioComponent);
	if (TrackedSourceKeys.Contains(ObjectKey))
	{
		return;
	}

	FWPTrackedPortalAudioSource Record;
	Record.ObjectKey = ObjectKey;
	Record.AudioComponent = AudioComponent;

	TrackedSourceKeys.Add(ObjectKey);
	TrackedSources.Add(MoveTemp(Record));

#if !UE_BUILD_SHIPPING
	WP_LOG(
		this,
		VeryVerbose,
		TEXT("[PortalAudio][SourceTracked] Audio=%s Owner=%s TrackedCount=%d"),
		*GetNameSafe(AudioComponent),
		*GetNameSafe(AudioComponent->GetOwner()),
		TrackedSources.Num());
#endif
}

void UWPPortalAudioSubsystem::CompactTrackedAudioComponents()
{
	check(IsInGameThread());

	for (int32 Index = TrackedSources.Num() - 1; Index >= 0; --Index)
	{
		FWPTrackedPortalAudioSource& Record = TrackedSources[Index];
		if (IsSupportedSource(Record.AudioComponent.Get()))
		{
			if (!HasSupportedSpatialization(Record.AudioComponent.Get()) && Record.SourceBus.IsValid())
			{
				ReleaseSourceBusSend(Record);
			}
			continue;
		}

		ReleaseSourceBusSend(Record);
		TrackedSourceKeys.Remove(Record.ObjectKey);
		TrackedSources.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	}
}

USoundSourceBus* UWPPortalAudioSubsystem::EnsureSourceBus(
	FWPTrackedPortalAudioSource& SourceRecord,
	UAudioComponent& SourceAudio)
{
	check(IsInGameThread());

	if (!HasSupportedSpatialization(&SourceAudio) || !PortalBusSoundClass)
	{
		return nullptr;
	}

	USoundSourceBus* SourceBus = SourceRecord.SourceBus.Get();
	if (!IsValid(SourceBus))
	{
		SourceBus = NewObject<USoundSourceBus>(
			this,
			MakeUniqueObjectName(this, USoundSourceBus::StaticClass(), WPPortalAudioPrivate::SourceBusBaseName),
			RF_Transient);
		if (!SourceBus)
		{
			return nullptr;
		}

		SourceBus->SourceBusChannels = ESourceBusChannels::Mono;
		SourceBus->SourceBusDuration = 0.0f;
		SourceBus->bAutoDeactivateWhenSilent = false;
		SourceBus->bEnableBusSends = false;
		SourceBus->VirtualizationMode = EVirtualizationMode::Restart;
		SourceRecord.SourceBus.Reset(SourceBus);
		SourceRecord.LastBoundPlayOrder = 0;
		SourceRecord.bHasConfiguredSourceBusSend = false;
	}

	USoundBase* SourceSound = SourceAudio.Sound.Get();
	check(SourceSound);

	// Source effect와 component fader는 post-effect bus 신호에 이미 포함됩니다. 여기서는 출력 routing만 복원합니다.
	SourceBus->SoundClassObject = PortalBusSoundClass;
	SourceBus->bEnableBaseSubmix = SourceSound->bEnableBaseSubmix;
	SourceBus->bEnableSubmixSends = SourceSound->bEnableSubmixSends;
	SourceBus->SoundSubmixObject = SourceSound->SoundSubmixObject;
	SourceBus->SoundSubmixSends = SourceSound->SoundSubmixSends;

	if (!SourceBus->SoundSubmixObject)
	{
		USoundClass* EffectiveSourceClass = SourceAudio.SoundClassOverride
			? SourceAudio.SoundClassOverride.Get()
			: SourceSound->GetSoundClass();
		if (EffectiveSourceClass)
		{
			SourceBus->SoundSubmixObject = EffectiveSourceClass->Properties.DefaultSubmix.Get();
		}
	}

	return SourceBus;
}

void UWPPortalAudioSubsystem::RefreshSourceBusSend(
	FWPTrackedPortalAudioSource& SourceRecord,
	UAudioComponent& SourceAudio)
{
	check(IsInGameThread());

	USoundSourceBus* SourceBus = SourceRecord.SourceBus.Get();
	if (!IsValid(SourceBus)
		|| SourceAudio.GetPlayState() == EAudioComponentPlayState::Stopped)
	{
		return;
	}

	const uint32 CurrentPlayOrder = SourceAudio.GetLastPlayOrder();
	if (SourceRecord.bHasConfiguredSourceBusSend
		&& SourceRecord.LastBoundPlayOrder == CurrentPlayOrder)
	{
		return;
	}

	SourceAudio.SetSourceBusSendPostEffect(SourceBus, 1.0f);
	SourceRecord.LastBoundPlayOrder = CurrentPlayOrder;
	SourceRecord.bHasConfiguredSourceBusSend = true;
}

void UWPPortalAudioSubsystem::ReleaseSourceBusSend(FWPTrackedPortalAudioSource& SourceRecord)
{
	check(IsInGameThread());

	UAudioComponent* SourceAudio = SourceRecord.AudioComponent.Get();
	USoundSourceBus* SourceBus = SourceRecord.SourceBus.Get();
	if (SourceRecord.bHasConfiguredSourceBusSend
		&& IsValid(SourceAudio)
		&& IsValid(SourceBus)
		&& SourceAudio->GetPlayState() != EAudioComponentPlayState::Stopped)
	{
		SourceAudio->SetSourceBusSendPostEffect(SourceBus, 0.0f);
	}

	SourceRecord.LastBoundPlayOrder = 0;
	SourceRecord.bHasConfiguredSourceBusSend = false;
}

bool UWPPortalAudioSubsystem::IsSupportedSource(const UAudioComponent* AudioComponent) const
{
	if (!IsValid(AudioComponent)
		|| AudioComponent->IsTemplate()
		|| AudioComponent->GetWorld() != GetWorld())
	{
		return false;
	}

	if (AudioComponent->ComponentHasTag(WPPortalAudioTags::Disabled)
		|| AudioComponent->ComponentHasTag(WPPortalAudioTags::Generated)
		|| AudioComponent->ComponentHasTag(WPTransitTags::Generated))
	{
		return false;
	}

	if (const AActor* Owner = AudioComponent->GetOwner())
	{
		if (Owner->ActorHasTag(WPPortalAudioTags::Disabled)
			|| Owner->ActorHasTag(WPPortalAudioTags::Generated)
			|| Owner->ActorHasTag(WPTransitTags::Generated))
		{
			return false;
		}
	}

	return true;
}

bool UWPPortalAudioSubsystem::IsEligibleSource(const UAudioComponent* AudioComponent) const
{
	if (!IsSupportedSource(AudioComponent)
		|| !AudioComponent->IsRegistered()
		|| !HasSupportedSpatialization(AudioComponent)
		|| AudioComponent->bCanPlayMultipleInstances)
	{
		return false;
	}

	const EAudioComponentPlayState PlayState = AudioComponent->GetPlayState();
	return PlayState != EAudioComponentPlayState::Stopped;
}

bool UWPPortalAudioSubsystem::HasSupportedSpatialization(const UAudioComponent* AudioComponent) const
{
	if (!IsSupportedSource(AudioComponent)
		|| !AudioComponent->Sound
		|| AudioComponent->Sound->IsA<USoundSourceBus>()
		|| !AudioComponent->bAllowSpatialization
		|| AudioComponent->bIsUISound)
	{
		return false;
	}

	// Source Bus proxy에는 SoundCue 내부 attenuation node를 옮길 수 없으므로 명시적인
	// component/sound attenuation 설정이 있는 실제 3D source만 지원합니다.
	const FSoundAttenuationSettings* Attenuation = AudioComponent->GetAttenuationSettingsToApply();
	return Attenuation && Attenuation->bSpatialize;
}

bool UWPPortalAudioSubsystem::CanSourceReachPortal(
	const UAudioComponent& AudioComponent,
	const AWormholePortalActor& EntryPortal) const
{
	const FVector SourceLocation = AudioComponent.GetComponentLocation();
	const float PortalRadius = EntryPortal.GetPortalRadius();
	if (SourceLocation.ContainsNaN() || !FMath::IsFinite(PortalRadius) || PortalRadius <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const float DistanceToPortal = FVector::Distance(SourceLocation, EntryPortal.GetActorLocation());
	if (DistanceToPortal <= PortalRadius + 1.0f)
	{
		// Portal 내부 또는 seam 위의 음원은 방향을 하나로 정할 수 없으므로 현재 1-hop route에서 제외합니다.
		return false;
	}

	const FSoundAttenuationSettings* Attenuation = AudioComponent.GetAttenuationSettingsToApply();
	if (!Attenuation)
	{
		return false;
	}

	if (!Attenuation->bAttenuate)
	{
		return true;
	}

	const float MaximumDistance = Attenuation->GetMaxDimension();
	return FMath::IsFinite(MaximumDistance)
		&& MaximumDistance > KINDA_SMALL_NUMBER
		&& DistanceToPortal <= MaximumDistance + PortalRadius;
}

void UWPPortalAudioSubsystem::RefreshPairTopology()
{
	check(IsInGameThread());

	bPairTopologyDirty = false;
	TMap<FGuid, FWPPortalAudioActivePairState> NewActivePairs;

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
		if (!IsValid(PortalA)
			|| !IsValid(PortalB)
			|| PortalA->GetWorld() != World
			|| PortalB->GetWorld() != World)
		{
			continue;
		}

		FWPPortalAudioActivePairState PairState;
		PairState.PortalA = PortalA;
		PairState.PortalB = PortalB;
		NewActivePairs.Add(PairSnapshot.PairId, MoveTemp(PairState));
	}

	ActivePairs = MoveTemp(NewActivePairs);
}

void UWPPortalAudioSubsystem::ReconcileAllRoutes()
{
	check(IsInGameThread());

	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!Settings || !Settings->bEnablePortalAudio)
	{
		DestroyAllRouteStates();
		for (FWPTrackedPortalAudioSource& SourceRecord : TrackedSources)
		{
			ReleaseSourceBusSend(SourceRecord);
		}
		return;
	}

	TSet<FWPPortalAudioRouteKey> DesiredRoutes;
	DesiredRoutes.Reserve(RouteStates.Num() + 8);

	for (const TPair<FGuid, FWPPortalAudioActivePairState>& PairEntry : ActivePairs)
	{
		AWormholePortalActor* PortalA = PairEntry.Value.PortalA.Get();
		AWormholePortalActor* PortalB = PairEntry.Value.PortalB.Get();
		if (!IsValid(PortalA) || !IsValid(PortalB))
		{
			continue;
		}

		ReconcileDirection(PairEntry.Key, PortalA, PortalB, DesiredRoutes);
		ReconcileDirection(PairEntry.Key, PortalB, PortalA, DesiredRoutes);
	}

	for (TMap<FWPPortalAudioRouteKey, FWPPortalAudioRouteState>::TIterator It(RouteStates); It; ++It)
	{
		if (DesiredRoutes.Contains(It.Key()))
		{
			continue;
		}

		DestroyRouteState(It.Value());
		It.RemoveCurrent();
	}

	TSet<TObjectKey<UAudioComponent>> SourcesUsingBus;
	for (const FWPPortalAudioRouteKey& RouteKey : DesiredRoutes)
	{
		if (const FWPPortalAudioRouteState* RouteState = RouteStates.Find(RouteKey);
			RouteState && RouteState->bUseSourceBus)
		{
			SourcesUsingBus.Add(RouteKey.SourceAudioKey);
		}
	}

	for (FWPTrackedPortalAudioSource& SourceRecord : TrackedSources)
	{
		if (SourceRecord.bHasConfiguredSourceBusSend
			&& !SourcesUsingBus.Contains(SourceRecord.ObjectKey))
		{
			ReleaseSourceBusSend(SourceRecord);
		}
	}
}

void UWPPortalAudioSubsystem::ReconcileDirection(
	const FGuid& PairId,
	AWormholePortalActor* EntryPortal,
	AWormholePortalActor* ExitPortal,
	TSet<FWPPortalAudioRouteKey>& OutDesiredRoutes)
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World
		|| !PairId.IsValid()
		|| !IsValid(EntryPortal)
		|| !IsValid(ExitPortal)
		|| EntryPortal == ExitPortal
		|| EntryPortal->GetWorld() != World
		|| ExitPortal->GetWorld() != World
		|| EntryPortal->GetLinkedPortal() != ExitPortal
		|| ExitPortal->GetLinkedPortal() != EntryPortal
		|| !WPPortalAudioPrivate::IsUnitScalePortal(*EntryPortal)
		|| !WPPortalAudioPrivate::IsUnitScalePortal(*ExitPortal)
		|| !FMath::IsNearlyEqual(
			EntryPortal->GetPortalRadius(),
			ExitPortal->GetPortalRadius(),
			WPPortalAudioPrivate::PortalRadiusToleranceCm))
	{
		return;
	}

	FWPTransform Mapping;
	if (!FWPTransform::Build(EntryPortal, ExitPortal, Mapping))
	{
		return;
	}

	for (FWPTrackedPortalAudioSource& Record : TrackedSources)
	{
		UAudioComponent* SourceAudio = Record.AudioComponent.Get();
		if (!IsEligibleSource(SourceAudio) || !CanSourceReachPortal(*SourceAudio, *EntryPortal))
		{
			continue;
		}

		FAudioDevice* SourceAudioDevice = SourceAudio->GetAudioDevice();
		const FSoundAttenuationSettings* SourceAttenuation =
			SourceAudio->GetAttenuationSettingsToApply();
		const bool bUsesListenerFocus = SourceAttenuation
			&& SourceAttenuation->bEnableListenerFocus;
		bool bUseSourceBus = SourceAudio->Sound->IsPlayWhenSilent()
			&& SourceAudioDevice
			&& SourceAudioDevice->PlayWhenSilentEnabled()
			// Post-effect bus에는 source 기준 listener focus가 이미 들어오므로
			// portal 기준 focus를 한 번만 적용할 수 있는 fallback을 사용합니다.
			&& !bUsesListenerFocus;
		USoundSourceBus* SourceBus = nullptr;
		if (bUseSourceBus)
		{
			SourceBus = EnsureSourceBus(Record, *SourceAudio);
			bUseSourceBus = IsValid(SourceBus);
			if (bUseSourceBus)
			{
				RefreshSourceBusSend(Record, *SourceAudio);
			}
		}

		if (!bUseSourceBus && Record.bHasConfiguredSourceBusSend)
		{
			ReleaseSourceBusSend(Record);
		}

		const FWPPortalAudioRouteKey RouteKey {
			PairId,
			TObjectKey<AWormholePortalActor>(EntryPortal),
			Record.ObjectKey
		};
		OutDesiredRoutes.Add(RouteKey);

		FWPPortalAudioRouteState* RouteState = RouteStates.Find(RouteKey);
		if (!RouteState)
		{
			FWPPortalAudioRouteState NewState;
			NewState.EntryPortal = EntryPortal;
			NewState.ExitPortal = ExitPortal;
			NewState.SourceAudio = SourceAudio;
			NewState.SourceBus = SourceBus;
			NewState.bUseSourceBus = bUseSourceBus;
			RouteState = &RouteStates.Add(RouteKey, MoveTemp(NewState));
		}

		if (RouteState->bUseSourceBus != bUseSourceBus
			|| RouteState->SourceBus.Get() != SourceBus
			|| (RouteState->ProxyAudio.IsValid()
				&& RouteState->SourceSoundAtProxyCreation.Get() != SourceAudio->Sound.Get()))
		{
			DestroyRouteProxy(*RouteState);
		}

		RouteState->EntryPortal = EntryPortal;
		RouteState->ExitPortal = ExitPortal;
		RouteState->SourceAudio = SourceAudio;
		RouteState->SourceBus = SourceBus;
		RouteState->bUseSourceBus = bUseSourceBus;
	}
}

AActor* UWPPortalAudioSubsystem::EnsureProxyHost()
{
	check(IsInGameThread());

	UWorld* World = GetWorld();
	if (!World || bDeinitializing)
	{
		return nullptr;
	}

	if (AActor* ExistingHost = ProxyHostActor.Get(); ExistingHost && ExistingHost->GetWorld() == World)
	{
		return ExistingHost;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		AActor::StaticClass(),
		WPPortalAudioPrivate::ProxyHostBaseName);
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* NewHost = World->SpawnActor<AActor>(
		AActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	if (!IsValid(NewHost))
	{
		return nullptr;
	}

	NewHost->Tags.AddUnique(WPPortalAudioTags::Generated);
	NewHost->SetActorEnableCollision(false);
	NewHost->SetActorTickEnabled(false);
	NewHost->SetReplicates(false);

	const FName RootName = MakeUniqueObjectName(
		NewHost,
		USceneComponent::StaticClass(),
		WPPortalAudioPrivate::ProxyRootBaseName);
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
	RootComponent->RegisterComponentWithWorld(World);

	if (!RootComponent->IsRegistered())
	{
		NewHost->Destroy();
		return nullptr;
	}

	ProxyHostActor = NewHost;
	return NewHost;
}

UAudioComponent* UWPPortalAudioSubsystem::CreateProxyAudio(
	const UAudioComponent& SourceAudio,
	USoundBase& ProxySound,
	const bool bUseSourceBus,
	const FWPTransform& Mapping)
{
	check(IsInGameThread());

	AActor* ProxyHost = EnsureProxyHost();
	UWorld* World = GetWorld();
	if (!ProxyHost || !World || !SourceAudio.Sound)
	{
		return nullptr;
	}

	const FName ProxyName = MakeUniqueObjectName(
		ProxyHost,
		UAudioComponent::StaticClass(),
		WPPortalAudioPrivate::ProxyAudioBaseName);
	UAudioComponent* ProxyAudio = NewObject<UAudioComponent>(
		ProxyHost,
		UAudioComponent::StaticClass(),
		ProxyName,
		RF_Transient);
	if (!ProxyAudio)
	{
		return nullptr;
	}

	ProxyHost->AddInstanceComponent(ProxyAudio);
	ProxyAudio->OnComponentCreated();
	ProxyAudio->ComponentTags.AddUnique(WPPortalAudioTags::Generated);

	if (USceneComponent* HostRoot = ProxyHost->GetRootComponent())
	{
		ProxyAudio->SetupAttachment(HostRoot);
	}

	ProxyAudio->SetMobility(EComponentMobility::Movable);
	ConfigureProxyAudioBeforeRegistration(*ProxyAudio, SourceAudio, ProxySound, bUseSourceBus);
	ProxyAudio->SetWorldLocationAndRotation(
		Mapping.MapRayOrigin(SourceAudio.GetComponentLocation()),
		Mapping.MapRot(SourceAudio.GetComponentQuat()),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);

	ProxyAudio->RegisterComponentWithWorld(World);
	if (!ProxyAudio->IsRegistered())
	{
		ProxyAudio->DestroyComponent();
		return nullptr;
	}

	return ProxyAudio;
}

void UWPPortalAudioSubsystem::ConfigureProxyAudioBeforeRegistration(
	UAudioComponent& ProxyAudio,
	const UAudioComponent& SourceAudio,
	USoundBase& ProxySound,
	const bool bUseSourceBus) const
{
	ProxyAudio.SetAutoActivate(false);
	ProxyAudio.bAutoDestroy = false;
	ProxyAudio.bStopWhenOwnerDestroyed = true;
	ProxyAudio.bAutoManageAttachment = false;
	ProxyAudio.bCanPlayMultipleInstances = false;
	ProxyAudio.bAllowSpatialization = true;
	ProxyAudio.bIsUISound = false;
	ProxyAudio.bSuppressSubtitles = true;
	ProxyAudio.bShouldRemainActiveIfDropped = bUseSourceBus
		? false
		: SourceAudio.bShouldRemainActiveIfDropped;
	ProxyAudio.bOverridePriority = SourceAudio.bOverridePriority;
	ProxyAudio.Priority = SourceAudio.Priority;

	if (bUseSourceBus)
	{
		// Source effect, parameters, pitch, random modulation과 component fade는 post-effect bus에 이미 반영됩니다.
		ProxyAudio.SoundClassOverride = PortalBusSoundClass;
		ProxyAudio.SourceEffectChain = nullptr;
		ProxyAudio.DefaultParameters.Reset();
		ProxyAudio.InstanceParameters.Reset();
	}
	else
	{
		// Fallback은 SoundBase를 새로 재생하므로 component-level 설정을 한 번 다시 적용합니다.
		ProxyAudio.SoundClassOverride = SourceAudio.SoundClassOverride;
		ProxyAudio.SourceEffectChain = SourceAudio.SourceEffectChain;
		ProxyAudio.DefaultParameters = SourceAudio.DefaultParameters;
		ProxyAudio.InstanceParameters = SourceAudio.InstanceParameters;
	}
	ProxyAudio.ConcurrencySet.Reset();

	// Sound asset의 concurrency로 fallback하지 않도록 route마다 독립 concurrency identity를 둡니다.
	USoundConcurrency* ProxyConcurrency = NewObject<USoundConcurrency>(
		&ProxyAudio,
		NAME_None,
		RF_Transient);
	if (ProxyConcurrency)
	{
		ProxyConcurrency->Concurrency.MaxCount = 1;
		ProxyConcurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
		ProxyAudio.ConcurrencySet.Add(ProxyConcurrency);
	}

	ProxyAudio.SetSound(&ProxySound);

	if (const FSoundAttenuationSettings* EffectiveAttenuation = SourceAudio.GetAttenuationSettingsToApply())
	{
		const FSoundAttenuationSettings PortalAttenuation =
			WPPortalAudioPrivate::MakePortalAttenuation(*EffectiveAttenuation);
		ProxyAudio.AdjustAttenuation(PortalAttenuation);
	}
	else
	{
		ProxyAudio.SetOverrideAttenuation(false);
		ProxyAudio.SetAttenuationSettings(nullptr);
	}

	ProxyAudio.OcclusionCheckInterval = -1.0f;
	ProxyAudio.VolumeMultiplier = 0.0f;
	ProxyAudio.PitchMultiplier = bUseSourceBus ? 1.0f : SourceAudio.PitchMultiplier;
	ProxyAudio.bEnableHighPassFilter = SourceAudio.bEnableHighPassFilter;
	ProxyAudio.HighPassFilterFrequency = SourceAudio.HighPassFilterFrequency;
	ProxyAudio.bEnableLowPassFilter = SourceAudio.bEnableLowPassFilter;
	ProxyAudio.LowPassFilterFrequency = SourceAudio.LowPassFilterFrequency;
}

float UWPPortalAudioSubsystem::EstimateFallbackPlaybackStartTime(
	const UAudioComponent& SourceAudio) const
{
	USoundBase* SourceSound = SourceAudio.Sound.Get();
	FAudioDevice* AudioDevice = SourceAudio.GetAudioDevice();
	if (!SourceSound || !AudioDevice)
	{
		return 0.0f;
	}

	const double ElapsedSeconds = FMath::Max(
		AudioDevice->GetAudioTime() - static_cast<double>(SourceAudio.TimeAudioComponentPlayed),
		0.0);
	const double EffectivePitch = FMath::Max(
		FMath::Abs(static_cast<double>(SourceAudio.PitchMultiplier)
			* static_cast<double>(SourceSound->GetPitchMultiplier())),
		0.0);
	double EstimatedStartTime = ElapsedSeconds * EffectivePitch;

	if (SourceSound->IsLooping())
	{
		// USoundWave의 GetDuration은 loop 시 sentinel을 반환하므로 raw Duration이 유효할 때만 감습니다.
		const double LoopDuration = static_cast<double>(SourceSound->Duration);
		if (FMath::IsFinite(LoopDuration)
			&& LoopDuration > KINDA_SMALL_NUMBER
			&& LoopDuration < INDEFINITELY_LOOPING_DURATION)
		{
			EstimatedStartTime = FMath::Fmod(EstimatedStartTime, LoopDuration);
		}
	}
	else
	{
		const double Duration = static_cast<double>(SourceSound->GetDuration());
		if (FMath::IsFinite(Duration) && Duration > KINDA_SMALL_NUMBER)
		{
			EstimatedStartTime = FMath::Min(
				EstimatedStartTime,
				FMath::Max(Duration - 0.01, 0.0));
		}
	}

	return static_cast<float>(FMath::Max(EstimatedStartTime, 0.0));
}

void UWPPortalAudioSubsystem::SynchronizeFallbackPlayback(
	FWPPortalAudioRouteState& RouteState,
	const UAudioComponent& SourceAudio,
	UAudioComponent& ProxyAudio) const
{
	const EAudioComponentPlayState SourcePlayState = SourceAudio.GetPlayState();
	if (SourcePlayState == EAudioComponentPlayState::Stopped)
	{
		ProxyAudio.Stop();
		RouteState.LastSourcePlayOrder = 0;
		RouteState.bFallbackProxyStarted = false;
		return;
	}

	const uint32 CurrentPlayOrder = SourceAudio.GetLastPlayOrder();
	const bool bNeedsStart = !RouteState.bFallbackProxyStarted
		|| RouteState.LastSourcePlayOrder != CurrentPlayOrder;
	if (bNeedsStart)
	{
		if (RouteState.bFallbackProxyStarted)
		{
			ProxyAudio.Stop();
		}

		ProxyAudio.SetVolumeMultiplier(0.0f);
		ProxyAudio.SetPitchMultiplier(SourceAudio.PitchMultiplier);
		ProxyAudio.Play(EstimateFallbackPlaybackStartTime(SourceAudio));
		RouteState.LastSourcePlayOrder = CurrentPlayOrder;
		RouteState.bFallbackProxyStarted = true;
	}

	ProxyAudio.SetPitchMultiplier(SourceAudio.PitchMultiplier);
	ProxyAudio.SetPaused(SourcePlayState == EAudioComponentPlayState::Paused);
}

void UWPPortalAudioSubsystem::UpdateAllRoutes()
{
	check(IsInGameThread());

	FTransform ListenerTransform = FTransform::Identity;
	const bool bHasListener = ResolvePrimaryListener(ListenerTransform);
	const double NowRealSeconds = FPlatformTime::Seconds();

	for (TPair<FWPPortalAudioRouteKey, FWPPortalAudioRouteState>& Entry : RouteStates)
	{
		FWPPortalAudioRouteState& RouteState = Entry.Value;
		if (!bHasListener)
		{
			DestroyRouteProxy(RouteState);
			continue;
		}

		UpdateRoute(RouteState, ListenerTransform.GetLocation(), NowRealSeconds);
	}
}

void UWPPortalAudioSubsystem::UpdateRoute(
	FWPPortalAudioRouteState& RouteState,
	const FVector& ListenerLocation,
	const double NowRealSeconds)
{
	UAudioComponent* SourceAudio = RouteState.SourceAudio.Get();
	USoundSourceBus* SourceBus = RouteState.SourceBus.Get();
	AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();
	const UWPSettings* Settings = GetDefault<UWPSettings>();

	if (!IsValid(SourceAudio)
		|| !IsValid(EntryPortal)
		|| !IsValid(ExitPortal)
		|| !Settings
		|| !SourceAudio->Sound
		|| (RouteState.bUseSourceBus && !IsValid(SourceBus)))
	{
		DestroyRouteProxy(RouteState);
		return;
	}

	FWPTransform Mapping;
	if (!FWPTransform::Build(EntryPortal, ExitPortal, Mapping)
		|| EntryPortal->GetLinkedPortal() != ExitPortal
		|| ExitPortal->GetLinkedPortal() != EntryPortal
		|| !WPPortalAudioPrivate::IsUnitScalePortal(*EntryPortal)
		|| !WPPortalAudioPrivate::IsUnitScalePortal(*ExitPortal)
		|| !FMath::IsNearlyEqual(
			EntryPortal->GetPortalRadius(),
			ExitPortal->GetPortalRadius(),
			WPPortalAudioPrivate::PortalRadiusToleranceCm))
	{
		DestroyRouteProxy(RouteState);
		return;
	}

	const FVector SourceLocation = SourceAudio->GetComponentLocation();
	const FVector VirtualSource = Mapping.MapRayOrigin(SourceLocation);
	const FQuat ProxyRotation = Mapping.MapRot(SourceAudio->GetComponentQuat());

	WPPortalAudioMath::FSphericalPortalPath Path;
	const bool bPathResolved = WPPortalAudioMath::ResolveSphericalPortalPath(
		VirtualSource,
		ListenerLocation,
		ExitPortal->GetActorLocation(),
		ExitPortal->GetPortalRadius(),
		Path);

	if (!bPathResolved)
	{
		DestroyRouteProxy(RouteState);
		return;
	}

	const FVector SourceDirection = Mapping.UnmapDir(Path.SourceDirection).GetSafeNormal();
	const FVector SourceEntrySurfacePoint = SourceLocation
		+ SourceDirection * Path.SourceToEntryDistance;

	// 두 공간에서 계산한 Entry 표면점이 같은 Portal 좌표를 나타내는지 fail-closed로 확인합니다.
	if (SourceDirection.IsNearlyZero()
		|| !Mapping.MapRayOrigin(SourceEntrySurfacePoint).Equals(
			Path.VirtualEntrySurfacePoint,
			WPPortalAudioPrivate::PortalRadiusToleranceCm))
	{
		DestroyRouteProxy(RouteState);
		return;
	}

	const float SourcePickupGain = WPPortalAudioPrivate::ResolveSourcePickupGain(
		*SourceAudio,
		SourceEntrySurfacePoint);
	if (SourcePickupGain <= KINDA_SMALL_NUMBER)
	{
		DestroyRouteProxy(RouteState);
		return;
	}

	USoundBase* ExpectedProxySound = RouteState.bUseSourceBus
		? static_cast<USoundBase*>(SourceBus)
		: SourceAudio->Sound.Get();
	UAudioComponent* ProxyAudio = RouteState.ProxyAudio.Get();
	if (!IsValid(ProxyAudio)
		|| ProxyAudio->Sound.Get() != ExpectedProxySound
		|| RouteState.SourceSoundAtProxyCreation.Get() != SourceAudio->Sound.Get())
	{
		DestroyRouteProxy(RouteState);
		ProxyAudio = CreateProxyAudio(
			*SourceAudio,
			*ExpectedProxySound,
			RouteState.bUseSourceBus,
			Mapping);
		if (!IsValid(ProxyAudio))
		{
			return;
		}

		RouteState.ProxyAudio = ProxyAudio;
		RouteState.SourceSoundAtProxyCreation = SourceAudio->Sound.Get();
	}

	ProxyAudio->SetWorldLocationAndRotation(
		Path.EffectiveProxyLocation,
		ProxyRotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);

	if (RouteState.bUseSourceBus)
	{
		if (!ProxyAudio->IsPlaying())
		{
			ProxyAudio->SetVolumeMultiplier(0.0f);
			ProxyAudio->Play(0.0f);
		}
	}
	else
	{
		SynchronizeFallbackPlayback(RouteState, *SourceAudio, *ProxyAudio);
	}

	UpdateRouteOcclusion(
		RouteState,
		SourceLocation,
		SourceEntrySurfacePoint,
		Path.ExitSurfacePoint,
		ListenerLocation,
		SourceDirection,
		Path.ExitDirection,
		NowRealSeconds);

	const float OcclusionGain = RouteState.bOccluded
		? FMath::Clamp(Settings->PortalAudioOccludedVolumeMultiplier, 0.0f, 1.0f)
		: 1.0f;
	float ModeVolumeMultiplier = FMath::Max(SourceAudio->VolumeMultiplier, 0.0f);
	if (RouteState.bUseSourceBus)
	{
		ModeVolumeMultiplier = 1.0f;
		if (UWorld* World = GetWorld())
		{
			if (FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
			{
				// Post-effect bus에는 source에 적용된 device gain/headroom이 들어 있으므로 proxy에서
				// 한 번 더 적용되는 몫을 상쇄합니다.
				const float ReappliedDeviceGain = AudioDevice->GetPrimaryVolume()
					* AudioDevice->GetPlatformAudioHeadroom();
				ModeVolumeMultiplier = ReappliedDeviceGain > KINDA_SMALL_NUMBER
					? 1.0f / ReappliedDeviceGain
					: 0.0f;
			}
		}
	}

	const float FinalVolume = FMath::Max(Settings->PortalAudioTransmissionGain, 0.0f)
		* SourcePickupGain
		* OcclusionGain;

	ProxyAudio->SetVolumeMultiplier(FinalVolume * ModeVolumeMultiplier);
	ProxyAudio->SetHighPassFilterEnabled(SourceAudio->bEnableHighPassFilter);
	ProxyAudio->SetHighPassFilterFrequency(SourceAudio->HighPassFilterFrequency);

	const bool bEnablePortalLowPass = RouteState.bOccluded && Settings->bEnablePortalAudioOcclusion;
	const bool bEnableLowPass = SourceAudio->bEnableLowPassFilter || bEnablePortalLowPass;
	float LowPassFrequency = SourceAudio->LowPassFilterFrequency;
	if (bEnablePortalLowPass)
	{
		const float OcclusionFrequency = FMath::Clamp(
			Settings->PortalAudioOcclusionLowPassFilterFrequency,
			20.0f,
			20000.0f);
		LowPassFrequency = SourceAudio->bEnableLowPassFilter
			? FMath::Min(SourceAudio->LowPassFilterFrequency, OcclusionFrequency)
			: OcclusionFrequency;
	}
	ProxyAudio->SetLowPassFilterEnabled(bEnableLowPass);
	ProxyAudio->SetLowPassFilterFrequency(LowPassFrequency);
}

void UWPPortalAudioSubsystem::UpdateRouteOcclusion(
	FWPPortalAudioRouteState& RouteState,
	const FVector& SourceLocation,
	const FVector& SourceEntrySurfacePoint,
	const FVector& ExitSurfacePoint,
	const FVector& ListenerLocation,
	const FVector& SourceDirection,
	const FVector& ExitDirection,
	const double NowRealSeconds) const
{
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	UAudioComponent* SourceAudio = RouteState.SourceAudio.Get();
	AWormholePortalActor* EntryPortal = RouteState.EntryPortal.Get();
	AWormholePortalActor* ExitPortal = RouteState.ExitPortal.Get();

	if (!Settings
		|| !Settings->bEnablePortalAudioOcclusion
		|| !IsValid(SourceAudio)
		|| !IsValid(EntryPortal)
		|| !IsValid(ExitPortal))
	{
		RouteState.bOccluded = false;
		RouteState.NextOcclusionCheckRealSeconds = NowRealSeconds;
		return;
	}

	if (NowRealSeconds < RouteState.NextOcclusionCheckRealSeconds)
	{
		return;
	}

	const float Bias = FMath::Max(Settings->PortalAudioOcclusionSurfaceBias, 0.0f);
	FVector SourceTraceStart = FVector::ZeroVector;
	FVector SourceTraceEnd = FVector::ZeroVector;
	FVector ExitTraceStart = FVector::ZeroVector;
	FVector ExitTraceEnd = FVector::ZeroVector;

	const bool bHasSourceLeg = WPPortalAudioPrivate::MakeBiasedSegment(
		SourceLocation,
		SourceEntrySurfacePoint,
		SourceDirection,
		0.0f,
		Bias,
		SourceTraceStart,
		SourceTraceEnd);
	const bool bHasExitLeg = WPPortalAudioPrivate::MakeBiasedSegment(
		ExitSurfacePoint,
		ListenerLocation,
		ExitDirection,
		Bias,
		Bias,
		ExitTraceStart,
		ExitTraceEnd);

	const bool bSourceBlocked = bHasSourceLeg && IsSegmentOccluded(
		SourceTraceStart,
		SourceTraceEnd,
		*SourceAudio,
		*EntryPortal,
		*ExitPortal);
	const bool bExitBlocked = bHasExitLeg && IsSegmentOccluded(
		ExitTraceStart,
		ExitTraceEnd,
		*SourceAudio,
		*EntryPortal,
		*ExitPortal);

	RouteState.bOccluded = bSourceBlocked || bExitBlocked;
	RouteState.NextOcclusionCheckRealSeconds = NowRealSeconds
		+ FMath::Max(Settings->PortalAudioOcclusionCheckInterval, 0.01f);
}

bool UWPPortalAudioSubsystem::ResolvePrimaryListener(FTransform& OutListenerTransform) const
{
	OutListenerTransform = FTransform::Identity;

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	if (FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
	{
		for (int32 ListenerIndex = 0;
			ListenerIndex < WPPortalAudioPrivate::MaximumListenerProbeCount;
			++ListenerIndex)
		{
			uint32 ListenerWorldId = INDEX_NONE;
			if (!AudioDevice->GetListenerWorldID(ListenerIndex, ListenerWorldId))
			{
				break;
			}

			if (ListenerWorldId == World->GetUniqueID()
				&& AudioDevice->GetListenerTransform(ListenerIndex, OutListenerTransform))
			{
				return true;
			}
		}
	}

	if (GEngine)
	{
		if (APlayerController* PlayerController = GEngine->GetFirstLocalPlayerController(World))
		{
			FVector ListenerLocation = FVector::ZeroVector;
			FVector FrontDirection = FVector::ForwardVector;
			FVector RightDirection = FVector::RightVector;
			PlayerController->GetAudioListenerPosition(
				ListenerLocation,
				FrontDirection,
				RightDirection);

			if (!ListenerLocation.ContainsNaN())
			{
				OutListenerTransform.SetLocation(ListenerLocation);
				return true;
			}
		}
	}

	return false;
}

bool UWPPortalAudioSubsystem::IsSegmentOccluded(
	const FVector& Start,
	const FVector& End,
	const UAudioComponent& SourceAudio,
	const AWormholePortalActor& EntryPortal,
	const AWormholePortalActor& ExitPortal) const
{
	UWorld* World = GetWorld();
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	if (!World || !Settings || FVector::DistSquared(Start, End) <= 1.0f)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(
		TEXT("WPPortalAudioOcclusion"),
		SCENE_QUERY_STAT_ONLY(WPPortalAudioOcclusion),
		false);
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(&EntryPortal);
	QueryParams.AddIgnoredActor(&ExitPortal);

	if (const AActor* SourceOwner = SourceAudio.GetOwner())
	{
		QueryParams.AddIgnoredActor(SourceOwner);
	}

	if (const AActor* ProxyHost = ProxyHostActor.Get())
	{
		QueryParams.AddIgnoredActor(ProxyHost);
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PlayerController = It->Get())
		{
			QueryParams.AddIgnoredActor(PlayerController);
			QueryParams.AddIgnoredActor(PlayerController->GetPawn());
			QueryParams.AddIgnoredActor(PlayerController->GetViewTarget());
		}
	}

	ECollisionChannel OcclusionTraceChannel = Settings->PortalAudioOcclusionTraceChannel;
	if (OcclusionTraceChannel == Settings->PortalTraceChannel)
	{
		// Portal trace volumes are routing helpers, not acoustic blockers.
		OcclusionTraceChannel = Settings->PortalTraceChannel != ECC_Visibility
			? ECC_Visibility
			: ECC_Camera;
	}

	return World->LineTraceTestByChannel(
		Start,
		End,
		OcclusionTraceChannel,
		QueryParams,
		FCollisionResponseParams::DefaultResponseParam);
}

void UWPPortalAudioSubsystem::DestroyRouteProxy(FWPPortalAudioRouteState& RouteState)
{
	check(IsInGameThread());

	if (UAudioComponent* ProxyAudio = RouteState.ProxyAudio.Get())
	{
		ProxyAudio->Stop();
		ProxyAudio->DestroyComponent();
	}

	RouteState.ProxyAudio.Reset();
	RouteState.SourceSoundAtProxyCreation.Reset();
	RouteState.NextOcclusionCheckRealSeconds = 0.0;
	RouteState.LastSourcePlayOrder = 0;
	RouteState.bFallbackProxyStarted = false;
	RouteState.bOccluded = false;
}

void UWPPortalAudioSubsystem::DestroyRouteState(FWPPortalAudioRouteState& RouteState)
{
	check(IsInGameThread());

	DestroyRouteProxy(RouteState);
	RouteState.SourceAudio.Reset();
	RouteState.SourceBus.Reset();
	RouteState.bUseSourceBus = false;
	RouteState.EntryPortal.Reset();
	RouteState.ExitPortal.Reset();
}

void UWPPortalAudioSubsystem::DestroyAllRouteStates()
{
	check(IsInGameThread());

	for (TPair<FWPPortalAudioRouteKey, FWPPortalAudioRouteState>& Entry : RouteStates)
	{
		DestroyRouteState(Entry.Value);
	}
	RouteStates.Reset();
}

void UWPPortalAudioSubsystem::HandleActorSpawned(AActor* SpawnedActor)
{
	check(IsInGameThread());

	if (bDeinitializing || !IsValid(SpawnedActor) || SpawnedActor->GetWorld() != GetWorld())
	{
		return;
	}

	if (SpawnedActor->ActorHasTag(WPTransitTags::Generated))
	{
		// Transit은 FinishSpawning 전에 Generated tag를 붙입니다. 기본 spawn delegate는
		// component 등록이 끝난 뒤 호출되므로 auto-activate된 twin audio를 즉시 정지합니다.
		DiscoverAudioComponentsOnActor(SpawnedActor);
		return;
	}

	// Construction/registration 중간 상태를 피하기 위해 실제 component 검색은 PostActorTick에 수행합니다.
	PendingSpawnedActors.AddUnique(SpawnedActor);
}

void UWPPortalAudioSubsystem::HandleWorldPostActorTick(
	UWorld* TickedWorld,
	ELevelTick TickType,
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
	const UWPSettings* Settings = GetDefault<UWPSettings>();
	const float ReconcileInterval = Settings
		? FMath::Max(Settings->PortalAudioSourceReconcileInterval, 0.05f)
		: 0.25f;

	if (NowSeconds >= NextFullSourceReconcileRealSeconds)
	{
		DiscoverAllAudioComponents();
		NextFullSourceReconcileRealSeconds = NowSeconds + ReconcileInterval;
	}

	CompactTrackedAudioComponents();

	if (bPairTopologyDirty)
	{
		RefreshPairTopology();
	}

	ReconcileAllRoutes();
	UpdateAllRoutes();
}

void UWPPortalAudioSubsystem::HandlePortalPairAdded(const FWPPortalPairSnapshot& PairSnapshot)
{
	check(IsInGameThread());
	(void)PairSnapshot;
	bPairTopologyDirty = true;
}

void UWPPortalAudioSubsystem::HandlePortalPairRemoved(const FWPPortalPairSnapshot& PairSnapshot)
{
	check(IsInGameThread());
	(void)PairSnapshot;
	// Removed snapshot의 endpoint는 이미 파괴됐을 수 있으므로 다음 tick에 PairId snapshot 전체를 다시 읽습니다.
	bPairTopologyDirty = true;
}
