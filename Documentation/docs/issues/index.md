# Issues

This page lists current limitations and the first checks to perform when
Wormhole Portal does not behave as expected.

!!! info "Current compatibility target"

    Version 1.0 is officially tested and supported on Unreal Engine 5.8,
    Win64, DirectX 12, and SM6. See
    [Compatibility and Support](../support/index.md) before using another environment.

## Known limitations

| Area | Current limitation |
| --- | --- |
| Supported release environment | Unreal Engine 5.8, Win64, DirectX 12, and SM6. Other environments are not declared supported. |
| Rendering views | Portal compositing supports one primary, perspective, non-stereo Game View. |
| Unsupported views | Scene Capture, Reflection Capture, Planar Reflection, orthographic, stereo/VR, split-screen, and other multi-view families do not composite the portal. |
| Movie Render Queue | The main deferred mono beauty output is supported. Custom passes, Virtual Texture passes, stereo, and multi-view outputs omit the composite. |
| Server targets | The renderer module is excluded from Server targets. Do not expect portal visuals on a dedicated server. |
| Portal scale | Both linked Portal Actors must use Actor Scale `(1, 1, 1)`. Use metric or visual-scale APIs instead. |
| Transit primitives | Transit requires actor-owned, Movable, collision-enabled Primitive Components. Instanced Static Mesh Components are unsupported. |
| Physics transit | At least one non-instanced Static Mesh Component with a valid mesh, or supported Shape Component, must have Physics collision enabled and be simulating. |
| Voxel collision | Baking supports non-instanced Static Mesh Simple Collision plus Box, Sphere, and Capsule Components. Static Mesh elements may be Sphere, Box, Capsule, or Convex. |
| Portal traces | Public Blueprint APIs provide Line Trace By Channel operations. Object Type, Profile, and Sweep variants are not implemented. |
| Ray transform | `Transform Ray Through Portal` accepts a ray approaching the portal from outside toward its interior. |
| Trace exit offset | Portal Exit Offset prevents immediate self-hit but does not consume remaining distance. The offset gap is not collision-tested. |
| Audio | Portal audio supports one primary listener, one portal hop, and eligible active spatialized 3D Audio Components. |
| Lighting | Only Point and Spot Lights are transmitted. Directional Lights may be collected internally but are not transmitted. |

## The portal is not visible

Check in order:

1. The Portal Actor has a valid **Linked Portal**.
2. Both endpoints use Actor Scale `(1, 1, 1)`.
3. The project is running Win64 with DirectX 12 and SM6, using a primary
   Perspective Game View.
4. The view is not a Scene Capture, reflection, stereo/VR, or split-screen
   family.
5. If **Transition Length** is greater than zero, wait for the asynchronous
   runtime fallback or bake a compatible LUT.
6. Filter **Output Log** by `LogWormhole` and inspect Renderer entries for the
   `Reason=...` field. To include unsupported-view result changes, temporarily
   run `Log LogWormhole VeryVerbose` before reproducing the issue.

Typical renderer reasons include `InvalidPairOrTransform`, `MetricInvalid`,
`MetricMismatch`, `UnsupportedScale`, `CaptureNotSubmitted`,
`MissingTextureReference`, `UnresolvedReferencedTexture`, and
`WrongTextureDimension`.

Unsupported-view reasons include `SceneCaptureOrReflection`,
`UnsupportedGameProjectionOrFeatureLevel`, `StereoView`,
`NotExactlyOneFamilyView`, and `NonPrimaryPlayerView`.

## The WPPortalTrace warning keeps appearing

Wormhole Portal requires a dedicated Trace Channel named `WPPortalTrace`. Use
**Add Automatically** in the startup notification when possible, then restart
the Editor.

| Problem | Resolution |
| --- | --- |
| Wrong channel type | Make sure `WPPortalTrace` is a **Trace Channel**, not an Object Channel. |
| Channel mismatch | Align **Portal Trace Channel** in plugin settings with the Game Trace Channel assigned by Unreal. |
| No free Game channel | Free an unused Game Trace Channel before retrying. |
| Read-only configuration | Check out or make `Config/DefaultEngine.ini` writable. |
| Manual setup | Create `WPPortalTrace` with **Default Response: Ignore**, align the plugin setting, and restart. |

