# 기능

Wormhole Portal은 물리학에서 영감을 받은 구형 포털 렌더러와 통과,
트레이스, 오디오, 조명, World Partition 시스템을 함께 제공합니다. 모든
시스템이 동일한 포털 연결과 Metric 모델을 사용하므로 포털을 통해 보이는
목적지와 실제 게임플레이 이동 경로가 일치합니다.

## 입체적인 웜홀 렌더링

포털은 평면 창이 아니라 볼륨으로 렌더링됩니다. 입구, 목, 전이 구간을
통해 목적지 화면이 연속적인 구형 이미지로 휘어집니다.

- **입체적인 목:** 카메라가 포털 볼륨에 접근하고 내부로 들어갈 수
  있습니다.
- **공간 렌즈 효과:** 설정된 웜홀 Metric에 따라 광선이 휘어집니다.
- **연속적인 목적지 화면:** 입구에서 목 안쪽까지 연결된 공간이 이어져
  보입니다.
- **공용 LUT 렌더링:** 베이크한 룩업 데이터로 광선 매핑을
  가속합니다. 설정을 켜 두면 호환되는 데이터가 없을 때 런타임 대체
  데이터를 비동기로 만들 수 있습니다.
- **적응형 캡처:** 포털의 가시성과 화면 점유율에 따라 Cubemap 해상도가
  달라지며, Safe Proxy 내부에는 별도 해상도를 사용합니다.

<!-- CAPTURE SLOT F-01: 입구, 목, 강한 렌즈 효과, 목적지 화면을 보여 주는 시네마틱 인게임 영상. -->

**Project Settings > Plugins > Wormhole Portal > Scene Capture**의 품질
옵션은 포털 Cubemap 캡처에만 적용됩니다. 플레이어의 일반 Game View에는
영향을 주지 않습니다.

## 연결된 포털 쌍과 Metric

각 `WormholePortalActor`는 다른 포털 하나와 연결됩니다. **Linked
Portal**을 지정하면 반대 방향 연결도 생성되고, 포털 쌍의 Metric 값이
동기화됩니다.

형태는 세 속성으로 정의합니다.

| 속성 | 의미 |
| --- | --- |
| **Portal Radius** | 물리 통과 판정에 사용하는 중앙 `l = 0` Seam의 반지름 |
| **Throat Half Length** | 연결된 목 길이의 절반 |
| **Transition Length** | 일반 공간과 목 사이의 전이 거리 |

Mouth Radius는 `Portal Radius + Throat Half Length`이며, Transition
Radius는 그 결과에 **Transition Length**를 더한 값입니다.

!!! warning "Actor Scale 대신 Metric 사용"

    각 포털의 Transform Scale은 `(1, 1, 1)`로 유지하세요. Metric 계산은
    설정된 치수를 사용하며 Actor Scale을 크기 조절 값으로 취급하지
    않습니다.

## 액터 통과

포털을 통과해야 하는 액터에 `WPTransitComponent`를 추가합니다.
**Transit Type: Auto**는 액터와 움직일 수 있는 충돌 컴포넌트를 검사해
호환되는 방식을 선택합니다.

지원하는 통과 유형은 다음과 같습니다.

- **Character**
- **Pawn**
- **Projectile**
- **Physics**

통과 중에는 원본 액터와 반대편 액터의 관계를 유지하고, 연결된 포털을
기준으로 Transform과 이동을 매핑하며, 즉시 역방향으로 다시 통과하지
않도록 짧은 Cooldown을 적용합니다. 포털 연결, Metric, 이동 및 통과 관계
상태는 복제됩니다. 전체 게임플레이 동작은 프로젝트의 네트워크 모델에서
직접 테스트하세요.

<!-- CAPTURE SLOT F-02: 같은 포털 쌍을 Character와 물리 오브젝트가 통과하는 짧은 게임플레이 영상. -->

표현과 충돌을 위한 두 가지 선택 기능도 있습니다.

- **Material Clip**은 연속된 네 개의 Custom Primitive Data 슬롯에 Sphere
  Clip의 중심, Normal, 반지름을 기록해, 호환 머티리얼이 경계 너머 부분을
  숨길 수 있게 합니다.
- **Voxel Collision**은 오브젝트가 두 공간에 걸쳐 있는 동안 참여하는
  Static Mesh의 충돌을 베이크한 박스 복셀 바디로 교체합니다.

Voxel Collision은 사용할 수 있는 Simple Collision이 있는 Static Mesh를
대상으로 합니다. Transit Component에서 기능을 켠 뒤 해당 컴포넌트의
Details 패널에서 **Bake Voxel Body**를 실행하세요.

## 포털 인식 트레이스

일반 Unreal Line Trace는 연결된 포털 공간으로 이어지지 않습니다. 포털
인식 API는 `WPPortalTrace`로 포털을 별도 감지하고 연결된 출구에서
논리적인 광선을 이어 갑니다.

