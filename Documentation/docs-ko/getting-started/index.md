# 시작하기

5~10분 안에 첫 Wormhole Portal 쌍을 만들고 통과해 보세요. 이 과정을
마치면 포털을 통해 목적지 공간을 확인하고, 플레이어 캐릭터를 포털
너머로 이동시킬 수 있습니다.

## 만들 내용

- **포털 렌더링:** 포털 입구 안쪽에 목적지 환경이 표시됩니다.
- **캐릭터 통과:** 플레이어가 포털 경계를 통과해 연결된 포털에서
  나옵니다.

## 시작하기 전에

다음 항목이 필요합니다.

- Unreal Engine 5.8.
- 공식 테스트 환경인 Win64, DX12, SM6의 Perspective Game View.
- 플레이 가능한 Character Blueprint가 포함된 프로젝트.
- Fab에서 Wormhole Portal을 추가하고 **Edit > Plugins**에서 활성화한
  뒤 에디터를 다시 시작한 상태.

!!! note "Character Blueprint 사용"

    이 빠른 시작 가이드는 루트 Capsule Component, Character Movement,
    물리 시뮬레이션이 꺼진 Skeletal Mesh를 갖춘 Character Blueprint를
    사용합니다.

## 1. 포털 트레이스 설정

플러그인이 시작될 때 프로젝트에 필요한 포털 트레이스 채널이 없으면
알림이 표시됩니다.

1. 알림에서 **Add Automatically**를 선택합니다.
2. 플러그인이 `Config/DefaultEngine.ini`에 `WPPortalTrace`를 추가하고
   플러그인 설정을 새 트레이스 채널에 맞춥니다.
3. 포털 인식 트레이스를 사용하기 전에 에디터를 다시 시작합니다.

!!! warning "다시 시작 필요"

    에디터를 다시 시작하기 전에는 트레이스 채널이 안정적으로 작동하지
    않습니다. 알림이 다시 나타나면 자동 설정이 완료되었는지 확인한 뒤
    에디터를 다시 시작하세요.

`WPPortalTrace`는 포털 인식 트레이스에서 사용됩니다. 캐릭터 통과는 별도의
오버랩 처리로 동작하지만, 프로젝트 설정 과정에서 이 채널도 구성해야
합니다.

??? info "WPPortalTrace 수동 설정"

    1. **Edit > Project Settings > Engine > Collision**을 엽니다.
    2. `WPPortalTrace`라는 이름의 **Trace Channel**을 추가합니다.
    3. **Default Response**를 **Ignore**로 설정합니다.
    4. **Project Settings > Plugins > Wormhole Portal**을 엽니다.
    5. **Portal Trace Channel**을 `WPPortalTrace`에 할당된 Game Trace
       Channel로 설정합니다.
    6. 에디터를 다시 시작합니다.

## 2. 포털 쌍 만들기

1. **Place Actors**를 열고 `WormholePortalActor`를 검색합니다.
2. 서로 다른 위치에 네이티브 `WormholePortalActor` 인스턴스 두 개를
   배치합니다.
3. 첫 번째 포털을 선택하고 **Linked Portal**을 두 번째 포털로
   설정합니다.

반대 방향의 연결은 자동으로 생성됩니다. 첫 번째 포털의 Metric 값도
연결된 포털에 복사되므로 두 번째 포털을 별도로 설정할 필요가 없습니다.

!!! warning "액터 스케일을 1로 유지"

    두 포털의 Transform Scale을 `(1, 1, 1)`로 유지하세요. 액터의
    스케일을 조정하는 대신 Metric 속성으로 포털의 물리적 크기를
    변경하세요.

기본 Metric은 다음과 같습니다.

| 속성 | 기본값 | 역할 |
| --- | ---: | --- |
| **Portal Radius** | `50 cm` | 물리 통과 판정에 사용하는 중앙 Seam의 반지름 |
| **Throat Half Length** | `100 cm` | 연결된 목 길이의 절반 |
| **Transition Length** | `200 cm` | 목으로 들어가고 나오는 전이 구간의 블렌드 거리 |

## 3. 비주얼 데이터 베이크

항상 동일한 빠른 시작 결과를 얻을 수 있도록 테스트 전에 LUT 데이터를
베이크합니다.

1. 메인 **Tools** 메뉴를 엽니다.
2. **Wormhole Portal** 아래에서 **Bake All LUTs**를 선택합니다.
3. **LUT Bake Settings**에서 **Quality**를 **Balanced**로
   설정합니다.
4. **Domain**을 **Current Level Auto**로 설정합니다.
5. **Bake**를 선택합니다.
6. `LUT ready:`로 시작하는 성공 알림이 표시될 때까지 기다립니다.

