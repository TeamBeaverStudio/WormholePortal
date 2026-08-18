// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WPRendererService.h"

#include "Engine/Texture.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Passes/WPCubeAAPass.h"
#include "Rendering/WPSelectedCubeCapture.h"
#include "WPRenderState.h"
#include "WPSceneViewExtension.h"
#include "WPLog.h"
#include "WormholePortalStats.h"

namespace
{
	FString PairIdToString(const FGuid& PairId)
	{
		return PairId.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	template <typename TextureType>
	FTextureReferenceRHIRef GetStableTextureReference(const TWeakObjectPtr<TextureType>& Texture)
	{
		TextureType* TextureObject = Texture.Get();
		if (!IsValid(TextureObject) || !TextureObject->GetResource())
		{
			return FTextureReferenceRHIRef();
		}
		if (!TextureObject->TextureReference.TextureReferenceRHI.IsValid())
		{
			return FTextureReferenceRHIRef();
		}
		return TextureObject->TextureReference.TextureReferenceRHI;
	}

	struct FWPCubeAAPreflight
	{
		FTextureRHIRef InputTexture;
		FTextureRHIRef OutputTexture;
		FTextureReferenceRHIRef PublishedReference;
	};

	bool ResolveWPCubeAAPreflight(
		UTextureRenderTargetCube& InputCubeTarget,
		UTextureRenderTargetCube& OutputCubeTarget,
		UTextureRenderTargetCube& PublishedReferenceOwner,
		const bool bDirectPublish,
		FWPCubeAAPreflight& OutPreflight,
		const TCHAR*& OutFailureReason)
	{
		OutPreflight = FWPCubeAAPreflight();
		OutFailureReason = TEXT("Unknown");
		const bool bObjectTopologyValid = bDirectPublish
			? &InputCubeTarget != &OutputCubeTarget
				&& (&PublishedReferenceOwner == &InputCubeTarget
					|| &PublishedReferenceOwner == &OutputCubeTarget)
			: &InputCubeTarget == &OutputCubeTarget
				&& &PublishedReferenceOwner == &InputCubeTarget;
		if (!bObjectTopologyValid)
		{
			OutFailureReason = TEXT("ObjectTopologyMismatch");
			return false;
		}

		FTextureRenderTargetResource* InputResource =
			InputCubeTarget.GameThread_GetRenderTargetResource();
		FTextureRenderTargetResource* OutputResource =
			OutputCubeTarget.GameThread_GetRenderTargetResource();
		if (!InputResource || !OutputResource)
		{
			OutFailureReason = TEXT("MissingRenderTargetResource");
			return false;
		}

		OutPreflight.InputTexture = InputResource->GetShaderResourceTexture();
		OutPreflight.OutputTexture = OutputResource->GetShaderResourceTexture();
		OutPreflight.PublishedReference =
			PublishedReferenceOwner.TextureReference.TextureReferenceRHI;
		if (!OutPreflight.InputTexture.IsValid()
			|| !OutPreflight.OutputTexture.IsValid()
			|| !OutPreflight.PublishedReference.IsValid()
			|| (bDirectPublish
				&& OutPreflight.InputTexture == OutPreflight.OutputTexture))
		{
			OutFailureReason = TEXT("InvalidRHITextureOrReference");
			return false;
		}

		const FRHITextureDesc& InputDesc = OutPreflight.InputTexture->GetDesc();
		const FRHITextureDesc& OutputDesc = OutPreflight.OutputTexture->GetDesc();
		const bool bInputValid =
			InputDesc.Dimension == ETextureDimension::TextureCube
			&& InputDesc.Extent.X > 1
			&& InputDesc.Extent.X == InputDesc.Extent.Y
			&& InputDesc.ArraySize == 1
			&& InputDesc.NumMips == 1
			&& InputDesc.NumSamples == 1
			&& InputDesc.Format == PF_FloatRGBA;
		const bool bOutputValid =
			OutputDesc.Dimension == ETextureDimension::TextureCube
			&& OutputDesc.Extent == InputDesc.Extent
			&& OutputDesc.ArraySize == InputDesc.ArraySize
			&& OutputDesc.NumMips == InputDesc.NumMips
			&& OutputDesc.NumSamples == InputDesc.NumSamples
			&& OutputDesc.Format == InputDesc.Format
			&& (!bDirectPublish
				|| EnumHasAnyFlags(OutputDesc.Flags, ETextureCreateFlags::UAV));
		if (!bInputValid || !bOutputValid)
		{
			OutFailureReason = TEXT("ResourceContractMismatch");
			return false;
		}

		FGlobalShaderMap* ShaderMap =
			GetGlobalShaderMap(GMaxRHIFeatureLevel);
		if (!ShaderMap
			|| !ShaderMap->HasShader(
				&FWPCubeAACS::GetStaticType(),
				0))
		{
			OutFailureReason = TEXT("CubeAAShaderUnavailable");
			return false;
		}

		OutFailureReason = TEXT("None");
		return true;
	}

	const TCHAR* GetWorldTypeName(const EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::None: return TEXT("None");
		case EWorldType::Game: return TEXT("Game");
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		case EWorldType::GamePreview: return TEXT("GamePreview");
		case EWorldType::GameRPC: return TEXT("GameRPC");
		case EWorldType::Inactive: return TEXT("Inactive");
		default: return TEXT("Unknown");
		}
	}

	bool IsOwnershipEntryReadyForRendering(
		const FWPGameThreadOwnershipEntry& Entry)
	{
		return Entry.Ownership.IsReadyForRendering(Entry.PacketSequence);
	}

