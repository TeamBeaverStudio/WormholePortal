# Features

Wormhole Portal combines a physically inspired spherical portal renderer with
gameplay systems for transit, traces, audio, light, and World Partition. The
systems share the same linked-portal and metric model, so the view through a
portal and the gameplay route lead to the same destination.

## Volumetric wormhole rendering

The portal is rendered as a volume rather than a flat window. Its opening,
throat, and transition region bend the destination view into a continuous
spherical image.

- **Volumetric throat:** the camera can approach and enter the portal volume.
- **Spatial lensing:** rays bend through the configured wormhole metric.
- **Continuous destination view:** the linked space remains visible across the
  opening and throat.
- **Shared LUT rendering:** baked lookup data accelerates the ray mapping. A
  runtime fallback can generate compatible transient data when enabled.
- **Adaptive captures:** cubemap resolution changes with visibility and
  on-screen size, with a separate resolution for the inside of the safe proxy.

<!-- CAPTURE SLOT F-01: Cinematic in-engine video showing the mouth, throat, strong lensing, and destination view. -->

The capture-quality switches in **Project Settings > Plugins > Wormhole
Portal > Scene Capture** affect only the portal's cube capture. They do not
change the player's normal game view.

## Linked pairs and metric controls

Each `WormholePortalActor` links to one other portal. Assigning **Linked
Portal** creates the reciprocal link and synchronizes the metric values across
the pair.

Three properties define the shape:

| Property | Meaning |
| --- | --- |
| **Portal Radius** | Radius of the central `l = 0` seam used for physical traversal |
| **Throat Half Length** | Half of the connected throat length |
| **Transition Length** | Blend distance between ordinary space and the throat |

The derived mouth radius is `Portal Radius + Throat Half Length`, and the
transition radius adds **Transition Length** to that result.

!!! warning "Use metric properties, not Actor Scale"

    Keep each portal's Transform Scale at `(1, 1, 1)`. The metric calculations
    use the configured dimensions and do not treat Actor Scale as a size
    control.

## Actor transit

Add `WPTransitComponent` to an actor that must cross a portal. **Transit Type:
Auto** resolves a compatible strategy from the actor and its movable collision
components.

Supported transit types are:

- **Character**
- **Pawn**
- **Projectile**
- **Physics**

During a crossing, the system maintains the relationship between the original
actor and its counterpart, maps transform and movement through the linked
portals, and applies a short cooldown to prevent an immediate reverse transit.
Portal link, metric, movement, and transit relationship state are replicated;
test the complete gameplay behavior under your project's own network model.

<!-- CAPTURE SLOT F-02: Short gameplay video of a Character and a physics object crossing the same portal pair. -->

Two optional presentation and collision tools are available:

- **Material Clip** writes the sphere-clip center, normal, and radius into four
  consecutive Custom Primitive Data slots so compatible materials can hide the
  part crossing the boundary.
- **Voxel Collision** replaces participating static-mesh collision with a
  baked box-voxel body while the object is split across the two spaces.

Voxel collision is intended for static meshes with usable simple collision.
Enable it on the Transit Component, then use **Bake Voxel Body** in that
component's Details panel.

## Portal-aware traces

Standard Unreal line traces do not continue through linked portal space. The
portal-aware APIs detect portals separately on `WPPortalTrace` and continue the
logical ray from the linked exit.

Blueprint and C++ APIs provide:

- single-hit and multi-hit line traces;
- compact and detailed results;
- logical distance across every segment;
- traversal count and per-portal entry/exit events.

The project must have a trace channel named `WPPortalTrace`. The plugin can add
and align this channel from its startup notification; restart the editor after
the change.

<!-- CAPTURE SLOT F-03: Blueprint or debug-view capture of one line trace entering a portal and continuing from the linked exit. -->

## Spatial audio through portals

The audio subsystem discovers spatialized Audio Components and creates a
re-radiated proxy on the other side of a linked pair.

- Routes are evaluated independently in both directions.
- Source-to-entry and exit-to-listener occlusion are tested separately.
- Blocked routes can reduce volume and apply a low-pass filter.
- Runtime-created spatialized Audio Components are periodically discovered.

The current model supports one primary listener and one portal hop. Add the
`WP.PortalAudio.Disabled` tag to an Audio Component or its owner to exclude it.

<!-- CAPTURE SLOT F-04: Video with audible before/after comparison while a spatialized source is heard through a portal and then occluded. -->

## Point and Spot light transmission

Point and Spot lights that affect an entry portal can produce a synchronized
proxy at the linked exit. The proxy follows the source transform and light
properties, restricts illumination to the portal aperture, and can transmit
source-side visibility for shadows.

<!-- CAPTURE SLOT F-05: Short video showing a moving Point or Spot light illuminating geometry through the linked exit. -->

The current light model does not transmit Directional Lights, Global
Illumination, reflections, volumetrics, translucent shadows, colored shadows,
lights located inside a portal, or pairs with asymmetric radii. See
[Issues](../issues/#portal-lighting) before planning a lighting-dependent
scene.

## World Partition destination streaming

In a World Partition level, each portal can keep the linked destination area
loaded before the player reaches the opening.

- **Streaming Preload Distance** controls when the destination source is
  requested.
- **Streaming Release Distance** controls when it can be released.
- **Streaming Query Interval** controls how often the distance is reevaluated.
- `IsLinkedPortalAreaReady()` lets gameplay check whether the destination is
  ready.

The release distance should remain greater than the preload distance to avoid
rapid load/unload changes. Destination readiness can block transit until the
linked area is available.

## Editor workflow and diagnostics

The plugin adds two commands under **Tools > Wormhole Portal**:

| Tool | Purpose |
| --- | --- |
| **Transit Manager** | Scan actors, report setup problems, and add transit to compatible actors |
| **Bake All LUTs** | Build shared rendering lookup data for the portals in the current level |

<!-- CAPTURE SLOT F-06: Transit Manager with Ready, Needs Setup, and Not Supported rows visible. -->

Portal actors can draw metric boundaries in the editor. Runtime transit
debugging is available on `WPTransitComponent`. For broader diagnostics:

- filter **Output Log** by `LogWormhole`;
- run `stat WormholePortal` for plugin counters and CPU timing;
- use Unreal's `stat gpu` or GPU Profiler for actual GPU execution cost.

## Where to go next

- Build a working pair in [Getting Started](../getting-started/).
- Look up properties, enums, functions, and defaults in
  [Reference](../reference/).
- Diagnose known limitations in [Issues](../issues/).
