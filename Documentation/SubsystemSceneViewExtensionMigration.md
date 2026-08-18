# Subsystem · SceneViewExtension production 구조

> 기준일: 2026-07-20  
> 대상: `WormholePortal` 플러그인, Unreal Engine 5.8  
> 문서 성격: 현재 production 계약과 운영·검증 절차. 과거 CL 전환 과정은 마지막 부록에만 보존한다.

## 1. 현재 결론

웜홀의 최종 화면은 Actor별 material raster가 아니라 World 단위 Subsystem이 준비한 packet과
`FWPSceneViewExtension`의 Global Shader/RDG pass가 만든다.

```text
AWormholePortalActor
    endpoint transform/metric/link, collision/trigger
    non-raster OuterProxy bounds, authored streaming 설정
    Tick 없음, material/MID/capture/LUT 렌더 책임 없음
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
UWPRegistrySubsystem    UWPTransitSubsystem
pair topology/lifetime  gameplay transit event
          └─────────┬─────────┘
                    ▼
UWPRuntimeSubsystem
    모든 등록 pair의 Disabled/Warmup/Production 상태
    metric/transform/cube/LUT contract와 renderer packet
    고정 30 ms pair capture scheduler
          ├────────────────────────────┐
          ▼                            ▼
UWPCaptureManager       UWPLUTEndpointManager
capture component/RT 수명   endpoint LUT request/snapshot
          └────────────┬───────────────┘
                       ▼
IWPWormholeRenderer / FWPRendererService
    Game Thread packet → UObject 없는 Render Thread snapshot
                       ▼
FWPSceneViewExtension
    View 필터, pair preflight, 전역 endpoint 순서, RDG 연결
                       ▼
WPRayCommon.ush + WPComposite.usf
    analytic mask/SceneDepth + LUT/cube composite
                       ▼
최종 SceneColor
```

현재 경로에는 legacy material fallback이 없다.

- `M_WormholePortal`, `M_TempPortalRender` asset은 삭제됐다.
- `WormholeMask.ush`, `WormholeComposite.ush`는 삭제됐다.
- Actor는 runtime material, MID, material parameter를 만들거나 갱신하지 않는다.
- `OuterProxy`는 직렬화된 component 구조와 analytic bounds를 위해 유지하지만 Main/Depth/CustomDepth
  pass에 참가하지 않는다.
- `HiddenPrimitives`나 primitive-ID suppression은 production 소유권 계약에 사용하지 않는다.
- 새 경로가 꺼지거나 실패하면 이전 material로 돌아가지 않는다. 입력 SceneColor를 그대로 반환하고
  해당 View의 포털은 보이지 않는 것이 의도된 fail-closed 결과다.

## 2. 책임 분리

### 2.1 `AWormholePortalActor`

Actor에 남은 책임은 endpoint authoring과 gameplay 경계다.

- portal transform과 `PortalRadius`, throat, transition metric
- reciprocal linked portal 참조
- collision, trigger, trace와 transit 진입점
- streaming authoring 값과 `UWorldPartitionStreamingSourceComponent`
- Editor용 persistent debug proxy 설정
- SceneViewExtension의 analytic 후보 bounds를 제공하는 non-raster `OuterProxy`

Actor의 `PrimaryActorTick`은 꺼져 있다. Actor에는 다음 실행 책임이 없다.

- cube capture component/RT 생성, `CaptureScene()` 제출 또는 cadence
- LUT request, binding, revision 관리
- runtime material/MID 생성과 parameter 갱신
- View별 mask/composite
- streaming refcount 계산

`OuterProxy`는 component를 삭제하면서 Blueprint/map 직렬화 구조를 깨지 않기 위해 유지한다. 항상 등록 가능한
bounds component로 취급하며 `bRenderInMainPass=false`, `bRenderInDepthPass=false`,
`bRenderCustomDepth=false` 정책을 적용한다. material override는 production 입력이 아니다.

### 2.2 `UWPRegistrySubsystem`

Registry는 World의 portal 목록, reciprocal pair topology와 `PairId` lifetime의 단일 권위다.

- portal 등록/해제와 invalid weak pointer 정리
- link/metric/render-resource 변경 이벤트
- canonical pair 추가/제거 이벤트
- 재링크를 기존 pair mutation이 아닌 새 pair lifetime으로 발행

