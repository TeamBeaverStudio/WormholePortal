# Reference

This page summarizes the public authoring properties, Blueprint nodes, C++
interfaces, settings, defaults, and diagnostics provided by Wormhole Portal.

!!! note "Defaults and project overrides"

    Values on this page are plugin code defaults. Values serialized in a
    project's configuration or on an Actor override them for that project.

## Portal Actor { #portal-actor }

`AWormholePortalActor` represents one endpoint. Setting **Linked Portal** on one
Actor establishes the reciprocal link and copies that Actor's metric to both
endpoints.

Keep Portal Actor Transform Scale at `(1, 1, 1)`. Change the physical metric or
render-only visual scale through the explicit APIs below.

### Authoring properties

| Property | Code default | Constraint and purpose |
| --- | ---: | --- |
| **Linked Portal** | None | Destination endpoint. A portal cannot link to itself. |
| **Portal Radius** (`ρ`) | `50 cm` | Seam and traversal-gate radius; minimum `1 cm`. Constrained by the active LUT domain when `T > 0`. |
| **Throat Half Length** (`a`) | `100 cm` | Distance from the seam to the mouth; minimum `0`. |
| **Transition Length** (`T`) | `200 cm` | Distance from the mouth to flat space; minimum `0`. `0` uses the analytic no-transition path. |
| **LUT Asset Override** | None | Optional per-instance replacement for the project LUT catalog. |
| **Streaming Preload Distance** | `15,000 cm` | Starts destination loading in World Partition. |
| **Streaming Release Distance** | `20,000 cm` | Keeps the request active; clamped to at least the preload distance. |
| **Streaming Query Interval** | `0.2 s` | Proximity reevaluation interval; minimum `0.01 s`. |
| **Draw Portal Debug** | On in Editor | Draws seam, mouth, and transition boundaries. Forced off when PIE begins. |

```text
Mouth Radius      = Portal Radius + Throat Half Length
Transition Radius = Mouth Radius + Transition Length
LUT ratio         = Transition Length / Portal Radius
```

### Link and query Blueprint API

| Node | Kind | Purpose |
| --- | --- | --- |
| `Set Linked Portal` | Callable | Creates or replaces the reciprocal link and synchronizes the metric. |
| `Clear Linked Portal` | Callable | Removes the reciprocal link. |
| `Get Linked Portal` / `Has Linked Portal` | Pure | Reads link state. |
| `Is Linked Portal Area Ready` | Pure | Always `true` in a regular level; reports destination streaming readiness in World Partition. |
| Metric getters | Pure | `Get Portal Radius`, `Get Mouth Radius`, `Get Throat Half Length`, `Get Transition Length`, and `Get Transition Radius`. |
| `Get Portal Cube Render Target` | Pure | Returns a borrowed runtime render target. Do not resize, initialize, update, or release it. |
| `Get LUT Texture` / `Get LUT Z` | Pure | Reads the active volume LUT and the endpoint's logical slice. |
| `Transform Ray Through Portal` | Callable | Maps an entry point and inward-traveling direction to the linked endpoint. It does not perform a trace. Default exit offset: `2 cm`. |
| `Set Draw Portal Debug` / `Is Portal Debug Enabled` | Callable / Pure | Controls Editor boundary visualization. |

### Physical metric and visual scale

| Node | Runtime contract |
| --- | --- |
| `Set Portal Radius` | Authoring/pre-BeginPlay setter. Independent calls after BeginPlay are rejected. |
| `Set Throat Half Length` | Authoring/pre-BeginPlay setter. Independent calls after BeginPlay are rejected. |
| `Set Transition Length` | Authoring/pre-BeginPlay setter. Independent calls after BeginPlay are rejected. |
| `Initialize Physical Metric` | Atomically establishes `ρ`, `a`, and `T`. The first successful runtime call captures the base shape and ratios `a/ρ` and `T/ρ`. Later calls are accepted only when those ratios are preserved. |
| `Set Uniform Physical Metric Scale` | Uniformly scales the initialized physical shape. Updates collision, bounds, visibility queries, LUT/capture state, and dynamic capture resolution; use sparingly. |
| `Get Uniform Physical Metric Scale` | Returns physical scale relative to the initialized shape. |
| `Set Portal Visual Scale` | Render-only scale intended for per-frame growth. Collision, metric, bounds, LUT identity, ownership/warmup, capture cadence, and dynamic resolution remain unchanged. |
| `Get Portal Visual Scale` | Returns the render-only compositor scale. |
| `Set Metric Parameters` | Deprecated compatibility alias of `Initialize Physical Metric`. Do not use it in new code or Blueprints. |

