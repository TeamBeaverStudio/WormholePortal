# 자주 묻는 질문

## 어떤 Unreal Engine 버전을 기준으로 하나요?

버전 1.0의 공식 테스트·지원 환경은 Unreal Engine 5.8, Win64, DX12,
SM6입니다. Linux, macOS, DX11, 모바일, 콘솔 및 Dedicated Server는 현재
지원 대상으로 선언하지 않습니다. 다른 환경은 실제 프로젝트에 사용하기
전에 직접 검증하세요.

## 모든 포털에 Linked Portal이 필요한가요?

목적지를 렌더링하고 Transit, Trace, Audio, Lighting, 목적지 Streaming을
사용하려면 유효한 포털 쌍이 필요합니다. **Linked Portal**을 지정하면
반대 방향 연결이 생성되고 포털 쌍의 Metric 값이 동기화됩니다.

## Actor Scale로 포털 크기를 바꿔도 되나요?

안 됩니다. 연결된 두 Portal Actor를 `(1, 1, 1)`로 유지하고 다음 속성을
사용하세요.

- **Portal Radius**
- **Throat Half Length**
- **Transition Length**

Actor는 다른 Scale 값을 입력받을 수 있지만 Metric 반지름 계산에는 그
Scale을 적용하지 않습니다. 따라서 표시 Mesh, Collision과 Portal Math가
달라질 수 있으므로 제작 계약상 단위 Scale을 유지해야 합니다.

## 런타임에 포털이 커지는 효과는 어떻게 만드나요?

최종 크기를 `Initialize Physical Metric`으로 한 번 설정한 뒤
`Set Portal Visual Scale`을 `0`에 가까운 값에서 `1`까지 애니메이션하세요.
이 방식은 Collision, Bounds, LUT 및 Capture 정책을 바꾸지 않습니다.

실제 통과 반지름까지 커져야 한다면 `Set Uniform Physical Metric Scale`을
사용할 수 있지만 더 비쌉니다. `Set Portal Radius`, `Set Throat Half Length`,
`Set Transition Length`의 독립적인 런타임 변경은 BeginPlay 이후
거부됩니다. `Set Metric Parameters`는 Deprecated이므로 새 코드에서는
`Initialize Physical Metric`을 사용하세요.

## LUT를 반드시 Bake해야 하나요?

**Allow Runtime LUT Fallback**이 켜져 있다면 필수는 아닙니다. Baked LUT가
없을 때 기본 설정은 호환되는 임시 데이터를 비동기로 생성할 수 있습니다.

시작 준비 시점을 예측 가능하게 만들려면 미리 Bake하는 것을 권장합니다.
`Transition Length = 0`인 포털은 Analytic 경로를 사용하므로 LUT가 필요하지
않습니다.

## Scene Capture, VR, Split Screen 또는 모든 Movie Render Queue Pass에서 포털이 보이나요?

현재는 지원하지 않습니다. Renderer는 하나의 Primary Perspective
Non-stereo Game View를 지원합니다.

Scene Capture, Reflection Capture, Planar Reflection, Stereo/VR, Split
Screen 및 다른 Multi-view는 제외됩니다. Movie Render Queue는 Main
Deferred Mono Beauty 출력을 지원하며, Custom Pass, Virtual Texture Pass,
Stereo 및 Multi-view 출력에는 포털이 합성되지 않습니다.

## Dedicated Server에서 포털이 렌더링되나요?

아닙니다. `WormholePortalRenderer` 모듈은 Server Target에서 제외되며 버전
1.0은 Dedicated Server를 공식 지원 대상으로 선언하지 않습니다. 네트워크
게임에서는 프로젝트의 Replication, Ownership, Relevancy와 Transit 동작을
사용할 실제 Client/Server 구성에서 검증하세요.

## 어떤 Actor가 Transit을 사용할 수 있나요?

Actor에 `WPTransitComponent`를 추가해 Transit을 활성화합니다. 지원하는
타입은 다음과 같습니다.

- `Character`
- `Projectile`
- `Pawn`
- `Physics`
- `Auto`