Registry는 capture 정책, renderer handle 또는 pixel 합성을 소유하지 않는다.

### 2.3 `UWPTransitSubsystem`

Transit은 crossing 시작, commit/cancel, twin/physics 상태를 책임지고 에는 sequence와 시간이 포함된
snapshot event만 제공한다. gameplay commit 결과는 렌더 입력 중 하나이며, View별 camera side와 ray destination은
SceneViewExtension/Shader에서 다시 계산한다.

### 2.4 `UWPRuntimeSubsystem`

Runtime Subsystem은 World별 Game Thread 렌더 상태 준비자다.

- Registry pair topology 구독
- pair별 metric/transform/cube/LUT/capture contract 수집
- pair capture scheduler와 Transit 즉시 capture 요청
- immutable-by-convention render packet 작성과 변경 기반 publish
- renderer service/handle/sequence 수명 관리
- renderer feedback에 따른 `Disabled → Warmup → Production` 전환

Render Thread에서 Actor/UObject를 읽거나 RDG pass를 만드는 일은 하지 않는다.

### 2.5 Renderer Service와 SceneViewExtension

Renderer Service는 모듈·스레드 경계다. Game Thread의 texture reference를 stable RHI reference로 변환하고,
stale handle/sequence를 양쪽 스레드에서 거부한다.

SceneViewExtension은 World 상태 소유자가 아니라 View별 렌더 연결자다.

- Game/PIE View, perspective, feature level 조건 검사
- SceneCapture, reflection capture, planar reflection 제외
- 활성 Warmup/Production pair의 resource/metric preflight
- 보이는 endpoint를 far-to-near surface, stable selector, PairId, handle, side 순으로 정렬
- `After Motion Blur` 위치에서 production RDG pass 연결
- pair 또는 pass 실패 시 partial output을 사용하지 않고 원본 SceneColor로 원자적 rollback

## 3. 현재 production 상태 기계

pair의 유효한 상태 이름은 아래 세 가지뿐이다.

| 상태 | 의미 | 화면 계약 |
| --- | --- | --- |
| `Disabled` | Runtime pipeline이 비활성화됨 | pass 없음. 원본 SceneColor 유지, 포털 absent |
| `Warmup` | production이 요청됐지만 입력 준비 또는 같은 epoch ACK를 기다림 | 입력 미준비 시 pass 없음. 준비 후 production과 동일한 output을 표시하며 연속 2 render frame 성공을 기다림 |
| `Production` | 같은 epoch warmup ACK가 commit됨 | 모든 활성 pair를 결정적 순서로 합성 |

전환 요약:

```text
Disabled
   │ Runtime pipeline이 켜지고 pair가 등록됨
   ▼
Warmup
   │ 같은 epoch에서 production output 2개 연속 render frame 성공
   ▼
Production

Warmup 입력 미준비 → 같은 epoch에서 pass 없이 대기
Warmup 실패       → 같은 epoch의 fresh packet에서 재시도
Production 실패   → 새 ownership epoch의 Warmup으로 복구
RuntimeEnabled off → Disabled
```

Warmup은 투명 dry run이 아니다. 실제 production shader와 동일한 output을 검증하고 표시한다.
`SceneViewExtensionEnabled=0`은 Render Thread callback만 끄므로 ownership 상태는 유지하지만 화면에는 pass를
제출하지 않는다.

preflight나 pass 제출이 실패하면 다음 원칙을 적용한다.

1. base SceneColor는 immutable 입력으로 유지한다.
2. 일부 pair/endpoint의 중간 출력은 최종 결과로 commit하지 않는다.
3. View 전체 composite를 버리고 untouched SceneColor를 반환한다.
4. material fallback이 없으므로 그 View의 포털은 absent다.
5. 실패 사유, pair/epoch/sequence와 CPU 시간을 로그에 남긴다.

## 4. Production 활성화 계약

별도의 선택적 배포 설정은 없다. Runtime pipeline이 활성화되면 Registry에 등록된 모든 reciprocal
pair가 각각 독립된 ownership epoch로 Warmup/Production 전환을 요청한다. pair별 입력 준비 여부와 renderer
feedback만 상태 전환을 결정한다.

