# Compatibility, Releases, and Support

Wormhole Portal is published by **Team Beaver Studio**. This page defines the
officially tested environment for version 1.0, records the release scope, and
explains how to submit a reproducible support request.

## Compatibility matrix

The official tested and supported configuration for version 1.0 is:

| Item | Supported target |
| --- | --- |
| Plugin version | `1.0` |
| Unreal Engine | `5.8` |
| Host/packaged platform | `Win64` |
| Rendering API | `DirectX 12` |
| Shader model | `SM6` |
| Portal render view | One primary, perspective, non-stereo Game View |

Linux, macOS, DirectX 11, mobile, consoles, and dedicated-server targets are
not declared supported for version 1.0. The renderer module is excluded from
Server targets, so dedicated servers do not render portal visuals.

Stereo/VR, split-screen, orthographic, Scene Capture, Reflection Capture,
Planar Reflection, and other multi-view families do not composite the portal.
Movie Render Queue supports the main deferred mono beauty output; see
[Issues](../issues/index.md#known-limitations) for the complete view limitations.

An environment outside this matrix may or may not run, but it has not been
declared supported. Do not make a production commitment until your exact engine
build, RHI, hardware, packaging settings, and gameplay path have been validated.

## Required plugin dependencies

The plugin descriptor enables these Unreal Engine plugins:

- **Enhanced Input**;
- **StateTree**; and
- **Gameplay StateTree**.

Enhanced Input supports the included sample controls. The demo's shooter
variant contains StateTree-based AI content. Keep the declared dependencies
available and enabled when loading the complete plugin and demo.

The runtime renderer is not part of a Server target. C++ gameplay modules that
use public Wormhole Portal types should add `WormholePortalRuntime` to their
module dependencies; see [Integration examples](../reference.md#integration-examples).

## Version and release notes

### Version 1.0 — Unreal Engine 5.8

Initial Fab release scope:

- physically inspired volumetric linked-portal rendering;
- linked metric authoring, physical scale, and render-only visual scale;
- Character, Pawn, Projectile, and Physics transit;
- optional Material Clip and baked Voxel Collision;
- portal-aware line traces;
- portal audio and Point/Spot light transmission;
- World Partition destination preloading;
- Transit Manager, LUT baking, diagnostics, and included demo content; and
- Blueprint and C++ runtime interfaces.

Before adopting the release, review [Known limitations](../issues/index.md#known-limitations),
especially the supported view, platform, Physics, Voxel, audio, and lighting
boundaries.

## Upgrade checklist

When installing a replacement package or a later compatible build:

1. Commit or back up the project, configuration, and any copied sample content.
2. Close Unreal Editor and remove/disable the old plugin version through the
   normal launcher or project workflow.
3. Install the package that matches the project's supported Unreal Engine
   version.
4. Confirm Enhanced Input, StateTree, and Gameplay StateTree dependencies are
   enabled.
5. Restart the Editor and resolve any `WPPortalTrace` startup notification.
6. Confirm Portal Actors remain at unit Transform Scale and linked pairs retain
   their metric values.
7. Re-bake LUT data when the LUT schema/domain or portal metrics changed.
8. Re-bake Voxel data after collision or participating Primitive changes.
9. Verify `/Game/WormholePortal/Generated/LUT` in **Additional Asset Directories to
   Cook**.
10. Run PIE, Standalone, and a Win64 DX12 SM6 packaged-build smoke test.

Do not overwrite the only copy of production content inside the installed demo
folder. Duplicate adapted sample assets into the project's own `/Game/...`
content first.

## Before requesting support

Check [Getting Started](../getting-started/index.md), [FAQ](../faq/index.md), and
[Issues](../issues/index.md) first. Then prepare:

- plugin version and the source of the installed package;
- exact Unreal Engine version, including any source-build changes;
- Windows version, GPU, driver, `Win64`, `DX12`, and `SM6` status;
- PIE, SIE, Standalone, packaged, or Movie Render Queue context;
- the smallest deterministic reproduction steps;
- whether the included demo reproduces the issue;
- relevant Blueprint screenshots or a minimal C++ call site;
- Portal Actor, Transit Component, and Project Settings screenshots;
- Output Log entries filtered by `LogWormhole`;
- the full non-Shipping Verbose transit rejection line when applicable; and
- `stat WormholePortal` output for render/capture reports.

Remove passwords, access tokens, licensed third-party assets, personal data, and
other secrets before attaching logs or a reproduction project.

## Contact and response scope

The official support contact for this release is
[beavergametech@gmail.com](mailto:beavergametech@gmail.com). Use a concise
subject such as `Wormhole Portal 1.0 / UE 5.8 / <short issue>` and include the
checklist above in the first message.

Support is provided for reproducible plugin defects and documented workflows in
the official compatibility matrix. The following normally require project-side
engineering or validation rather than a plugin defect fix:

- unsupported engine versions, platforms, RHIs, shader models, or view types;
- general Unreal Engine installation, packaging, networking, or source-build
  issues unrelated to Wormhole Portal;
- custom gameplay, material, AI, rendering, or build-system implementation;
- third-party plugin conflicts without a minimal reproduction; and
- performance targets without a representative scene, capture policy, and
  profiling data.

No response-time or future-version guarantee is implied by this documentation.
Use the Fab product page to confirm the currently published package and listing
information before downloading or updating.

## What support covers

Useful reports can lead to documentation corrections, reproducible defect
investigation, configuration guidance, or a workaround within the declared
release scope. A complete report does not guarantee a particular resolution,
but it allows Team Beaver Studio to distinguish a plugin defect from an
unsupported environment or project-specific integration issue quickly.