Blueprint와 C++ API는 다음을 제공합니다.

- 단일 및 다중 히트 Line Trace
- 간단한 결과와 상세 결과
- 모든 구간을 합친 논리 거리
- 통과 횟수와 포털별 진입·진출 이벤트

프로젝트에는 `WPPortalTrace`라는 Trace Channel이 필요합니다. 플러그인
시작 알림에서 채널을 자동으로 추가하고 설정을 맞출 수 있으며, 변경 후
에디터를 다시 시작해야 합니다.

<!-- CAPTURE SLOT F-03: 하나의 Line Trace가 포털에 들어가 연결된 출구에서 이어지는 Blueprint 또는 디버그 화면. -->

## 포털을 통한 공간 오디오

오디오 서브시스템은 공간화된 Audio Component를 찾아 연결된 포털
반대편에 재방사 프록시를 만듭니다.

- 양방향 경로를 서로 독립적으로 계산합니다.
- 음원에서 입구까지, 출구에서 리스너까지의 Occlusion을 따로
  검사합니다.
- 경로가 막히면 볼륨을 줄이고 Low-pass Filter를 적용할 수 있습니다.
- 런타임에 생성된 공간화 Audio Component도 주기적으로 찾습니다.

현재 모델은 하나의 주 리스너와 한 번의 포털 Hop을 지원합니다. Audio
Component 또는 소유 액터에 `WP.PortalAudio.Disabled` 태그를 추가하면
대상에서 제외할 수 있습니다.

<!-- CAPTURE SLOT F-04: 공간화 음원이 포털을 통해 들리고 Occlusion이 적용되는 전후를 소리와 함께 보여 주는 영상. -->

## Point 및 Spot Light 전달

입구 포털에 영향을 주는 Point Light와 Spot Light는 연결된 출구에
동기화된 프록시를 만들 수 있습니다. 프록시는 원본의 Transform과 조명
속성을 따라가고, 조명 범위를 포털 입구로 제한하며, 그림자를 위해 원본
공간의 가시성도 전달할 수 있습니다.

<!-- CAPTURE SLOT F-05: 움직이는 Point 또는 Spot Light가 연결된 출구 너머의 지오메트리를 비추는 짧은 영상. -->

현재 조명 모델은 Directional Light, Global Illumination, 반사, 볼류메트릭,
반투명 그림자, 색이 있는 그림자, 포털 내부에 있는 조명, 반지름이 서로
다른 포털 쌍을 전달하지 않습니다. 조명에 의존하는 장면을 만들기 전에
[문제 해결](../issues/#portal-lighting)을 확인하세요.

## World Partition 목적지 스트리밍

World Partition 레벨에서는 플레이어가 입구에 도달하기 전에 연결된
목적지 영역을 로드 상태로 유지할 수 있습니다.

- **Streaming Preload Distance**는 목적지 소스를 요청할 거리를 정합니다.
- **Streaming Release Distance**는 목적지를 해제할 거리를 정합니다.
- **Streaming Query Interval**은 거리를 다시 계산하는 주기를 정합니다.
- `IsLinkedPortalAreaReady()`로 목적지가 준비되었는지 확인할 수 있습니다.

반복적인 로드·언로드를 막으려면 Release Distance를 Preload Distance보다
크게 유지하세요. 연결된 영역이 준비되지 않았다면 목적지가 준비될 때까지
통과가 차단될 수 있습니다.

## 에디터 작업 흐름과 진단

플러그인은 **Tools > Wormhole Portal** 아래에 두 명령을 추가합니다.

| 도구 | 용도 |
| --- | --- |
| **Transit Manager** | 액터를 검사하고 설정 문제를 표시하며 호환되는 액터에 Transit을 추가 |
| **Bake All LUTs** | 현재 레벨의 포털에 사용할 공용 렌더링 룩업 데이터 생성 |

<!-- CAPTURE SLOT F-06: Ready, Needs Setup, Not Supported 행이 함께 보이는 Transit Manager. -->

포털 액터는 에디터에서 Metric 경계를 그릴 수 있고,
`WPTransitComponent`에는 런타임 통과 디버그가 있습니다. 더 넓은 범위의
진단에는 다음을 사용하세요.

- **Output Log**를 `LogWormhole`로 필터링합니다.
- `stat WormholePortal`로 플러그인 카운터와 CPU 시간을 확인합니다.
- 실제 GPU 실행 비용은 Unreal의 `stat gpu` 또는 GPU Profiler로
  확인합니다.

## 다음 문서

- [시작하기](../getting-started/)에서 실제 포털 쌍을 만드세요.
- 속성, Enum, 함수, 기본값은 [레퍼런스](../reference/)를 참고하세요.
- 알려진 제한과 해결 방법은 [문제 해결](../issues/)을 참고하세요.
