// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

#include "Transit/WPTransitTypeResolver.h"

class AActor;
class UWPTransitComponent;

/**
 * @brief Actor-check options used only by the Transit Manager.
 *
 * FWPTransitTypeResolver evaluates Runtime support; the Editor adds only the
 * project-specific exclusion list.
 */
struct FWPTransitManagerOptions
{
	/** @brief Actor classes excluded from the Transit Manager list and automatic application. */
	TArray<TSubclassOf<AActor>> ExcludedClasses;
};

/**
 * @brief Stores the latest resolve state for one actor in the Transit Manager.
 *
 * Holds the actor through a weak reference because it may be removed during Editor
 * operations.
 */
struct FWPTransitManagerActorEntry
{
	/** @brief Actor displayed in the list and managed for Transit application. */
	TWeakObjectPtr<AActor> Actor;

	/** @brief Latest Transit resolve result computed with the same criteria as Runtime. */
	FWPTransitResolveResult ResolveResult;

	/** @brief User-facing status text generated from ResolveResult. */
	FText StatusText;

	/** @brief Whether ResolveResult and StatusText were recorded by the current check. */
	bool bWasChecked = false;
};

/** @brief Shared-pointer alias for a Transit Manager actor entry. */
using FWPTransitManagerActorEntryPtr = TSharedPtr<FWPTransitManagerActorEntry>;

/**
 * @brief Checks actors in the Editor World and adds or removes Transit Components.
 *
 * Handles actor collection, shared Runtime resolution, and undoable Component changes
 * independently of the Slate UI.
 */
class FWPTransitManagerActorController
{
public:
	/**
	 * @brief Rebuilds the actor-entry list from the current Editor World.
	 * @param bRunCheck Whether to resolve Transit conditions while rebuilding the list.
	 * @param Options Editor-only excluded-class settings.
	 */
	void RebuildActorEntries(bool bRunCheck, const FWPTransitManagerOptions& Options);

	/**
	 * @brief Rechecks every actor in the current list using the shared Runtime criteria.
	 * @param Options Editor-only excluded-class settings.
	 */
	void CheckAll(const FWPTransitManagerOptions& Options);

	/**
	 * @brief Rechecks and bulk-applies Transit to actors that were Ready in the latest
	 *        check and have no existing Transit Component.
	 * @param Options Editor options used for the pre-apply recheck.
	 */
	void ApplyReadyActors(const FWPTransitManagerOptions& Options);

	/**
	 * @brief Changes one actor's Transit Component application state through an undoable
	 *        operation.
	 * @param Entry Actor-list entry to change.
	 * @param bEnabled Adds or enables the Component when true; removes or disables it when
	 *                 false.
	 * @param Options Editor options used to resolve state before and after the change.
	 */
	void SetTransitEnabled(const FWPTransitManagerActorEntryPtr& Entry, bool bEnabled, const FWPTransitManagerOptions& Options);

	/**
	 * @brief Returns the currently collected actor-entry list.
	 * @return All actor entries available to the Transit Manager.
	 */
	const TArray<FWPTransitManagerActorEntryPtr>& GetActorEntries() const;

private:
	/**
	 * @brief Finds a Transit Component on the actor, preferring an enabled one.
	 * @param Actor Actor to inspect.
	 * @return Enabled Component when available; otherwise, the first disabled Component.
	 */
	static UWPTransitComponent* FindTransitComp(AActor* Actor);

	/**
	 * @brief Checks whether an actor matches the Editor-only excluded-class list.
	 * @param Actor Actor to inspect.
	 * @param Options Excluded-class settings.
	 * @return true when the actor is excluded.
	 */
	static bool IsExcluded(const AActor* Actor, const FWPTransitManagerOptions& Options);

	/**
	 * @brief Converts a Runtime resolve result to concise text for the list.
	 * @param Result Shared resolve result to convert.
	 * @return Ready or text describing the required setup.
	 */
	static FText GetStatusText(const FWPTransitResolveResult& Result);

	/** @brief All actor entries collected from the current Editor World. */
	TArray<FWPTransitManagerActorEntryPtr> ActorEntries;

	/** @brief Clears or refreshes an actor entry's resolve result according to bRunCheck. */
	void RefreshActorEntry(const FWPTransitManagerActorEntryPtr& Entry, bool bRunCheck, const FWPTransitManagerOptions& Options);

	/** @brief Resolves an actor entry against the exclusion settings and Runtime Resolver criteria. */
	void CheckActorEntry(const FWPTransitManagerActorEntryPtr& Entry, const FWPTransitManagerOptions& Options);

	/** @brief Adds an instance Transit Component to a Ready actor and configures it with the resolved TransitType. */
	void AddTransitComponent(const FWPTransitManagerActorEntryPtr& Entry, const FWPTransitResolveResult& ResolveResult, const FWPTransitManagerOptions& Options);

	/**
	 * @brief Removes Components added directly to the actor instance and disables
	 *        Components inherited from its Class.
	 * @param Entry Actor entry from which to remove Transit application.
	 */
	void DisableTransitComponents(const FWPTransitManagerActorEntryPtr& Entry);

	/** @brief Returns the current Editor World inspected by the Transit Manager. */
	UWorld* GetEditorWorld() const;
};