Do not pass `WPPortalTrace` as the ordinary collision channel for a
portal-aware trace. It is reserved for internal portal detection.

## LUT baking fails

Open **Tools > Wormhole Portal > Bake All LUTs**, then check:

- an Editor World and at least one Portal Actor exist;
- **Generated LUT Asset Path** is a valid Unreal Long Package Directory with
  the case-sensitive prefix `/Game/`;
- the portal metric is valid;
- the requested `Transition Length / Portal Radius` range is inside the chosen
  LUT domain;
- source control permits generated packages to be checked out and saved; and
- the bake was not cancelled.

The code-default generated directory is:

```text
/Game/WormholePortal/Generated/LUT
```

Installed-plugin paths such as `/WormholePortal/...` are not valid generated
LUT locations. A success notification begins with `LUT ready:`. A baked LUT is
optional while **Allow Runtime LUT Fallback** is enabled; `Transition Length =
0` uses the analytic path and needs no LUT.

## The LUT works in Editor but not in a packaged build

The Editor module attempts to synchronize **Generated LUT Asset Path** with
`DirectoriesToAlwaysCook`. If it reports that the project configuration could
not be updated:

1. Check out or make the reported config file writable.
2. Restart the Editor so startup synchronization runs again, or add the path
   manually under **Project Settings > Packaging**.
3. Confirm the configured path appears in **Additional Asset Directories to
   Cook**.

