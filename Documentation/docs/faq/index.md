# FAQ

## Which Unreal Engine version does the plugin target?

Version 1.0 is officially tested and supported with Unreal Engine 5.8 on
Win64, DirectX 12, and SM6. Linux, macOS, DirectX 11, mobile, consoles, and
dedicated-server targets are not declared supported.

## Where is the included demo?

In the Content Browser, enable **Show Plugin Content**, then open:

```text
WormholePortal Content/Demo/Lv_WormholePortal_Content_Demo
```

See [Demo](../demo/index.md) for the entry map, examples, dependencies, and reuse
guidance.

## Does every portal need a linked portal?

A valid pair is required for destination rendering and for transit, traces,
audio, lighting, and destination streaming. Assigning **Linked Portal** creates
the reciprocal link and synchronizes the pair's metric.

## Can I resize a portal with Actor Scale?

No. Keep both Portal Actors at `(1, 1, 1)`. Before BeginPlay, author **Portal
Radius**, **Throat Half Length**, and **Transition Length**. At runtime, use
**Initialize Physical Metric** once, then either **Set Uniform Physical Metric
Scale** for a physical resize or **Set Portal Visual Scale** for render-only
animation.

## Why do the individual metric setters stop working after BeginPlay?

They are authoring/pre-BeginPlay setters. Independent runtime changes could make
`ρ`, `a`, and `T` drift into an inconsistent physical shape.

The first successful **Initialize Physical Metric** call establishes all three
values atomically and captures the `a/ρ` and `T/ρ` ratios. Later initialization
calls must preserve those ratios. **Set Metric Parameters** is only a deprecated
compatibility alias.

## Which scale API should I animate?

Use **Set Portal Visual Scale** for per-frame opening or growth effects. It
changes only compositor size and leaves collision, metric, bounds, LUT identity,
capture ownership/warmup/cadence, and dynamic resolution unchanged.

Use **Set Uniform Physical Metric Scale** only when the physical traversal gate
and collision must also resize. It updates more systems and is intentionally
more expensive.

## Do I have to bake a LUT?

Not while **Allow Runtime LUT Fallback** is enabled. The runtime can generate
compatible transient data asynchronously. Pre-bake when predictable startup
readiness matters. `Transition Length = 0` uses the analytic path and needs no
LUT.

## Where are generated LUTs stored and cooked?

The code-default location is `/Game/WormholePortal/Generated/LUT`.
The Editor module attempts to add the configured directory to **Additional
Asset Directories to Cook**.

A custom LUT location must be a valid Unreal Long Package Directory beginning
with the case-sensitive prefix `/Game/`. Confirm the exact path in
Packaging settings before shipping.

## Can portals appear in Scene Capture, VR, split-screen, or every Movie Render Queue pass?

No. In the supported Win64 DX12 SM6 environment, rendering supports one
primary, perspective, non-stereo Game View.

Scene Capture, Reflection Capture, Planar Reflection, orthographic, stereo/VR,
split-screen, and other multi-view families are excluded. Movie Render Queue
supports the main deferred mono beauty output; custom passes, Virtual Texture
passes, stereo, and multi-view output omit the portal composite.

## Are portals rendered on a dedicated server?

No. The renderer module is excluded from Server targets. Gameplay and
replication still need project-specific dedicated-server validation; do not
interpret replicated portal state as a promise of rendering on the server.

## Which Actors can use transit?

Add `WPTransitComponent`. Supported handler types are Character, Projectile,
Pawn, Physics, and Auto. Auto checks Character, Projectile, Pawn, then Physics.

Participating Primitives must be actor-owned, Movable, and collision-enabled.
Physics additionally needs a supported body with Physics collision enabled and
simulation running: a non-instanced Static Mesh Component with a valid mesh, or
a supported Shape Component. Instanced Static Mesh Components are unsupported.

## What does DoesNotFitGate mean?

The Actor's projected traversal cross-section perpendicular to its movement
does not fit the source portal gate/core radius. Increase **Portal Radius** or
correct/reduce the Actor collision bounds. Do not scale the Portal Actor.

