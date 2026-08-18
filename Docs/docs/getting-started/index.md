# Getting Started

Build and traverse your first Wormhole Portal pair in 5–10 minutes. At the end,
the linked destination will be visible through the opening and a playable
Character will be able to cross it.

## What you will build

- **Portal rendering:** the destination environment appears through a
  volumetric portal.
- **Character transit:** the player crosses the boundary and emerges from the
  linked endpoint.

## Before you begin

You need:

- Unreal Engine 5.8;
- a Win64 project using DirectX 12 and SM6, with a Perspective viewport in Game
  View;
- a project with a playable Character Blueprint; and
- Wormhole Portal installed from Fab, enabled in **Edit > Plugins**, and loaded
  after an Editor restart.

This quickstart assumes the Character has a root Capsule Component, Character
Movement, and a non-simulating Skeletal Mesh.

## 1. Configure portal tracing

After the plugin starts, a notification appears when the project does not have
the required portal trace channel.

1. Select **Add Automatically** in the notification.
2. The plugin adds `WPPortalTrace` to `Config/DefaultEngine.ini` and aligns its
   setting with the assigned Game Trace Channel.
3. Restart the Editor before using portal-aware traces.

!!! warning "Restart required"

    The channel is not reliable until the Editor restarts. If the notification
    returns, confirm the configuration file was writable and repeat the setup.

`WPPortalTrace` is reserved for internal portal detection by portal-aware
traces. Character transit uses overlap handling independently, but configuring
the channel now avoids a partially configured project.

??? info "Configure WPPortalTrace manually"

    1. Open **Edit > Project Settings > Engine > Collision**.
    2. Add a **Trace Channel** named `WPPortalTrace`.
    3. Set its **Default Response** to **Ignore**.
    4. Open **Project Settings > Plugins > Wormhole Portal**.
    5. Set **Portal Trace Channel** to the Game Trace Channel assigned to
       `WPPortalTrace`.
    6. Restart the Editor.

## 2. Create a portal pair

1. Open **Place Actors** and search for `WormholePortalActor`.
2. Place two native `WormholePortalActor` instances at different locations.
3. Select the first portal and set **Linked Portal** to the second.

The reciprocal link is created automatically. The first endpoint's metric is
copied to the linked endpoint, so configure the pair from one side.

!!! warning "Keep Actor Scale at one"

    Leave both Transform scales at `(1, 1, 1)`. Use the portal's Metric
    properties to control its physical dimensions.

The authoring defaults are:

| Property | Default | Controls |
| --- | ---: | --- |
| **Portal Radius** | `50 cm` | Radius of the central seam used for traversal |
| **Throat Half Length** | `100 cm` | Half of the connected throat length |
| **Transition Length** | `200 cm` | Blend distance into and out of the throat |

## 3. Bake the visual data

Bake the lookup data before the first test for deterministic startup behavior.

1. Open the main **Tools** menu.
2. Under **Wormhole Portal**, select **Bake All LUTs**.
3. In **LUT Bake Settings**, choose **Quality: Balanced**.
4. Choose **Domain: Current Level Auto**.
5. Select **Bake**.
6. Wait for the success notification beginning with `LUT ready:`.

Wormhole Portal can create compatible transient lookup data asynchronously when
**Allow Runtime LUT Fallback** is enabled. Baking is still recommended when
predictable startup readiness matters.

## 4. Enable Character transit

1. Open the playable Character Blueprint.
2. In the Components panel, add `WPTransitComponent`.
3. Keep **Transit Enabled** selected.
4. Keep **Transit Type** set to **Auto**.
5. **Compile** and **Save** the Blueprint.

Use **Tools > Wormhole Portal > Transit Manager** when several Actors need to
be inspected or configured. A **Ready** row means the authoring setup resolves
to a supported transit handler; it does not replace a runtime crossing test.

## 5. Test the portal

Start Play In Editor and approach the first endpoint.

- **Portal rendering:** look through the opening and confirm that the linked
  environment is visible.
- **Character transit:** walk across the boundary and confirm that the
  Character emerges from the linked endpoint.

When both checks pass, the pair is ready for level design and gameplay
integration.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| The `WPPortalTrace` warning returns | Select **Add Automatically**, confirm the config update completed, and restart the Editor. |
| The portal is not visible | Confirm **Linked Portal**, unit Actor Scale, a primary Perspective Game View in the supported Win64 DX12 SM6 environment, and a compatible LUT. |
| The Character does not cross | Confirm `WPTransitComponent`, **Transit Type: Auto**, **Transit Enabled**, and the expected Capsule and Character Movement components. |
| The log reports `RuntimeReason=EWPTransitFailReason::DoesNotFitGate` | The Actor's projected traversal cross-section does not fit the source gate/core radius. Increase **Portal Radius** or reduce/fix the Actor's collision bounds; do not scale the Portal Actor. |
| Transit Manager does not report `Ready` | Select the Actor and resolve the exact setup item shown before testing again. |
| A packaged build cannot find baked LUT data | Confirm `/Game/WormholePortal/Generated/LUT` is included in **Additional Asset Directories to Cook**. |

For a complete non-Shipping transit rejection line, raise the category to
Verbose before reproducing the problem:

```text
Log LogWormhole Verbose
```

Also run `stat WormholePortal` for runtime counters and timings. See
[Issues](../issues/index.md) for the full diagnostic format and failure enums.

## Cook generated LUT data

The code-default **Generated LUT Asset Path** is
`/Game/WormholePortal/Generated/LUT`. This is a writable project-content path.
The Editor module synchronizes the configured path with **Project Settings >
Packaging > Additional Asset Directories to Cook**.

Verify the following entry before shipping:

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysCook=(Path="/Game/WormholePortal/Generated/LUT")
```

A custom LUT directory must be a valid Unreal Long Package Directory beginning
with the case-sensitive prefix `/Game/`. If the project configuration is
read-only, add the validated path to the Packaging settings manually.

## Next steps

- Open the included examples in the [Demo guide](../demo/index.md).
- Learn the rendering, transit, trace, audio, lighting, and streaming systems in
  [Features](../features/index.md).
- Copy accurate Blueprint and C++ patterns from [Reference](../reference.md).
- Review limitations and workarounds in [Issues](../issues/index.md).
- Check the supported target and release checklist in
  [Compatibility and Support](../support/index.md).
