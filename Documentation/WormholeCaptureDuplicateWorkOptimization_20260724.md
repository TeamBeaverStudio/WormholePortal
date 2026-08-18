# WormholeActor 단일 쌍 Capture 중복 작업 최적화

- 측정일: 2026-07-24
- 대상 맵: `/Game/Maps/WormHoleRender`
- 범위: Wormhole endpoint 2개로 구성된 단일 pair
- 고정 품질 조건: cube 768×768, `PF_FloatRGBA`, endpoint별 최소 24Hz
- 실행 조건: 1920×1080, D3D12, VSync/동적 해상도 off, Screen Percentage 100

## 1. 결론

이번 단계에서는 Volumetric Cloud나 Shadow의 품질을 끄지 않고, custom Cube AA 뒤에 있던 **동일 cube 전체 copy-back**만 제거했다.

- 엔진 post-process AA는 이 capture 계약에서 실행되지 않는다.
  - Capture source가 `SCS_SceneColorHDRNoAlpha`이므로 post processing이 비활성화되고 view의 AA method는 `AAM_None`이다.
  - 따라서 custom Cube AA는 중복 AA가 아니며 유지해야 한다.
- 기존 경로는 AA 결과 cube 전체를 다시 published cube로 복사했다.
- 새 Direct Publish 경로는 두 physical cube를 ping-pong하고, 외부에 공개되는 `UTextureRenderTargetCube` 객체와 texture-reference proxy는 고정한다.
- 768 cube의 copy-back 논리 트래픽은 endpoint 갱신 1회당 54MiB이다.
- endpoint 2개가 각각 24Hz로 갱신되므로 한 pair에서 이론상 약 **2.531GiB/s**의 read+write 트래픽을 제거한다.
- 같은 최종 빌드의 Legacy와 Direct 이미지 비교는 10/10 기준을 통과했다. 52프레임 중 43프레임이 완전히 같고, 최대 RGB8 채널 차이는 2이며 차이가 2를 넘는 픽셀은 없었다.
- Direct 경로의 D3D12 RHI validation 실행에서도 validation error, fatal, assert, ensure가 없었다.

이 변경은 정확히 동일한 결과를 다른 physical cube에 이미 만들어 놓고 다시 복사하던 작업만 제거한다. Volumetric Cloud, dynamic shadow, ambient occlusion, sky lighting, cube 해상도, endpoint 24Hz cadence는 그대로 유지한다.

## 2. 변경 전후 데이터 흐름

### 2.1 Legacy

```text
Stable published cube P에 SceneCapture
  -> custom Cube AA: P를 읽어 transient filtered cube T 생성
  -> T의 6 faces 전체를 P로 CopyTexture
  -> composite가 P를 sample
```

AA가 끝난 시점에 올바른 결과는 이미 `T`에 있지만, stable published object를 유지하기 위해 6 faces 전체를 `P`로 다시 복사했다.

### 2.2 Direct Publish

```text
Stable published object/reference owner = P
Physical capture/output cubes           = P, Q

Frame N:
  capture P -> AA P→Q -> P의 texture reference를 Q physical texture로 retarget
  next capture role = Q, next AA output role = P

Frame N+1:
  capture Q -> AA Q→P -> P의 texture reference를 P physical texture로 retarget
  next capture role = P, next AA output role = Q
```

중요한 계약:

1. 외부에 공개되는 `Record.RenderTarget` UObject 포인터는 항상 `P`로 고정한다.
2. capture용 physical target과 AA output target만 교대한다.
3. composite shader는 render thread에서 proxy를 미리 raw texture로 풀지 않고 `FRHITextureReference` proxy 자체를 bind한다.
4. D3D12 RHI thread가 `UpdateTextureReference`를 적용한 뒤 draw가 proxy를 통해 최신 AA 결과를 읽는다.
5. 두 physical cube 모두 pass 종료 상태를 `SRVMask`로 선언한다.
6. AA enqueue preflight가 실패하면 raw capture를 진행하지 않는다. 필터되지 않은 capture가 published 결과로 노출되는 실패 경로를 차단한다.

## 3. 제거한 비용과 메모리 trade-off

### 3.1 Copy-back 트래픽

768×768, 6 faces, `PF_FloatRGBA` 8 bytes/pixel:

