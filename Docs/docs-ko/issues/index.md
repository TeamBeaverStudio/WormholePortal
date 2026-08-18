# 문제 해결

이 페이지에서는 Wormhole Portal의 현재 제한사항과 예상대로 동작하지 않을
때 먼저 확인할 항목을 안내합니다.

!!! info "현재 호환성 기준"

    버전 1.0의 공식 테스트·지원 환경은 Unreal Engine 5.8, Win64, DX12,
    SM6입니다.

## 알려진 제한사항 { #known-limitations }

| 영역 | 현재 제한사항 |
| --- | --- |
| 렌더링 View | Win64, DX12, SM6의 단일 Primary Perspective Non-stereo Game View에서 포털 Composite를 지원합니다. |
| 미지원 View | Scene Capture, Reflection Capture, Planar Reflection, Orthographic, Stereo/VR, Split Screen 및 다른 Multi-view에는 포털이 합성되지 않습니다. |
| Movie Render Queue | Main Deferred Mono Beauty 출력을 지원합니다. Custom Pass, Virtual Texture Pass, Stereo 및 Multi-view 출력에는 포털이 합성되지 않습니다. |
| 포털 Scale | 연결된 두 Portal Actor의 Actor Scale은 모두 `(1, 1, 1)`이어야 합니다. 크기는 Metric 속성으로 변경합니다. |
| Transit Primitive | 지원되는 Movable Collision Primitive Component가 필요합니다. Instanced Static Mesh Component는 Transit Primitive로 지원하지 않습니다. |
| Physics Transit | Physics Collision 및 Simulation이 켜진 지원 단일 Body Primitive가 하나 이상 필요합니다. 유효한 Mesh의 Static Mesh 또는 Shape Component를 지원하며 Instanced Static Mesh는 제외합니다. |
| Voxel Collision | Instanced가 아닌 Static Mesh의 Sphere, Box, Capsule, Convex Simple Collision과 Box/Sphere/Capsule Shape Component Bake를 지원합니다. |
| Portal Trace | 공개 Blueprint API는 Line Trace By Channel을 제공합니다. Object Type, Profile 및 Sweep 버전은 구현되어 있지 않습니다. |
| Ray 변환 | `Transform Ray Through Portal`은 포털 바깥에서 내부 방향으로 접근하는 Ray를 대상으로 합니다. |
| Trace Exit Offset | **Portal Exit Offset**은 즉시 자기 자신을 다시 Hit하는 것을 막지만 남은 Trace 거리에서 차감되지 않습니다. 출구 표면과 Offset 시작점 사이 구간도 Collision을 검사하지 않습니다. |
| Audio | 하나의 주 Listener, 한 번의 Portal Hop, 조건을 만족하는 활성 Spatialized 3D Audio Component를 지원합니다. |
| Lighting | Point 및 Spot Light만 전달합니다. Directional Light는 내부 수집 대상일 수 있지만 전달되지는 않습니다. |

## 포털이 보이지 않음

다음 순서로 확인하세요.

1. Portal Actor에 유효한 **Linked Portal**이 있는지 확인합니다.
2. 두 포털의 Actor Scale이 `(1, 1, 1)`인지 확인합니다.
3. 공식 지원 환경인 Win64, DX12, SM6의 Perspective Game View를 사용합니다.
4. Scene Capture, Reflection, Stereo 또는 Split Screen이 아닌 Primary
   Mono View인지 확인합니다.
5. **Transition Length**가 0보다 크다면 비동기 Runtime LUT Fallback이
   끝날 때까지 기다리거나 호환되는 LUT를 Bake합니다.
6. **Output Log**에서 `LogWormhole`을 필터링하고 Renderer 항목의
   `Reason=...` 필드를 확인합니다. 미지원 View의 결과 변경까지 기록하려면 문제를
   재현하기 전에 `Log LogWormhole VeryVerbose`를 임시로 실행합니다.

대표적인 Renderer 사유는 다음과 같습니다.

- `InvalidPairOrTransform`
- `MetricInvalid`
- `MetricMismatch`
- `UnsupportedScale`
- `CaptureNotSubmitted`
- `MissingTextureReference`
- `UnresolvedReferencedTexture`
- `WrongTextureDimension`

미지원 View에서는 다음 사유가 기록될 수 있습니다.

- `SceneCaptureOrReflection`
- `UnsupportedGameProjectionOrFeatureLevel`
- `StereoView`
- `NotExactlyOneFamilyView`
- `NonPrimaryPlayerView`

## WPPortalTrace 경고가 계속 표시됨

Wormhole Portal은 `WPPortalTrace`라는 전용 Trace Channel이 필요합니다.

가능하면 시작 알림에서 **Add Automatically**를 사용하고 Editor를 다시
시작하세요.

자동 설정에 실패한다면 다음을 확인하세요.