endpoint의 stable selector는 정렬과 진단용 결정적 identity다. 사용자가 특정 pair의 배포 여부를 고르는
설정이 아니며 Actor tag로 override하지 않는다. 개별 pair의 authored capture/quality 설정은 별도 pair 계약으로
전달하되 production 렌더 소유권 자체를 선택하거나 우회하지 않는다.

전체 출력을 끌 때는 `RuntimeEnabled` 또는 `SceneViewExtensionEnabled` canonical gate를 사용한다. 어느 gate도
legacy material, Actor Tick 또는 Actor capture로 rollback하지 않는다.

## 5. 운영 CVar와 명령

### 5.1 canonical gate

| CVar | 기본값 | 현재 의미 |
| --- | ---: | --- |
| `wp.RuntimeEnabled` | `1` | production render packet pipeline의 canonical switch |
| `wp.SceneViewExtensionEnabled` | `1` | Warmup/Production View 렌더의 canonical master switch |
| `wp.RuntimeSummaryInterval` | `5.0` | Runtime 집계 로그 간격(초) |
| `wp.ViewSummaryInterval` | `5.0` | SceneViewExtension 집계 로그 간격(초) |
| `wp.SimulateViewEnabled` | `1` | PIE Simulate level viewport의 production pass 허용 gate |
| `wp.CaptureSchedulerMode` | `1` | 고정 30 ms Runtime production capture. `0`은 deprecated alias이며 Actor capture로 돌아가지 않음 |
| `wp.CaptureSchedulerFailureRollbackThreshold` | `1` | 연속 pair capture 제출 실패 후 manager resource/authority 복구를 시작하는 최소 횟수 |

### 5.2 정상 production

```text
Log LogWormhole Verbose
wp.RuntimeEnabled 1
wp.SceneViewExtensionEnabled 1
wp.ViewSummaryInterval 1
```

### 5.3 출력 비활성화

전체  출력을 끌 때는 다음 canonical gate를 사용한다.

```text
wp.SceneViewExtensionEnabled 0
wp.RuntimeEnabled 0
```

어느 경우에도 Actor Tick, Actor capture 또는 material/MID 경로가 재활성화되지 않는다.

### 5.4 Production 실패 검증

| CVar | 값 |
| --- | --- |
| `wp.OwnershipForceProductionFailureCallbacks` | development 실패 주입. `0` normal, `-1` persistent, 양수 N은 다음 N회 Production attempt 실패 |

실패 주입은 production texture/resource를 변경하지 않는다. 같은 양수 값을 재사용하려면 먼저 `0`을 설정한다.
테스트 후 반드시 `0`으로 복구한다.

## 6. 현재 shader와 resource 계약

현재 plugin shader는 다음 두 파일이다.

| 파일 | 역할 |
| --- | --- |
| `WPRayCommon.ush` | material 문맥에 의존하지 않는 ray/LUT 공통 수학 |
| `WPComposite.usf` | analytic mask/SceneDepth와 LUT/cube를 적용하는 production composite |

production composite의 주요 입력은 다음과 같다.

- endpoint A/B world center와 orthonormal basis
- portal/throat/transition metric
- endpoint별 baked/transient 3D LUT와 logical Z/revision/layout contract
- local/linked cube texture, extent/format/mip/resource generation
- View origin, projection, `PreViewTranslation`, SceneColor와 SceneDepth
- capture generation. 시작 content-ready gate/진단이며 매 capture마다 packet identity를 바꾸지 않음

Render Thread snapshot은 Actor/UObject 포인터를 보유하지 않는다. World 위치는 packet에서 double로 유지하고,
View별 translated-world 좌표로 바꾼 뒤 shader float parameter로 내린다.

Capture는 pair당 고정 30 ms cadence와 Transit 즉시 요청을 사용한다. 저 FPS, 화면 크기, scene dirty 기반의
가변 주기 최적화는 현재 production 계약에 없다.

## 7. 로그와 성능 계측

주요 현재 prefix:

| Prefix | 확인 목적 |
| --- | --- |
| `[ActorLifetime]` | Actor Tick/capture/LUT/MID 실행 책임이 0인지와 수명 CPU 시간 |
| `[Render][Proxy]`, `[ActorProxy]` | OuterProxy non-raster bounds 정책과 CPU 시간 |
| `[Runtime]` | pair validation, packet publish, subsystem 상태 |
| `[Runtime][ProductionOwnership]` | Disabled/Warmup/Production, epoch와 전환 사유 |
| `[Runtime][ProductionOwnershipPerf]` | pair 수, 상태 전환과 정책 CPU 집계 |
| `[CaptureScheduler]` | 30 ms due, Transit force, atomic pair submit과 CPU submit 시간 |
| `[CaptureManager]` | capture component/RT 할당, contract, generation과 제출 시간 |
| `[LUTEndpointManager]` | LUT request/completion/binding/release와 CPU 시간 |
| `[GameThread][RendererService][ProductionBridge]` | handle/sequence, RT snapshot enqueue와 queue latency |
| `[RenderThread][Production]` | View별 pass 성공/실패와 portal-absent fail-closed |
| `[RenderThread][Production][MultiPair]` | pair preflight, endpoint 순서와 View atomic rollback |
| `[RenderThread][ProductionSummary]` | Warmup ACK, Production pass/failure, CPU/GPU stat 집계 |

문제 재현 로그에는 가능한 한 다음 key를 함께 기록한다.

```text
World / WorldType / NetMode
PairId / PortalA / PortalB / SelectorA / SelectorB
RequestedOwnership / EffectiveOwnership / OwnershipEpoch / Decision
Handle / ServiceId / PacketSequence
CubeResourceGenerationA/B / CaptureGenerationA/B / LUTGenerationA/B
ViewKey / PlayerIndex / StereoViewIndex / skip reason
WarmupAck / ProductionFailure / UntouchedSceneColor / PortalAbsentFailClosed
CpuMs / CpuSubmitMs / QueueLatencyMs / GpuMs
```

시간 의미를 섞지 않는다.

- `CpuMs`: 해당 CPU 함수의 wall time
- `CpuSubmitMs`: render/capture work를 enqueue한 CPU 시간
- `QueueLatencyMs`: Game Thread enqueue부터 Render Thread 적용까지의 지연
- `GpuMs`: 실제 GPU 실행 시간. RDG GPU stat, Insights 또는 GPU capture로 측정

production GPU stat은 `WP.ProductionComposite`다. CPU timer를 GPU 완료 시간으로 해석하면 안 된다.

## 8. 현재 검증 매트릭스

| 범주 | 설정/시나리오 | 통과 기준 |
| --- | --- | --- |
| 기본 배포 | canonical gate on, 정상 reciprocal pair | 모든 등록 pair가 각각 `Disabled → Warmup → Production`, 같은 epoch 연속 2 frame ACK, 화면 출력 정상 |
| 전체 비활성 | RuntimeEnabled 또는 SceneViewExtensionEnabled 0 | 모든 pair Disabled 또는 pass 0, untouched SceneColor, portal absent |
| canonical gate | RuntimeEnabled 또는 SceneViewExtensionEnabled 0 | crash 없이 pipeline/extension 비활성, Actor fallback 없이 portal absent |
| resource warmup | LUT/cube 없음, 한쪽만 준비, capture 0→1 | invalid output 없음, 준비 전 portal absent, 준비 후 fresh packet Warmup |
| production 실패 | failure callback 1회 주입 | partial output 없음, untouched SceneColor/portal absent, 새 epoch Warmup 복구 |
| multi-pair 실패 | 2개 이상 pair 중 한 pair preflight/submit 실패 | View atomic rollback, 일부 pair만 합성된 frame 없음 |
| capture 제외 | SceneCapture/reflection/planar | pass 0, capture feedback/재귀 없음 |
| multi-view | split screen 2인, stereo, spectator | View별 side/좌표/texture slice가 섞이지 않음 |
| camera/mask | flat, transition, throat, 뒤, proxy 내부, near-plane | mask/side 연속, NaN·jitter 없음 |
| occlusion | opaque 물체 앞/교차/뒤 | SceneDepth rejection과 bias가 기대와 일치 |
| LWC | 원점과 먼 좌표의 동일 상대 배치 | 허용 오차 내 동일, world-origin 이동에서 흔들림 없음 |
| lifetime | PIE 20회, Simulate→F8, map travel, module shutdown | stale handle/RHI leak/crash 없음, 종료 후 callback 0 |
| performance | pair 1/2/4/8, 화면 안/밖 | callback/endpoint/capture 수와 CPU/GPU 비용을 분리해 설명 가능 |