```text
cube payload
= 768 × 768 × 6 × 8
= 28,311,552 bytes
= 27MiB

legacy copy-back logical traffic
= 27MiB read + 27MiB write
= 54MiB / endpoint update

single pair at 24Hz per endpoint
= 54MiB × 24 × 2
= 2,592MiB/s
= 2.53125GiB/s
```

이는 논리적 texture copy 트래픽이다. 실제 DRAM 트래픽은 GPU 압축, cache, copy engine 구현에 따라 다를 수 있으므로 GPU pass 시간과 함께 판단한다.

### 3.2 Persistent color memory

이 프로젝트의 `UTextureRenderTargetCube`는 6개 cube face뿐 아니라 persistent 2D render surface도 가진다.

| 경로 | endpoint당 persistent cube 수 | pair persistent color memory |
|---|---:|---:|
| Legacy | 1 | 약 63MiB |
| Direct Publish | 2 | 약 126MiB |

Direct Publish는 pair당 persistent color memory를 약 63MiB 늘리는 대신, 매초 반복되는 full-cube copy-back을 없앤다. Legacy의 AA filtered cube는 RDG transient이므로 표의 persistent 합계에는 포함하지 않았다.

## 4. AA 감사

### 4.1 엔진 AA와 custom AA 관계

| 항목 | 현재 상태 | 판단 |
|---|---|---|
| Engine post-process AA | capture contract상 `AAM_None` | 제거할 중복 AA 없음 |
| Custom Cube AA filter | 6면 경계와 cube sample 품질을 위한 실제 AA | 유지 |
| Legacy CopyBack | AA 계산이 아니라 결과 전체 복사 | 제거 대상 |

해상도를 768에서 낮추거나 AA를 끄는 방법은 사용하지 않았다. Cube resolution은 이후 1024 이상 상향 실험도 가능하도록 CVar와 profiler 범위를 유지한다.

### 4.2 GPU stat 분리

다음 GPU stat을 따로 기록한다.

- `WP.CubeAA`: 전체 custom Cube AA
- `WP.CubeAA.Filter`: 실제 filter compute
- `WP.CubeAA.CopyBack`: Legacy validation 경로의 copy-back

Direct Publish에서는 `Filter`만 남고 `CopyBack`은 없어야 한다. Legacy validation CVar는 동일 빌드에서 품질과 성능을 되돌려 비교하기 위해 유지한다.

```text
wp.CubeAADirectPublish 1  # Direct Publish, 기본값
wp.CubeAADirectPublish 0  # Legacy copy-back 검증
```

## 5. 품질 검증

모든 비교는 PNG의 RGB8 값을 대상으로 프레임별 MAE, RMSE, PSNR, 최대 채널 차이, 변경 픽셀 비율과 임계값 초과 비율을 계산했다. 비교 구간은 frame 10–61의 52프레임이다.

### 5.1 최종 Legacy 대 Direct

| 지표 | 결과 |
|---|---:|
| 판정 | PASS 10/10 |
| 완전 동일 프레임 | 43 / 52 |
| MAE mean / max | 0.00000743 / 0.000232 |
| PSNR minimum | 84.47dB |
| 최대 RGB8 채널 차이 | 2 |
| changed fraction max | 0.000676 |
| 채널 차이 `>2`, `>4`, `>10` | 모두 0 |

같은 실행 파일과 같은 scene 조건에서 Legacy와 Direct 결과는 반복 capture 노이즈 범위 안이다.

### 5.2 Direct 반복 실행

Direct run 1과 repeat는 52프레임 중 19프레임이 완전히 같았다. 최대 채널 차이는 2이고 `>2` 픽셀은 없었지만, 기존 CL239 repeat에서 잡은 매우 엄격한 noise upper bound에 대해서는 4개 지표가 실패했다.

- MAE mean / max: 0.0000822 / 0.000950
- PSNR minimum: 78.35dB
- changed fraction max: 0.002620
- worst frame: 33, 35

차이는 ±1 위주로 특정 프레임에 집중되며 구조적 화질 손상 패턴이 아니다. 최종 변경의 직접 판정에는 같은 빌드의 Legacy↔Direct 결과를 사용한다.

### 5.3 과거 CL239 baseline 주의

과거 CL239 baseline과 최종 Legacy 사이에는 최대 채널 차이 178의 큰 차이가 있다. 그러나 최종 Legacy와 Direct가 매우 근접하므로 이 차이는 Direct Publish만의 영향이 아니라 과거 빌드와 현재 공통 경로 또는 실행 환경 차이다. 서로 다른 빌드의 캡처를 Direct 최적화의 품질 판정 기준으로 사용하지 않는다.

