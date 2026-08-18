# 호환성, 릴리스 및 지원

이 페이지는 Wormhole Portal 1.0의 공식 지원 범위와 문의 절차를
정리합니다. 게시자와 공식 지원 주체는 **Team Beaver Studio**입니다.

## 호환성 표

| 항목 | 버전 1.0 상태 |
| --- | --- |
| 플러그인 버전 | `1.0` |
| Unreal Engine | **5.8 지원 및 테스트 완료** |
| 운영체제/Target | **Win64 지원 및 테스트 완료** |
| RHI / Shader Model | **DX12 + SM6 지원 및 테스트 완료** |
| Render View | 하나의 Primary Perspective Non-stereo Game View |
| Movie Render Queue | Main Deferred Mono Beauty 출력 |
| Dedicated Server | 현재 공식 지원 대상으로 선언하지 않음. Renderer 모듈은 Server Target에서 제외됨 |
| Linux, macOS, DX11 | 현재 공식 지원 대상으로 선언하지 않음 |
| 모바일, 콘솔 | 현재 공식 지원 대상으로 선언하지 않음 |
| VR/Stereo, Split Screen, Multi-view | 포털 합성 미지원 |

표에 없는 환경에서 동작할 가능성은 공식 지원을 의미하지 않습니다. 목표
플랫폼과 엔진 버전에서 직접 검증한 뒤 프로젝트에 채택하세요. 자세한 View
제약은 [문제 해결](../issues/index.md#known-limitations)을 참고하세요.

## 필요한 플러그인 의존성

`WormholePortal.uplugin`은 다음 Unreal 플러그인을 활성화합니다.

- `EnhancedInput`
- `StateTree`
- `GameplayStateTree`

Fab에서 설치한 뒤 **Edit > Plugins**에서 Wormhole Portal이 활성화되어
있는지 확인하고 에디터를 다시 시작하세요. C++ 프로젝트는 엔진 버전에
맞는 바이너리를 사용하거나 해당 엔진에서 플러그인을 빌드해야 합니다.
공개 타입을 사용하는 C++ 모듈은 `WormholePortalRuntime`에 의존해야 하며,
[연동 예제](../reference.md#integration-examples)에서 Build.cs와 호출 예를 볼
수 있습니다.

## 버전 및 릴리스 정보

### 1.0 — 최초 Fab 릴리스

- 물리학에서 영감을 받은 입체 웜홀 렌더링과 LUT Bake/Fallback
- Character, Projectile, Pawn 및 단일 Body Physics Transit
- 선택적 Material Clip 및 Voxel Collision
- 포털 인식 Blueprint/C++ Line Trace
- World Partition 목적지 Streaming
- Spatial Audio 및 Point/Spot Light 전달
- Transit Manager, LUT Baker, 통계와 로그 진단
- 공개 데모 맵과 `WormholePortalSample` 예제 모듈

## 업데이트 체크리스트

플러그인 버전을 올리기 전에 프로젝트를 Source Control에 저장하거나
백업하고 다음 순서로 확인하세요.

1. 대상 Unreal Engine 버전과 이 페이지의 지원 범위를 확인합니다.
2. Editor를 종료한 뒤 플러그인을 업데이트합니다.
3. 프로젝트를 열고 `WPPortalTrace` 경고와 Project Settings를 확인합니다.
4. Deprecated 경고가 있다면 `Set Metric Parameters`를
   `Initialize Physical Metric`으로 교체합니다.
5. LUT를 다시 Bake하고 `/Game/WormholePortal/Generated/LUT` Cook 설정을
   확인합니다.
6. Voxel Collision을 사용한다면 관련 Actor의 Bake 상태를 확인합니다.
7. [공개 데모](../demo/index.md)와 프로젝트의 핵심 PIE/Standalone/Packaged 흐름을
   다시 테스트합니다.

Release Note에 별도 Migration 지시가 있다면 그 지시가 우선합니다.

## 문의 전에 준비할 정보

재현 가능한 보고서는 해결 시간을 크게 줄입니다. 다음 정보를 함께
보내세요.

- Wormhole Portal 버전과 Unreal Engine 전체 버전
- Windows 버전, GPU/Driver, Win64/DX12/SM6 여부 및 PIE, SIE,
  Standalone, Packaged, MRQ 중 발생 환경
- 최소 재현 절차와 기대 결과/실제 결과
- 포함된 공개 데모에서도 재현되는지 여부
- `LogWormhole`로 필터링한 Output Log의 `RuntimeReason`, `ResolveReason`,
  `FailedComponents`
- Portal Actor, Transit Component, Project Settings의 관련 값
- 렌더링 문제라면 `stat WormholePortal` 결과와 화면 캡처 또는 짧은 영상
- 재현에 필요한 최소 프로젝트나 Asset을 공유할 수 있는지 여부

먼저 [문제 해결](../issues/index.md)과 [FAQ](../faq/index.md)에서 알려진 제한 및 설정
문제를 확인하세요.

!!! warning "민감한 정보는 보내지 마세요"

    API Key, 계정 비밀번호, 개인 정보, 배포 인증서, 비공개 저장소 Token
    또는 제3자 라이선스 Asset을 지원 메일에 포함하지 마세요. 로그와 재현
    프로젝트를 공유하기 전에 민감한 경로와 데이터를 제거하세요.

## 연락처와 응답 범위

공식 지원 채널은 이메일
[beavergametech@gmail.com](mailto:beavergametech@gmail.com)입니다.
문의 제목에 `[Wormhole Portal 1.0]`과 문제 요약을 포함하세요.

응답 시간이나 수정 일정을 보장하는 SLA는 제공하지 않습니다. 재현 가능성,
영향도, 지원 환경 여부에 따라 확인 순서가 달라질 수 있습니다. 새 기능
요청은 검토할 수 있지만 반영을 보장하지 않습니다.

## 지원 범위

지원 대상에는 다음이 포함됩니다.

- 공식 지원 환경에서의 설치, 활성화 및 플러그인 설정 문제
- 문서화된 Runtime/Editor API의 재현 가능한 결함
- 공개 데모와 문서의 오류 또는 누락
- 플러그인 자체의 Packaging 및 Cook 문제

다음 항목은 개별 프로젝트의 구현 또는 별도 검증이 필요합니다.

- 공식 지원 대상으로 선언하지 않은 Engine, 운영체제, RHI 또는 플랫폼
- 프로젝트 전용 게임플레이, 네트워크 아키텍처와 타사 플러그인 충돌
- 샘플 코드를 기반으로 한 완성 기능의 대행 제작
- Engine Source 수정 또는 프로젝트 전체의 성능 최적화

지원 문의 시 가능한 최소 재현 프로젝트를 제공하면 플러그인 문제와
프로젝트 통합 문제를 구분하는 데 도움이 됩니다.
