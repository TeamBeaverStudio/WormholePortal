// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "Stats/Stats.h"

/**
 * Runtime profiling group enabled with `stat WormholePortal`.
 *
 * Cycle stats report CPU submission/setup time on the thread named in the label.
 * GPU execution remains visible through `stat gpu` / ProfileGPU; the workload
 * counters below make the portal-generated GPU demand visible in this group.
 */
DECLARE_STATS_GROUP_VERBOSE(TEXT("Wormhole Portal"), STATGROUP_WormholePortal, STATCAT_Advanced);

// Game thread cycles.
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Cube Capture Initialize"), STAT_WP_CubeCaptureInitialize, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Cube Capture Submit CPU"), STAT_WP_CubeCaptureSubmit, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Registry Change Dispatch"), STAT_WP_RegistryChangeDispatch, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Runtime Tick"), STAT_WP_RuntimeTick, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Capture Scheduler Runtime Submit"), STAT_WP_CaptureSchedulerRuntimeSubmit, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Portal Pair Rebuild"), STAT_WP_PairRebuild, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Render Packet Build"), STAT_WP_RenderPacketBuild, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] Render Packet Publish"), STAT_WP_RenderPacketPublish, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] LUT Request / Resolve"), STAT_WP_LUTRequest, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] LUT Asset Completion"), STAT_WP_LUTAssetLoadComplete, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[GT] LUT Fallback Upload"), STAT_WP_LUTFallbackUpload, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);

// Worker/render thread cycles.
DECLARE_CYCLE_STAT_EXTERN(TEXT("[Worker] LUT Fallback Build"), STAT_WP_LUTFallbackBuild, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] Render Packet Apply"), STAT_WP_RenderPacketApply, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] View Filter"), STAT_WP_ViewFilter, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] Visibility Observation"), STAT_WP_VisibilityObservation, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] Post Process Callback"), STAT_WP_PostProcessCallback, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] RDG Pass Setup"), STAT_WP_RDGPassSetup, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("[RT] Cube AA Pass Setup"), STAT_WP_CubeAASetup, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);

// Per-frame workload/current-state counters.
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Portal Actors Ticked"), STAT_WP_PortalActorsTicked, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cube Capture Targets"), STAT_WP_CubeCaptureTargets, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Cube RT Color Memory (MiB)"), STAT_WP_CubeRenderTargetMiB, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cube Captures Submitted"), STAT_WP_CubeCapturesSubmitted, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Cube Capture Work (MPix)"), STAT_WP_CubeCaptureMegapixels, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cube AA Passes Submitted"), STAT_WP_CubeAAPassesSubmitted, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Pairs Submitted"), STAT_WP_CaptureRuntimePairsSubmitted, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Endpoints Submitted"), STAT_WP_CaptureRuntimeEndpointsSubmitted, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Atomic Submissions"), STAT_WP_CaptureRuntimeAtomicSubmissions, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Staggered Submissions"), STAT_WP_CaptureRuntimeStaggeredSubmissions, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Faces Submitted"), STAT_WP_CaptureRuntimeFacesSubmitted, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Capture Runtime Rollbacks"), STAT_WP_CaptureRuntimeRollbacks, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Registry Change Events"), STAT_WP_RegistryChangeEvents, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Active Portal Pairs"), STAT_WP_ActivePairs, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Render Packets Published"), STAT_WP_RenderPacketsPublished, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Render Packets Applied"), STAT_WP_RenderPacketsApplied, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Render Packets Dropped"), STAT_WP_RenderPacketsDropped, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("LUT Requests"), STAT_WP_LUTRequests, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("LUT Ready Cache Hits"), STAT_WP_LUTCacheHits, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("LUT Asset Loads"), STAT_WP_LUTAssetLoads, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("LUT Fallback Starts"), STAT_WP_LUTFallbackStarts, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Accepted Main Views"), STAT_WP_AcceptedViews, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Scene Capture Views Skipped"), STAT_WP_SceneCaptureViewsSkipped, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Production Composite Passes Submitted"), STAT_WP_ProductionCompositePasses, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Composite Work (MPix)"), STAT_WP_CompositeMegapixels, STATGROUP_WormholePortal, WORMHOLEPORTALRUNTIME_API);
