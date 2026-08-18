# Demo

Wormhole Portal includes a content map and sample gameplay assets for evaluating
the renderer, transit handlers, portal-aware interactions, and presentation
helpers before integrating them into a production project.

## Open the included demo

1. Enable Wormhole Portal in **Edit > Plugins** and restart the Editor if
   requested.
2. Open the Content Browser or Content Drawer.
3. Open its **Settings** menu and enable **Show Plugin Content**.
4. Navigate to **WormholePortal Content > Demo**.
5. Open `Lv_WormholePortal_Content_Demo`.
6. Choose **Play > Selected Viewport**.

The package path is:

```text
/WormholePortal/Demo/Lv_WormholePortal_Content_Demo
```

`Lv_WormholePortal_Content_Demo` is the documented customer-facing entry map.
Do not base evaluation steps on `Dev_Demo`, which is not the documented entry
point.

## What the map demonstrates

Follow the map in order; a later area may remain locked until the preceding
stage's condition is complete.

| Area | What to verify |
| --- | --- |
| Stage 0 | Basic controls, documentation/support links, and linked-portal creation with the Portal Gun |
| Stage 1 | Character and general Actor transit, portal links, and physical metric behavior |
| Stage 2 | Partial crossing of long meshes, Material Clip, Voxel Collision, and advanced transit setup |
| Stage 3 | Portal-aware line traces and the public Blueprint/C++ API examples |
| Stage 4 | Spatial audio and Point/Spot light transmission through a portal |

Inspect Actors while they straddle the boundary, their exit velocity and
direction, and the logical hit order of portal-aware traces—not only the
rendered destination view.

Run in the supported Win64 DirectX 12 SM6 environment and use a primary
Perspective Game View. The portal composite does not appear in Scene Capture,
orthographic, stereo/VR, or split-screen views.

## Inspect the examples

| Example | Content Browser path or class | What to study |
| --- | --- | --- |
| Demo map | `/WormholePortal/Demo/Lv_WormholePortal_Content_Demo` | Portal placement, pair configuration, and complete test scenarios |
| First-person Character | `/WormholePortal/Demo/FirstPerson/Blueprints/BP_FirstPersonCharacter` | Character transit component and movement setup |
| Portal gun | `/WormholePortal/Demo/Variant_Shooter/Blueprints/Pickups/Weapons/BP_ShooterWeapon_PortalGun` | A gameplay-facing portal placement example |
| Physics cube | `BP_PhysicsCube` | Simulated Physics transit setup |
| Projectile cube | `BP_ProjectileCube` | Projectile transit setup |
| Long mesh | `BP_LongMesh` | Boundary crossing and fit/collision behavior |
| Actor spawner | `BP_SActorSpawner` | Repeatable sample-Actor spawning |
| Clip material | `/WormholePortal/Demo/Materials/M_VisualClip` | Integration of the transit clip material function |
| Clip material function | `/WormholePortal/Materials/MaterialFunctions/MF_WPTransitClip` | Runtime material function used by Material Clip-compatible materials |
| Translucent material | `/WormholePortal/Demo/Materials/M_Translucent` | Portal-scene material behavior to evaluate in context |
| Bundled sample assets | `/WormholePortal/Generated` | Read-only Voxel data used by the demo, plus LUT assets available for an explicit per-Portal override |

`/WormholePortal/Generated` is installed, read-only Plugin Content. The included
demo references its bundled Voxel data. Its LUT catalog is not the runtime
default: unless a Portal has **LUT Asset Override**, the runtime searches the
configured project catalog, which defaults to
`/Game/WormholePortal/Generated/LUT/DA_WPLUTCatalog`. If no compatible baked LUT
is found and **Allow Runtime LUT Fallback** is enabled, compatible transient data
is generated asynchronously. New LUT and Voxel bakes default to the writable
project path `/Game/WormholePortal/Generated`; never select Plugin Content as a
bake output.

