# Wormhole Portal

![Wormhole Portal icon](Resources/Icon128.png)

**Wormhole Portal** is an Unreal Engine plugin that makes separate locations
look and behave like parts of one continuous world. Instead of displaying the
destination through a flat window, it renders a volumetric wormhole whose view
bends through physically inspired mouth, throat, and transition regions.

Rendering, actor transit, traces, streaming, audio, and lighting all use the
same portal links and metric model, keeping the visible destination consistent
with the gameplay space beyond it.

[Documentation](https://teambeaverstudio.github.io/WormholePortal_Docs/) ·
[Korean documentation](https://teambeaverstudio.github.io/WormholePortal_Docs/ko/) ·
[Offline documentation](Documentation/index.html)

## Features

- **Volumetric wormhole rendering** — A three-dimensional throat, spatial lensing,
  LUT-based ray mapping, and adaptive cubemap capture based on screen coverage
- **Actor transit** — `WPTransitComponent` support for Characters, Pawns,
  Projectiles, and Physics Actors, with automatic transit type detection
- **Boundary presentation and collision** — Optional Material Clip and Voxel
  Collision systems
- **Portal-aware traces** — Blueprint and C++ Line Trace APIs that combine
  multiple portal segments into one logical distance
- **Connected-space systems** — World Partition destination preloading, spatial
  audio, and Point/Spot Light transmission
- **Editor tools** — Transit Manager, LUT/Voxel baking, metric debug
  visualization, configuration checks, and runtime diagnostics
- **Sample content** — A public demo map with a Portal Gun and staged feature
  examples

## Supported Environment

| Item | Supported configuration |
| --- | --- |
| Plugin version | `1.0` |
| Unreal Engine | `5.8` |
| Platform | Win64 |
| Rendering | DX12 + SM6 |
| Game View | One Primary Perspective Non-stereo View |
| Movie Render Queue | Main Deferred Mono Beauty output |

Linux, macOS, DX11, mobile, consoles, and Dedicated Server are not currently
declared supported. Portal compositing is also unavailable in VR/Stereo, Split
Screen, Multi-view, Orthographic View, Scene Capture, and Reflection Capture.

The plugin enables these Unreal Engine plugins:

- Enhanced Input
- StateTree
- Gameplay StateTree

## Installation

### Install from Fab

1. Install Wormhole Portal for Unreal Engine 5.8 from your Fab Library.
2. Enable **Wormhole Portal** under **Edit > Plugins** in your project.
3. Restart Unreal Editor.

### Install from source or as a local plugin

Place the complete `WormholePortal` directory at:

```text
<Project>/Plugins/WormholePortal
```

For a C++ project, regenerate the project files and build the plugin with
Unreal Engine 5.8. To use the demo project in this repository, open
`WormholePortalDemo.uproject` with Unreal Engine 5.8.

## Quick Start

### 1. Configure the portal trace channel

If the required `WPPortalTrace` channel is missing when the plugin starts, a
notification appears. Select **Add Automatically**, then restart the Editor.
To configure it manually, open **Edit > Project Settings > Engine > Collision**,
create a Trace Channel with the same name, and set its Default Response to
**Ignore**.

### 2. Create a portal pair

1. Place two `WormholePortalActor` instances in the level from **Place Actors**.
2. Set the first portal's **Linked Portal** to the second portal.
3. Keep the Transform Scale of both Portal Actors at `(1, 1, 1)`.

The reciprocal link and metric values are synchronized automatically. Adjust
the portal's size through its metric properties rather than Actor Scale.

| Property | Default | Description |
| --- | ---: | --- |
| Portal Radius | `50 cm` | Radius of the central transit gate |
| Throat Half Length | `100 cm` | Half of the connected throat length |
| Transition Length | `200 cm` | Blend distance between ordinary space and the throat |

### 3. Bake the LUT

Open **Tools > Wormhole Portal > Bake All LUTs** and use these recommended
settings:

- Quality: **Balanced**
- Domain: **Current Level Auto**

A runtime fallback is available, but baking in advance makes startup timing
and visual results more predictable.

### 4. Enable actor transit

Add `WPTransitComponent` to the Character or Actor that should pass through the
portal, then verify these values:

- Transit Enabled: enabled
- Transit Type: `Auto`

Compile and save the Blueprint, then test portal rendering and bidirectional
transit in PIE. Use **Tools > Wormhole Portal > Transit Manager** to inspect the
configuration of multiple Actors at once.

## Included Demo

Enable **Settings > Show Plugin Content** in the Content Browser, then open:

```text
/WormholePortal/WormholePortal/Demo/Lv_WormholePortal_Content_Demo
```

| Stage | Examples |
| --- | --- |
| Stage 0 | Basic controls and the Portal Gun |
| Stage 1 | Character and general Actor transit |
| Stage 2 | Partial transit, Material Clip, and Voxel Collision |
| Stage 3 | Portal-aware Line Trace and Blueprint/C++ APIs |
| Stage 4 | Spatial audio and Point/Spot Light transmission |

`WormholePortalSample` and `/WormholePortal/WormholePortal/Demo` are learning and integration
examples. They are not required by the production runtime.

## C++ Integration

Add the runtime module to the `.Build.cs` file of any module that uses the
public API:

```csharp
PublicDependencyModuleNames.AddRange(
    new string[]
    {
        "WormholePortalRuntime"
    });
```

The plugin contains four modules:

| Module | Role |
| --- | --- |
| `WormholePortalRuntime` | Portal Actors, transit, traces, audio, lighting, streaming, and runtime APIs |
| `WormholePortalRenderer` | Portal compositing through a Scene View Extension and Global Shaders |
| `WormholePortalEditor` | Transit Manager, baking, configuration, and debug tools |
| `WormholePortalSample` | Portal Gun and demo gameplay examples |

Add a `WormholePortalSample` dependency only if you use the sample Portal Gun
directly. See the [API reference](https://teambeaverstudio.github.io/WormholePortal_Docs/reference/)
for detailed Blueprint and C++ examples.

## Packaging and Diagnostics

Generated LUT assets use this path by default:

```text
/WormholePortal/WormholePortal/Generated/LUT
```

Before packaging, confirm that this path is listed under **Project Settings >
Packaging > Additional Asset Directories to Cook**.

Use these commands when diagnosing a problem:

```text
stat WormholePortal
Log LogWormhole Verbose
```

Filter the Output Log by `LogWormhole` to find transit rejection reasons and
configuration issues. Measure actual GPU cost with `stat gpu` or Unreal's GPU
Profiler.

## Documentation and Support

- [Getting Started](https://teambeaverstudio.github.io/WormholePortal_Docs/getting-started/)
- [Feature Guide](https://teambeaverstudio.github.io/WormholePortal_Docs/features/)
- [Demo Guide](https://teambeaverstudio.github.io/WormholePortal_Docs/demo/)
- [Troubleshooting](https://teambeaverstudio.github.io/WormholePortal_Docs/issues/)
- [FAQ](https://teambeaverstudio.github.io/WormholePortal_Docs/faq/)
- [Compatibility and Support](https://teambeaverstudio.github.io/WormholePortal_Docs/support/)

Support: [beavergametech@gmail.com](mailto:beavergametech@gmail.com)

Copyright &copy; 2026 Team Beaver Studio. All rights reserved.