Call **Initialize Physical Metric** once before runtime animation. Use physical
scale only when gameplay geometry must grow; otherwise animate visual scale.

## Transit Component { #transit-component }

Add `UWPTransitComponent` to an Actor to opt it into portal traversal.

### Properties

| Property | Default | Purpose |
| --- | ---: | --- |
| **Transit Enabled** | On | Allows a portal overlap to start transit. |
| **Transit Type** | `Auto` | Selects a concrete handler or automatic resolution. |
| **Ignore Time** | `0.15 s` | Reentry lockout after a successful commit. |
| **Draw Debug** | Off | Draws active transit geometry. |
| **Use Voxel Collision** | Off | Uses baked voxel collision while Master and Twin coexist. |
| **Use Material Clip** | Off | Drives compatible materials through Custom Primitive Data. |
| **Voxel Size** | `20 cm` | Requested voxel edge length for the Editor bake. |
| **Max Voxel Count** | `256` | Maximum boxes for each baked Primitive; range `1–512`. |
| **Center Mode** | `Root Component Location` | Reference used to select the entry tangent plane. Other option: `Actor OBB Center`. |

`Auto` resolves in this order: Character, Projectile, Pawn, then Physics. A
concrete type validates only that handler and does not fall back.

Transit Primitives must be owned by the participating Actor, Movable, and
collision-enabled. Physics transit needs at least one supported simulated body
with Physics collision: a non-instanced Static Mesh Component with a valid mesh,
or a supported Shape Component. Instanced Static Mesh Components are not
supported.

### Delegates

| Delegate | When it fires |
| --- | --- |
| `On Phase Changed` | The owner's transit phase changes. |
| `On Twin Preparing` | Template duplication is complete, immediately before the Twin Construction Script. Only synchronous work is safe; do not use latent actions such as `Delay`. |
| `On Twin Created` | Twin creation and run preparation are complete. |
| `On Twin Removing` | Immediately before the Twin is removed. |

### Blueprint API

| Node | Purpose |
| --- | --- |
| `Get Phase` | Returns `Idle`, `Check`, `Crossing`, or `Cooldown`. |
| `Get Transit Role` | Returns `None`, `Master`, or `Twin`. |
| `Get Counter Part Actor` | Returns the opposite Actor in the active relationship. |
| `Get Master Actor` / `Get Twin Actor` | Returns a specific side of the relationship. |
| `Get Source Portal` / `Get Dest Portal` | Returns the entry and destination endpoints. |
| `Get Transit Type` | Returns the concrete type after `Auto` resolution. |
| `Get Transit Sequence` | Returns the World-local identifier for the current transit. |
| `Cancel Transit` | Cancels active transit in an authoritative World. |
| `Get Entry Data` / `Get Exit Data` | Returns the surface point and outward normal. |
| `Map Point`, `Map Direction`, `Map Rotation`, `Map Transform` | Maps values through the current transit. Returns `false` without a valid mapping. |
| `Is Point Inside Portal` | Tests the active entry tangent plane. |
| `Refresh From Owner` | Re-resolves the handler and component caches. Allowed only while `Idle`. |

### Transit enums

| Enum | Values |
| --- | --- |
| `EWPTransitType` | `Auto`, `Physics`, `Character`, `Projectile`, `Pawn` |
| `EWPTransitPhase` | `Idle`, `Check`, `Crossing`, `Cooldown` |
| `EWPTransitRole` | `None`, `Master`, `Twin` |
| `EWPTransitResult` | `None`, `Rejected`, `Committed`, `Cancelled` |