!!! note

    Wormhole Portal에는 런타임 대체 경로가 있지만, 이 빠른 시작
    가이드에서는 모든 테스트가 동일한 비주얼 상태에서 시작하도록
    데이터를 먼저 베이크합니다.

## 4. 캐릭터 통과 활성화

1. 플레이 가능한 Character Blueprint를 엽니다.
2. Components 패널에서 **Add**를 선택하고 `WPTransitComponent`를
   추가합니다.
3. 새로 추가된 컴포넌트를 선택합니다.
4. **Transit Enabled**를 선택된 상태로 유지합니다.
5. **Transit Type**을 **Auto**로 유지합니다.
6. Blueprint를 **Compile**하고 **Save**합니다.

!!! tip "여러 액터 설정하기"

    여러 액터의 통과 설정을 검사하거나 한꺼번에 적용해야 한다면
    **Tools**를 열고 **Wormhole Portal** 아래에서 **Transit Manager**를
    선택하세요. **Ready** 상태는 액터 설정이 호환된다는 의미이며,
    런타임 이동 테스트를 대신하지는 않습니다.

## 5. 포털 테스트

Play In Editor를 시작하고 캐릭터로 첫 번째 포털에 접근합니다.

- **☐ 포털 렌더링:** 입구 너머를 바라보고 목적지 공간이 보이는지
  확인합니다.
- **☐ 캐릭터 통과:** 경계를 통과해 캐릭터가 연결된 포털에서 나오는지
  확인합니다.

!!! success "빠른 시작 완료"

    두 항목을 모두 통과하면 포털 쌍을 레벨 디자인과 게임플레이에
    적용할 준비가 완료된 것입니다.

## 문제 해결

| 증상 | 확인할 항목 |
| --- | --- |
| `WPPortalTrace` 경고가 계속 표시됨 | **Add Automatically**를 선택하고 설정 파일 업데이트가 완료되었는지 확인한 뒤 에디터를 다시 시작합니다. |
| 포털이 보이지 않음 | **Linked Portal**, 액터의 단위 스케일, Win64/DX12/SM6 Perspective Game View, 완료된 LUT 베이크를 확인합니다. |
| 플레이어가 포털을 통과하지 못함 | `WPTransitComponent`, **Transit Type: Auto**, **Transit Enabled** 및 필수 Character 컴포넌트를 확인합니다. |
| Runtime Log에 `RuntimeReason=EWPTransitFailReason::DoesNotFitGate`가 표시됨 | Actor의 투영 통과 단면이 Source Gate/Core Radius 안에 들어가지 않습니다. **Portal Radius**를 늘리거나 Actor Collision Bounds를 수정·축소하고, Portal Actor는 스케일링하지 마세요. |
| Transit Manager에 `Ready`가 표시되지 않음 | Transit Manager에서 액터를 선택하고 보고된 설정 문제를 해결한 뒤 다시 테스트합니다. |
| 패키징된 빌드에서 베이크 데이터를 찾지 못함 | 프로젝트의 Cook 디렉터리에 `/Game/WormholePortal/Generated/LUT`가 포함되어 있는지 확인합니다. |

??? info "추가 진단 정보 수집"

    - **Output Log**를 `LogWormhole`로 필터링합니다.
    - 콘솔에서 `stat WormholePortal`을 실행해 런타임 통계를 표시합니다.
    - **Transit Manager**를 열고 액터가 **Ready** 상태인지 확인합니다.

## 다음 단계

다음 내용을 살펴보세요.

- 포털 Metric과 렌더링.
- Transit Manager 작업 흐름.
- 머티리얼 클리핑과 복셀 충돌.
- 포털 인식 트레이스, 오디오, 조명 및 World Partition.
- 포함된 전체 예제는 [데모](../demo/index.md)를 참고하세요.
- 기능 가이드는 [기능](../features/index.md)을 참고하세요.
- 설정과 인터페이스는 [레퍼런스](../reference.md)를 참고하세요.
- 알려진 문제와 해결 방법은 [문제 해결](../issues/index.md)을 참고하세요.

!!! warning "생성된 LUT 데이터 Cook"

    플러그인은 설정된 **Generated LUT Asset Path**를
    **Project Settings > Packaging > Additional Asset Directories to Cook**와
    동기화합니다. 패키징 전에 기본 경로
    `/Game/WormholePortal/Generated/LUT` 또는 직접 지정한 `/Game/...` 경로가
    포함되어 있는지 확인하세요. 기본 설정 항목은 다음과 같습니다.

    ```ini
    [/Script/UnrealEd.ProjectPackagingSettings]
    +DirectoriesToAlwaysCook=(Path="/Game/WormholePortal/Generated/LUT")
    ```