	FWPGameThreadOwnershipEntry MakeGameThreadOwnershipEntry(
		const FWPRenderPacket& Packet)
	{
		FWPGameThreadOwnershipEntry Result;
		Result.Ownership.PairId = Packet.PairId;
		Result.Ownership.StableSelectorNameA = Packet.StableSelectorNameA;
		Result.Ownership.StableSelectorNameB = Packet.StableSelectorNameB;
		Result.Ownership.RequestedOwnership = Packet.RequestedOwnership;
		Result.Ownership.EffectiveOwnership = Packet.EffectiveOwnership;
		Result.Ownership.OwnershipEpoch = Packet.OwnershipEpoch;
		Result.Ownership.bEndpointAReady = Packet.bOwnershipEndpointAReady;
		Result.Ownership.bEndpointBReady = Packet.bOwnershipEndpointBReady;
		Result.Ownership.bOwnershipInputsReady = Packet.bOwnershipInputsReady;
		Result.PacketSequence = Packet.PacketSequence;
		Result.bVisibilityObservationReady = Packet.bEnabled
			&& Packet.bCaptureVisibilityFeedbackEnabled
			&& Packet.bHasReferenceView
			&& Packet.PairId.IsValid()
			&& Packet.OwnershipEpoch != 0
			&& Packet.PacketSequence != 0
			&& Packet.ReferenceViewActorId != 0
			&& !Packet.PortalACenterWorld.ContainsNaN()
			&& !Packet.PortalBCenterWorld.ContainsNaN()
			&& Packet.MetricA.IsFiniteAndValid()
			&& Packet.MetricB.IsFiniteAndValid();
		return Result;
	}
}

FWPRendererService::FWPRendererService()
	: ServiceId(FMath::Max<uint64>(FPlatformTime::Cycles64(), 1))
{
}

FWPRendererService::~FWPRendererService()
{
	if (!bShutdown)
	{
		Shutdown();
	}
}