`EWPTransitFailReason` describes a runtime lifecycle result:

```text
None, NotReady, PortalUnavailable, AlreadyInTransit, CooldownActive,
DoesNotFitGate, TwinCreationFailed, PortalDestroyed, InternalError,
UnsupportedActor, InvalidSetup, TransitDisabled, MissingPrimitives,
MissingPhysicsMesh, MissingVoxelData, InvalidVelocity, InvalidCenter,
InvalidPlane, BeginFailed, VoxelBeginFailed, UpdateFailed, RuntimeStateLost,
MappingInvalid, InvalidRunState, ReturnedToSource, CommitFailed,
CancelRequested, SubsystemClosed
```

`EWPTransitResolveFailReason` describes the detailed Actor-setup resolver result:

```text
None, UnsupportedActor, TransitDisabled, MissingPrimitives, NoPhysicsBody,
MissingVoxelData, TypeMismatch, MissingMovement, InvalidMoveOwner,
InvalidUpdatedPart, InvalidRootPart, InvalidPartOwner, PartNotMovable,
PartNoCollision, UnsupportedPart, ExcludedPart, SimPhysics, InvalidMoveType,
NoOverlapPart
```

`DoesNotFitGate` means the Actor's projected traversal cross-section,
perpendicular to its movement, does not fit the source portal gate/core radius.

## Portal-aware line traces { #portal-aware-line-traces }

Do not pass the configured `WPPortalTrace` channel as the scene trace channel.
The library uses it internally to detect portals.

| Blueprint node | Result |
| --- | --- |
| `Portal Line Trace By Channel` | Final blocking `Hit Result` and termination status. |
| `Portal Line Trace Detailed By Channel` | `FWPPortalTraceResult` with logical distance and portal events. |
| `Portal Line Trace Multi By Channel` | Overlap hits plus the first blocking hit in logical path order. |
| `Portal Line Trace Multi Detailed By Channel` | Detailed scene hits and portal events across all segments. |

Common defaults are `Ignore Self = true`, `Max Portal Depth = 4`, `Portal Exit
Offset = 2 cm`, `Draw Time = 5 s`, red trace color, and green hit color.

`FHitResult.Distance` restarts on each post-portal segment. Use
`FWPPortalTraceHit.LogicalDistance` for distance from the original start.
Logical distance excludes travel inside a portal and the Portal Exit Offset.

!!! warning "Portal Exit Offset behavior"

    The offset prevents an immediate self-hit. It does not consume remaining
    trace distance, and the gap between the exit surface and offset trace start
    is not collision-tested.

### Trace result types

| Type | Important fields |
| --- | --- |
| `FWPPortalTraceResult` | `Status`, `bBlockingHit`, `RequestedDistance`, `ProcessedDistance`, `PortalTraversalCount`, `SceneHits`, `PortalEvents` |
| `FWPPortalTraceHit` | `Hit`, `LogicalDistance`, `SegmentIndex` |
| `FWPPortalTracePortalEvent` | `DetectionHit`, `EntryPortal`, `ExitPortal`, `EntryDirection`, `ExitTraceStart`, `ExitDirection`, `LogicalDistance`, `PortalDepth`, `Outcome` |
| `EWPPortalTraceStatus` | `Completed`, `InvalidInput`, `MaxPortalDepthReached`, `PortalTransformFailed` |
| `EWPPortalTracePortalOutcome` | `Traversed`, `MaxDepthReached`, `TransformFailed` |

C++ additionally exposes `PortalLineTraceTestByChannel`,
`PortalLineTraceSingleByChannel`, and `PortalLineTraceMultiByChannel`.

## Project Settings { #project-settings }

Open **Edit > Project Settings > Plugins > Wormhole Portal**. These values are
stored in `DefaultEngine.ini`.

### Trace and transit