`Auto`는 Character, Projectile, Pawn, Physics 순서로 검사합니다. 각
타입에 필요한 Movement, Collision 또는 Physics Component도 올바르게
구성해야 합니다. PIE를 실행하기 전에 **Transit Manager**로 설정 문제를
확인하세요.

모든 타입은 Actor가 직접 소유하는 Movable Collision Primitive가 필요하고,
그중 하나 이상이 포털 Trigger와 Overlap할 수 있어야 합니다. Physics는
Physics Collision/Simulation이 켜진 지원 단일 Body Primitive를 하나 이상
요구합니다. 유효한 Mesh의 Static Mesh 또는 Shape Component를 지원하며
Instanced Static Mesh는 제외합니다.

## DoesNotFitGate는 무엇을 의미하나요?

해당 Transit에서 Actor가 포털의 사용 가능한 입구 안에 들어가지 않는다는
의미입니다. **Portal Radius**를 늘리거나 Actor의 Collision Bounds를
줄이고 Collision 구성을 개선하세요. Portal Actor를 스케일링하면 안
됩니다.

## Voxel Collision은 무엇을 Bake할 수 있나요?

Instanced가 아닌 Static Mesh의 Sphere, Box, Capsule, Convex Simple
Collision과 `BoxComponent`, `SphereComponent`, `CapsuleComponent`를
지원합니다. Tapered Capsule, Level Set, Skinned Triangle Collision과
Instanced Static Mesh는 지원하지 않습니다.

**Max Voxel Count**는 Actor 전체가 아니라 각 참여 Primitive별 상한입니다.
기본 출력 경로는 `/Game/WormholePortal/Generated/Voxels`이며, 사용자 지정
경로도 대소문자를 구분하는 `/Game/` Long Package Directory여야 합니다.

## Max Portal Depth는 무엇인가요?

Portal-aware Line Trace는 여러 포털을 통과할 수 있습니다. **Max Portal
Depth**는 연결된 끝점 사이의 무한 반복을 막습니다.

기본값은 `4`입니다. 의도한 논리 Trace에 더 많은 통과가 실제로 필요할
때만 늘리세요.

## Portal-aware Trace에는 어떤 Collision Channel을 사용하나요?

Hit하려는 Object에 맞는 일반 Collision Channel을 사용하세요.

`WPPortalTrace`를 사용하면 안 됩니다. 이 채널은 플러그인의 내부 포털
교차 감지 전용입니다.

## Transit은 Multiplayer를 지원하나요?

포털의 Movement, Link, Metric State가 복제되며 Transit Component도 서버
권한의 Transit State를 복제합니다.

Transit에 참여하는 게임 Actor도 Unreal의 일반적인 Replication, Ownership,
Relevancy, Movement 규칙에 맞게 구성해야 합니다. 실제 프로젝트에서
사용할 네트워크 조건으로 전체 동작을 테스트하세요.

## World Partition과 함께 사용할 수 있나요?

네. Runtime은 연결된 포털 목적지 주변의 Streaming을 요청할 수 있습니다.
목적지 로딩 중에는 Transit이 `NotReady`를 반환할 수 있습니다.

각 Portal Actor의 **Streaming Preload Distance**, **Streaming Release
Distance**, **Streaming Query Interval**로 조절합니다. Blueprint에서는
`IsLinkedPortalAreaReady()`로 확인할 수 있습니다.

## 어떤 Sound가 포털을 통과하나요?

재생 중인 Audio Component에 Spatialization과 Attenuation이 켜져 있어야
하며, 가청 범위가 포털까지 도달해야 합니다.

UI/2D Sound, Source Bus Asset, Multiple-instance Component, 비활성 Source,
포털 Sphere 내부의 Source, 생성된 Proxy는 제외됩니다. 현재 Portal Audio는
하나의 주 Listener와 한 번의 Portal Hop을 지원합니다.

## 어떤 Light 타입을 전달하나요?