검증에서 단순히 FPS만 비교하지 않는다. capture face 제출 수, production endpoint pass 수,
`AverageCallbackCpuMs`, `AverageQueueLatencyMs`, `WP.ProductionComposite` GPU 시간을 함께 저장한다.

## 9. 남은 production 위험

### 9.1 멀티뷰와 stereo

SceneViewExtension callback은 View마다 실행된다. pair packet의 reference-camera 진단을 모든 View의 정답으로
사용하지 않고 현재 `FSceneView`의 origin/projection으로 side, ray, depth UV를 다시 계산해야 한다.
SceneDepth array slice가 모호하면 추측하지 않고 fail-closed한다.

### 9.2 SceneCapture 재귀

cube capture도 같은 World의 View를 만든다. 다음 View는 production 합성보다 먼저 제외한다.

```text
View.bIsSceneCapture
View.bIsReflectionCapture
View.bIsPlanarReflection
```

fullscreen SceneViewExtension은 Actor의 component 숨김만으로 차단할 수 없다.

### 9.3 LWC

absolute world position을 너무 일찍 float matrix로 내리지 않는다. portal center와 View origin을 double에서 빼고
작은 translated position을 shader에 넘긴다. 원점, 수십 km 거리, world origin/rebase에서 같은 상대 결과를
검증한다.

### 9.4 World/module 수명

Editor World, PIE server/client World와 preview World가 동시에 존재할 수 있다. extension과 snapshot은 World별로
관리하고 pair 0, map teardown, module shutdown에서 queued Render Thread 작업과 handle을 안전하게 정리한다.
dedicated server에는 renderer를 만들지 않는다.

## 10. Historical migration appendix

이 절은 현재 운영 지침이 아니라 원인 추적용 변경 이력이다. 전환 과정에서 사용한 copy/mask/ray 진단
패스와 선택적 배포 설정은 production composite에서 분리한 뒤 제거했다.

| Migration 영역 | 현재 결과 |
| --- | --- |
| Registry/Transit 관찰 | Registry `PairId`와 event가 production 입력이 됨 |
| analytic proxy/SceneDepth 검증 | production composite의 analytic mask 계약으로 통합 |
| LUT ray/cube/composite 검증 | 단일 production composite shader로 고정 |
| ownership feedback | 모든 등록 pair에 독립된 Warmup/Production epoch 적용 |
| capture scheduler 이전 | Runtime/CaptureManager 고정 30 ms production capture로 전환 |
| Actor 책임 제거 | Actor Tick 0, World manager/SceneViewExtension 소유 완료 |
| legacy 정리 | production-only, non-raster OuterProxy bounds만 유지 |

과거 문서와 로그에서 보일 수 있는 다음 용어는 현재 기능으로 해석하지 않는다.

- `LegacyOnly`, `NewWarmup`, `NewOnly`
- legacy material fallback 또는 compatibility raster
- `HiddenPrimitives`, primitive-ID suppression/ownership ABI
- `M_WormholePortal`, `M_TempPortalRender`, `WormholeMask.ush`, `WormholeComposite.ush`
## 11. 완료 조건

구조 이전의 코드 완료 조건은 충족됐다.

- Main View 최종  픽셀은 Global Shader/RDG가 만든다.
- 상태는 `Disabled/Warmup/Production`으로 표현한다.
- canonical gate off는 Disabled이며 legacy rollback을 제공하지 않는다.
- Runtime/CaptureManager가 capture cadence와 resource 수명을 소유한다.
- SceneCapture/reflection/planar View를 제외한다.
- Actor Tick, material/MID/LUT/capture 실행 책임이 없다.
- OuterProxy는 non-raster bounds component로만 남는다.
- legacy material/shader/assets와 primitive suppression ABI가 제거됐다.
- 실패는 untouched SceneColor와 portal absent로 닫힌다.

코드 구조 완료와 모든 플랫폼의 visual/performance sign-off는 별개다. split screen, 실제 HMD stereo, LWC,
network client, 반복 PIE/map travel, 대규모 pair와 GPU 비용은 8절 매트릭스로 계속 검증한다.