| Setting | Plugin default |
| --- | ---: |
| **Portal Trace Channel** | `ECC_GameTraceChannel1` |
| **Portal Trace Channel Name** | `WPPortalTrace` |
| **Plane Tie Angle** | `1°` |
| **Plane Priority** | `YZ > XZ > XY` |
| **Clip Base Index** | `28` |
| **Generated Voxel Asset Path** | `/Game/WormholePortal/Generated/Voxels` |

**Clip Base Index** reserves four consecutive Custom Primitive Data values and
must match the material using `MF_WPTransitClip`.

The Voxel path is writable project content by default. A custom Voxel path must
be a valid Long Package Directory under the case-sensitive `/Game/` prefix.

### Dynamic cubemap resolution

| Setting | Plugin code default |
| --- | ---: |
| **Lowest Visible Resolution** | `64` |
| **Resolution Tier 1** | `30%` / `128` |
| **Resolution Tier 2** | `60%` / `256` |
| **Resolution Tier 3** | `100%` / `512` |
| **Inside SafeProxy Resolution** | `768` |

The tier array is editable: add, remove, or reorder entries in Project Settings.
At runtime, thresholds are clamped to `0–100%` and stable-sorted in ascending
order. Positive resolutions round **up** to the next multiple of 8 in the
`8–2048` range. Tier resolutions are forced to be nondecreasing from **Lowest
Visible Resolution**, and the inside resolution is at least that lowest value.
An empty tier list uses the lowest resolution outside the safe proxy.

There is no plugin-local VRAM-budget resolution downgrade. Cubemap memory is
reported for diagnosis, but the effective policy comes from these settings.

### Scene-capture show flags

The plugin code default is On for:

- Lumen Reflections;
- Screen Space Ambient Occlusion;
- Volumetric Cloud Bundle;
- Dynamic Shadows Bundle;
- Sky Lighting Bundle;
- Sky Atmosphere Bundle;
- Deferred Lighting Bundle;
- Lighting Master Bundle;
- Fog Master Bundle; and
- Volumetric Fog.

These switches affect managed cube captures only. **Lighting Master Bundle**
and **Fog Master Bundle** are parent switches for dependent options.

### Portal audio

| Setting | Plugin default |
| --- | ---: |
| **Enable Portal Audio** | On |
| **Transmission Gain** | `1.0` |
| **Enable Occlusion** | On |
| **Occlusion Channel** | `Visibility` |
| **Occlusion Check Interval** | `0.1 s` |
| **Occluded Volume Multiplier** | `0.35` |
| **Occlusion Low-pass Frequency** | `1200 Hz` |
| **Occlusion Surface Bias** | `2 cm` |
| **Source Reconcile Interval** | `0.25 s` |

### Portal source shadows

| Setting | Plugin default |
| --- | ---: |
| **Enable Portal Source Shadows** | On |
| **Base Resolution** | `256` |
| **Update Rate** | `30 Hz` |
| **Captures Per Frame** | `2` |
| **Active Routes** | `8` |
| **Depth Bias** | `1 cm` |
| **PCF Radius** | `1 texel` |

### LUT

| Setting | Plugin default |
| --- | --- |
| **Generated LUT Asset Path** | `/Game/WormholePortal/Generated/LUT` |
| **Impact / Transition / Ratio Samples** | `512 / 48 / 24` |
| **Integration Steps** | `192` |
| **Transition Ratio Domain** | `0.5–8.0` |
| **Tail Flatten Start** | `0.5` |
| **Allow Runtime LUT Fallback** | On |

The generated LUT directory must be a valid Unreal Long Package Directory with
the case-sensitive prefix `/Game/`. Forward slashes and the trailing slash are
normalized before validation. Installed-plugin paths such as
`/WormholePortal/...` are rejected because generated assets must remain
writable project content.

## Tags

| Actor or Component tag | Effect |
| --- | --- |
| `Wormhole.Ignore` | Excludes an Actor or Primitive Component from transit resolution. |
| `WP.PortalAudio.Disabled` | Excludes an Audio Component or its owning Actor from portal audio. |
| `WP.PortalLight.Disabled` | Excludes a Light Component or its owning Actor from portal light collection. |