The default entry is:

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysCook=(Path="/Game/WormholePortal/Generated/LUT")
```

For a custom path, use the validated `/Game/...` value exactly.

## Runtime metric changes are rejected

`Set Portal Radius`, `Set Throat Half Length`, and `Set Transition Length` are
independent authoring/pre-BeginPlay setters. Calls after BeginPlay are rejected
to prevent the three physical values from drifting apart.

At runtime:

1. call **Initialize Physical Metric** once with the complete `ρ`, `a`, and `T`
   shape;
2. use **Set Uniform Physical Metric Scale** for a ratio-preserving physical
   resize; or
3. use **Set Portal Visual Scale** for inexpensive render-only animation.

A later **Initialize Physical Metric** call is accepted only if it preserves
the original `a/ρ` and `T/ρ` ratios. **Set Metric Parameters** is a deprecated
alias and should not be used in new work.

## Transit does not start

Open **Tools > Wormhole Portal > Transit Manager** and inspect the exact Actor
status.

| Status | What to check |
| --- | --- |
| `Not Supported` | The Actor does not match Character, Projectile, Pawn, or Physics requirements. |
| `Needs Setup: Transit is disabled` | Enable **Transit Enabled** on its `WPTransitComponent`. |
| `Needs Setup: Transit settings are invalid` | Review the selected Transit Type and reported components. |
| `Needs Setup: No movable collision Primitive was found` | Add an actor-owned, Movable, collision-enabled Primitive that can overlap the portal trigger. |
| `Needs Setup: No supported Primitive is simulating Physics.` | For Physics transit, enable Physics collision and simulation on a supported non-instanced Static Mesh or Shape Component. |
| `Needs Setup: Bake Voxel Body for a supported Primitive` | Enable voxel collision and bake data for the indicated supported Primitive. |
| `Needs Setup: Bake Voxel Body for <asset/component names>` | Bake or repair voxel data for every name reported by Transit Manager. |

Runtime rejection details are Verbose and compiled only in non-Shipping builds.
Before reproducing, enter `Log LogWormhole Verbose` in the console or launch
with `-LogCmds="LogWormhole Verbose"`.

```text
[Transit][Rejected] TimestampSeconds=... RuntimeReason=EWPTransitFailReason::... ResolveReason=EWPTransitResolveFailReason::... FailedComponents=... Actor=... SourcePortal=... DestinationPortal=... TransitType=EWPTransitType::...
```

- `RuntimeReason` identifies the transit lifecycle failure.
- `ResolveReason` identifies the setup resolver failure.
- `FailedComponents` names the components implicated by resolution.

`DoesNotFitGate` means the Actor's projected cross-section perpendicular to its
movement does not fit the source portal gate/core radius. Increase **Portal
Radius** or correct/reduce the Actor collision bounds; never scale the Portal
Actor.

Possible runtime reasons are listed in full in
[Reference](../reference.md#transit-component). With World Partition,
`NotReady` can mean the linked area has not finished streaming. Keep the Actor
outside the boundary until `IsLinkedPortalAreaReady()` returns `true`.

## Voxel Body baking fails

Verify that:

1. **Use Voxel Collision** is enabled on `WPTransitComponent`.
2. Each participating Primitive is actor-owned and non-instanced.
3. A Static Mesh is saved and has usable Simple Collision, or the component is
   a supported Box, Sphere, or Capsule Component.
4. Static Mesh Simple Collision uses Sphere, Box, Capsule, or Convex elements.
5. **Voxel Size** is greater than zero.
6. **Max Voxel Count** is `1–512` and is sufficient for each Primitive.

**Max Voxel Count** applies per baked Primitive, not once per Static Mesh or
Actor. Tapered capsules, level sets, skinned triangle collision, instanced
meshes, and other unsupported collision types cannot be baked.

The default generated Voxel directory is
`/Game/WormholePortal/Generated/Voxels`. A custom Voxel directory must be a
valid Long Package Directory under the case-sensitive `/Game/` prefix.

## Dynamic cubemap resolution is unexpected

Project configuration can override the plugin defaults. Inspect **Lowest
Visible Resolution**, every **Resolution Tier**, and **Inside SafeProxy
Resolution** under the plugin's Scene Capture settings.

At runtime, tier thresholds clamp to `0–100%` and are stable-sorted. Resolutions
round up to an 8-pixel multiple in `8–2048`, tiers cannot decrease below the
previous effective tier, and inside resolution cannot fall below the lowest
resolution. An empty tier list uses the lowest value outside the safe proxy.

There is no plugin-local memory-budget downgrade. Use `stat WormholePortal` to
observe cubemap memory; edit the resolution policy to reduce it.

## Material clipping does not work

Material clipping is optional and does not modify arbitrary materials.

Check that **Use Material Clip** is enabled, the material uses the supplied
transit clip Material Function, the material and Project Settings use the same
**Clip Base Index**, and four consecutive Custom Primitive Data slots beginning
at that index are free. The default index is `28`.

## Portal audio is not heard

Portal audio needs an eligible active 3D Audio Component. Check that:

- spatialization and spatialized attenuation are enabled;
- the sound is not a UI/2D sound or Source Bus asset;
- the component is registered, playing, and not configured for multiple
  simultaneous instances;
- its attenuation range reaches the portal;
- the source is outside the portal sphere; and
- neither component nor owner has `WP.PortalAudio.Disabled`.

Generated proxies are excluded from retransmission, so audio currently travels
through one portal hop.

## Portal lighting is not visible { #portal-lighting }

Only Point and Spot Lights are transmitted. Check that the source is enabled,
affects the World, has nonzero intensity, reaches the entry aperture, is not
inside the portal, and is not tagged `WP.PortalLight.Disabled`. Both endpoints
must use unit scale and matching radii.

For source shadows, also enable **Enable Portal Source Shadows**, **Cast
Shadows**, and **Cast Dynamic Shadows**. Directional and Rect Lights,
asymmetric radii, lights inside a portal, GI, reflections, volumetrics,
translucent shadows, and colored shadows are not transmitted.

## Diagnostics to collect

Before requesting support, collect:

- Unreal Engine version and plugin version;
- target platform and whether the renderer is present;
- PIE, SIE, Standalone, packaged, or Movie Render Queue context;
- the smallest reproduction steps;
- Output Log entries filtered by `LogWormhole`;
- the full non-Shipping Verbose transit rejection line, when applicable;
- screenshots of the Portal Actor and relevant component/settings panels; and
- `stat WormholePortal` output for rendering or capture problems.

Use the submission checklist on the [Support page](../support/index.md) so a report can
be reproduced without a second round of basic questions.
