// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "WPPortalTraceChannelConfig.h"
#include "Containers/Ticker.h"

class FSpawnTabArgs;
class SDockTab;
struct FPropertyChangedEvent;

class FWormholePortalEditor : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	
	/** @brief Unique name that identifies the Transit Manager tab in the Global Tab Manager. */
	static const FName TransitManagerTabName;

private:
	/** These functions should eventually be extracted into a dedicated Portal Trace Channel notification class. */
	void ValidatePortalTraceChannel();
	void ShowPortalTraceChannelNotification(FWPPortalTraceChannelConfig::EStatus Status);
	void ShowCompletionNotification(const FText& Message, SNotificationItem::ECompletionState CompletionState) const;
	void AddPortalTraceChannelToProjectConfig();
	void OpenCollisionProjectSettings() const;
	
	void StartPortalTraceValidationTicker();
	void StopPortalTraceValidationTicker();
	bool TickPortalTraceValidation(float DeltaTime);
	void DismissPortalTraceChannelNotification();

	void RegisterLUTSettingsSync();
	void UnregisterLUTSettingsSync();
	void HandleLUTSettingsChanged(UObject* SettingsObject, FPropertyChangedEvent& PropertyChangedEvent);
	bool SyncLUTCookPath(bool bNotifyFailure);
	void ResetLUTCacheForPathChange() const;
	
	// TODO: If the user changes the Portal Trace Channel manually, update UWPSettings to match or display a notification.
	
	
	/**
	 * @brief Registers the Wormhole Portal Transit Manager command in the Level Editor's Tools menu.
	 *
	 * Uses FToolMenuOwnerScoped to assign ownership of the menu entry to this module.
	 * ShutdownModule removes all entries registered under that owner.
	 */
	void RegisterTransitManagerMenus();
	
	void RegisterTransitDetails();
	void UnregisterTransitDetails();

	/** Registers or unregisters the Component Visualizer that displays metric HUD labels for selected portals. */
	void RegisterPortalDebugVisualizer();
	void UnregisterPortalDebugVisualizer();

	/**
	 * @brief Creates the dockable tab and SWPTransitManagerWidget when the Transit Manager command is invoked.
	 * @param Args Tab-spawn information supplied by the Global Tab Manager; currently unused.
	 * @return Nomad tab containing the Transit Manager widget.
	 */
	TSharedRef<SDockTab> SpawnTransitManagerTab(const FSpawnTabArgs& Args);
	
private:
	TWeakPtr<SNotificationItem> PortalTraceChannelNotification;
	FTSTicker::FDelegateHandle PortalTraceValidationTickerHandle;
	FDelegateHandle LUTSettingsChangedHandle;
	FString LastSynchronizedLUTCookPath;
};