`WP.PortalAudio.Generated`, `WP.PortalLight.Generated`, and
`WormholeGeneratedTransit` are reserved for plugin-created objects and should
not be assigned manually.

## Editor tools

| Location | Tool |
| --- | --- |
| **Tools > Wormhole Portal > Transit Manager** | Checks Actor compatibility, reports `Ready`, `Needs Setup`, or `Not Supported`, and can add transit to ready Actors. |
| **Tools > Wormhole Portal > Bake All LUTs** | Opens **LUT Bake Settings** and generates the shared Volume LUT and catalog. |
| Transit Component Details | **Bake Voxel Body** generates data for each supported non-instanced Static Mesh or Box/Sphere/Capsule Primitive when **Use Voxel Collision** is enabled. |
| Portal Actor Details | Displays the active LUT domain and constrains metric controls to it. |
| Startup notification | Offers **Add Automatically** or **Open Collision Settings** when `WPPortalTrace` is missing or mismatched. |

LUT presets are:

| Preset group | Options |
| --- | --- |
| **Quality** | `Fast (256/24/12/64)`, `Balanced (512/48/24/192)`, `High (768/64/32/384)`, `Cinematic (1024/96/48/768)` |
| **Domain** | `Current Level Auto`, `Standard (0.5–8)`, `Wide (0.25–16)`, `Narrow (1–4)` |

Quality numbers are Impact Samples, Transition Samples, Ratio Samples, and
Integration Steps.

## Integration examples { #integration-examples }

### Blueprint patterns

For a runtime-created pair:

1. Call **Set Linked Portal**.
2. Call **Initialize Physical Metric** once with the full `ρ`, `a`, and `T`
   shape.
3. Drive **Set Portal Visual Scale** from a Timeline for a low-cost visual
   opening effect.
4. Use **Set Uniform Physical Metric Scale** only if collision and the physical
   traversal gate must grow with the visual.

For transit, add `WPTransitComponent` in the Blueprint Components panel, keep
**Transit Type: Auto** and **Transit Enabled**, compile, then verify the Actor in
Transit Manager. For a trace, connect **Portal Line Trace Detailed By Channel**
to **Break WPPortalTraceResult** and inspect `Status`, `Scene Hits`, `Portal
Events`, and logical distance.

### C++ module dependency

Add the runtime module to your module's `.Build.cs` file:

```csharp
PublicDependencyModuleNames.AddRange(
    new string[]
    {
        "WormholePortalRuntime"
    });
```

### Configure and animate a pair

```cpp
#include "WormholePortalActor.h"

void ConfigurePortalPair(
    AWormholePortalActor* PortalA,
    AWormholePortalActor* PortalB)
{
    if (!IsValid(PortalA) || !IsValid(PortalB))
    {
        return;
    }

    PortalA->SetLinkedPortal(PortalB);
    PortalA->InitializePhysicalMetric(75.0f, 150.0f, 300.0f);

    // Preferred for a per-frame opening effect: rendering only.
    PortalA->SetPortalVisualScale(0.5f);

    // Use instead when the physical gate and collision must resize.
    // PortalA->SetUniformPhysicalMetricScale(1.25f);
}
```

Run replicated gameplay mutations from the authoritative side. Do not call the
three independent metric setters every Tick.

### Refresh an existing Transit Component

```cpp
#include "Transit/WPTransitComponent.h"

if (UWPTransitComponent* Transit =
        Actor->FindComponentByClass<UWPTransitComponent>())
{
    Transit->SetTransitType(EWPTransitType::Auto);
    Transit->SetTransitEnabled(true);
    Transit->RefreshFromOwner(); // Only succeeds while Idle.
}
```

### Perform a portal-aware trace

```cpp
#include "Trace/WPTraceLibrary.h"

FWPPortalTraceResult Result;
const bool bBlockingHit = UWPTraceLibrary::PortalLineTraceSingleByChannel(
    WorldContextObject,
    Result,
    Start,
    End,
    ECC_Visibility);
```

