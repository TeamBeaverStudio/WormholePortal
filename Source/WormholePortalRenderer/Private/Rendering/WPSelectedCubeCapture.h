// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class ISceneRenderBuilder;
class USceneCaptureComponentCube;

/**
 * Adds one selected-face Cubemap capture to an Engine SceneRenderBuilder.
 *
 * This renderer-only implementation mirrors the native SceneCaptureCube single-pass
 * topology while keeping all Engine source files untouched. Every selected face becomes
 * one View in a single ViewFamily and a single SceneRenderer. The renderer draws into a
 * fixed 3x2 atlas and copies only the selected tiles to the persistent Cubemap slices.
 *
 * @param CaptureComponent Registered Cube component whose settings, view states, target,
 *        transform, hidden primitives, post-process settings, and ShowFlags are reused.
 * @param SelectedFaceMask Low six bits selecting +X, -X, +Y, -Y, +Z, and -Z.
 * @param SceneRenderBuilder Existing World builder that owns and later executes the new
 *        SceneRenderer. The caller remains responsible for Execute().
 * @return True only when exactly one SceneRenderer was staged successfully.
 */
bool AddWPSelectedCubeCaptureRenderer(
	USceneCaptureComponentCube& CaptureComponent,
	uint8 SelectedFaceMask,
	ISceneRenderBuilder& SceneRenderBuilder);