Point 및 Spot Light를 전달합니다. Directional Light는 Light Collection
Subsystem에서 수집될 수 있지만, 현재 Transmission Subsystem은 Directional
Light 경로를 만들지 않습니다. Rect Light도 전달하지 않습니다.

나머지 제한사항은 [Portal Lighting 문제 해결](../issues/index.md#portal-lighting)을
참고하세요.

## Material Clip은 자동으로 적용되나요?

아닙니다. Transit Component에서 **Use Material Clip**을 켜고 제공되는
Transit Clip Material Function을 Actor의 Material에 연결해야 합니다.

Material은 설정된 **Clip Base Index**부터 연속된 Custom Primitive Data
네 개를 읽습니다. 기본 Index는 `28`입니다. 제공 함수의 정확한 경로는
`/WormholePortal/Materials/MaterialFunctions/MF_WPTransitClip`입니다.

## Material Clip과 Voxel Collision은 무엇이 다른가요?

**Material Clip**은 포털 경계와 겹치는 동안 호환 Mesh가 보이는 방식만
바꿉니다. **Voxel Collision**은 통과 중 지원되는 Static Mesh 또는
Box/Sphere/Capsule Shape Component의 충돌을 양쪽 공간에 표현하는 방식을
바꿉니다.

서로 독립된 기능이므로 하나만 사용하거나, 둘 다 사용하거나, 둘 다 끌 수
있습니다.

## 생성된 LUT Asset이 패키지에 포함되나요?

Editor 모듈은 설정된 **Generated LUT Asset Path**를 프로젝트 Cook
디렉터리에 추가하려고 시도합니다. 패키징 전에 Packaging Settings에 해당
경로가 있는지 확인하세요. 특히 `Config/DefaultGame.ini`가 읽기 전용이거나
생성 경로를 바꿨다면 반드시 확인해야 합니다.

코드 기본 경로는 `/Game/WormholePortal/Generated/LUT`입니다.

## 포함된 데모는 어디에서 여나요?

Content Browser에서 **Show Plugin Content**를 켜고 다음 맵을 엽니다.

```text
/WormholePortal/Demo/Lv_WormholePortal_Content_Demo
```

`Dev_Demo`는 내부 제작용이며 공개 시작점이 아닙니다. 단계별 확인 항목은
[데모 가이드](../demo/index.md)를 참고하세요.

## 동적 Cubemap 해상도는 어떻게 선택되나요?

Safe Proxy의 화면 높이 비율을 Project Settings의 **Resolution Tiers**와
비교합니다. 카메라가 Safe Proxy 내부에 있으면 **Inside SafeProxy
Resolution**이 우선하고, Tier 배열이 비어 있으면 **Lowest Visible
Resolution**을 사용합니다.

해상도는 다음 8 배수로 올림되어 `8–2048` 범위로 제한되고, Tier는
해상도가 줄어드는 순서가 되지 않도록 보정됩니다. 플러그인 내부 VRAM
Budget으로 이 값을 다시 낮추는 CVar는 없습니다.

## Runtime 성능은 어떻게 확인하나요?

다음 명령을 실행하세요.

```text
stat WormholePortal
```

실제 GPU 실행 비용은 `stat gpu` 또는 ProfileGPU에서 확인합니다. 거부
원인은 **Output Log**에서 다음 값으로 필터링하세요.

```text
LogWormhole
```

Non-Shipping 빌드에서 Transit 거부 상세 정보가 필요하면 다음 명령을 실행한
뒤 문제를 다시 재현하세요.

```text
Log LogWormhole Verbose
```

`RuntimeReason`, `ResolveReason`, `FailedComponents`를 함께 수집합니다.

## 공식 지원은 어디로 문의하나요?

Team Beaver Studio의 공식 지원 이메일은
[beavergametech@gmail.com](mailto:beavergametech@gmail.com)입니다. 엔진과
플러그인 버전, 재현 절차, 기대/실제 결과, `LogWormhole` 로그와 관련 설정을
함께 보내세요. 자세한 체크리스트는 [호환성 및 지원](../support/index.md)에 있습니다.