`ECC_Visibility` is the ordinary scene channel in this example. Never replace
it with the plugin's configured `WPPortalTrace` channel.

## Native subsystems

| Type | Scope and public use |
| --- | --- |
| `UWPRegistrySubsystem` | World-scoped portal and pair registry with native registration, change, and pair delegates. |
| `UWPTransitSubsystem` | Server-authoritative transit coordinator and native lifecycle delegates. |
| `UWPPortalStreamingSubsystem` | World Partition destination requests. Normal gameplay reads readiness from the Portal Actor. |
| `UWPPortalAudioSubsystem` | Opposite-side audio routes. Blueprint exposes tracked-source and proxy counts. |
| `UWPPortalLightCollectionSubsystem` | Collects supported Point, Spot, and Directional Lights for C++ snapshots. |
| `UWPPortalLightTransmissionSubsystem` | Generates Point/Spot proxy lights and source-shadow routes. |
| `UWPLUTCacheSubsystem` | Engine-scoped asynchronous baked/fallback LUT cache; completion returns on the Game Thread. |
| `IWPRenderer` | Advanced modular-feature boundary for pair registration and immutable render packets. |

`FWPTransform` provides point, direction, rotation, inverse, exit, and complete
Transform mapping.

## Diagnostics

Run `stat WormholePortal` for CPU setup/submission timing, pair counts, cubemap
memory and megapixels, capture submissions, LUT requests/cache hits/fallbacks,
render packets, and composite workload. Use `stat gpu` or ProfileGPU for GPU
execution cost.

In a non-Shipping build, raise transit log verbosity before reproducing a
rejection:

```text
Log LogWormhole Verbose
```

The complete line has this shape:

```text
[Transit][Rejected] TimestampSeconds=... RuntimeReason=EWPTransitFailReason::... ResolveReason=EWPTransitResolveFailReason::... FailedComponents=... Actor=... SourcePortal=... DestinationPortal=... TransitType=EWPTransitType::...
```

`RuntimeReason` is the lifecycle failure. `ResolveReason` is the detailed setup
resolver result, and `FailedComponents` identifies implicated components. This
Verbose rejection line is compiled out of Shipping builds.

### Common rendering CVars

| CVar | Code default | Purpose |
| --- | ---: | --- |
| `wp.RuntimeEnabled` | `1` | Production render-packet pipeline master switch. |
| `wp.SceneViewExtensionEnabled` | `1` | Warmup and production compositing master switch. |
| `wp.SimulateViewEnabled` | `1` | Enables the SIE viewport production pass. |
| `wp.CaptureSchedulerMode` | `2` | `1`: atomic pair; `2`: staggered endpoints. `0` is a deprecated alias of `1`. |
| `wp.CaptureTargetEndpointHz` | `30` | Visible-endpoint update rate; runtime range `5–120 Hz`. |
| `wp.CaptureVisibilityInvisibleHoldSeconds` | `0.5` | Both-invisible time before captures pause. |
| `wp.CaptureOcclusionTraceIntervalSeconds` | `0.1` | CPU safe-proxy occlusion-test interval. |
| `wp.CaptureResolutionUpgradeHoldSeconds` | `0.75` | Delay before increasing resolution. |
| `wp.CaptureResolutionDowngradeHoldSeconds` | `0.15` | Delay before decreasing resolution. |
| `wp.CaptureResolutionMinimumDwellSeconds` | `0.5` | Minimum interval between completed resolution transitions. |
| `wp.CaptureMaxViewDistanceCm` | `-1` | Positive values enable capture distance culling; `<=0` is unlimited. |
| `wp.CaptureLODDistanceFactor` | `1` | Capture LOD multiplier; range `1–10`. |
| `wp.CubeLumenParityMode` | `0` | `0`: no Lumen, `1`: GI, `2`: GI and reflections. |
| `wp.ViewSummaryInterval` | `5` | Aggregate rendering log interval in seconds. |

Use CVars for profiling and controlled overrides. Keep Project Settings as the
normal authoring surface.
