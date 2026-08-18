// Copyright 2026 Team Beaver. All Rights Reserved.

#include "WormholePortalStats.h"

#if STATS
#include "Misc/CoreDelegates.h"
#include "Stats/StatsSystemTypes.h"
#endif

DEFINE_STAT(STAT_WP_CubeCaptureInitialize);
DEFINE_STAT(STAT_WP_CubeCaptureSubmit);
DEFINE_STAT(STAT_WP_RegistryChangeDispatch);
DEFINE_STAT(STAT_WP_RuntimeTick);
DEFINE_STAT(STAT_WP_CaptureSchedulerRuntimeSubmit);
DEFINE_STAT(STAT_WP_PairRebuild);
DEFINE_STAT(STAT_WP_RenderPacketBuild);
DEFINE_STAT(STAT_WP_RenderPacketPublish);
DEFINE_STAT(STAT_WP_LUTRequest);
DEFINE_STAT(STAT_WP_LUTAssetLoadComplete);
DEFINE_STAT(STAT_WP_LUTFallbackUpload);
DEFINE_STAT(STAT_WP_LUTFallbackBuild);
DEFINE_STAT(STAT_WP_RenderPacketApply);
DEFINE_STAT(STAT_WP_ViewFilter);
DEFINE_STAT(STAT_WP_VisibilityObservation);
DEFINE_STAT(STAT_WP_PostProcessCallback);
DEFINE_STAT(STAT_WP_RDGPassSetup);
DEFINE_STAT(STAT_WP_CubeAASetup);

DEFINE_STAT(STAT_WP_PortalActorsTicked);
DEFINE_STAT(STAT_WP_CubeCaptureTargets);
DEFINE_STAT(STAT_WP_CubeRenderTargetMiB);
DEFINE_STAT(STAT_WP_CubeCapturesSubmitted);
DEFINE_STAT(STAT_WP_CubeCaptureMegapixels);
DEFINE_STAT(STAT_WP_CubeAAPassesSubmitted);
DEFINE_STAT(STAT_WP_CaptureRuntimePairsSubmitted);
DEFINE_STAT(STAT_WP_CaptureRuntimeEndpointsSubmitted);
DEFINE_STAT(STAT_WP_CaptureRuntimeAtomicSubmissions);
DEFINE_STAT(STAT_WP_CaptureRuntimeStaggeredSubmissions);
DEFINE_STAT(STAT_WP_CaptureRuntimeFacesSubmitted);
DEFINE_STAT(STAT_WP_CaptureRuntimeRollbacks);
DEFINE_STAT(STAT_WP_RegistryChangeEvents);
DEFINE_STAT(STAT_WP_ActivePairs);
DEFINE_STAT(STAT_WP_RenderPacketsPublished);
DEFINE_STAT(STAT_WP_RenderPacketsApplied);
DEFINE_STAT(STAT_WP_RenderPacketsDropped);
DEFINE_STAT(STAT_WP_LUTRequests);
DEFINE_STAT(STAT_WP_LUTCacheHits);
DEFINE_STAT(STAT_WP_LUTAssetLoads);
DEFINE_STAT(STAT_WP_LUTFallbackStarts);
DEFINE_STAT(STAT_WP_AcceptedViews);
DEFINE_STAT(STAT_WP_SceneCaptureViewsSkipped);
DEFINE_STAT(STAT_WP_ProductionCompositePasses);
DEFINE_STAT(STAT_WP_CompositeMegapixels);

#if STATS
namespace
{
	class FWormholePortalStatCommandBridge final
	{
	public:
		FWormholePortalStatCommandBridge()
		{
			StatManager = &IStatGroupEnableManager::Get();
			// Register the verbose group before the first console command. The
			// high-performance manager ignores enable requests for unknown groups.

			StatEnabledHandle = FCoreDelegates::StatEnabled.AddStatic(&HandleStatEnabled);
			StatDisabledHandle = FCoreDelegates::StatDisabled.AddStatic(&HandleStatDisabled);
			StatDisableAllHandle = FCoreDelegates::StatDisableAll.AddStatic(&HandleStatDisableAll);
		}

		~FWormholePortalStatCommandBridge()
		{
			FCoreDelegates::StatEnabled.Remove(StatEnabledHandle);
			FCoreDelegates::StatDisabled.Remove(StatDisabledHandle);
			FCoreDelegates::StatDisableAll.Remove(StatDisableAllHandle);

			SetCollectionEnabled(false);
			StatManager = nullptr;
		}

	private:
		static bool IsWormholePortalStat(const TCHAR* InName)
		{
			return InName != nullptr
				&& FCString::Stricmp(InName, TEXT("WormholePortal")) == 0;
		}

		static void SetCollectionEnabled(bool bEnabled)
		{
			if (StatManager != nullptr)
			{
				StatManager->SetHighPerformanceEnableForGroup(
					FName(TEXT("STATGROUP_WormholePortal")),
					bEnabled);
			}
		}

		static void HandleStatEnabled(const TCHAR* InName)
		{
			if (IsWormholePortalStat(InName))
			{
				SetCollectionEnabled(true);
			}
		}

		static void HandleStatDisabled(const TCHAR* InName)
		{
			if (IsWormholePortalStat(InName))
			{
				SetCollectionEnabled(false);
			}
		}

		static void HandleStatDisableAll(bool /*bInAnyViewport*/)
		{
			SetCollectionEnabled(false);
		}

		inline static IStatGroupEnableManager* StatManager = nullptr;
		FDelegateHandle StatEnabledHandle;
		FDelegateHandle StatDisabledHandle;
		FDelegateHandle StatDisableAllHandle;
	};

	FWormholePortalStatCommandBridge WormholePortalStatCommandBridge;
}
#endif