| 문제 | 해결 방법 |
| --- | --- |
| 잘못된 Channel Type | `WPPortalTrace`가 Object Channel이 아니라 **Trace Channel**인지 확인합니다. |
| Channel 불일치 | 플러그인의 **Portal Trace Channel**과 Unreal에서 할당된 Channel을 일치시킵니다. |
| 남은 Game Channel 없음 | 사용하지 않는 Game Trace Channel을 확보한 뒤 다시 시도합니다. |
| 읽기 전용 설정 | `Config/DefaultEngine.ini`를 체크아웃하거나 쓰기 가능 상태로 변경합니다. |
| 수동 설정 | **Project Settings > Engine > Collision**에서 `WPPortalTrace`를 만들고 **Default Response: Ignore**로 지정한 뒤 플러그인 설정을 맞춥니다. |

Portal-aware Trace의 일반 Collision Channel로 `WPPortalTrace`를 전달하지
마세요. 플러그인의 내부 포털 감지 전용입니다.

## LUT Bake 실패

**Tools > Wormhole Portal > Bake All LUTs**를 열고 다음을 확인하세요.

- Editor World와 하나 이상의 Portal Actor가 존재해야 합니다.
- **Generated LUT Asset Path**가 올바른 `/Game/...` Long Package
  경로여야 합니다.
- Portal Metric 값이 유효해야 합니다.
- 요청한 `Transition Length / Portal Radius` 범위가 LUT Domain 안에
  있어야 합니다.
- Source Control에서 생성 Package를 체크아웃하고 저장할 수 있어야
  합니다.
- Bake가 취소되지 않았는지 확인합니다.

기본 생성 경로는 다음과 같습니다.

```text
/Game/WormholePortal/Generated/LUT
```

성공 알림은 다음 문구로 시작합니다.

```text
LUT ready:
```

**Allow Runtime LUT Fallback**이 켜져 있다면 Baked LUT는 필수가 아니지만,
시작 준비 시점을 예측 가능하게 만들려면 Bake를 권장합니다.
`Transition Length = 0`은 Analytic 경로를 사용하므로 LUT가 필요하지
않습니다.

## Editor에서는 LUT가 보이지만 패키지에서 보이지 않음

Editor 모듈은 **Generated LUT Asset Path**를
`DirectoriesToAlwaysCook`와 동기화하려고 시도합니다.

`Config/DefaultGame.ini`를 업데이트하지 못했다는 경고가 표시되면:

1. 파일을 체크아웃하거나 쓰기 가능 상태로 변경합니다.
2. Editor를 다시 시작해 시작 시 동기화를 재실행하거나, Project Packaging
   Settings에 경로를 직접 추가합니다.
3. **Additional Asset Directories to Cook**에 설정한 경로가 포함되어
   있는지 확인합니다.