The sample C++ module also contains focused reference implementations:

- `UWPPortalGunComponent`;
- `UWPGrabComponent`;
- `AWPSampleActorSpawner`;
- `AWPTransitBomb`;
- `AWPSampleTurret`;
- `AWPDemoRoom`; and
- `AWPDemoRoomManager`.

Use [Reference](../reference.md#integration-examples) for the supported runtime
module dependency and minimal integration patterns.

## Input and included dependencies

The sample uses Enhanced Input assets under `/WormholePortal/Demo/Input` and
`/WormholePortal/Demo/Variant_Shooter/Input`. The bindings below were verified
directly from the shipped Mapping Contexts in UE 5.8:

| Mapping Context | Input Action | Keyboard / mouse | Gamepad |
| --- | --- | --- | --- |
| `IMC_Default` | `IA_Jump` | `Space Bar` | Face Button Bottom |
| `IMC_Default` | `IA_Move` | `W`, `A`, `S`, `D`; Arrow Keys | Left Stick 2D |
| `IMC_Default` | `IA_Look` | — | Right Stick 2D |
| `IMC_MouseLook` | `IA_MouseLook` | Mouse 2D | — |
| `IMC_Weapons` | `IA_SwapWeapon` | Left Shift | Face Button Top |
| `IMC_Weapons` | `IA_Shoot` | Left Mouse Button | Right Trigger Axis; Right Shoulder |
| `IMC_Weapons` | `IA_FirePortalB` | Right Mouse Button | — |
| `IMC_Weapons` | `IA_PortalDir` | Mouse Wheel Axis | — |
| `IMC_Weapons` | `IA_Grab` | Left Shift | — |

If you remap the sample, treat the supplied Mapping Contexts and the in-map
prompts as the authoritative runtime state rather than assuming a fixed layout.

The plugin descriptor enables these Unreal plugins:

- **Enhanced Input**;
- **StateTree**; and
- **Gameplay StateTree**.

Enhanced Input is used by the sample controls. The included shooter variant
contains StateTree-based AI content, so keep the descriptor dependencies enabled
when evaluating the complete demo.

## Reuse demo content safely

Treat the demo as an example and test fixture, not as an application framework.

1. Duplicate the Blueprint or asset you intend to adapt into your project's own
   `/Game/...` content folder.
2. Preserve references to Wormhole Portal runtime types, material functions,
   and generated data that the copy still needs.
3. Replace sample-specific input, GameMode, UI, and AI assumptions with the
   equivalents from your project.
4. Re-run Transit Manager on copied Actors.
5. Re-bake Voxel data after collision changes and bake LUT data for the metric
   domain used by the destination level.
6. Test the copy in PIE, Standalone, and a packaged build.

Do not edit installed plugin sample assets as the only copy of production work;
a plugin update can replace them. For C++, depend on `WormholePortalRuntime`
from your own module and adapt the sample logic instead of making gameplay code
depend indefinitely on the sample module.

## Demo troubleshooting

| Symptom | What to check |
| --- | --- |
| The `WormholePortal Content` folder is missing | Enable **Show Plugin Content** in Content Browser Settings. |
| The map or assets show missing dependencies | Confirm Enhanced Input, StateTree, and Gameplay StateTree are enabled, then restart the Editor. |
| The portal is absent in the viewport | Use the documented map and a primary Perspective Game View in Win64 DX12 SM6; confirm the linked pair and LUT are valid. |
| An Actor does not cross | Run Transit Manager and resolve the exact setup status. For Physics, verify a supported body is simulating. |
| Voxel Collision reports missing data | Enable **Use Voxel Collision** and bake each reported supported Primitive. |
| Packaged output cannot find LUT data | Verify `/Game/WormholePortal/Generated/LUT` under **Additional Asset Directories to Cook**. |

For deeper diagnosis, continue with [Issues](../issues/index.md) and include the
[support checklist](../support/index.md#before-requesting-support) with any report.