## 6. 최종 CPU·GPU A-B-B-A 측정

동일한 최종 빌드에서 순서를 `Direct A1 → Legacy B1 → Legacy B2 → Direct A2`로 배치해 시간 흐름과 warm cache의 편향을 줄였다.

- 각 run: 600 frames
- warm-up 제외: 처음 120 frames
- 분석: steady-state 480 frames/run
- cube: 768×768
- scheduler: endpoint 분산 mode 2
- target refresh: endpoint별 24Hz
- main view: 1920×1080

설정당 960 steady-state 프레임을 합친 값이다.

| 지표 | Direct mean / median / P95 / P99 / max | Legacy mean / median / P95 / P99 / max | Direct − Legacy mean |
|---|---:|---:|---:|
| FrameTime | 21.996 / 23.195 / 27.130 / 28.705 / 37.960ms | 22.217 / 23.647 / 27.462 / 28.605 / 36.829ms | -0.220ms (-0.99%) |
| GameThread | 2.964 / 2.884 / 3.733 / 4.212 / 4.858ms | 3.003 / 2.919 / 3.858 / 4.524 / 6.109ms | -0.038ms |
| RenderThread | 22.001 / 23.195 / 27.160 / 28.644 / 37.919ms | 22.210 / 23.586 / 27.453 / 28.530 / 36.709ms | -0.209ms (-0.94%) |
| RHIThread | 3.782 / 3.819 / 4.891 / 5.591 / 6.265ms | 3.809 / 3.864 / 4.896 / 5.656 / 6.670ms | -0.027ms |
| GPUTime | 20.526 / 21.449 / 25.896 / 26.705 / 32.286ms | 20.669 / 21.606 / 26.137 / 27.332 / 31.097ms | -0.143ms (-0.69%) |

| 설정 | 41.67ms 초과 | 최대 FrameTime | 최대값 환산 fps |
|---|---:|---:|---:|
| Direct | 0 / 960 | 37.960ms | 26.34fps |
| Legacy | 0 / 960 | 36.829ms | 27.15fps |

제거 대상 pass 자체는 더 명확하게 분리된다.

| Cube AA 하위 pass | Direct 전체 프레임 평균 | Legacy 전체 프레임 평균 | Direct 활성 프레임 평균 | Legacy 활성 프레임 평균 |
|---|---:|---:|---:|---:|
| `WPCubeAAFilter` | 0.0977ms | 0.0868ms | 0.1222ms | 0.1085ms |
| `WPCubeAACopyBack` | 없음 | 0.0978ms | 없음 | 0.1222ms |
| Filter + CopyBack | 0.0977ms | 0.1846ms | 0.1222ms | 0.2307ms |

- Direct CSV에는 `WPCubeAACopyBack` column과 실행 sample이 모두 없다.
- custom Cube AA 하위 pass 합계는 전체 프레임 기준 0.0869ms, AA 활성 프레임 기준 0.1086ms 감소했다.
- 하위 pass 비용 감소율은 약 47.1%다.
- VRAM `LocalUsedMB` 차이는 평균 +63.04MiB로 persistent color memory 계산과 일치한다.
- 각 run은 endpoint submission 479회다. Direct run 하나가 제거한 copy-back 논리 트래픽은 `479 × 54MiB = 25.260GiB`다.

전체 GPU 상위 pass:

| 순위 | pass | Direct mean | Legacy mean |
|---:|---|---:|---:|
| 1 | VolumetricCloud | 5.153ms | 5.324ms |
| 2 | TemporalSuperResolution | 2.963ms | 2.913ms |
| 3 | RenderDeferredLighting | 1.886ms | 1.795ms |
| 4 | ShadowDepths | 1.508ms | 1.532ms |
| 5 | ShadowProjection | 0.964ms | 0.991ms |

Volumetric Cloud 계열 합계는 Direct 5.498ms로 전체 GPU의 약 26.8%, ShadowDepths+Projection은 2.472ms로 약 12.0%다. 두 항목은 여전히 다음 최적화의 큰 후보지만, 이번 변경은 그 작업량을 바꾸지 않는다. Direct와 Legacy 사이의 Cloud/Shadow 차이는 외부 부하와 run 변동으로 취급하며 copy-back 제거 효과에 포함하지 않는다.

해석상 주의:

- Frame, RenderThread, GPU 평균은 run index를 맞춘 두 비교쌍에서 모두 Direct가 빨랐다.
- Frame P99는 Direct가 0.100ms 높고 단일 max는 1.132ms 높다. 두 값 모두 41.67ms 아래다.
- 설정당 독립 run은 2회뿐이다. paired t-test는 Frame `p≈0.294`, GPU `p≈0.332`로 전체 프레임 개선량을 통계적으로 확정하기에는 부족하다.
- 다른 사용자 프로세스를 종료하지 않은 환경이므로 외부 CPU/GPU 부하가 완전히 통제되지는 않았다.
- 결론은 “CopyBack pass 제거와 직접 비용 감소는 확정, 전체 frame 개선은 좋은 방향이지만 추가 반복으로 범위를 좁혀야 함”이다.

24fps의 frame budget은 41.67ms다. 평균만 보지 않고 각 run의 P95, P99, max와 41.67ms 초과 프레임 수를 함께 기록한다.

## 7. Volumetric Cloud·Shadow 중복 작업 감사

### 7.1 현재 유지한 이유

한 번의 cube capture는 6개 view direction을 렌더링한다. 한 pair는 endpoint 2개이므로 capture가 동시에 실행되면 최대 12개 scene view가 제출된다.

| pass | 중복처럼 보이는 부분 | 그대로 재사용할 수 없는 이유 | 이번 조치 |
|---|---|---|---|
| Volumetric Cloud main raymarch | 같은 scene/cloud volume을 여러 face가 봄 | view ray, frustum, depth, atmosphere 관계가 face마다 다름 | 유지 |
| Volumetric Cloud shadow / SkyAO auxiliary data | scene 또는 light 기준 데이터가 일부 겹칠 가능성 | Renderer private lifetime, view key, temporal state를 검증하지 않고 공유하면 오염 가능 | 후보로 기록, 미변경 |
| ShadowDepths | 같은 light와 caster가 여러 view에 등장 | visible caster set과 projection/cascade 조건이 view마다 달라질 수 있음 | 유지 |
| ShadowProjection | 같은 shadow map을 참조할 수 있음 | projection은 각 capture view의 screen/depth 공간에 종속 | 유지 |
| SSAO | 유사 geometry를 다시 계산 | view-space depth/normal과 projection에 종속 | 유지 |
| SkyLightDiffuse | scene lighting은 공통 | view visibility와 shading 대상이 다름 | 유지 |
| Engine AA | cube capture마다 겹칠 가능성 | 실제로 이 capture source에서는 실행되지 않음 | 변경 없음 |
| Custom Cube AA CopyBack | 동일 AA 결과 cube를 원본으로 전체 복사 | 결과가 이미 다른 physical cube에 완성돼 있음 | 제거 |

품질 차이 없는 중복 제거라는 조건에서 이번에 확정적으로 제거할 수 있던 것은 custom AA 뒤의 copy-back이다. Cloud/Shadow를 단순 show-flag off하거나 해상도를 낮추는 방식은 품질 계약에 맞지 않아 적용하지 않았다.

### 7.2 다음 안전한 실험

1. `VolumetricCloud`, cloud auxiliary map, `ShadowDepths`, `ShadowProjection`을 각각 별도 GPU stat과 view count로 계측한다.
2. auxiliary resource의 cache key를 scene, light, atmosphere, cloud material, temporal history, view-dependent parameter로 분해한다.
3. key가 완전히 같은 경우에만 single pair 내부에서 공유하는 실험 경로를 만든다.
4. 한 번에 한 pass만 A/B하고 현재 PNG metric과 camera motion 구간을 모두 통과시킨다.
5. 단일 pair에서 검증된 뒤에만 다중 pair 공유 cache로 확장한다.

## 8. CPU 및 실패 진단 로그

원인을 나중에 추적할 수 있도록 다음을 로그에 포함한다.

- allocation/reuse/repair/release CPU ms
- AA preflight, transform, capture submit, endpoint total CPU ms
- resource epoch/generation/capture generation
- physical capture/output/alternate target와 stable published owner
- Direct/Legacy mode, UAV contract, role swap, texture-reference retarget 여부
- endpoint 수, persistent target 수, 예상 resident color memory
- capture count, AA pass count, direct publish count
- endpoint별/누적 copy-back payload와 logical traffic avoided
- enqueue 실패 시 published content 안전성, retry 필요 여부
- shutdown allocation/release balance와 전체 allocation/release CPU ms