## What can Voxel Collision bake?

Voxel baking supports:

- non-instanced Static Mesh Components with usable Simple Collision; and
- Box, Sphere, and Capsule Components.

Static Mesh Simple Collision can contain Sphere, Box, Capsule, and Convex
elements. **Max Voxel Count** applies separately to each baked Primitive. It is
not one budget per Static Mesh or Actor.

The default generated location is `/Game/WormholePortal/Generated/Voxels`. A
custom Voxel path must be a valid Long Package Directory under the
case-sensitive `/Game/` project-content prefix.

## What is Max Portal Depth?

Portal-aware line traces can cross multiple portals. **Max Portal Depth**
prevents an infinite loop between linked endpoints. The default is `4`; raise
it only when the logical trace genuinely needs more crossings.

## Which collision channel should a portal-aware trace use?

Use the ordinary scene channel for the objects you want to hit. Never use
`WPPortalTrace` as that argument: the plugin reserves it for internal portal
intersection detection.

## Does transit support multiplayer?

Portal movement, link, and metric state are replicated, and the Transit
Component carries server-authoritative replicated transit state.

Participating gameplay Actors still need Unreal's normal replication,
ownership, relevancy, and movement configuration. Test the complete behavior
under your project's real network conditions.

## Does World Partition work with portals?

Yes. The runtime can request destination-side streaming around a linked portal.
Transit can report `NotReady` until the destination area is loaded.

Tune **Streaming Preload Distance**, **Streaming Release Distance**, and
**Streaming Query Interval**. Blueprint code can query
`IsLinkedPortalAreaReady()`.

## Which sounds travel through a portal?

The source must be an active Audio Component with spatialization and
spatialized attenuation enabled, and its audible range must reach the portal.

UI/2D sounds, Source Bus assets, multiple-instance components, disabled sources,
sources inside the portal sphere, and generated proxies are excluded. Portal
audio supports one primary listener and one portal hop.

## Which Light types are transmitted?

Point and Spot Lights are transmitted. Directional Lights may be collected by
the light-collection subsystem, but the transmission subsystem does not create
Directional Light routes. Rect Lights are not transmitted.

See [Portal lighting troubleshooting](../issues/index.md#portal-lighting) for the
remaining limitations.

## Is Material Clip automatic?

No. Enable **Use Material Clip** and integrate the supplied transit clip
Material Function into the Actor's material. It reads four consecutive Custom
Primitive Data values beginning at **Clip Base Index**; the default is `28`.

## What is the difference between Material Clip and Voxel Collision?

**Material Clip** changes only how a compatible mesh is drawn at the boundary.
**Voxel Collision** changes how supported collision is represented on both
sides while the Actor is split. They are independent and may be used together
or separately.

## How does dynamic cubemap resolution choose a size?

The policy starts at **Lowest Visible Resolution**, then evaluates the editable
screen-height tier array. Thresholds are clamped and stable-sorted. Resolutions
round up to a multiple of 8 in `8–2048` and are forced nondecreasing. Inside the
safe proxy, the dedicated inside value applies and cannot be lower than the
lowest value.

There is no plugin-local VRAM budget that silently lowers the result. Use
`stat WormholePortal` to observe memory and edit the policy when needed.

## How can I inspect runtime performance or a transit rejection?

Run `stat WormholePortal`; use `stat gpu` or ProfileGPU for GPU cost. Filter
Output Log by `LogWormhole`.

For the complete transit rejection line in a non-Shipping build, first enter:

```text
Log LogWormhole Verbose
```

Then reproduce the issue and collect `RuntimeReason`, `ResolveReason`, and
`FailedComponents`. See [Issues](../issues/index.md#transit-does-not-start).

## Where do I request support?

Wormhole Portal is published by **Team Beaver Studio**. Email the official
support contact at
[beavergametech@gmail.com](mailto:beavergametech@gmail.com), and include the
checklist from [Compatibility, Releases, and Support](../support/index.md).