FWPRenderHandle FWPRendererService::RegisterPair(UWorld& World, const FGuid& PairId)
{
	check(IsInGameThread());
	// 로그 전용: Pair 등록 전체 구간의 CPU 시간을 기록합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	FWPRenderHandle Handle;
	const bool bDedicatedServer = World.GetNetMode() == NM_DedicatedServer;
	const bool bCanEverRender = FApp::CanEverRender();
	if (bShutdown || !PairId.IsValid() || bDedicatedServer || !bCanEverRender)
	{
		WP_LOG(&World, Error,
			TEXT("[RendererService] RegisterPair rejected. World=%s WorldType=%s PairId=%s Shutdown=%d PairIdValid=%d DedicatedServer=%d CanEverRender=%d Reason=%s CpuMs=%.3f"),
			*World.GetName(), GetWorldTypeName(World.WorldType), *PairIdToString(PairId),
			bShutdown ? 1 : 0, PairId.IsValid() ? 1 : 0, bDedicatedServer ? 1 : 0,
			bCanEverRender ? 1 : 0,
			bShutdown ? TEXT("ServiceShutdown")
				: (!PairId.IsValid() ? TEXT("InvalidPairId")
					: (bDedicatedServer ? TEXT("DedicatedServer") : TEXT("ProcessCannotRender"))),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return Handle;
	}

	const uint32 WorldId = World.GetUniqueID();
	FWorldEntry* WorldEntry = Worlds.Find(WorldId);
	if (!WorldEntry)
	{
		FWorldEntry& NewWorldEntry = Worlds.Add(WorldId);
		NewWorldEntry.World = &World;
		NewWorldEntry.RenderState = MakeShared<FWPRenderState, ESPMode::ThreadSafe>(
			World.GetName(), GetWorldTypeName(World.WorldType), World.WorldType == EWorldType::PIE);
		NewWorldEntry.ViewExtension = FSceneViewExtensions::NewExtension<FWPSceneViewExtension>(
			&World, NewWorldEntry.RenderState.ToSharedRef());
		WorldEntry = &NewWorldEntry;
#if !UE_BUILD_SHIPPING
		WP_LOG(&World, Verbose,
			TEXT("[RendererService] World SceneViewExtension created. World=%s WorldType=%s WorldId=%u ServiceId=%llu CpuMs=%.3f"),
			*World.GetName(), GetWorldTypeName(World.WorldType), WorldId, ServiceId,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}

	while (NextHandleValue == 0 || Pairs.Contains(NextHandleValue))
	{
		++NextHandleValue;
	}
	Handle.ServiceId = ServiceId;
	Handle.Value = NextHandleValue++;

	FPairEntry& PairEntry = Pairs.Add(Handle.Value);
	PairEntry.PairId = PairId;
	PairEntry.WorldId = WorldId;
	PairEntry.RenderState = WorldEntry->RenderState;
	PairEntry.OwnershipFeedbackState =
		MakeShared<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe>();
	WorldEntry->PairHandles.Add(Handle.Value);
	WorldEntry->RenderState->RegisteredPairCount.Store(
		WorldEntry->RenderState->RegisteredPairCount.Load() + 1);

#if !UE_BUILD_SHIPPING
	WP_LOG(&World, Verbose,
		TEXT("[RendererService] Pair registered. World=%s PairId=%s Handle=%llu ServiceId=%llu WorldPairCount=%d TotalPairCount=%d OwnershipFeedbackAllocated=%d ActiveOwnershipPairs=%d ActiveVisibilityObservationPairs=%d CpuMs=%.3f"),
		*World.GetName(), *PairIdToString(PairId), Handle.Value, Handle.ServiceId,
		WorldEntry->PairHandles.Num(), Pairs.Num(), PairEntry.OwnershipFeedbackState.IsValid() ? 1 : 0,
		WorldEntry->RenderState->ActiveOwnershipPairCount.Load(),
		WorldEntry->RenderState->ActiveVisibilityObservationPairCount.Load(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return Handle;
}

TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> FWPRendererService::FindRenderStateForWorld(UWorld* World) const
{
	check(IsInGameThread());
	if (bShutdown || !IsValid(World)) return nullptr;
	const FWorldEntry* WorldEntry = Worlds.Find(World->GetUniqueID());
	return WorldEntry && WorldEntry->World.Get() == World ? WorldEntry->RenderState : nullptr;
}

bool FWPRendererService::UpdatePair(
	const FWPRenderHandle& Handle, const FWPRenderPacket& Packet)
{
	SCOPE_CYCLE_COUNTER(STAT_WP_RenderPacketPublish);
	check(IsInGameThread());
#if !UE_BUILD_SHIPPING
	// 로그 전용: Pair update enqueue 전체 구간의 CPU 시간을 기록합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (bShutdown || !IsHandleOwned(Handle))
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(nullptr, Verbose,
			TEXT("[GameThread][RendererService] UpdatePair rejected. PairId=%s Handle=%llu HandleServiceId=%llu CurrentServiceId=%llu Sequence=%llu Reason=UnknownOrStaleHandle CpuMs=%.3f"),
			*PairIdToString(Packet.PairId), Handle.Value, Handle.ServiceId, ServiceId,
			Packet.PacketSequence, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	FPairEntry& PairEntry = Pairs.FindChecked(Handle.Value);
	if (PairEntry.PairId != Packet.PairId || Packet.PacketSequence <= PairEntry.LastSubmittedSequence)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(nullptr, Verbose,
			TEXT("[GameThread][RendererService] UpdatePair rejected. PairId=%s RegisteredPairId=%s Handle=%llu Sequence=%llu LastSequence=%llu Reason=%s CpuMs=%.3f"),
			*PairIdToString(Packet.PairId), *PairIdToString(PairEntry.PairId), Handle.Value,
			Packet.PacketSequence, PairEntry.LastSubmittedSequence,
			PairEntry.PairId != Packet.PairId ? TEXT("PairIdMismatch") : TEXT("StaleSequence"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

#if !UE_BUILD_SHIPPING
	// 로그 전용: GT ownership snapshot 갱신 비용을 분리 측정합니다.
	const double OwnershipBridgeStartSeconds = FPlatformTime::Seconds();
#endif
	TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState = PairEntry.RenderState;
	check(RenderState.IsValid());
	check(PairEntry.OwnershipFeedbackState.IsValid());

	const FWPGameThreadOwnershipEntry* PreviousOwnershipEntry =
		RenderState->OwnershipSnapshotsGameThread.Find(Handle.Value);
	const bool bWasActiveOwnership = PreviousOwnershipEntry
		&& IsOwnershipEntryReadyForRendering(*PreviousOwnershipEntry);
	const bool bWasActiveVisibilityObservation = PreviousOwnershipEntry
		&& PreviousOwnershipEntry->bVisibilityObservationReady;
#if !UE_BUILD_SHIPPING
	const bool bOwnershipStateChanged = !PreviousOwnershipEntry
		|| PreviousOwnershipEntry->Ownership.StableSelectorNameA != Packet.StableSelectorNameA
		|| PreviousOwnershipEntry->Ownership.StableSelectorNameB != Packet.StableSelectorNameB
		|| PreviousOwnershipEntry->Ownership.RequestedOwnership != Packet.RequestedOwnership
		|| PreviousOwnershipEntry->Ownership.EffectiveOwnership != Packet.EffectiveOwnership
		|| PreviousOwnershipEntry->Ownership.OwnershipEpoch != Packet.OwnershipEpoch
		|| PreviousOwnershipEntry->Ownership.bEndpointAReady != Packet.bOwnershipEndpointAReady
		|| PreviousOwnershipEntry->Ownership.bEndpointBReady != Packet.bOwnershipEndpointBReady
		|| PreviousOwnershipEntry->Ownership.bOwnershipInputsReady != Packet.bOwnershipInputsReady;
#endif

	FWPGameThreadOwnershipEntry OwnershipEntry =
		MakeGameThreadOwnershipEntry(Packet);
	const bool bIsActiveOwnership = IsOwnershipEntryReadyForRendering(OwnershipEntry);
	const bool bIsActiveVisibilityObservation =
		OwnershipEntry.bVisibilityObservationReady;
	RenderState->OwnershipSnapshotsGameThread.Add(Handle.Value, MoveTemp(OwnershipEntry));
	if (bWasActiveOwnership != bIsActiveOwnership)
	{
		const int32 ActiveOwnershipPairCount = FMath::Max(
			RenderState->ActiveOwnershipPairCount.Load() + (bIsActiveOwnership ? 1 : -1), 0);
		RenderState->ActiveOwnershipPairCount.Store(ActiveOwnershipPairCount);
	}
	if (bWasActiveVisibilityObservation != bIsActiveVisibilityObservation)
	{
		const int32 ActiveVisibilityObservationPairCount = FMath::Max(
			RenderState->ActiveVisibilityObservationPairCount.Load()
				+ (bIsActiveVisibilityObservation ? 1 : -1),
			0);
		RenderState->ActiveVisibilityObservationPairCount.Store(
			ActiveVisibilityObservationPairCount);
	}
#if !UE_BUILD_SHIPPING
	const double OwnershipBridgeCpuMs =
		(FPlatformTime::Seconds() - OwnershipBridgeStartSeconds) * 1000.0;
#endif

	PairEntry.LastSubmittedSequence = Packet.PacketSequence;
	FWPRenderThreadPacket RenderThreadPacket = MakeRenderThreadPacket(
		Packet, PairEntry.OwnershipFeedbackState.ToSharedRef());
#if !UE_BUILD_SHIPPING
	// 로그 전용: GT enqueue부터 RT apply까지의 queue latency를 측정합니다.
	RenderThreadPacket.QueueSubmitSeconds = FPlatformTime::Seconds();
#endif
	const uint64 HandleValue = Handle.Value;

	ENQUEUE_RENDER_COMMAND(WPUpdatePair)(
		[RenderState, HandleValue, RenderThreadPacket = MoveTemp(RenderThreadPacket)](FRHICommandListImmediate& RHICmdList) mutable
		{
			SCOPE_CYCLE_COUNTER(STAT_WP_RenderPacketApply);
			(void)RHICmdList;
#if !UE_BUILD_SHIPPING
			// 로그 전용: queued RT command 처리 시간을 기록합니다.
			const double CommandStartSeconds = FPlatformTime::Seconds();
#endif
			if (!RenderState.IsValid() || RenderState->bShuttingDown.Load())
			{
				return;
			}

			if (const FWPRenderThreadPacket* Existing = RenderState->PairsRenderThread.Find(HandleValue))
			{
				if (RenderThreadPacket.PacketSequence <= Existing->PacketSequence)
				{
					INC_DWORD_STAT(STAT_WP_RenderPacketsDropped);
#if !UE_BUILD_SHIPPING
					WP_LOG(nullptr, VeryVerbose,
						TEXT("[RenderThread][RendererService][ProductionBridge] Ownership packet dropped. World=%s PairId=%s Handle=%llu Sequence=%llu ExistingSequence=%llu OwnershipEpoch=%llu Reason=StaleSequence CommandCpuMs=%.4f"),
						*RenderState->WorldName, *PairIdToString(RenderThreadPacket.PairId),
						HandleValue, RenderThreadPacket.PacketSequence, Existing->PacketSequence,
						RenderThreadPacket.OwnershipEpoch,
						(FPlatformTime::Seconds() - CommandStartSeconds) * 1000.0);
#endif
					return;
				}
			}

#if !UE_BUILD_SHIPPING
			// 로그 전용: 개발용 pair/view 결과에 queue latency를 제공합니다.
			RenderThreadPacket.QueueLatencyMs =
				(FPlatformTime::Seconds() - RenderThreadPacket.QueueSubmitSeconds) * 1000.0;
#endif
			RenderState->PairsRenderThread.Add(HandleValue, MoveTemp(RenderThreadPacket));
			INC_DWORD_STAT(STAT_WP_RenderPacketsApplied);
		});
	INC_DWORD_STAT(STAT_WP_RenderPacketsPublished);

#if !UE_BUILD_SHIPPING
	if (bOwnershipStateChanged)
	{
		WP_LOG(nullptr, Verbose,
			TEXT("[GameThread][RendererService][ProductionBridge] Ownership state changed. World=%s PairId=%s Handle=%llu Sequence=%llu RequestedMode=%s EffectiveMode=%s OwnershipEpoch=%llu RenderingReady=%d ActiveWarmupOrProductionPairs=%d SnapshotCount=%d CpuMs=%.4f ActiveDefinition=ReadyWarmupOrProduction"),
			*RenderState->WorldName, *PairIdToString(Packet.PairId), Handle.Value,
			Packet.PacketSequence, GetWPPairOwnershipModeName(Packet.RequestedOwnership),
			GetWPPairOwnershipModeName(Packet.EffectiveOwnership), Packet.OwnershipEpoch,
			bIsActiveOwnership ? 1 : 0,
			RenderState->ActiveOwnershipPairCount.Load(),
			RenderState->OwnershipSnapshotsGameThread.Num(), OwnershipBridgeCpuMs);
	}
#endif

#if !UE_BUILD_SHIPPING
	if (bWasActiveVisibilityObservation != bIsActiveVisibilityObservation)
	{
		WP_LOG(nullptr, VeryVerbose,
			TEXT("[GameThread][RendererService][VisibilityObservation] State changed. World=%s PairId=%s Handle=%llu Sequence=%llu OwnershipEpoch=%llu ObservationReady=%d PreviousObservationReady=%d ActiveVisibilityObservationPairs=%d ActiveOwnershipPairs=%d FeedbackEnabled=%d ReferenceViewActorId=%u MetricAValid=%d MetricBValid=%d CubeResourcesRequired=0 LUTResourcesRequired=0 CaptureGenerationRequired=0 CpuMs=%.4f"),
			*RenderState->WorldName, *PairIdToString(Packet.PairId), Handle.Value,
			Packet.PacketSequence, Packet.OwnershipEpoch,
			bIsActiveVisibilityObservation ? 1 : 0,
			bWasActiveVisibilityObservation ? 1 : 0,
			RenderState->ActiveVisibilityObservationPairCount.Load(),
			RenderState->ActiveOwnershipPairCount.Load(),
			Packet.bCaptureVisibilityFeedbackEnabled ? 1 : 0,
			Packet.ReferenceViewActorId,
			Packet.MetricA.IsFiniteAndValid() ? 1 : 0,
			Packet.MetricB.IsFiniteAndValid() ? 1 : 0,
			OwnershipBridgeCpuMs);
	}
#endif

	return true;
}

bool FWPRendererService::QueryPairOwnershipFeedback(
	const FWPRenderHandle& Handle,
	FWPPairOwnershipFeedback& OutFeedback) const
{
	return QueryPairOwnershipFeedback(Handle, OutFeedback, false);
}

bool FWPRendererService::QueryPairOwnershipFeedback(
	const FWPRenderHandle& Handle,
	FWPPairOwnershipFeedback& OutFeedback,
	const bool bIncludeVisibility) const
{
	check(IsInGameThread());
	// 로그 전용: ownership feedback query의 CPU 시간을 기록합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	OutFeedback = FWPPairOwnershipFeedback{};

	if (bShutdown || !IsHandleOwned(Handle))
	{
#if !UE_BUILD_SHIPPING
		// 로그 전용: rejected query 원인을 안정된 문자열로 출력하기 위한 snapshot입니다.
		const TCHAR* Reason = bShutdown
			? TEXT("ServiceShutdown")
			: (!Handle.IsValid()
				? TEXT("InvalidHandle")
				: (Handle.ServiceId != ServiceId ? TEXT("StaleServiceId") : TEXT("UnknownHandle")));
		WP_LOG(nullptr, Verbose,
			TEXT("[GameThread][RendererService][ProductionBridge] Ownership feedback query rejected. Handle=%llu HandleServiceId=%llu CurrentServiceId=%llu IncludeVisibility=%d VisibilityMailboxFieldsRead=0 Reason=%s CpuMs=%.4f"),
			Handle.Value, Handle.ServiceId, ServiceId,
			bIncludeVisibility ? 1 : 0, Reason,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	const FPairEntry& PairEntry = Pairs.FindChecked(Handle.Value);
	if (!PairEntry.OwnershipFeedbackState.IsValid())
	{
		WP_LOG(nullptr, Warning,
			TEXT("[GameThread][RendererService][ProductionBridge] Ownership feedback query failed. PairId=%s Handle=%llu IncludeVisibility=%d VisibilityMailboxFieldsRead=0 Reason=MissingFeedbackState CpuMs=%.4f"),
			*PairIdToString(PairEntry.PairId), Handle.Value,
			bIncludeVisibility ? 1 : 0,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}

	OutFeedback = PairEntry.OwnershipFeedbackState->ReadGameThread(
		bIncludeVisibility);
	if (!OutFeedback.bSnapshotCoherent)
	{
		const FWPGameThreadOwnershipEntry* OwnershipEntry =
			PairEntry.RenderState.IsValid()
				? PairEntry.RenderState->OwnershipSnapshotsGameThread.Find(Handle.Value)
				: nullptr;
		const bool bSyntheticFailure = OwnershipEntry
			&& OwnershipEntry->Ownership.EffectiveOwnership
				== EWPPairOwnershipMode::Production
			&& OwnershipEntry->Ownership.OwnershipEpoch != 0;
		if (bSyntheticFailure)
		{
			OutFeedback.ProductionFailedEpoch =
				OwnershipEntry->Ownership.OwnershipEpoch;
			OutFeedback.ProductionFailedPacketSequence =
				OwnershipEntry->PacketSequence;
		}
		constexpr double IncoherentFeedbackWarningIntervalSeconds = 1.0;
		const double FeedbackNowSeconds = FPlatformTime::Seconds();
		// 로그 전용: incoherent feedback warning에 누적 read 횟수를 출력합니다.
		++PairEntry.IncoherentFeedbackReadCount;
		const bool bShouldLogIncoherentFeedback =
			!PairEntry.bHasLoggedIncoherentFeedback
			|| PairEntry.LastIncoherentFeedbackLoggedVersion != OutFeedback.SnapshotVersion
			|| FeedbackNowSeconds - PairEntry.LastIncoherentFeedbackLogSeconds
				>= IncoherentFeedbackWarningIntervalSeconds;
		if (bShouldLogIncoherentFeedback)
		{
			PairEntry.bHasLoggedIncoherentFeedback = true;
			PairEntry.LastIncoherentFeedbackLoggedVersion = OutFeedback.SnapshotVersion;
			PairEntry.LastIncoherentFeedbackLogSeconds = FeedbackNowSeconds;
			WP_LOG(nullptr, Warning,
				TEXT("[GameThread][RendererService][ProductionBridge] Ownership feedback snapshot remained incoherent after bounded retries. World=%s PairId=%s Handle=%llu IncludeVisibility=%d VisibilityMailboxFieldsRead=%d SnapshotVersion=%llu SnapshotReadRetries=%u IncoherentReadCount=%llu EffectiveOwnership=%s OwnershipEpoch=%llu PacketSequence=%llu FailClosedAction=%s WarningRateLimitSeconds=%.1f CpuMs=%.4f"),
				PairEntry.RenderState.IsValid() ? *PairEntry.RenderState->WorldName : TEXT("<missing>"),
				*PairIdToString(PairEntry.PairId), Handle.Value,
				bIncludeVisibility ? 1 : 0,
				bIncludeVisibility ? 1 : 0,
				OutFeedback.SnapshotVersion, OutFeedback.SnapshotReadRetryCount,
				PairEntry.IncoherentFeedbackReadCount,
				OwnershipEntry
					? GetWPPairOwnershipModeName(OwnershipEntry->Ownership.EffectiveOwnership)
					: TEXT("Unknown"),
				OwnershipEntry ? OwnershipEntry->Ownership.OwnershipEpoch : 0,
				OwnershipEntry ? OwnershipEntry->PacketSequence : 0,
				bSyntheticFailure
					? TEXT("SyntheticSameEpochFailureRollback")
					: TEXT("HoldDisabledOrWarmupNoCommit"),
				IncoherentFeedbackWarningIntervalSeconds,
				(FeedbackNowSeconds - StartSeconds) * 1000.0);
		}
		return bSyntheticFailure;
	}
	const bool bFeedbackAvailable = OutFeedback.WarmupSucceededEpoch != 0
		|| OutFeedback.ProductionFailedEpoch != 0
		|| OutFeedback.WarmupPassCount != 0
		|| OutFeedback.ProductionFailureCount != 0
		|| (bIncludeVisibility && OutFeedback.VisibilitySampleSequence != 0);
	return bFeedbackAvailable;
}

bool FWPRendererService::CanEnqueueCubeAAPass(
	UTextureRenderTargetCube& InputCubeTarget,
	UTextureRenderTargetCube& OutputCubeTarget,
	UTextureRenderTargetCube& PublishedReferenceOwner,
	const bool bDirectPublish) const
{
	check(IsInGameThread());
	const double StartSeconds = FPlatformTime::Seconds();
	if (bShutdown)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(&InputCubeTarget, Verbose,
			TEXT("[GameThread][CubeAA][Preflight] Rejected. Input=%s Output=%s PublishedReferenceOwner=%s Mode=%s Reason=RendererShutdown CpuMs=%.4f"),
			*InputCubeTarget.GetName(), *OutputCubeTarget.GetName(),
			*PublishedReferenceOwner.GetName(),
			bDirectPublish ? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	FWPCubeAAPreflight Preflight;
	const TCHAR* FailureReason = TEXT("Unknown");
	if (!ResolveWPCubeAAPreflight(
		InputCubeTarget,
		OutputCubeTarget,
		PublishedReferenceOwner,
		bDirectPublish,
		Preflight,
		FailureReason))
	{
		WP_LOG(&InputCubeTarget, Error,
			TEXT("[GameThread][CubeAA][Preflight] Rejected. Input=%s Output=%s PublishedReferenceOwner=%s Mode=%s InputObject=%p OutputObject=%p PublishedObject=%p InputTextureValid=%d OutputTextureValid=%d PublishedReferenceValid=%d InputOutputDistinct=%d PublishedOwnerInPhysicalSet=%d Reason=%s CpuMs=%.4f"),
			*InputCubeTarget.GetName(), *OutputCubeTarget.GetName(),
			*PublishedReferenceOwner.GetName(),
			bDirectPublish ? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			&InputCubeTarget, &OutputCubeTarget, &PublishedReferenceOwner,
			Preflight.InputTexture.IsValid() ? 1 : 0,
			Preflight.OutputTexture.IsValid() ? 1 : 0,
			Preflight.PublishedReference.IsValid() ? 1 : 0,
			&InputCubeTarget != &OutputCubeTarget ? 1 : 0,
			(&PublishedReferenceOwner == &InputCubeTarget
				|| &PublishedReferenceOwner == &OutputCubeTarget) ? 1 : 0,
			FailureReason,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return false;
	}
	return true;
}

bool FWPRendererService::EnqueueCubeAAPass(
	UTextureRenderTargetCube& InputCubeTarget,
	UTextureRenderTargetCube& OutputCubeTarget,
	UTextureRenderTargetCube& PublishedReferenceOwner,
	const bool bDirectPublish,
	const uint8 FaceMask)
{
	check(IsInGameThread());
#if !UE_BUILD_SHIPPING
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (bShutdown)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(&InputCubeTarget, Verbose,
			TEXT("[GameThread][CubeAA] Enqueue rejected after caller preflight. Input=%s Output=%s PublishedReferenceOwner=%s Mode=%s Reason=RendererShutdown CpuMs=%.4f"),
			*InputCubeTarget.GetName(), *OutputCubeTarget.GetName(),
			*PublishedReferenceOwner.GetName(),
			bDirectPublish ? TEXT("DirectPublish") : TEXT("LegacyCopyValidation"),
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return false;
	}

	FWPCubeAAPreflight Preflight;
	const TCHAR* FailureReason = TEXT("Unknown");
	const bool bResolved = ResolveWPCubeAAPreflight(
		InputCubeTarget,
		OutputCubeTarget,
		PublishedReferenceOwner,
		bDirectPublish,
		Preflight,
		FailureReason);
	if (!ensureAlwaysMsgf(
		bResolved,
		TEXT("CubeAA resource contract changed after caller preflight. Reason=%s"),
		FailureReason))
	{
		return false;
	}

	ENQUEUE_RENDER_COMMAND(WPCubeAA)(
		[InputCubeTexture = Preflight.InputTexture,
			OutputCubeTexture = Preflight.OutputTexture,
			PublishedReference = Preflight.PublishedReference,
			bDirectPublish,
			FaceMask](
			FRHICommandListImmediate& RHICmdList)
		{
			SCOPE_CYCLE_COUNTER(STAT_WP_CubeAASetup);
			FRDGBuilder GraphBuilder(
				RHICmdList,
				RDG_EVENT_NAME(
					"WP.CubeAA.%s",
					bDirectPublish
						? "DirectPublish" : "LegacyCopyValidation"));
			if (!AddWPCubeAAPass(
				GraphBuilder,
				InputCubeTexture.GetReference(),
				OutputCubeTexture.GetReference(),
				bDirectPublish,
				FaceMask))
			{
				ensureAlwaysMsgf(
					false,
					TEXT("CubeAA Render Thread pass rejected after successful Game Thread ")
					TEXT("preflight. Two-buffer direct publish has no filtered fallback."));
				return;
			}
			GraphBuilder.Execute();
			if (bDirectPublish)
			{
				RHICmdList.UpdateTextureReference(
					PublishedReference.GetReference(),
					OutputCubeTexture.GetReference());
			}
		});
#if !UE_BUILD_SHIPPING
	WP_LOG(&InputCubeTarget, VeryVerbose,
		TEXT("[GameThread][CubeAA] Enqueued. Input=%s Output=%s FaceMask=0x%02x DirectPublish=%d GameThreadWait=0 CpuMs=%.4f"),
		*InputCubeTarget.GetName(), *OutputCubeTarget.GetName(), static_cast<uint32>(FaceMask & 0x3f),
		bDirectPublish ? 1 : 0, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	return true;
}

bool FWPRendererService::AddSelectedCubeCaptureRenderer(
	USceneCaptureComponentCube& CaptureComponent, const uint8 SelectedFaceMask, ISceneRenderBuilder& SceneRenderBuilder)
{
	return AddWPSelectedCubeCaptureRenderer(CaptureComponent, SelectedFaceMask, SceneRenderBuilder);
}

void FWPRendererService::UnregisterPair(const FWPRenderHandle& Handle)
{
	check(IsInGameThread());
#if !UE_BUILD_SHIPPING
	// 로그 전용: Pair 해제 전체 구간의 CPU 시간을 기록합니다.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	if (!IsHandleOwned(Handle))
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(nullptr, Verbose,
			TEXT("[GameThread][RendererService] UnregisterPair ignored. Handle=%llu HandleServiceId=%llu CurrentServiceId=%llu Reason=UnknownOrStaleHandle CpuMs=%.3f"),
			Handle.Value, Handle.ServiceId, ServiceId,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		return;
	}

	const FPairEntry PairEntry = Pairs.FindChecked(Handle.Value);
	Pairs.Remove(Handle.Value);
	FWorldEntry* WorldEntry = Worlds.Find(PairEntry.WorldId);
	TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState = PairEntry.RenderState;
#if !UE_BUILD_SHIPPING
	// 로그 전용: GT ownership snapshot 제거 비용을 분리 측정합니다.
	const double OwnershipRemovalStartSeconds = FPlatformTime::Seconds();
#endif
	const FWPGameThreadOwnershipEntry* OwnershipEntry = RenderState.IsValid()
		? RenderState->OwnershipSnapshotsGameThread.Find(Handle.Value)
		: nullptr;
	const bool bRemovedActiveOwnership = OwnershipEntry
		&& IsOwnershipEntryReadyForRendering(*OwnershipEntry);
	const bool bRemovedActiveVisibilityObservation = OwnershipEntry
		&& OwnershipEntry->bVisibilityObservationReady;
#if !UE_BUILD_SHIPPING
	// 로그 전용: 반환된 제거 수만 진단에 사용하며 Remove 호출 자체는 실제 상태 변경입니다.
	const int32 RemovedOwnershipSnapshotCount = RenderState.IsValid()
		? RenderState->OwnershipSnapshotsGameThread.Remove(Handle.Value)
		: 0;
#else
	if (RenderState.IsValid())
	{
		RenderState->OwnershipSnapshotsGameThread.Remove(Handle.Value);
	}
#endif
	if (RenderState.IsValid() && bRemovedActiveOwnership)
	{
		RenderState->ActiveOwnershipPairCount.Store(FMath::Max(
			RenderState->ActiveOwnershipPairCount.Load() - 1, 0));
	}
	if (RenderState.IsValid() && bRemovedActiveVisibilityObservation)
	{
		RenderState->ActiveVisibilityObservationPairCount.Store(FMath::Max(
			RenderState->ActiveVisibilityObservationPairCount.Load() - 1, 0));
	}
#if !UE_BUILD_SHIPPING
	const double OwnershipRemovalCpuMs =
		(FPlatformTime::Seconds() - OwnershipRemovalStartSeconds) * 1000.0;
#endif
	if (WorldEntry)
	{
		WorldEntry->PairHandles.Remove(Handle.Value);
		RenderState->RegisteredPairCount.Store(FMath::Max(RenderState->RegisteredPairCount.Load() - 1, 0));
	}

	const uint64 HandleValue = Handle.Value;
	ENQUEUE_RENDER_COMMAND(WPRemovePair)(
		[RenderState, HandleValue](FRHICommandListImmediate& RHICmdList)
		{
			(void)RHICmdList;
			if (RenderState.IsValid())
			{
				RenderState->PairsRenderThread.Remove(HandleValue);
			}
		});

	const bool bRemoveWorld = WorldEntry && WorldEntry->PairHandles.IsEmpty();
#if !UE_BUILD_SHIPPING
	// 로그 전용: World entry를 제거한 뒤에도 이름을 출력하기 위한 snapshot입니다.
	UWorld* const WorldContext = WorldEntry ? WorldEntry->World.Get() : nullptr;
	const FString WorldName = WorldEntry ? GetNameSafe(WorldEntry->World.Get()) : TEXT("UnknownWorld");
#endif
	if (bRemoveWorld)
	{
		RenderState->bShuttingDown.Store(true);
		RenderState->OwnershipSnapshotsGameThread.Reset();
		RenderState->ActiveOwnershipPairCount.Store(0);
		RenderState->ActiveVisibilityObservationPairCount.Store(0);
		WorldEntry->ViewExtension.Reset();
		Worlds.Remove(PairEntry.WorldId);
	}

#if !UE_BUILD_SHIPPING
	WP_LOG(WorldContext, Verbose,
		TEXT("[GameThread][RendererService] Pair unregistered. World=%s PairId=%s Handle=%llu ServiceId=%llu RemainingPairs=%d WorldExtensionRemoved=%d OwnershipSnapshotRemoved=%d RemovedActiveOwnership=%d RemovedActiveVisibilityObservation=%d ActiveOwnershipPairs=%d ActiveVisibilityObservationPairs=%d OwnershipRemovalCpuMs=%.4f CpuMs=%.3f"),
		*WorldName, *PairIdToString(PairEntry.PairId), Handle.Value, Handle.ServiceId,
		Pairs.Num(), bRemoveWorld ? 1 : 0, RemovedOwnershipSnapshotCount,
		bRemovedActiveOwnership ? 1 : 0,
		bRemovedActiveVisibilityObservation ? 1 : 0,
		RenderState.IsValid() ? RenderState->ActiveOwnershipPairCount.Load() : 0,
		RenderState.IsValid()
			? RenderState->ActiveVisibilityObservationPairCount.Load() : 0,
		OwnershipRemovalCpuMs, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

void FWPRendererService::Shutdown()
{
	check(IsInGameThread());
	if (bShutdown)
	{
		return;
	}

	bShutdown = true;
#if !UE_BUILD_SHIPPING
	// 로그 전용: renderer service shutdown 전체 구간과 제거 규모를 기록합니다.
	const double StartSeconds = FPlatformTime::Seconds();
	// 로그 전용: container reset 전 제거 규모를 출력하기 위한 snapshot/counter입니다.
	const int32 PairCount = Pairs.Num();
	const int32 WorldCount = Worlds.Num();
	int32 OwnershipSnapshotCount = 0;
	int32 ActiveOwnershipPairCount = 0;
	int32 ActiveVisibilityObservationPairCount = 0;
#endif

	for (TPair<uint32, FWorldEntry>& World : Worlds)
	{
		TSharedPtr<FWPRenderState, ESPMode::ThreadSafe> RenderState = World.Value.RenderState;
		if (RenderState.IsValid())
		{
#if !UE_BUILD_SHIPPING
			OwnershipSnapshotCount += RenderState->OwnershipSnapshotsGameThread.Num();
			ActiveOwnershipPairCount += RenderState->ActiveOwnershipPairCount.Load();
			ActiveVisibilityObservationPairCount +=
				RenderState->ActiveVisibilityObservationPairCount.Load();
#endif
			RenderState->bShuttingDown.Store(true);
			RenderState->RegisteredPairCount.Store(0);
			RenderState->OwnershipSnapshotsGameThread.Reset();
			RenderState->ActiveOwnershipPairCount.Store(0);
			RenderState->ActiveVisibilityObservationPairCount.Store(0);
			ENQUEUE_RENDER_COMMAND(WPClearWorld)(
				[RenderState](FRHICommandListImmediate& RHICmdList)
				{
					(void)RHICmdList;
					RenderState->PairsRenderThread.Reset();
				});
		}
		World.Value.ViewExtension.Reset();
	}

	Pairs.Reset();
	Worlds.Reset();
	FlushRenderingCommands();

#if !UE_BUILD_SHIPPING
	WP_LOG(nullptr, Verbose,
		TEXT("[GameThread][RendererService] Shutdown complete. ServiceId=%llu RemovedWorlds=%d RemovedPairs=%d RemovedOwnershipSnapshots=%d RemovedActiveOwnershipPairs=%d RemovedActiveVisibilityObservationPairs=%d FlushedRenderingCommands=1 CpuMs=%.3f"),
		ServiceId, WorldCount, PairCount, OwnershipSnapshotCount, ActiveOwnershipPairCount,
		ActiveVisibilityObservationPairCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
}

FWPRenderThreadPacket FWPRendererService::MakeRenderThreadPacket(
	const FWPRenderPacket& Packet,
	const TSharedRef<FWPPairOwnershipFeedbackState, ESPMode::ThreadSafe>& OwnershipFeedbackState)
{
	FWPRenderThreadPacket Result;
	Result.PairId = Packet.PairId;
	Result.StableSelectorNameA = Packet.StableSelectorNameA;
	Result.StableSelectorNameB = Packet.StableSelectorNameB;
	Result.RequestedOwnership = Packet.RequestedOwnership;
	Result.EffectiveOwnership = Packet.EffectiveOwnership;
	Result.OwnershipEpoch = Packet.OwnershipEpoch;
	Result.OwnershipFeedbackState = OwnershipFeedbackState;
	Result.PortalAToWorld = Packet.PortalAToWorld;
	Result.WorldToPortalA = Packet.WorldToPortalA;
	Result.PortalBToWorld = Packet.PortalBToWorld;
	Result.WorldToPortalB = Packet.WorldToPortalB;
	Result.PortalACenterWorld = Packet.PortalACenterWorld;
	Result.PortalBCenterWorld = Packet.PortalBCenterWorld;
	Result.MetricA = Packet.MetricA;
	Result.MetricB = Packet.MetricB;
	Result.VisualA = Packet.VisualA;
	Result.VisualB = Packet.VisualB;
	Result.CurrentSide = Packet.CurrentSide;
	Result.EntrySide = Packet.EntrySide;
	Result.Region = Packet.Region;
	Result.SignedEllCm = Packet.SignedEllCm;
	Result.TransitionAlpha = Packet.TransitionAlpha;
	Result.ReferenceViewActorId = Packet.ReferenceViewActorId;
	Result.TransitActorId = Packet.TransitActorId;
	Result.TransitEventSequence = Packet.TransitEventSequence;
	Result.CubeA = GetStableTextureReference(Packet.CubeA);
	Result.CubeB = GetStableTextureReference(Packet.CubeB);
	Result.CubeContractA = Packet.CubeContractA;
	Result.CubeContractB = Packet.CubeContractB;
	Result.RayLUTA = GetStableTextureReference(Packet.RayLUTA);
	Result.RayLUTB = GetStableTextureReference(Packet.RayLUTB);
	Result.RayLUTContractA = Packet.RayLUTContractA;
	Result.RayLUTContractB = Packet.RayLUTContractB;
	Result.bAnalyticNoTransitionA = Packet.bAnalyticNoTransitionA;
	Result.bAnalyticNoTransitionB = Packet.bAnalyticNoTransitionB;
	Result.RayLUTZA = FMath::Clamp(Packet.RayLUTZA, 0.0f, 1.0f);
	Result.RayLUTZB = FMath::Clamp(Packet.RayLUTZB, 0.0f, 1.0f);
	Result.RayLUTRevisionA = Packet.RayLUTRevisionA;
	Result.RayLUTRevisionB = Packet.RayLUTRevisionB;
	Result.PacketSequence = Packet.PacketSequence;
	Result.CaptureGenerationA = Packet.CaptureGenerationA;
	Result.CaptureGenerationB = Packet.CaptureGenerationB;
	Result.bEnabled = Packet.bEnabled;
	Result.bHasReferenceView = Packet.bHasReferenceView;
	Result.bTransitActive = Packet.bTransitActive;
	Result.bMetricCompatible = Packet.bMetricCompatible;
	const bool bRayEndpointAReady = Result.bAnalyticNoTransitionA
		|| (Result.RayLUTA.IsValid() && Result.RayLUTContractA.IsValid());
	const bool bRayEndpointBReady = Result.bAnalyticNoTransitionB
		|| (Result.RayLUTB.IsValid() && Result.RayLUTContractB.IsValid());
	Result.bResourcesReady = Packet.bResourcesReady
		&& Result.CubeA.IsValid() && Result.CubeB.IsValid()
		&& Result.CubeContractA.IsValid() && Result.CubeContractB.IsValid()
		&& bRayEndpointAReady && bRayEndpointBReady;
	Result.bCaptureReady = Packet.bCaptureReady;
	Result.bScaleSupported = Packet.bScaleSupported;
	Result.bOwnershipEndpointAReady = Packet.bOwnershipEndpointAReady;
	Result.bOwnershipEndpointBReady = Packet.bOwnershipEndpointBReady;
	Result.bOwnershipInputsReady = Packet.bOwnershipInputsReady;
	Result.CaptureOcclusionVisibleEndpointMask =
		Packet.CaptureOcclusionVisibleEndpointMask;
	Result.bCaptureOcclusionValid = Packet.bCaptureOcclusionValid;
	Result.bCaptureVisibilityFeedbackEnabled =
		Packet.bCaptureVisibilityFeedbackEnabled;
	return Result;
}

bool FWPRendererService::IsHandleOwned(const FWPRenderHandle& Handle) const
{
	return Handle.IsValid() && Handle.ServiceId == ServiceId && Pairs.Contains(Handle.Value);
}
