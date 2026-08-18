// Copyright 2026 Team Beaver. All Rights Reserved.

/**
 * @file WPRuntimeRenderPublication.inl
 *
 * Implements Renderer registration, the ownership handshake, and Packet build/publish
 * behavior in an
 * independent object while reusing policy helpers private to the Runtime translation
 * unit.
 */

FWPRuntimeRenderPublication::FWPRuntimeRenderPublication(
	UWPRuntimeSubsystem& InRuntime,
	TObjectPtr<UWPCaptureManager>& InCaptureManager,
	TObjectPtr<UWPLUTEndpointManager>& InLUTEndpointManager,
	FWPRuntimeTelemetry& InTelemetry)
	: Runtime(InRuntime)
	, CaptureManager(InCaptureManager)
	, LUTEndpointManager(InLUTEndpointManager)
	, Telemetry(InTelemetry)
{
}

UWorld* FWPRuntimeRenderPublication::GetWorld() const
{
	return Runtime.GetWorld();
}

FWPMetricSettings FWPRuntimeRenderPublication::MakeMetricSettings(
	const AWormholePortalActor& Portal) const
{
	return Runtime.MakeMetricSettings(Portal);
}

void FWPRuntimeRenderPublication::EnsureRendererRegistration(FWPPortalPairState& PairState)
{
	// Logging only: measures CPU time for stale-handle processing and registration.
	const double EnsureRegistrationStartSeconds = FPlatformTime::Seconds();
	IWPRenderer* Renderer = IWPRenderer::Find();
	UWorld* World = GetWorld();
	if (!Renderer || !World)
	{
		return;
	}

	if (PairState.Ownership.RenderHandle.IsValid() && PairState.Ownership.RenderHandle.ServiceId != Renderer->GetServiceId())
	{
		WP_LOG(&Runtime, Warning,
			TEXT("[Runtime] Stale renderer handle discarded. PairId=%s OldHandle=%llu OldServiceId=%llu NewServiceId=%llu CpuMs=%.3f"),
			*PairIdToString(PairState.Identity.PairId), PairState.Ownership.RenderHandle.Value,
			PairState.Ownership.RenderHandle.ServiceId, Renderer->GetServiceId(),
			(FPlatformTime::Seconds() - EnsureRegistrationStartSeconds) * 1000.0);
		PairState.Ownership.RenderHandle.Reset();
		PairState.Publication.bDirty = true;
	}

	if (!PairState.Ownership.RenderHandle.IsValid())
	{
		// Logging only: measures CPU time spent in the actual RegisterPair call.
		const double StartSeconds = FPlatformTime::Seconds();
		PairState.Ownership.RenderHandle = Renderer->RegisterPair(*World, PairState.Identity.PairId);
		PairState.Publication.bDirty = true;
		if (PairState.Ownership.RenderHandle.IsValid())
		{
			// A renderer registration owns a fresh feedback mailbox whose sample
			// sequence starts over. Invalidate the old tuple so the scheduler
			// fails open until the new handle publishes a coherent current-epoch
			// visibility sample. Preserve CaptureVisibility.bPaused here so the
			// normal Paused->Active path emits diagnostics and requests one atomic
			// resume prewarm on the next scheduler callback.
			PairState.Ownership.LastOwnershipFeedback = FWPPairOwnershipFeedback();
			PairState.Capture.Visibility.RequiredPacketSequence = 0;
			PairState.Capture.Visibility.LastOwnershipEpoch = 0;
			PairState.Capture.Visibility.LastSampleSequence = 0;
			PairState.Capture.Visibility.LastSampleReceiptSeconds = -1.0e30;
			PairState.Capture.Visibility.LastVisibleEndpointMask = 0x3;
			PairState.Capture.Visibility.RejectedOwnershipEpoch = 0;
			PairState.Capture.Visibility.RejectedThroughSampleSequence = 0;
			PairState.Capture.Visibility.InvisibleElapsedSeconds = 0.0;
			PairState.Capture.Visibility.LastInvisibleSampleReceiptSeconds =
				-1.0e30;
			PairState.Capture.Visibility.LastInvisibleOwnershipEpoch = 0;
			PairState.Capture.Visibility.LastInvisibleSampleSequence = 0;
			PairState.Capture.Visibility.GuardViewActorId = 0;
			PairState.Capture.Visibility.GuardCameraLocation = FVector::ZeroVector;
			PairState.Capture.Visibility.GuardCameraRotation = FRotator::ZeroRotator;
			PairState.Capture.Visibility.GuardCameraFOVDegrees = 0.0f;
			PairState.Capture.Visibility.bGuardInitialized = false;
			PairState.Capture.Visibility.bAwaitingPostGuardSample = false;
#if !UE_BUILD_SHIPPING
			WP_LOG(&Runtime, Verbose,
				TEXT("[Runtime] Pair renderer registration. PairId=%s Handle=%llu ServiceId=%llu Success=1 VisibilityFeedbackInvalidated=1 VisibilityRequiredPacketFloorReset=1 VisibilityCameraGuardReset=1 VisibilityUnknownFailOpen=1 CpuMs=%.3f"),
				*PairIdToString(PairState.Identity.PairId), PairState.Ownership.RenderHandle.Value,
				PairState.Ownership.RenderHandle.ServiceId,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		else
		{
			WP_LOG(&Runtime, Error,
				TEXT("[Runtime] Pair renderer registration. PairId=%s Handle=0 ServiceId=0 Success=0 CpuMs=%.3f"),
				*PairIdToString(PairState.Identity.PairId),
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		}
	}
}

void FWPRuntimeRenderPublication::UpdatePairOwnership(
	FWPPortalPairState& PairState,
	FWPRenderPacket& Packet,
	IWPRenderer* Renderer)
{
#if !UE_BUILD_SHIPPING
	// Includes evaluation CPU time in ownership-transition and ACK-rejection logs.
	const double StartSeconds = FPlatformTime::Seconds();
#endif
	// While the Runtime pipeline is active, each Registry Pair owns an independent
	// Production Epoch,
	// warmup ACK state, and failure-recovery state.
	const EWPPairOwnershipMode DesiredRequested =
		EWPPairOwnershipMode::Production;

	const bool bRayEndpointAReady = Packet.bAnalyticNoTransitionA
		|| (Packet.RayLUTA.IsValid() && Packet.RayLUTContractA.IsValid());
	const bool bRayEndpointBReady = Packet.bAnalyticNoTransitionB
		|| (Packet.RayLUTB.IsValid() && Packet.RayLUTContractB.IsValid());
	Packet.bOwnershipEndpointAReady = Packet.MetricA.IsFiniteAndValid()
		&& Packet.CubeA.IsValid()
		&& Packet.CubeContractA.IsValid()
		&& bRayEndpointAReady
		&& Packet.CaptureGenerationA > 0;
	Packet.bOwnershipEndpointBReady = Packet.MetricB.IsFiniteAndValid()
		&& Packet.CubeB.IsValid()
		&& Packet.CubeContractB.IsValid()
		&& bRayEndpointBReady
		&& Packet.CaptureGenerationB > 0;
	UWorld* OwnershipWorld = GetWorld();
	const bool bExactlyOneLocalPlayer = GEngine && OwnershipWorld
		&& GEngine->GetGamePlayers(OwnershipWorld).Num() == 1;
	Packet.bOwnershipInputsReady = Packet.bOwnershipEndpointAReady
		&& Packet.bOwnershipEndpointBReady
		&& Packet.bMetricCompatible
		&& Packet.bScaleSupported
		&& Renderer
		&& PairState.Ownership.RenderHandle.IsValid()
		&& PairState.Ownership.RenderHandle.ServiceId == Renderer->GetServiceId()
		&& bExactlyOneLocalPlayer;

	FWPPairOwnershipFeedback Feedback;
	const bool bFeedbackAvailable = Renderer
		&& PairState.Ownership.RenderHandle.IsValid()
		&& Renderer->QueryPairOwnershipFeedback(
			PairState.Ownership.RenderHandle,
			Feedback,
			Packet.bCaptureVisibilityFeedbackEnabled);
	if (bFeedbackAvailable)
	{
		PairState.Ownership.LastOwnershipFeedback = Feedback;
		if (Packet.bCaptureVisibilityFeedbackEnabled
			&& Feedback.bSnapshotCoherent
			&& Feedback.VisibilitySampleSequence != 0
			&& (Feedback.VisibilityOwnershipEpoch
					!= PairState.Capture.Visibility.LastOwnershipEpoch
				|| Feedback.VisibilitySampleSequence
					!= PairState.Capture.Visibility.LastSampleSequence))
		{
			PairState.Capture.Visibility.LastOwnershipEpoch =
				Feedback.VisibilityOwnershipEpoch;
			PairState.Capture.Visibility.LastSampleSequence =
				Feedback.VisibilitySampleSequence;
			PairState.Capture.Visibility.LastVisibleEndpointMask =
				static_cast<uint8>(Feedback.VisibleEndpointMask & 0x3u);
			PairState.Capture.Visibility.LastSampleReceiptSeconds =
				FPlatformTime::Seconds();
			switch (PairState.Capture.Visibility.GetLastVisibleEndpointCount())
			{
			case 0:
				break;
			case 1:
				break;
			default:
				break;
			}
		}
	}

	const bool bStableSelectorIdentityChanged = PairState.Ownership.bOwnershipObservationInitialized
		&& (PairState.Identity.StableSelectorNameA != Packet.StableSelectorNameA
			|| PairState.Identity.StableSelectorNameB != Packet.StableSelectorNameB);
	const bool bMetricOrTransformIdentityChanged =
		PairState.Ownership.bOwnershipResourceIdentityInitialized
		&& (PairState.Ownership.LastOwnershipMetricResourceIdentityRevisionA
				!= Packet.MetricA.ResourceIdentityRevision
			|| PairState.Ownership.LastOwnershipMetricResourceIdentityRevisionB
				!= Packet.MetricB.ResourceIdentityRevision
			|| !PairState.Ownership.LastOwnershipTransformA.Equals(
				PairState.Identity.PortalA->GetActorTransform(), TransformTolerance)
			|| !PairState.Ownership.LastOwnershipTransformB.Equals(
				PairState.Identity.PortalB->GetActorTransform(), TransformTolerance));
	const bool bRendererIdentityChanged = PairState.Ownership.bOwnershipResourceIdentityInitialized
		&& (PairState.Ownership.LastOwnershipRenderHandle.ServiceId != Packet.RenderHandle.ServiceId
			|| PairState.Ownership.LastOwnershipRenderHandle.Value != Packet.RenderHandle.Value);
	const bool bResourceIdentityChanged = PairState.Ownership.bOwnershipResourceIdentityInitialized
		&& (PairState.Ownership.LastOwnershipCubeContractA != Packet.CubeContractA
			|| PairState.Ownership.LastOwnershipCubeContractB != Packet.CubeContractB
			|| PairState.Ownership.LastOwnershipRayLUTContractA != Packet.RayLUTContractA
			|| PairState.Ownership.LastOwnershipRayLUTContractB != Packet.RayLUTContractB
			|| bStableSelectorIdentityChanged
			|| bRendererIdentityChanged
			|| bMetricOrTransformIdentityChanged);

	FWPOwnershipPolicyInput PolicyInput;
	PolicyInput.CurrentRequested = PairState.Ownership.RequestedOwnership;
	PolicyInput.CurrentEffective = PairState.Ownership.EffectiveOwnership;
	PolicyInput.DesiredRequested = DesiredRequested;
	PolicyInput.CurrentEpoch = PairState.Ownership.OwnershipEpoch;
	PolicyInput.WarmupSucceededEpoch = bFeedbackAvailable
		? Feedback.WarmupSucceededEpoch : 0;
	PolicyInput.ProductionFailedEpoch = bFeedbackAvailable
		? Feedback.ProductionFailedEpoch : 0;
	PolicyInput.bRequestedChanged = DesiredRequested != PairState.Ownership.RequestedOwnership;
	PolicyInput.bResourceIdentityChanged = bResourceIdentityChanged;
	PolicyInput.bInputsReady = Packet.bOwnershipInputsReady;
	PolicyInput.bPreviousInputsReady = PairState.Ownership.bOwnershipObservationInitialized
		&& PairState.Ownership.bLastOwnershipInputsReady;
	const FWPOwnershipPolicyResult PolicyResult =
		EvaluateWPOwnershipPolicy(PolicyInput);
	const bool bWarmupAckAlreadyAccepted = bFeedbackAvailable
		&& PolicyResult.Effective == EWPPairOwnershipMode::Production
		&& Feedback.WarmupSucceededEpoch == PolicyResult.Epoch
		&& Feedback.ProductionFailedEpoch != PolicyResult.Epoch;
	const bool bWarmupAckRejected = bFeedbackAvailable
		&& Feedback.WarmupSucceededEpoch != 0
		&& !PolicyResult.bCommittedProduction
		&& !bWarmupAckAlreadyAccepted;
	if (bWarmupAckRejected)
	{
		const bool bRejectionFingerprintChanged =
			PairState.Ownership.LastRejectedWarmupAckEpoch != Feedback.WarmupSucceededEpoch
			|| PairState.Ownership.LastRejectedWarmupAckCurrentEpoch != PolicyResult.Epoch
			|| PairState.Ownership.LastRejectedWarmupAckFailureEpoch
				!= Feedback.ProductionFailedEpoch;
		if (bRejectionFingerprintChanged)
		{
			++Telemetry.RejectedOwnershipAckCount;
#if !UE_BUILD_SHIPPING
			const TCHAR* RejectionReason =
				Feedback.ProductionFailedEpoch == PolicyResult.Epoch
					? TEXT("SameEpochFailureObserved")
					: (Feedback.WarmupSucceededEpoch != PolicyResult.Epoch
						? TEXT("StaleOrFutureEpoch")
						: (!Packet.bOwnershipInputsReady
							? TEXT("OwnershipInputsNotReady")
							: TEXT("PolicyDidNotCommit")));
			WP_LOG(&Runtime, Verbose,
				TEXT("[Runtime][ProductionOwnershipAck] Warmup ACK rejected. World=%s PairId=%s Handle=%llu AckEpoch=%llu AckPacketSequence=%llu ProductionFailedEpoch=%llu ProductionFailedPacketSequence=%llu PacketSequenceCandidate=%llu LastPublishedPacketSequence=%llu PreviousCurrentEpoch=%llu EvaluatedEpoch=%llu PreviousEffective=%s EvaluatedEffective=%s InputsReady=%d Reason=%s RejectedAckCount=%llu CpuMs=%.4f"),
				*GetNameSafe(GetWorld()), *PairIdToString(PairState.Identity.PairId),
				PairState.Ownership.RenderHandle.Value, Feedback.WarmupSucceededEpoch,
				Feedback.WarmupSucceededPacketSequence, Feedback.ProductionFailedEpoch,
				Feedback.ProductionFailedPacketSequence, Packet.PacketSequence,
				PairState.Publication.PacketSequence,
				PairState.Ownership.OwnershipEpoch, PolicyResult.Epoch,
				GetWPPairOwnershipModeName(PairState.Ownership.EffectiveOwnership),
				GetWPPairOwnershipModeName(PolicyResult.Effective),
				Packet.bOwnershipInputsReady ? 1 : 0,
				RejectionReason, Telemetry.RejectedOwnershipAckCount,
				(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
		}
		PairState.Ownership.LastRejectedWarmupAckEpoch = Feedback.WarmupSucceededEpoch;
		PairState.Ownership.LastRejectedWarmupAckCurrentEpoch = PolicyResult.Epoch;
		PairState.Ownership.LastRejectedWarmupAckFailureEpoch = Feedback.ProductionFailedEpoch;
	}
	else
	{
		PairState.Ownership.LastRejectedWarmupAckEpoch = 0;
		PairState.Ownership.LastRejectedWarmupAckCurrentEpoch = 0;
		PairState.Ownership.LastRejectedWarmupAckFailureEpoch = 0;
	}

	const EWPPairOwnershipMode PreviousRequested = PairState.Ownership.RequestedOwnership;
	const EWPPairOwnershipMode PreviousEffective = PairState.Ownership.EffectiveOwnership;
	const uint64 PreviousEpoch = PairState.Ownership.OwnershipEpoch;
	const bool bOwnershipObservationChanged = !PairState.Ownership.bOwnershipObservationInitialized
		|| bResourceIdentityChanged
		|| PairState.Ownership.bLastOwnershipInputsReady != Packet.bOwnershipInputsReady
		|| PairState.Ownership.bOwnershipEndpointAReady != Packet.bOwnershipEndpointAReady
		|| PairState.Ownership.bOwnershipEndpointBReady != Packet.bOwnershipEndpointBReady;

	PairState.Ownership.RequestedOwnership = PolicyResult.Requested;
	PairState.Ownership.EffectiveOwnership = PolicyResult.Effective;
	PairState.Ownership.OwnershipEpoch = PolicyResult.Epoch;
	PairState.Identity.StableSelectorNameA = Packet.StableSelectorNameA;
	PairState.Identity.StableSelectorNameB = Packet.StableSelectorNameB;
	PairState.Ownership.LastOwnershipCubeContractA = Packet.CubeContractA;
	PairState.Ownership.LastOwnershipCubeContractB = Packet.CubeContractB;
	PairState.Ownership.LastOwnershipRayLUTContractA = Packet.RayLUTContractA;
	PairState.Ownership.LastOwnershipRayLUTContractB = Packet.RayLUTContractB;
	PairState.Ownership.LastOwnershipMetricResourceIdentityRevisionA =
		Packet.MetricA.ResourceIdentityRevision;
	PairState.Ownership.LastOwnershipMetricResourceIdentityRevisionB =
		Packet.MetricB.ResourceIdentityRevision;
	PairState.Ownership.LastOwnershipTransformA = PairState.Identity.PortalA->GetActorTransform();
	PairState.Ownership.LastOwnershipTransformB = PairState.Identity.PortalB->GetActorTransform();
	PairState.Ownership.LastOwnershipRenderHandle = Packet.RenderHandle;
	PairState.Ownership.bLastOwnershipInputsReady = Packet.bOwnershipInputsReady;
	PairState.Ownership.bOwnershipEndpointAReady = Packet.bOwnershipEndpointAReady;
	PairState.Ownership.bOwnershipEndpointBReady = Packet.bOwnershipEndpointBReady;
	PairState.Ownership.bOwnershipInputsReady = Packet.bOwnershipInputsReady;
	PairState.Ownership.bOwnershipObservationInitialized = true;
	PairState.Ownership.bOwnershipResourceIdentityInitialized = true;

	Packet.RequestedOwnership = PairState.Ownership.RequestedOwnership;
	Packet.EffectiveOwnership = PairState.Ownership.EffectiveOwnership;
	Packet.OwnershipEpoch = PairState.Ownership.OwnershipEpoch;

	if (PolicyResult.bStateChanged || bOwnershipObservationChanged)
	{
		PairState.Publication.bDirty = true;
#if !UE_BUILD_SHIPPING
		WP_LOG(&Runtime, Verbose,
			TEXT("[Runtime][ProductionOwnership] State or observation changed. World=%s PairId=%s PortalA=%s PortalB=%s SelectorA=%s SelectorB=%s OwnershipPolicy=AllRegisteredPairsProduction PacketSequenceCandidate=%llu LastPublishedPacketSequence=%llu StateChanged=%d ObservationChanged=%d PreviousRequested=%s Requested=%s PreviousEffective=%s Effective=%s PreviousEpoch=%llu OwnershipEpoch=%llu EndpointAReady=%d EndpointBReady=%d BothEndpointInputsReady=%d ExactlyOneLocalPlayer=%d PreviousInputsReady=%d ResourceIdentityChanged=%d StableSelectorIdentityChanged=%d MetricOrTransformIdentityChanged=%d RendererIdentityChanged=%d MetricRevisionA=%u MetricRevisionB=%u MetricResourceIdentityRevisionA=%u MetricResourceIdentityRevisionB=%u UniformMetricScalePreservesOwnership=1 CubeResourceGenerationA=%u CubeResourceGenerationB=%u LUTGenerationA=%u LUTGenerationB=%u FeedbackAvailable=%d WarmupSucceededEpoch=%llu WarmupSucceededPacketSequence=%llu ProductionFailedEpoch=%llu ProductionFailedPacketSequence=%llu WarmupPassCount=%llu ProductionFailureCount=%llu StartedWarmup=%d CommittedProduction=%d RestartedWarmup=%d RasterProxyDependency=0 RasterFallbackAvailable=0 Decision=%s CpuMs=%.4f"),
			*GetNameSafe(GetWorld()), *PairIdToString(PairState.Identity.PairId),
			*GetNameSafe(PairState.Identity.PortalA.Get()),
			*GetNameSafe(PairState.Identity.PortalB.Get()), *Packet.StableSelectorNameA.ToString(),
			*Packet.StableSelectorNameB.ToString(), Packet.PacketSequence, PairState.Publication.PacketSequence,
			PolicyResult.bStateChanged ? 1 : 0,
			bOwnershipObservationChanged ? 1 : 0,
			GetWPPairOwnershipModeName(PreviousRequested),
			GetWPPairOwnershipModeName(PairState.Ownership.RequestedOwnership),
			GetWPPairOwnershipModeName(PreviousEffective),
			GetWPPairOwnershipModeName(PairState.Ownership.EffectiveOwnership),
			PreviousEpoch, PairState.Ownership.OwnershipEpoch,
			Packet.bOwnershipEndpointAReady ? 1 : 0,
			Packet.bOwnershipEndpointBReady ? 1 : 0,
			Packet.bOwnershipInputsReady ? 1 : 0,
			bExactlyOneLocalPlayer ? 1 : 0,
			PolicyInput.bPreviousInputsReady ? 1 : 0,
			bResourceIdentityChanged ? 1 : 0,
			bStableSelectorIdentityChanged ? 1 : 0,
			bMetricOrTransformIdentityChanged ? 1 : 0,
			bRendererIdentityChanged ? 1 : 0,
			Packet.MetricA.Revision, Packet.MetricB.Revision,
			Packet.MetricA.ResourceIdentityRevision,
			Packet.MetricB.ResourceIdentityRevision,
			Packet.CubeContractA.ResourceGeneration,
			Packet.CubeContractB.ResourceGeneration,
			Packet.RayLUTContractA.Generation, Packet.RayLUTContractB.Generation,
			bFeedbackAvailable ? 1 : 0, Feedback.WarmupSucceededEpoch,
			Feedback.WarmupSucceededPacketSequence, Feedback.ProductionFailedEpoch,
			Feedback.ProductionFailedPacketSequence, Feedback.WarmupPassCount,
			Feedback.ProductionFailureCount,
			PolicyResult.bStartedWarmup ? 1 : 0,
			PolicyResult.bCommittedProduction ? 1 : 0,
			PolicyResult.bRestartedWarmup ? 1 : 0,
			PolicyResult.DecisionReason,
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
#endif
	}

}

bool FWPRuntimeRenderPublication::ResolveReferenceView(
	FVector& OutCameraLocation,
	FRotator& OutCameraRotation,
	AActor*& OutViewActor,
	float& OutCameraFOVDegrees) const
{
	OutCameraLocation = FVector::ZeroVector;
	OutCameraRotation = FRotator::ZeroRotator;
	OutViewActor = nullptr;
	OutCameraFOVDegrees = 0.0f;
	const APlayerController* PlayerController = nullptr;

	return ResolvePrimaryLocalReferenceView(
		GetWorld(), OutCameraLocation, OutCameraRotation, OutViewActor, PlayerController,
		OutCameraFOVDegrees);
}

void FWPRuntimeRenderPublication::UpdateReferenceViewState(
	FWPPortalPairState& PairState, const FVector& CameraLocation) const
{
	const AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	const AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	if (!PortalA || !PortalB)
	{
		PairState.Transit.CurrentSide = EWPSide::None;
		PairState.Transit.Region = EWPRegion::Flat;
		PairState.Transit.SignedEllCm = 0.0f;
		PairState.Transit.TransitionAlpha = 1.0f;
		return;
	}

	const double DistanceA = FVector::Distance(CameraLocation, PortalA->GetActorLocation());
	const double DistanceB = FVector::Distance(CameraLocation, PortalB->GetActorLocation());
	PairState.Transit.CurrentSide = DistanceA <= DistanceB ? EWPSide::SideA : EWPSide::SideB;

	const AWormholePortalActor* SelectedPortal = PairState.Transit.CurrentSide == EWPSide::SideA ? PortalA : PortalB;
	const double SelectedDistance = PairState.Transit.CurrentSide == EWPSide::SideA ? DistanceA : DistanceB;
	const FWPMetricSettings Metric = MakeMetricSettings(*SelectedPortal);
	const float SurfaceDistanceCm = static_cast<float>(SelectedDistance) - Metric.PortalRadiusCm;
	PairState.Transit.SignedEllCm = PairState.Transit.CurrentSide == EWPSide::SideA ? -SurfaceDistanceCm : SurfaceDistanceCm;

	if (SelectedDistance <= Metric.MouthRadiusCm)
	{
		PairState.Transit.Region = EWPRegion::Throat;
		PairState.Transit.TransitionAlpha = 0.0f;
	}
	else if (SelectedDistance <= Metric.OuterRadiusCm)
	{
		PairState.Transit.Region = EWPRegion::Transition;
		PairState.Transit.TransitionAlpha = FMath::Clamp(
			(static_cast<float>(SelectedDistance) - Metric.MouthRadiusCm)
			/ FMath::Max(Metric.OuterRadiusCm - Metric.MouthRadiusCm, KINDA_SMALL_NUMBER),
			0.0f, 1.0f);
	}
	else
	{
		PairState.Transit.Region = EWPRegion::Flat;
		PairState.Transit.TransitionAlpha = 1.0f;
	}
}

FWPRenderPacket FWPRuntimeRenderPublication::BuildRenderPacket(
	const FWPPortalPairState& PairState,
	const FVector& CameraLocation,
	const AActor* ReferenceViewActor,
	const bool bHasReferenceView) const
{
#if !UE_BUILD_SHIPPING
	// Logging only: measures CPU time spent building the Packet contract.
	const double ContractBuildStartSeconds = FPlatformTime::Seconds();
#endif
	SCOPE_CYCLE_COUNTER(STAT_WP_RenderPacketBuild);
	FWPRenderPacket Packet;
	const AWormholePortalActor* PortalA = PairState.Identity.PortalA.Get();
	const AWormholePortalActor* PortalB = PairState.Identity.PortalB.Get();
	if (!PortalA || !PortalB)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(&Runtime, Verbose,
			TEXT("[Runtime][RenderContract] Packet contract build rejected. PairId=%s PortalAValid=%d PortalBValid=%d CpuMs=%.4f"),
			*PairIdToString(PairState.Identity.PairId), PortalA ? 1 : 0, PortalB ? 1 : 0,
			(FPlatformTime::Seconds() - ContractBuildStartSeconds) * 1000.0);
#endif
		return Packet;
	}

	const FTransform TransformA = PortalA->GetActorTransform();
	const FTransform TransformB = PortalB->GetActorTransform();
	Packet.PairId = PairState.Identity.PairId;
	Packet.RenderHandle = PairState.Ownership.RenderHandle;
	Packet.StableSelectorNameA = PortalA->GetStableSelectorName();
	Packet.StableSelectorNameB = PortalB->GetStableSelectorName();
	Packet.RequestedOwnership = PairState.Ownership.RequestedOwnership;
	Packet.EffectiveOwnership = PairState.Ownership.EffectiveOwnership;
	Packet.OwnershipEpoch = PairState.Ownership.OwnershipEpoch;
	Packet.PortalAToWorld = FMatrix44d(TransformA.ToMatrixWithScale());
	Packet.WorldToPortalA = FMatrix44d(TransformA.ToInverseMatrixWithScale());
	Packet.PortalBToWorld = FMatrix44d(TransformB.ToMatrixWithScale());
	Packet.WorldToPortalB = FMatrix44d(TransformB.ToInverseMatrixWithScale());
	Packet.PortalACenterWorld = FVector3d(TransformA.GetLocation());
	Packet.PortalBCenterWorld = FVector3d(TransformB.GetLocation());
	Packet.ReferenceViewPositionWorld = FVector3d(CameraLocation);
	Packet.ReferenceViewPositionPortalA = FVector3f(TransformA.InverseTransformPositionNoScale(CameraLocation));
	Packet.ReferenceViewPositionPortalB = FVector3f(TransformB.InverseTransformPositionNoScale(CameraLocation));
	Packet.ReferenceViewActorId = IsValid(ReferenceViewActor) ? ReferenceViewActor->GetUniqueID() : 0;
	Packet.MetricA = MakeMetricSettings(*PortalA);
	Packet.MetricB = MakeMetricSettings(*PortalB);
	Packet.VisualA.UniformScale = PortalA->GetPortalVisualScale();
	Packet.VisualB.UniformScale = PortalB->GetPortalVisualScale();
	Packet.CurrentSide = PairState.Transit.CurrentSide;
	Packet.EntrySide = PairState.Transit.EntrySide;
	Packet.Region = PairState.Transit.Region;
	Packet.SignedEllCm = PairState.Transit.SignedEllCm;
	Packet.TransitionAlpha = PairState.Transit.TransitionAlpha;
	Packet.TransitActorId = PairState.Transit.TransitActorId;
	Packet.TransitEventSequence = PairState.Transit.LastTransitEventSequence;
	FWPCaptureEndpointSnapshot CaptureSnapshotA;
	FWPCaptureEndpointSnapshot CaptureSnapshotB;
	const bool bHasCaptureEndpointA = CaptureManager
		&& CaptureManager->GetPublishedEndpointSnapshot(PortalA, CaptureSnapshotA);
	const bool bHasCaptureEndpointB = CaptureManager
		&& CaptureManager->GetPublishedEndpointSnapshot(PortalB, CaptureSnapshotB);
	Packet.CubeA = bHasCaptureEndpointA ? CaptureSnapshotA.RenderTarget.Get() : nullptr;
	Packet.CubeB = bHasCaptureEndpointB ? CaptureSnapshotB.RenderTarget.Get() : nullptr;
	Packet.CubeContractA = bHasCaptureEndpointA
		? CaptureSnapshotA.CubeContract : FWPCubeContract();
	Packet.CubeContractB = bHasCaptureEndpointB
		? CaptureSnapshotB.CubeContract : FWPCubeContract();
	FWPLUTEndpointSnapshot LUTSnapshotA;
	FWPLUTEndpointSnapshot LUTSnapshotB;
	const bool bHasLUTEndpointA = LUTEndpointManager
		&& LUTEndpointManager->GetEndpointSnapshot(PortalA, LUTSnapshotA);
	const bool bHasLUTEndpointB = LUTEndpointManager
		&& LUTEndpointManager->GetEndpointSnapshot(PortalB, LUTSnapshotB);
	Packet.RayLUTA = bHasLUTEndpointA ? LUTSnapshotA.VolumeTexture.Get() : nullptr;
	Packet.RayLUTB = bHasLUTEndpointB ? LUTSnapshotB.VolumeTexture.Get() : nullptr;
	Packet.RayLUTContractA = bHasLUTEndpointA
		? LUTSnapshotA.Contract : FWPRayLUTContract();
	Packet.RayLUTContractB = bHasLUTEndpointB
		? LUTSnapshotB.Contract : FWPRayLUTContract();
	Packet.bAnalyticNoTransitionA = bHasLUTEndpointA
		&& LUTSnapshotA.bAnalyticNoTransition;
	Packet.bAnalyticNoTransitionB = bHasLUTEndpointB
		&& LUTSnapshotB.bAnalyticNoTransition;
	Packet.RayLUTZA = bHasLUTEndpointA ? LUTSnapshotA.RatioCoordinate01 : 0.0f;
	Packet.RayLUTZB = bHasLUTEndpointB ? LUTSnapshotB.RatioCoordinate01 : 0.0f;
	Packet.RayLUTRevisionA = bHasLUTEndpointA ? LUTSnapshotA.ResourceRevision : 0;
	Packet.RayLUTRevisionB = bHasLUTEndpointB ? LUTSnapshotB.ResourceRevision : 0;
	Packet.PacketSequence = PairState.Publication.PacketSequence + 1;
	Packet.CaptureGenerationA = bHasCaptureEndpointA
		? CaptureSnapshotA.CaptureGeneration : 0;
	Packet.CaptureGenerationB = bHasCaptureEndpointB
		? CaptureSnapshotB.CaptureGeneration : 0;
	Packet.bEnabled = true;
	Packet.bCaptureVisibilityFeedbackEnabled =
		WPCaptureVisibilityPolicyAlwaysEnabled;
	Packet.bHasReferenceView = bHasReferenceView;
	Packet.bTransitActive = PairState.Transit.bTransitActive;
	Packet.bMetricCompatible = Packet.MetricA.IsCompatibleWith(Packet.MetricB);
	const bool bRayEndpointAReady = Packet.bAnalyticNoTransitionA
		|| (Packet.RayLUTA.IsValid() && Packet.RayLUTContractA.IsValid());
	const bool bRayEndpointBReady = Packet.bAnalyticNoTransitionB
		|| (Packet.RayLUTB.IsValid() && Packet.RayLUTContractB.IsValid());
	Packet.bResourcesReady = Packet.CubeA.IsValid() && Packet.CubeB.IsValid()
		&& Packet.CubeContractA.IsValid() && Packet.CubeContractB.IsValid()
		&& bRayEndpointAReady && bRayEndpointBReady;
	Packet.bCaptureReady = Packet.CaptureGenerationA > 0 && Packet.CaptureGenerationB > 0;
	Packet.bScaleSupported = TransformA.GetScale3D().Equals(FVector::OneVector, SupportedScaleTolerance)
		&& TransformB.GetScale3D().Equals(FVector::OneVector, SupportedScaleTolerance);
	Packet.bOwnershipEndpointAReady = PairState.Ownership.bOwnershipEndpointAReady;
	Packet.bOwnershipEndpointBReady = PairState.Ownership.bOwnershipEndpointBReady;
	Packet.bOwnershipInputsReady = PairState.Ownership.bOwnershipInputsReady;
	Packet.CaptureOcclusionVisibleEndpointMask =
		PairState.Capture.Visibility.OcclusionVisibleEndpointMask;
	Packet.bCaptureOcclusionValid = PairState.Capture.Visibility.bOcclusionValid;
	return Packet;
}

FWPPublishDecision FWPRuntimeRenderPublication::MakePublishDecision(
	const FWPPortalPairState& PairState, const FWPRenderPacket& Packet,
	const FTransform& TransformA, const FTransform& TransformB, const double NowSeconds) const
{
	FWPPublishDecision Decision;
	Decision.bDirty = PairState.Publication.bDirty;
	Decision.bInitial = !PairState.Publication.bHasPublished;
	Decision.bResourceReadinessChanged = PairState.Publication.bHasPublished
		&& (Packet.bResourcesReady != PairState.Publication.bLastPublishedResourcesReady
			|| Packet.bMetricCompatible != PairState.Publication.bLastPublishedMetricCompatible
			|| Packet.bScaleSupported != PairState.Publication.bLastPublishedScaleSupported);
	Decision.bCaptureReadinessChangedA = PairState.Publication.bHasPublished
		&& HasWPEndpointCaptureReadinessChanged(
			Packet.CaptureGenerationA,
			PairState.Publication.LastPublishedCaptureGenerationA);
	Decision.bCaptureReadinessChangedB = PairState.Publication.bHasPublished
		&& HasWPEndpointCaptureReadinessChanged(
			Packet.CaptureGenerationB,
			PairState.Publication.LastPublishedCaptureGenerationB);
	Decision.bCaptureReadinessChanged = Decision.bCaptureReadinessChangedA
		|| Decision.bCaptureReadinessChangedB;
	Decision.bMetricChanged = Packet.MetricA.Revision != PairState.Publication.LastPublishedMetricRevisionA
		|| Packet.MetricB.Revision != PairState.Publication.LastPublishedMetricRevisionB;
	Decision.bCubeContractChanged = Packet.CubeContractA != PairState.Publication.LastPublishedCubeContractA
		|| Packet.CubeContractB != PairState.Publication.LastPublishedCubeContractB;
	Decision.bLUTContractChanged = Packet.RayLUTContractA != PairState.Publication.LastPublishedRayLUTContractA
		|| Packet.RayLUTContractB != PairState.Publication.LastPublishedRayLUTContractB
		|| Packet.RayLUTRevisionA != PairState.Publication.LastPublishedRayLUTRevisionA
		|| Packet.RayLUTRevisionB != PairState.Publication.LastPublishedRayLUTRevisionB;
	Decision.bTransformChanged = !TransformA.Equals(
		PairState.Publication.LastPublishedTransformA, TransformTolerance)
		|| !TransformB.Equals(PairState.Publication.LastPublishedTransformB, TransformTolerance);
	Decision.bTransitChanged = Packet.TransitEventSequence
			!= PairState.Publication.LastPublishedTransitEventSequence
		|| Packet.bTransitActive != PairState.Publication.bLastPublishedTransitActive;
	Decision.bOwnershipChanged = Packet.OwnershipEpoch != PairState.Publication.LastPublishedOwnershipEpoch
		|| Packet.RequestedOwnership != PairState.Publication.LastPublishedRequestedOwnership
		|| Packet.EffectiveOwnership != PairState.Publication.LastPublishedEffectiveOwnership
		|| Packet.StableSelectorNameA != PairState.Publication.LastPublishedStableSelectorNameA
		|| Packet.StableSelectorNameB != PairState.Publication.LastPublishedStableSelectorNameB
		|| Packet.bOwnershipEndpointAReady
			!= PairState.Publication.bLastPublishedOwnershipEndpointAReady
		|| Packet.bOwnershipEndpointBReady
			!= PairState.Publication.bLastPublishedOwnershipEndpointBReady
		|| Packet.bOwnershipInputsReady
			!= PairState.Publication.bLastPublishedOwnershipInputsReady
		|| Packet.bCaptureVisibilityFeedbackEnabled
			!= PairState.Publication.bLastPublishedCaptureVisibilityFeedbackEnabled;

	const bool bCameraDiagnosticChanged = Packet.ReferenceViewActorId
			!= PairState.Publication.LastPublishedReferenceViewActorId
		|| FVector::DistSquared(
			Packet.ReferenceViewPositionWorld,
			PairState.Publication.LastPublishedCameraLocation) > FMath::Square(CameraPublishDistanceCm);
	const bool bCaptureDiagnosticChanged = Packet.CaptureGenerationA
			!= PairState.Publication.LastObservedCaptureGenerationA
		|| Packet.CaptureGenerationB != PairState.Publication.LastObservedCaptureGenerationB;
	FWPPublishPolicyInput PolicyInput;
	PolicyInput.bHasPublished = PairState.Publication.bHasPublished;
	PolicyInput.bDirty = Decision.bDirty;
	PolicyInput.bInitial = Decision.bInitial;
	PolicyInput.bResourceReadinessChanged = Decision.bResourceReadinessChanged;
	PolicyInput.bCaptureReadinessChanged = Decision.bCaptureReadinessChanged;
	PolicyInput.bMetricChanged = Decision.bMetricChanged;
	PolicyInput.bCubeContractChanged = Decision.bCubeContractChanged;
	PolicyInput.bLUTContractChanged = Decision.bLUTContractChanged;
	PolicyInput.bTransformChanged = Decision.bTransformChanged;
	PolicyInput.bTransitChanged = Decision.bTransitChanged;
	PolicyInput.bOwnershipChanged = Decision.bOwnershipChanged;
	PolicyInput.bCaptureGenerationChanged = bCaptureDiagnosticChanged;
	PolicyInput.SecondsSinceLastPublish = NowSeconds - PairState.Publication.LastPublishSeconds;
	const FWPPublishPolicyResult PolicyResult = EvaluateWPPublishPolicy(PolicyInput);
	Decision.bHeartbeat = PolicyResult.bHeartbeat;
	Decision.bShouldPublish = PolicyResult.bShouldPublish;
	Decision.bCameraDiagnosticCoalesced = bCameraDiagnosticChanged
		&& !Decision.bShouldPublish;
	Decision.bCaptureDiagnosticCoalesced = PolicyResult.bCaptureDiagnosticCoalesced;
	if (Decision.bCaptureDiagnosticCoalesced
		&& PairState.Publication.bCaptureGenerationObservationInitialized)
	{
		// uint32 subtraction intentionally preserves the modulo delta across generation wrap.
		const uint32 AdvanceA = Packet.CaptureGenerationA
			- PairState.Publication.LastObservedCaptureGenerationA;
		const uint32 AdvanceB = Packet.CaptureGenerationB
			- PairState.Publication.LastObservedCaptureGenerationB;
		Decision.CoalescedCaptureGenerationAdvanceCount =
			static_cast<uint64>(AdvanceA) + static_cast<uint64>(AdvanceB);
	}
	return Decision;
}

void FWPRuntimeRenderPublication::CommitPublishedState(
	FWPPortalPairState& PairState, const FWPRenderPacket& Packet,
	const FTransform& TransformA, const FTransform& TransformB,
	const FVector& CameraLocation,
	const FWPPublishDecision& PublishDecision,
	const double NowSeconds)
{
#if !UE_BUILD_SHIPPING
	const double CommitStartSeconds = FPlatformTime::Seconds();
#endif
	++Telemetry.PublishedPacketCount;
	const uint64 PreviousVisibilityPacketFloor =
		PairState.Capture.Visibility.RequiredPacketSequence;
	const bool bAdvanceVisibilityPacketFloor =
		ShouldAdvanceWPCaptureVisibilityPacketFloor(
			Packet.bCaptureVisibilityFeedbackEnabled,
			PublishDecision.bInitial,
			PublishDecision.bResourceReadinessChanged,
			PublishDecision.bMetricChanged,
			PublishDecision.bTransformChanged,
			PublishDecision.bOwnershipChanged);
	if (!Packet.bCaptureVisibilityFeedbackEnabled)
	{
		PairState.Capture.Visibility.RequiredPacketSequence = 0;
	}
	else if (bAdvanceVisibilityPacketFloor)
	{
		// CommitPublishedState is reached only after Renderer::UpdatePair succeeds.
		// Heartbeat/camera/capture-generation-only publishes deliberately retain
		// the previous floor, while every changed visibility contract waits for
		// a sample rendered from this exact-or-newer successful packet.
		PairState.Capture.Visibility.RequiredPacketSequence = Packet.PacketSequence;
	}
	if (PreviousVisibilityPacketFloor
		!= PairState.Capture.Visibility.RequiredPacketSequence)
	{
#if !UE_BUILD_SHIPPING
		WP_LOG(&Runtime, Verbose,
			TEXT("[Runtime][CaptureVisibilityPacketFloor] State changed. PairId=%s PreviousRequiredPacketSequence=%llu CurrentRequiredPacketSequence=%llu SuccessfulPublishedPacketSequence=%llu VisibilityFeedbackEnabled=%d ReasonInitial=%d ReasonResourceReadiness=%d ReasonMetric=%d ReasonTransform=%d ReasonOwnershipOrFeedbackToggle=%d HeartbeatOnly=%d CameraDiagnosticOnlyDoesNotAdvance=1 CaptureGenerationOnlyDoesNotAdvance=1 CpuMs=%.4f"),
			*PairIdToString(PairState.Identity.PairId),
			static_cast<unsigned long long>(PreviousVisibilityPacketFloor),
			static_cast<unsigned long long>(
				PairState.Capture.Visibility.RequiredPacketSequence),
			static_cast<unsigned long long>(Packet.PacketSequence),
			Packet.bCaptureVisibilityFeedbackEnabled ? 1 : 0,
			PublishDecision.bInitial ? 1 : 0,
			PublishDecision.bResourceReadinessChanged ? 1 : 0,
			PublishDecision.bMetricChanged ? 1 : 0,
			PublishDecision.bTransformChanged ? 1 : 0,
			PublishDecision.bOwnershipChanged ? 1 : 0,
			PublishDecision.bHeartbeat ? 1 : 0,
			(FPlatformTime::Seconds() - CommitStartSeconds) * 1000.0);
#endif
	}
	PairState.Publication.PacketSequence = Packet.PacketSequence;
	PairState.Publication.LastPublishedCaptureGenerationA = Packet.CaptureGenerationA;
	PairState.Publication.LastPublishedCaptureGenerationB = Packet.CaptureGenerationB;
	PairState.Publication.LastPublishedMetricRevisionA = Packet.MetricA.Revision;
	PairState.Publication.LastPublishedMetricRevisionB = Packet.MetricB.Revision;
	PairState.Publication.LastPublishedCubeContractA = Packet.CubeContractA;
	PairState.Publication.LastPublishedCubeContractB = Packet.CubeContractB;
	PairState.Publication.LastPublishedRayLUTContractA = Packet.RayLUTContractA;
	PairState.Publication.LastPublishedRayLUTContractB = Packet.RayLUTContractB;
	PairState.Publication.LastPublishedRayLUTRevisionA = Packet.RayLUTRevisionA;
	PairState.Publication.LastPublishedRayLUTRevisionB = Packet.RayLUTRevisionB;
	PairState.Publication.LastPublishedReferenceViewActorId = Packet.ReferenceViewActorId;
	PairState.Publication.LastPublishedTransitEventSequence = Packet.TransitEventSequence;
	PairState.Publication.LastPublishedOwnershipEpoch = Packet.OwnershipEpoch;
	PairState.Publication.LastPublishedRequestedOwnership = Packet.RequestedOwnership;
	PairState.Publication.LastPublishedEffectiveOwnership = Packet.EffectiveOwnership;
	PairState.Publication.LastPublishedStableSelectorNameA = Packet.StableSelectorNameA;
	PairState.Publication.LastPublishedStableSelectorNameB = Packet.StableSelectorNameB;
	PairState.Publication.LastPublishedTransformA = TransformA;
	PairState.Publication.LastPublishedTransformB = TransformB;
	PairState.Publication.LastPublishedCameraLocation = CameraLocation;
	PairState.Publication.LastPublishSeconds = NowSeconds;
	PairState.Publication.bHasPublished = true;
	PairState.Publication.bLastPublishedResourcesReady = Packet.bResourcesReady;
	PairState.Publication.bLastPublishedCaptureReady = Packet.bCaptureReady;
	PairState.Publication.bLastPublishedMetricCompatible = Packet.bMetricCompatible;
	PairState.Publication.bLastPublishedScaleSupported = Packet.bScaleSupported;
	PairState.Publication.bLastPublishedTransitActive = Packet.bTransitActive;
	PairState.Publication.bLastPublishedOwnershipEndpointAReady = Packet.bOwnershipEndpointAReady;
	PairState.Publication.bLastPublishedOwnershipEndpointBReady = Packet.bOwnershipEndpointBReady;
	PairState.Publication.bLastPublishedOwnershipInputsReady = Packet.bOwnershipInputsReady;
	PairState.Publication.bLastPublishedCaptureVisibilityFeedbackEnabled =
		Packet.bCaptureVisibilityFeedbackEnabled;
	PairState.Publication.bDirty = false;
}