기본 경로를 사용하는 설정은 다음과 같습니다.

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysCook=(Path="/Game/WormholePortal/Generated/LUT")
```

**Generated LUT Asset Path**를 변경했다면 변경한 `/Game/...`
경로를
사용해야 합니다.

## Transit이 시작되지 않음

**Tools > Wormhole Portal > Transit Manager**를 열고 Actor 상태를
확인하세요.

| 상태 | 확인할 내용 |
| --- | --- |
| `Not Supported` | Actor가 Character, Projectile, Pawn 또는 Physics 요구사항과 맞지 않습니다. |
| `Needs Setup: Transit is disabled` | `WPTransitComponent`의 **Transit Enabled**를 켭니다. |
| `Needs Setup: Transit settings are invalid` | 선택한 Transit Type과 Component 구성을 확인합니다. |
| `Needs Setup: No movable collision Primitive was found` | Actor가 소유한 Movable Collision Primitive Component를 추가합니다. |
| `Needs Setup: No supported Primitive is simulating Physics.` | Physics Transit에는 Physics Collision과 Simulation이 켜진 지원 Static Mesh 또는 Shape Component가 필요합니다. |
| `Needs Setup: Bake Voxel Body...` | Voxel Collision을 켜고 표시된 각 호환 Primitive의 Voxel Data를 Bake합니다. |

Runtime 거부 상세 로그는 Non-Shipping 빌드에서 `LogWormhole`의 Verbose
상세도를 켰을 때 다음 형태입니다.

```text
Log LogWormhole Verbose
```

```text
[Transit][Rejected] TimestampSeconds=... RuntimeReason=EWPTransitFailReason::... ResolveReason=EWPTransitResolveFailReason::... FailedComponents=... Actor=... SourcePortal=... DestinationPortal=... TransitType=EWPTransitType::...
```

`RuntimeReason`은 Transit Lifecycle 실패, `ResolveReason`은 세부 설정 검사
실패, `FailedComponents`는 관련 Component를 나타냅니다. 이 상세 거부 행은
Shipping 빌드에서 컴파일되지 않습니다.

가능한 Runtime 사유에는 `NotReady`, `PortalUnavailable`, `AlreadyInTransit`,
`CooldownActive`, `DoesNotFitGate`, `UnsupportedActor`, `InvalidSetup`,
`TransitDisabled`, `MissingPrimitives`, `MissingPhysicsMesh`,
`MissingVoxelData` 등이 있습니다.

World Partition에서 `NotReady`는 연결된 영역의 Streaming이 끝나지 않았다는
뜻일 수 있습니다. `IsLinkedPortalAreaReady()`가 `true`를 반환할 때까지
Actor가 경계에 들어가지 않게 하세요.

## Voxel Body Bake 실패

다음을 확인하세요.

1. `WPTransitComponent`에서 **Use Voxel Collision**이 켜져 있어야 합니다.
2. Static Mesh를 사용한다면 원본 Asset을 저장하고 유효한 Simple
   Collision을 준비합니다.
3. Static Mesh Collision은 Sphere, Box, Capsule 또는 Convex여야 합니다.
4. Shape Component는 `BoxComponent`, `SphereComponent`,
   `CapsuleComponent` 중 하나여야 합니다.
5. **Voxel Size**가 0보다 커야 합니다.
6. **Max Voxel Count**가 `1–512` 사이여야 합니다. 이 값은 각 참여
   Primitive별 상한입니다.

기본 출력은 `/Game/WormholePortal/Generated/Voxels`입니다. 사용자 지정
**Generated Voxel Asset Path**도 대소문자를 구분하는 `/Game/` Prefix의
유효한 Long Package Directory여야 합니다.

Tapered Capsule, Level Set, Skinned Triangle Collision 및 다른 미지원
Collision Type은 현재 Voxel Baker로 Bake할 수 없습니다.

## Material Clipping이 작동하지 않음

Material Clipping은 선택 기능이며 임의의 Material을 자동으로 수정하지
않습니다.

다음을 확인하세요.

- Transit Component에서 **Use Material Clip**이 켜져 있어야 합니다.
- Material에서
  `/WormholePortal/Materials/MaterialFunctions/MF_WPTransitClip`을
  사용하고 Compile/Save합니다.
- Material과 Project Settings가 같은 **Clip Base Index**를 사용해야
  합니다.
- 해당 Index부터 연속된 Custom Primitive Data 슬롯 네 개가 비어 있어야
  합니다.

기본 **Clip Base Index**는 `28`입니다.

## 포털 너머의 Audio가 들리지 않음

Portal Audio에는 조건을 만족하는 활성 3D Audio Component가 필요합니다.

다음을 확인하세요.

- Spatialization이 켜져 있어야 합니다.
- Component에 Spatialization이 켜진 Attenuation Settings가 있어야
  합니다.
- UI/2D Sound 또는 Source Bus Asset이 아니어야 합니다.
- Component가 등록되고 재생 중이며 Multiple Instance 재생으로 설정되지
  않아야 합니다.
- Attenuation Range가 포털까지 도달해야 합니다.
- Source가 포털 Sphere 바깥에 있어야 합니다.
- Component와 소유 Actor에 `WP.PortalAudio.Disabled` Tag가 없어야
  합니다.

런타임 조건이 맞으면 `PlayWhenSilent` Source는 내부 Post-effect Source
Bus 경로를 사용할 수 있습니다. 그 밖의 조건에 맞는 Source와 해당 경로를
사용할 수 없는 경우에는 가능한 범위에서 별도 재생 인스턴스를 사용합니다.
생성된 Proxy는 재전송 대상에서 제외되므로 Audio는 현재 포털을 한 번만
통과합니다.

## 포털 Lighting이 보이지 않음 { #portal-lighting }

현재 Proxy Light 경로는 Point 및 Spot Light만 전달합니다.

다음을 확인하세요.

- Source가 Point 또는 Spot Light여야 합니다.
- 켜져 있고 World에 영향을 주며 Intensity가 0보다 커야 합니다.
- Attenuation Volume이 입구 포털에 닿아야 합니다.
- Source가 포털 내부에 있으면 안 됩니다.
- 두 포털이 Unit Scale과 같은 Radius를 사용해야 합니다.
- Light Component와 소유 Actor에 `WP.PortalLight.Disabled` Tag가 없어야
  합니다.

전달되는 Source Shadow에는 **Enable Portal Source Shadows**, **Cast
Shadows**, **Cast Dynamic Shadows**도 켜져 있어야 합니다. Directional
Light, Rect Light, 서로 다른 Radius, 포털 내부의 Light, GI, Reflection,
Volumetric, Translucent Shadow, Colored Shadow는 전달하지 않습니다.

## 문제 제보 시 준비할 정보

- Unreal Engine 버전
- 플러그인 버전 또는 Commit
- PIE, SIE, Standalone, Packaged 또는 Movie Render Queue 중 어디서
  발생했는지
- 최소 재현 절차
- `LogWormhole`로 필터링한 Output Log
- 표시된 `RuntimeReason`, `ResolveReason`, `FailedComponents` 값
- Portal Actor와 관련 Component 설정 스크린샷
- 렌더링 또는 Capture 문제라면 `stat WormholePortal` 결과

준비한 정보는 공식 지원 이메일
[beavergametech@gmail.com](mailto:beavergametech@gmail.com)으로 보내세요.
보내기 전에 [호환성 및 지원](../support/index.md)의 지원 범위와 민감 정보 안내를
확인하세요.
