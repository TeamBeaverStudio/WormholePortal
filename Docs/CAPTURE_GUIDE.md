# Wormhole Portal 문서 촬영 가이드

문서는 이미지가 없어도 완결되게 작성되어 있습니다. 아래 자료를 전달받으면
각 페이지의 `CAPTURE SLOT`에 삽입합니다. 영문과 한국어 페이지는 같은
자료를 재사용합니다.

## 공통 촬영 규칙

- **스크린샷:** `1920×1080` 권장, PNG 또는 무손실 WebP
- **영상:** `1920×1080`, H.264 MP4, 30fps 또는 60fps, 가능하면
  `8–15초`
- **Audio 영상:** H.264 MP4 + AAC, 48kHz Stereo
- Unreal Editor UI 배율은 `100%`를 권장합니다.
- 개인 폴더 경로, 로그인 이름, 프로젝트 기밀 이름은 화면에서 제외합니다.
- UI를 설명하는 자료는 Unreal의 영문 UI로 촬영합니다.
- Gameplay 영상은 콘솔, Stat, Selection Outline, 에디터 패널을 숨깁니다.
- 같은 장면의 Before/After를 찍을 때 카메라 위치와 노출을 고정합니다.
- 원본 파일명 앞에 아래 표의 Slot ID를 붙여 주세요.

최종 반영 경로는 `docs/assets/captures/`이며, 파일명은 소문자
kebab-case로 정리할 예정입니다. 한국어 페이지에서는 같은 파일을
`/WormholePortal_Docs/assets/captures/...` 경로로 참조해 두 언어가 하나의
원본을 공유합니다.

## 먼저 필요한 최소 세트

아래 9개를 우선 촬영하면 Getting Started와 주요 기능 페이지를 충분히
설명할 수 있습니다.

| 우선순위 | Slot | 형식 | 촬영 내용 | 권장 파일명 |
| --- | --- | --- | --- | --- |
| P1 | `GS-01`, `GS-07`, `F-01` | 영상 | 목적지 환경이 다른 포털 쌍. 사선에서 입구와 강한 렌즈 효과를 보여 준 뒤 Character가 끊김 없이 통과. UI 없이 촬영 | `f-01-wormhole-overview.mp4` |
| P1 | `GS-03`, `R-01` | 스크린샷 | 선택한 Portal Actor의 Transform Scale `(1,1,1)`, Linked Portal, Portal Radius, Throat Half Length, Transition Length, Active LUT Domain. Viewport에는 연결선과 디버그 경계 포함 | `r-01-portal-actor-details.webp` |
| P1 | `GS-04` | 스크린샷 | **LUT Bake Settings**에서 Quality `Balanced`, Domain `Current Level Auto`, Bake 버튼이 보이는 화면 | `gs-04-lut-bake-settings.webp` |
| P1 | `GS-06`, `R-02` | 스크린샷 | Character Blueprint의 `WPTransitComponent`. Transit Enabled, Transit Type `Auto`, Advanced와 Voxel 그룹이 읽히도록 촬영 | `r-02-transit-component.webp` |
| P1 | `F-02` | 영상 | 같은 포털 쌍을 Character와 Physics Static Mesh가 차례로 통과. 물체가 절반 걸친 장면과 이동 보존이 보이도록 촬영 | `f-02-actor-transit.mp4` |
| P1 | `F-03`, `R-03` | 스크린샷 또는 영상 | `Portal Line Trace Detailed By Channel`과 `Break WPPortalTraceResult` Blueprint 그래프, 포털을 지나 목적지 Target에 닿는 Debug Line | `r-03-portal-trace.webp` |
| P1 | `F-06`, `I-02` | 스크린샷 | Transit Manager 한 화면에 `Ready`, `Needs Setup`, `Not Supported` Actor를 각각 하나 이상 표시. 상단 작업 버튼도 포함 | `f-06-transit-manager.webp` |
| P2 | `F-04` | Audio 영상 | Spatialized Loop Sound가 출구 포털 방향에서 들리는 상태와, 한쪽 경로에 가림막을 넣어 볼륨·Low-pass가 변하는 상태 | `f-04-portal-audio.mp4` |
| P2 | `F-05` | 영상 | 움직이는 Point 또는 Spot Light가 출구 반대편 Geometry를 비추고 그림자가 반응하는 장면 | `f-05-portal-light.mp4` |

## 추가 설정 및 문제 해결 자료

| 우선순위 | Slot | 형식 | 촬영 내용 | 권장 파일명 |
| --- | --- | --- | --- | --- |
| P2 | `GS-02`, `I-01` | 스크린샷 | 최초 실행의 `WPPortalTrace` 알림. **Add Automatically**와 **Open Collision Settings**가 모두 보이게 촬영 | `i-01-trace-channel-notification.webp` |
| P2 | `GS-05` | 스크린샷 | LUT Bake 완료 후 `LUT ready:`로 시작하는 성공 알림 | `gs-05-lut-ready.webp` |
| P2 | `R-04` | 스크린샷 | **Project Settings > Plugins > Wormhole Portal**. 왼쪽 위치와 오른쪽의 주요 Setting Group이 함께 보이는 화면 | `r-04-project-settings.webp` |
| P2 | `I-03` | 스크린샷 | Output Log를 `LogWormhole`로 필터링하고 `[Transit][Rejected]` 또는 Renderer의 완전한 `Reason=` 행이 보이는 화면 | `i-03-log-rejection.webp` |

## Slot별 전체 목록

### Getting Started

| Slot | 용도 |
| --- | --- |
| `GS-01` | 완성된 포털과 최종 통과 영상 |
| `GS-02` | `WPPortalTrace` 자동 설정 알림 |
| `GS-03` | 연결된 포털, 단위 Scale, 기본 Metric |
| `GS-04` | Balanced + Current Level Auto LUT Bake 설정 |
| `GS-05` | `LUT ready:` 성공 알림 |
| `GS-06` | Character의 `WPTransitComponent` 설정 |
| `GS-07` | PIE에서 보이는 목적지와 성공적인 Character Exit |

### Features

| Slot | 용도 |
| --- | --- |
| `F-01` | 입구, 목, 렌즈 효과, 목적지 화면 |
| `F-02` | Character 및 Physics Actor Transit |
| `F-03` | 포털을 이어 가는 Line Trace |
| `F-04` | Portal Audio와 두 구간 Occlusion |
| `F-05` | Point/Spot Light 전달 |
| `F-06` | Transit Manager |

### Reference

| Slot | 용도 |
| --- | --- |
| `R-01` | Portal Actor Details |
| `R-02` | Transit Component Details |
| `R-03` | Detailed Portal Trace Blueprint |
| `R-04` | Wormhole Portal Project Settings |

### Issues

| Slot | 용도 |
| --- | --- |
| `I-01` | Trace Channel 설정 알림 |
| `I-02` | Transit Manager 상태 예시 |
| `I-03` | `LogWormhole` 거부 로그 |

FAQ에는 별도 이미지가 필요하지 않습니다.