프로파일 로그 verbosity를 `Warning`으로 낮춰 실행 간 verbose logging 편향을 줄였고, 별도 validation 실행과 source stat으로 세부 pass를 검증했다.

ABBA Warning 로그에서는 정상 경로의 `Graph executed`, `Endpoint submitted`, shutdown 상세가 의도적으로 필터링된다. 실행 횟수는 CSV에서 다음과 같이 교차 검증했다.

- 네 run 모두 `WPCubeAAFilter > 0`인 프레임: 478
- 첫 활성 프레임: 두 endpoint pass가 같이 실행
- 이후 단일 endpoint 활성 프레임: 477
- 합계: `2 + 477 = 479` graph / endpoint submissions per run
- Direct: retarget 479, CopyBack 0
- Legacy: retarget 0, CopyBack 479
- failure, reject, eligibility regression, shutdown imbalance: 0

## 9. 검증 결과

### 9.1 Build

- Configuration: `Development Editor | Win64`
- 결과: 성공
- 최종 build 시간: 27.63초
- 경고: 0
- 오류: 0

### 9.2 Automation

- filter: `WormholePortal`
- 결과: 13 / 13 passed
- succeeded with warnings / failed / not run: 0 / 0 / 0
- 최종 report: `Saved/Automation/WormholePortal_CubeAA_Final_20260724_0206/index.json`
- 최종 log: `Saved/Logs/WormholePortal_CubeAA_Final_Automation_20260724_0206.log`
- 기존 asset warning: `/WormholePortal/MaterialFunctions/MF_WPTransitClip` 누락
- 위 warning은 기존 콘텐츠 참조 문제이며 이번 Direct Publish 변경과 무관하다.

### 9.3 D3D12 RHI validation

- capture frames: 62
- AA graph executions: 48
- endpoint submissions: 48
- RHI validation error / fatal / assert / ensure: 0
- AA failure / proxy invalid / production failure: 0
- allocation/release balance: 0

## 10. 산출물

### 10.1 품질

- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/stable_ref_proxy_direct_run1`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/stable_ref_proxy_legacy_run1`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/stable_ref_proxy_direct_repeat`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/stable_ref_proxy_rhivalidation`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/comparisons/legacy_vs_direct/quality_summary.md`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/comparisons/direct_repeat/quality_summary.md`
- `Saved/Profiling/WormholeSinglePair/quality_ab_20260724/comparisons/baseline_vs_legacy/quality_summary.md`

### 10.2 성능

- session: `duplicate_aa_abba_retry_20260724_cube768_sched2_24hz_aadirect1`
- session: `duplicate_aa_abba_retry_20260724_cube768_sched2_24hz_aadirect0`
- trace session: `duplicate_aa_trace_20260724_cube768_sched2_24hz_aadirect1`
- trace: `Saved/Profiling/WormholeSinglePair/duplicate_aa_trace_20260724_cube768_sched2_24hz_aadirect1/WP_1920x1080_TraceFull_r1_duplicate_aa_trace_20260724_cube768_sched2_24hz_aadirect1.utrace` (약 187MB)
- aggregate report: `Saved/Profiling/WormholeSinglePair/analysis_duplicate_aa_abba_retry_20260724/ABBA_Performance_Report_KO.md`

### 10.3 도구

- `Tools/Profiling/Run-WormholeSinglePairProfile.ps1`
- `Tools/Profiling/Analyze-WormholeSinglePairProfile.py`
- `Tools/Profiling/Compare-WormholeQualityFrames.py`

## 11. 다중 pair 단계로 넘어가기 전 조건

단일 pair 최적화는 다음 조건을 모두 만족할 때 완료로 본다.

1. 768 resolution과 endpoint별 24Hz를 유지한다.
2. 같은 빌드 Legacy↔Direct 품질 기준을 통과한다.
3. D3D12 RHI validation 오류가 없다.
4. 41.67ms 초과 프레임과 GPU/RenderThread P99가 악화되지 않는다.
5. allocation/release balance가 0이고 enqueue 실패가 없다.
6. Cloud/Shadow는 개별 pass cache key와 품질 증거 없이 공유하지 않는다.

다중 pair 단계에서는 pair 수에 따라 persistent target memory가 선형 증가한다. Direct Publish의 추가 63MiB/pair를 그대로 확대하기 전에 visible pair만 활성화하는 pool, physical cube 재사용, update staggering을 함께 설계해야 한다.
