# ByteSieve

Windows용 디스크 사용량 분석 도구입니다. 지정한 폴더 아래의 파일 수·폴더 수·총 용량과 **실제 디스크 사용량**을
집계하고(논리 크기와의 차이를 클러스터 낭비·압축·스파스·클라우드 전용으로 나눠 분석), CLI(텍스트·JSON Lines)와
Win32 GUI(진행률·폴더 트리·용량·디스크 사용·변화 순위)로 결과를 보여 줍니다. 같은 폴더를 다시 스캔하면
**직전 스캔 대비 폴더별 용량 변화**(늘어남·줄어듦·새 폴더·삭제)를 보여 줍니다. 폴더 열거는 여러 스레드로 미리 읽어
대용량 폴더도 빠르게 스캔합니다(`C:\Windows` 기준 약 5배).
GUI의 드라이브 탭에서는 시스템의 모든 논리 드라이브 용량과 사용률을 한눈에 볼 수 있고,
정리 후보 탭에서는 `node_modules`·빌드 결과물·개발 캐시·임시·로그 파일을 찾아 **개발 환경(Node.js·Python·Rust·
Java·.NET·C/C++·Go)별로 분류**하고 예상 확보 용량을 보여 주며,
고른 항목을 확인을 거쳐 **Windows 휴지통으로** 옮깁니다(영구 삭제 없음, 시스템·중요 폴더 보호).
시스템 탭에서는 RAM·CPU 사용률을 1초마다 측정해 막대와 최근 60초 그래프로 보여 줍니다.
C11과 Win32 API만 사용하며 외부 라이브러리 의존성은 없습니다.

## 진행 현황

| 단계 | 내용 | 상태 |
|---|---|---|
| 1 | CLI 스캐너: 지정 디렉터리 탐색, 파일 수·폴더 수·총 용량 집계 | 완료 |
| 2 | 정렬 및 캐시 분류: 상위 용량 폴더와 개발자 캐시 확인 | 완료(개발 환경별 분류 포함) |
| 3 | 오류 및 취소 처리: 권한 거부, 긴 경로, 재분석 지점, 중단 처리 | 완료 |
| 4 | JSON Lines 출력: 시작·진행·결과·완료 이벤트 | 완료 |
| 5 | GUI 연동: 스캔 진행률, 폴더 트리, 용량 순위 화면 | 완료 |
| 6 | 고급 분석: 실제 디스크 점유량, 폴더별 용량 변화 추적, 대용량 스캔 성능·메모리 측정과 개선 | 완료(BitLocker 정보는 예정, RAM·CPU 모니터링은 대시보드 Phase 4) |

| 대시보드 | 내용 | 상태 |
|---|---|---|
| Phase 1 | 드라이브 사용량: 논리 드라이브 목록, 전체·사용·여유 용량, 사용률 막대, 새로고침 | 완료 |
| Phase 2 | 정리 후보 탐지: 개발 캐시·빌드 결과물·임시·로그, 예상 확보 용량, 유형 필터, 취소 (읽기 전용) | 완료 |
| Phase 3 | 안전한 정리: 항목 선택, 확인 창, 휴지통 이동(영구 삭제 없음), 시스템·중요 경로 보호, 성공·실패 표시 | 완료 |
| Phase 4 | 시스템 모니터링: RAM 전체·사용·사용 가능 용량과 사용률, CPU 사용률, 사용률 막대·60초 그래프, 1초 갱신 | 완료 |

## 구현된 기능

### 스캐너 (`scanner`)
- 동적 스택 기반 DFS로 하위 폴더 전체를 탐색합니다(재귀 호출 없음). `--max-depth`로 깊이를 제한할 수 있습니다.
- 폴더 핸들로 `GetFileInformationByHandleEx(FileFullDirectoryInfo)`를 호출해 이름·크기·속성·**할당 크기**를 64 KiB 버퍼에
  한 번에 받습니다. 이 정보 수준을 지원하지 않는 파일 시스템에서는 `FindFirstFileExW`(기본 정보·대용량 읽기)로
  대신하며, 그때는 할당 크기를 모르는 파일로 셉니다.
- **병렬 미리 열거**: 작업 스레드(기본 논리 프로세서 수, 최대 8개)가 아직 처리하지 않은 하위 폴더를 미리 열거해 둡니다.
  폴더를 열거한 쪽이 곧바로 그 하위 폴더 작업을 만들어 큐에 넣고, 작업 스레드는 호출 스레드가 **가장 먼저 필요로 할
  폴더부터**(깊이 우선 처리 순서 키로 정렬한 힙) 읽습니다. 훅(`on_entry` 등)은 언제나 호출 스레드에서 병렬이 아닐 때와
  **같은 순서**로 불리므로 트리·탐지 결과·오류 순서가 스레드 수와 상관없이 같습니다. 훅이 제외한 폴더(`should_skip_dir`)를
  미리 읽어 두었으면 결과를 버리고, 취소하면 작업 스레드를 멈춘 뒤 남은 작업을 해제합니다.
  미리 읽어 둔 결과는 32 MiB(또는 65,536개 폴더)를 넘지 않게 제한하며, 상한에 닿으면 호출 스레드가 직접 읽습니다.
  `scan_set_threads(1)`(CLI `--threads 1`)이면 병렬 없이 호출 스레드만 열거합니다.
- `\\?\` 확장 경로를 써서 260자를 넘는 긴 경로도 읽습니다(최대 32,767자).
- 정션·심볼릭 링크 같은 재분석 지점은 개수만 세고 하위로 들어가지 않습니다(순환 방지).
- 오류가 나도 스캔을 멈추지 않고 경로와 Windows 오류 코드를 기록합니다. 오류는 권한 거부, 경로 없음,
  잘못된 경로, 경로 너무 김, 메모리 부족, 기타로 분류합니다.
- 결과 상태는 세 가지입니다.
  - `complete`: 모든 폴더를 읽음
  - `partial`: 일부 폴더를 읽지 못했거나 취소됨
  - `fatal`: 대상 폴더 자체를 열지 못함
- 취소 요청은 폴더를 열기 직전과, 폴더 하나를 읽는 도중 256개 항목마다 확인합니다.
- 선택 훅 `should_skip_dir`로 특정 하위 폴더에 들어가지 않게 할 수 있습니다. 제외한 폴더는 오류가 아니라 `excluded_dir_count`로 셉니다(정리 후보 탐지가 시스템 폴더를 건너뛸 때 사용).

### 디스크 사용량 분석 (논리 크기 대 할당 크기)
파일의 논리 크기(`EndOfFile`)와 디스크에 실제로 할당된 크기(`AllocationSize`)를 함께 집계합니다. 할당 크기는 폴더
열거 결과에 들어 있으므로 **파일을 열지 않습니다.** 압축·스파스 파일만 열거 결과가 실제 점유량과 다를 수 있어
`GetCompressedFileSizeW`로 다시 구합니다(클라우드 자리표시자는 내려받기를 일으키지 않도록 건드리지 않음).

| 원인 | 판정 | 의미 |
|---|---|---|
| 클러스터 낭비(slack) | 할당 > 논리 | 클러스터 단위 반올림으로 남는 공간 |
| 압축 | 할당 < 논리, `COMPRESSED` | NTFS 압축으로 절약한 공간 |
| 스파스 | 할당 < 논리, `SPARSE_FILE` | 비어 있는 구간을 할당하지 않은 파일 |
| 클라우드 전용 | 할당 < 논리, `OFFLINE`·`RECALL_ON_DATA_ACCESS`·`RECALL_ON_OPEN` | OneDrive 등 내용이 로컬에 없는 파일 |
| 기타 | 할당 < 논리, 그 밖 | MFT에 들어간 아주 작은 파일, 중복 제거 등 |

- 원인이 겹치면 클라우드 전용 > 압축 > 스파스 > 기타 순으로 하나에만 넣습니다. 따라서
  `디스크 사용 = 논리 크기 + 클러스터 낭비 - (압축 + 스파스 + 클라우드 + 기타)`가 항상 맞습니다.
- 대상 볼륨의 클러스터 크기를 함께 보여 줍니다(`GetDiskFreeSpaceW`).
- 한계: 하드 링크는 열거만으로 알 수 없어 링크마다 중복 집계됩니다(예: `WinSxS`). 폴더 자체가 차지하는 색인 공간은
  넣지 않습니다. 쓰는 중인 파일은 NTFS가 폴더 항목의 크기를 늦게 갱신해 잠시 예전 값이 보일 수 있습니다.

### 데이터 모델 (`scan_model`)
- 스캔 결과를 폴더 트리로 만듭니다. 파일은 노드로 만들지 않고 소속 폴더에만 집계합니다.
- 폴더마다 바로 아래 파일 용량과 하위 트리 전체 용량을 계산합니다.
- 자식 폴더를 열거 순서·용량순·이름순으로 정렬할 수 있습니다.
- 폴더마다 디스크 할당 크기(바로 아래·하위 포함)도 계산합니다.
- 용량 순위를 하위 포함 용량, 바로 아래 파일 용량, 하위 포함 디스크 할당 크기 기준으로 구합니다.
- 폴더 이름은 노드마다 따로 할당하지 않고 64 KiB 덩어리(이름 저장소)에 이어 담습니다. 노드 배열은 1.5배씩 늘립니다.
  `C:\Windows`(폴더 16만 개) 기준으로 모델 해제 시간이 10 ms에서 3 ms로 줄었습니다.
- 진행률 콜백을 제공합니다. 진행률은 다 읽은 폴더 수 ÷ (다 읽은 폴더 + 남은 폴더)로 추정합니다.
- 폴더마다 상태 표시가 붙습니다: 읽기 실패, 일부 누락, 취소됨, 링크, 깊이 제한.

### 폴더별 용량 변화 추적 (`scan_history`)
- 스캔이 끝나면 폴더 트리를 **깊이 4까지** 스냅숏(폴더별 하위 포함 용량·할당 크기·파일 수·상태)으로 저장합니다.
  깊이 4 폴더의 용량에는 그 아래 전체가 들어 있으므로 더 깊은 곳의 변화도 조상 폴더에서 보입니다.
- 같은 대상 폴더를 다시 스캔하면 직전 스냅숏과 비교해 **늘어남·줄어듦·새 폴더·삭제** 폴더를 변화량(절댓값)이 큰
  순서로 구합니다. 새 폴더·삭제된 폴더 안의 하위 폴더는 따로 나오지 않고 가장 위 폴더 하나로 보입니다.
  용량이 같은 폴더는 넣지 않습니다. 경로는 대소문자를 구분하지 않고 맞춥니다.
- 어느 한쪽이라도 그 폴더나 하위를 다 읽지 못했으면(접근 거부·취소) 변화에 **불확실(?)** 표시를 붙입니다.
  두 스냅숏의 깊이가 다르면(예: `--max-depth 1`로 스캔) 얕은 쪽 깊이까지만 비교합니다.
- 저장 위치: 환경 변수 `BYTESIEVE_HISTORY_DIR`, 없으면 `%LOCALAPPDATA%\ByteSieve\history`. 대상 폴더마다 파일 하나
  (`<경로 해시>.snap`, UTF-8 텍스트)이며 직전 스냅숏만 남깁니다. 임시 파일에 쓴 뒤 바꿔치기해 중간에 끊겨도 깨지지
  않습니다. 대상 폴더를 열지 못했거나 스캔을 취소했으면 저장하지 않습니다.

### 백그라운드 스캔 (`scan_job`)
- 모델 생성을 별도 스레드에서 실행해 UI가 멈추지 않게 합니다.
- 진행 상황은 잠금으로 보호한 사본으로 넘기고, 화면 갱신 알림은 일정 간격(GUI는 100ms)으로 제한합니다.
- 어느 스레드에서든 취소를 요청할 수 있고, 끝난 뒤 완성된 모델을 넘겨받습니다.

### 드라이브 조회 (`drive`)
- `GetLogicalDrives`로 논리 드라이브(A:~Z:)를 구하고, 드라이브마다 `GetDriveTypeW`·`GetDiskFreeSpaceExW`·`GetVolumeInformationW`를 호출합니다.
- 전체 용량은 호출자에게 보이는 용량(디스크 할당량이 있으면 할당량), 사용 용량은 `전체 - 볼륨 여유`입니다. 사용률은 천분율(0.1% 단위, 내림)로 계산하며 전체가 0이거나 값이 아주 커도 안전합니다.
- 미디어가 없는 이동식·광학 드라이브에서 "디스크를 넣으세요" 시스템 대화상자가 뜨지 않도록 조회하는 스레드에만 `SEM_FAILCRITICALERRORS`를 적용합니다.
- 드라이브 하나가 실패해도 목록 전체는 만들어지고, 실패한 항목은 용량을 0으로 두고 상태와 Windows 오류 코드를 기록합니다.

  | 상태 | 예시 오류 | 화면 표시 |
  |---|---|---|
  | `ok` | – | 사용률 막대 |
  | `not_ready` | `ERROR_NOT_READY`, `ERROR_NO_MEDIA_IN_DRIVE` | 미디어 없음/준비 안 됨 |
  | `access_denied` | `ERROR_ACCESS_DENIED` | 접근 거부 |
  | `missing` | `ERROR_PATH_NOT_FOUND`, `ERROR_DEVICE_NOT_CONNECTED` (조회 도중 분리된 USB) | 분리됨 |
  | `disconnected` | `ERROR_BAD_NETPATH`, `ERROR_NETNAME_DELETED` | 연결 안 됨 |
  | `error` | 그 밖의 오류 | 조회 실패 |
- 조회 함수를 주입할 수 있어(`drive_list_build`) 이동식 드라이브 오류 같은 경우를 실제 장치 없이 테스트합니다.

### 정리 후보 탐지 (`cleanup_rules`, `cleanup_scan`, `cleanup_job`)
**읽기 전용입니다.** 디렉터리 열거로 얻은 이름·크기·속성만 쓰며, 파일을 열거나 내용을 읽지 않고, 삭제·이동·수정하지 않습니다. 정리(휴지통 이동)는 사용자가 고르고 확인한 항목에 대해서만 `cleanup_trash`가 따로 합니다.

탐지 규칙(이름은 대소문자 무시, 정확히 일치):

| 유형 | 폴더 | 파일 |
|---|---|---|
| Node.js | `node_modules` | – |
| 빌드 결과물 | `dist`, `build`, `out`, `cmake-build-*`, `target`\*, `bin`\*, `obj`\* | – |
| 개발 캐시 | `.cache`, `.next`, `.nuxt`, `.vite`, `.parcel-cache`, `.turbo`, `.svelte-kit`, `.angular`, `__pycache__`, `.pytest_cache`, `.mypy_cache`, `.ruff_cache`, `.tox`, `.nox`, `.hypothesis`, `.gradle`\*, `.vs`\* | – |
| 임시 파일 | `.tmp`, `.temp` | `*.tmp` |
| 로그 | – | `*.log` |

\* 이름이 너무 흔하거나 설정이 섞여 있는 폴더는 **같은 폴더에 해당 프로젝트 표시 파일이 있을 때만** 후보로 봅니다.
`target`은 `Cargo.toml`·`pom.xml`, `bin`·`obj`·`.vs`는 `*.csproj`·`*.fsproj`·`*.vbproj`·`*.sln`, `.gradle`은 Gradle·Maven
파일이 필요합니다. 그래서 사용자 폴더의 `bin`이나 홈의 `.gradle`(설정·인증 정보가 들어 있음)은 후보가 되지 않고,
그 안은 일반 폴더처럼 계속 탐지합니다(예: `bin\x.log`는 로그 항목).

**개발 환경 분류**: 항목마다 개발 환경을 붙입니다.

| 개발 환경 | 이름으로 정해지는 폴더 | 표시 파일(같은 폴더) |
|---|---|---|
| Node.js | `node_modules`, `.next`, `.nuxt`, `.vite`, `.parcel-cache`, `.turbo`, `.svelte-kit`, `.angular` | `package.json` |
| Python | `__pycache__`, `.pytest_cache`, `.mypy_cache`, `.ruff_cache`, `.tox`, `.nox`, `.hypothesis` | `pyproject.toml`, `setup.py`, `setup.cfg`, `requirements.txt`, `Pipfile`, `tox.ini` |
| Rust | – | `Cargo.toml` |
| Java·Kotlin | `.gradle` | `pom.xml`, `build.gradle(.kts)`, `settings.gradle(.kts)` |
| .NET·Visual Studio | `bin`, `obj`, `.vs` | `*.csproj`, `*.fsproj`, `*.vbproj`, `*.sln` |
| C/C++ | `cmake-build-*` | `CMakeLists.txt`, `Makefile` |
| Go | – | `go.mod` |
| 일반 | 임시·로그, 표시 파일이 없는 `dist`·`build`·`out`·`.cache` | – |

`dist`·`build`·`out`·`.cache`·`target`처럼 여러 환경에서 쓰는 이름은 같은 폴더의 표시 파일로 정하며, 여러 개가 있으면
이름마다 정한 순서를 따릅니다(`build`: Java > C/C++ > Node.js > Python, `dist`: Node.js > Python > Go > C/C++,
`out`: C/C++ > Java > Node.js > Go, `target`: Rust > Java). 표시 파일은 폴더보다 뒤에 열거될 수 있어, 그 폴더 열거가
끝날 때 한꺼번에 정합니다.

탐지 정책:
- **중첩**: 위에서부터 처음 맞은 폴더가 그 아래 전체를 차지합니다. `node_modules\pkg\dist`나 `build\app.log`는 따로 나오지 않고 바깥 항목 용량에 포함되므로, 항목 용량의 합(예상 확보 용량)에 중복이 없습니다.
- **용량**: 폴더는 하위 전체 파일의 논리 크기 합이며 스캐너와 같은 기준입니다(디스크 사용량은 폴더 스캔 탭·CLI에서 확인).
- **신뢰도**: `node_modules`와 빌드 결과물은 흔한 이름이라, 같은 폴더에 위 표의 프로젝트 표시 파일이 하나라도 있으면 **확인됨**, 없으면 **추정**으로 표시합니다.
- **시스템 보호 경로**: Windows 폴더, `Program Files`, `Program Files (x86)`, `ProgramData`와 볼륨 루트의 `$Recycle.Bin`, `System Volume Information`, `Recovery`, `Config.Msi`는 탐색 중 만나면 들어가지 않습니다. 사용자가 보호 경로 안을 직접 고르면 탐지는 하되 빨간 경고를 띄우고 모든 항목에 **시스템 경로** 표시를 붙입니다. `AppData`는 캐시·임시 파일이 많은 곳이라 제외하지 않습니다.
- **링크**: 정션·심볼릭 링크는 폴더든 파일이든 따라가지 않고 항목으로도 만들지 않습니다(순환 방지, pnpm 링크 중복 계산 방지).
- **선택한 경로 자체**는 탐색 범위일 뿐 항목이 되지 않습니다.
- **오류·취소**: 스캐너 정책 그대로입니다(접근 거부·긴 경로 등은 기록하고 계속). 하위를 다 읽지 못한 항목은 **일부 누락**으로 표시하며, 취소하면 그때까지 찾은 결과를 보여 줍니다.
- 탐지는 별도 스레드(`cleanup_job`)에서 하며 진행률 알림은 100ms 간격으로 제한합니다. 폴더마다 메모리를 잡지 않고 후보 항목만 저장합니다.

### 안전한 정리 (`cleanup_trash`)
탐지 결과에서 **사용자가 체크하고 확인한 항목만** Windows 휴지통으로 옮깁니다. 파일을 영구 삭제하는 코드 경로는 없습니다.

옮기는 방법: 셸의 `IFileOperation`에 `FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE`(확인·오류 창 없음)를 주고 항목마다 따로 실행합니다.
- 휴지통 용량 초과·휴지통 끄기 설정 등으로 셸이 **영구 삭제로 바꾸려 하면**, 진행 싱크의 `PreDeleteItem`에서 `TSF_DELETE_RECYCLE_IF_POSSIBLE`이 없는 것을 보고 거부합니다(`no_recycle`, 항목은 그대로).
- `PostDeleteItem`으로 휴지통 안의 실제 위치를 받고, 원래 경로가 남아 있으면(사용 중인 파일 등으로 일부만 옮겨진 경우 포함) 실패로 봅니다.

삭제 보호(하나라도 걸리면 그 항목은 건드리지 않음):

| 단계 | 거부 조건 | 상태 |
|---|---|---|
| 경로 판정 (탐지 직후·옮기기 직전) | 네트워크(UNC)·상대 경로, 260자 이상(휴지통 미지원) | `deny_path` |
| | 탐지 경로 밖이거나 탐지 경로 자체 | `deny_outside` |
| | Windows·`Program Files`·`ProgramData`, 볼륨 루트의 `$Recycle.Bin`·`System Volume Information` 등과 같거나 그 아래, 보호 경로 안에서 탐지한 항목 | `deny_protected` |
| | 볼륨 루트, `Users`, 사용자 프로필·공용 프로필, 바탕 화면·문서·다운로드·사진·음악·동영상·`AppData`(Roaming/Local/LocalLow), **실행 중인 프로그램 폴더**, 위 보호 경로와 같거나 **그 상위** | `deny_critical` |
| | 하위를 다 읽지 못한 항목(접근 거부·취소로 **일부 누락**) — 내용과 용량을 확신할 수 없음 | `deny_incomplete` |
| 옮기기 직전 확인 | 고정 디스크가 아님(이동식·네트워크·광학 드라이브는 휴지통이 없어 영구 삭제 위험) | `deny_drive` |
| | 이미 없음 | `missing` |
| | 탐지 때와 종류(폴더/파일)가 다름, 항목이 링크(정션·심볼릭 링크), `GetFinalPathNameByHandleW`로 구한 실제 위치가 표시 경로와 다름(중간 폴더가 정션으로 바뀐 경우, `subst` 드라이브 등) | `changed` |
| 옮기는 중 | 휴지통에 넣을 수 없어 영구 삭제될 상황 | `no_recycle` |
| | 사용 중·접근 거부 등 셸 오류(HRESULT 기록) | `failed` |

- 작업은 STA로 초기화한 별도 스레드(`CleanupTrashJob`)에서 하므로 큰 `node_modules`를 옮겨도 창이 멈추지 않습니다. 취소하면 옮기고 있던 항목만 끝까지 옮기고 나머지는 `cancelled`로 남깁니다.
- 보호 판정(`cleanup_guard_check`)은 문자열만 보는 순수 함수라, 실제 시스템 폴더를 건드리지 않고 단위 테스트합니다.

### 시스템 측정 (`sysmon`)
- **RAM**: `GlobalMemoryStatusEx`의 `ullTotalPhys`(전체)·`ullAvailPhys`(사용 가능)를 쓰고, 사용 = 전체 - 사용 가능, 사용률은 천분율(0.1% 단위, 내림)입니다. 전체가 0이거나 사용 가능이 전체보다 크면 `invalid`로 표시합니다.
- **CPU**: `GetSystemTimes`의 누적 idle·kernel·user 시간(kernel에 idle 포함)을 직전 측정값과 비교해 `1 - idle 증가분 ÷ (kernel + user) 증가분`으로 계산합니다(천분율, 반올림). 모든 논리 프로세서를 합친 사용률입니다.
  - 첫 측정은 비교할 값이 없어 `pending`(측정 중)입니다.
  - 카운터가 줄었거나 증가분이 0이면 `invalid`로 표시하고 이번 값을 새 기준으로 다시 계산합니다. idle이 전체보다 크게 늘면 0%로 봅니다.
- API가 실패하면 그 항목만 `error`와 Windows 오류 코드를 기록하고 값은 0으로 둡니다. RAM이 실패해도 CPU는 계속 측정하고, CPU가 실패하면 기준값을 버려 회복 직후 다시 `pending`부터 시작합니다.
- 큰 값(오래 켜 둔 시스템의 누적 시간, 아주 큰 메모리)도 곱셈이 넘치지 않게 계산합니다.
- 측정 함수를 주입할 수 있어(`sysmon_init`) API 실패·이상값을 가짜 함수로 테스트합니다.

### CLI (`disk-scanner`)
- 사람이 읽는 요약을 출력합니다. 오류는 종류별 개수와 함께 최대 20개까지 나열합니다.
- `On disk:` 줄에 디스크 사용량을, 그 아래에 0이 아닌 원인(`Slack`·`Compressed`·`Sparse`·`Cloud-only`·`Other`·`Unknown`)을 보여 줍니다.
- `--track`: 폴더 트리를 만들어 직전 `--track` 스캔과 비교하고(`Changes since ...` 아래에 변화량이 큰 폴더 최대 20개),
  새 스냅숏을 저장합니다. `--history-dir DIR`로 저장 위치를 바꿀 수 있습니다. 기록 저장 실패는 경고만 하고 종료 코드에는
  영향을 주지 않습니다.
- `--threads N`: 폴더 열거 작업 스레드 수(0 = 자동, 1 = 병렬 없음, 최대 8).
- `--jsonl`을 주면 표준 출력에 UTF-8 JSON Lines 이벤트를 출력합니다. 이벤트 순서는 `start` → `progress`(1,000개 항목마다) → `result` → `complete`입니다.
  `result`에는 `alloc_bytes`와 원인별 `disk` 객체가 들어가고, `--track`이면 `complete` 앞에 `changes` 이벤트
  (`previous`, `since`, `old_bytes`, `new_bytes`, `items[]`, `more`, `saved_to`)가 붙습니다(이때는 `progress`를 내지 않음).
- 종료 코드: `0` 완료, `1` 일부 실패, `2` 스캔 불가 또는 잘못된 인자.

### GUI (`bytesieve-gui`)
창은 **폴더 스캔**, **드라이브**, **정리 후보**, **시스템** 네 탭으로 나뉩니다.

**폴더 스캔 탭**
- 경로 입력, 찾아보기, 스캔, 취소 버튼이 있습니다.
- 진행률 표시줄과 상태 줄에 파일·폴더 수, 용량, 오류 수, 현재 폴더를 보여 줍니다.
- 둘째 줄에 **디스크 분석**(`디스크 사용 7.41 MiB(논리 크기 대비 +23.7 KiB) · 클러스터 낭비 +23.7 KiB · 압축 -… (n개) …`)과
  **직전 스캔 대비 변화**(`직전 스캔(10-31 02:20) 대비 +293.0 KiB(7.10 MiB → 7.39 MiB) · 늘어남 3 · 줄어듦 0 · 새 폴더 0 · 삭제 0`)를
  보여 줍니다. 첫 스캔이면 기준으로 저장했다고, 취소했으면 기록하지 않았다고 알립니다.
- **폴더 트리(TreeView)**: 용량이 큰 순서로 정렬하고, 폴더를 펼칠 때 하위 항목을 채웁니다. 각 항목에 용량과 전체 대비 비율, 상태 표시가 붙습니다.
- **오른쪽 목록(ListView)**: 보기 콤보 상자로 **용량 순위**, **디스크 사용 순위**, **변화 순위(직전 스캔 대비)** 중 하나를 고릅니다.
  열은 순위·경로·용량·변화·디스크 사용·파일 수·비율입니다. 순위 보기의 변화 열은 그 폴더의 직전 대비 변화량
  (`+1.20 GiB`, `새 폴더 +40 B`, 불확실하면 `(?)`)이고, 변화 순위 보기는 변화량이 큰 폴더 최대 100개를 삭제된 폴더
  (`삭제됨 -100 B`, 용량 열은 `-`)까지 포함해 보여 줍니다. 항목을 더블클릭하거나 Enter를 누르면 트리에서 그 폴더를 펼쳐
  선택합니다(삭제된 폴더 제외).
- 스캔 중에 창을 닫으면 스캔을 취소하고 스레드가 끝날 때까지 기다린 뒤 종료합니다.

**드라이브 탭 (`gui_drives`)**
- 드라이브마다 문자·볼륨 이름·종류·파일 시스템·전체·사용·여유 용량과 사용률 막대를 보여 줍니다. 사용률이 90% 이상이면 막대가 빨간색입니다.
- 조회에 실패한 드라이브는 막대 대신 상태와 오류 코드(예: `미디어 없음/준비 안 됨 (오류 21)`)를 표시합니다.
- 상단 줄에 드라이브 수, 정상 조회된 드라이브의 용량 합계, 조회 실패 수, 조회 시각을 보여 줍니다.
- 탭을 처음 열 때와 **[새로고침]**(Alt+R)을 누를 때 조회합니다. 조회는 작업 스레드에서 하므로 응답이 느린 네트워크·이동식 드라이브가 있어도 창이 멈추지 않습니다.

**정리 후보 탭 (`gui_cleanup`)**
- 경로를 입력하거나 [찾아보기]로 고른 뒤 [탐지](또는 Enter)를 누릅니다. 탐지 중에는 [취소]로 멈출 수 있습니다.
- 진행률 표시줄과 상태 줄에 폴더·파일 수, 지금까지 찾은 후보 수와 용량, 오류 수, 현재 폴더를 보여 줍니다. 끝나면 오류·건너뛴 폴더·제외한 시스템 폴더·링크 수와 걸린 시간을 보여 줍니다.
- 목록은 **용량 내림차순**으로 용량·유형·경로·파일 수·비고(확인됨/추정, 일부 누락, 시스템 경로)를 보여 줍니다. 수만 개 항목도 가볍게 그리도록 가상 목록을 씁니다.
- **유형** 콤보 상자로 전체·Node.js·빌드 결과물·개발 캐시·임시 파일·로그 중 하나만, **환경** 콤보 상자로 전체·Node.js·Python·
  Rust·Java·Kotlin·.NET·Visual Studio·C/C++·Go·일반 중 하나만 볼 수 있습니다(두 필터는 함께 걸림). 목록에 **개발 환경** 열이
  있습니다. 요약 줄에 `표시 n개 · 용량 / 전체 n개 · 예상 확보 용량`을 보여 줍니다.
- **정리**: 목록의 체크 상자로 항목을 고릅니다(클릭 또는 선택한 행에서 Space). [모두 선택]은 지금 필터에 보이는 항목 중 옮길 수 있는 것만 고르고, [선택 해제]는 보이는 항목을 해제합니다. 다른 필터에서 고른 항목도 선택이 유지됩니다.
  - 선택 줄에 `선택 n개 · 예상 확보 용량`과 지금까지 휴지통으로 옮긴 개수·용량을 보여 줍니다.
  - 보호 대상은 체크 상자가 없고 **정리** 열에 `보호: 시스템 보호 경로`처럼 사유가 나옵니다.
  - **[휴지통으로 이동...]**(Alt+D)을 누르면 선택한 항목의 유형·용량·경로(최대 12개, 나머지는 "외 n개")와 예상 확보 용량, '추정' 항목 수 경고를 담은 확인 창이 뜹니다. 기본 버튼은 **[아니요]**이며 [예]를 눌러야만 시작합니다.
  - 옮기는 동안 진행률·성공·실패 수를 보여 주고 [취소]로 남은 항목을 멈출 수 있습니다. 탐지·찾아보기·선택 변경은 막힙니다.
  - 끝나면 상태 줄에 `성공 n개(확보 용량) · 실패 n개 · 취소 n개`를, **정리** 열에 항목별로 `휴지통으로 이동됨`, `건너뜀: 탐지 후 바뀜(...)`, `실패: <Windows 오류 설명> (0x...)`을 표시합니다. 실패가 있으면 경로와 사유를 나열한 요약 창도 띄웁니다. 실패·취소한 항목은 선택이 남아 다시 시도할 수 있습니다.
- 탐지나 휴지통 이동 중에 창을 닫으면 작업을 취소하고 스레드가 끝날 때까지 기다린 뒤 종료합니다.

**시스템 탭 (`gui_monitor`)**
- **메모리(RAM)** 줄에 `사용 11.4 GiB / 전체 15.3 GiB · 사용 가능 3.99 GiB · 사용률 73.9%`처럼 전체·사용·사용 가능 용량과 사용률을, **CPU** 줄에 사용률과 논리 프로세서 수를 보여 줍니다.
- 각 줄 아래 사용률 막대가 있고, 90% 이상이면 빨간색(오류 상태)으로 바뀝니다.
- **최근 60초 사용률** 그래프에 CPU(파랑)·메모리(보라) 꺾은선을 0·25·50·75·100% 눈금과 함께 그립니다. 가장 최근 값이 오른쪽 끝이며, 메모리 DC에 그린 뒤 한 번에 옮겨 깜박이지 않습니다.
- 측정은 탭을 처음 열 때 시작하고, 이후 다른 탭으로 옮겨도 그래프 기록을 위해 계속합니다.
- **1초 간격 갱신·UI 멈춤 방지**: 전용 측정 스레드가 1초마다 측정합니다(측정에 걸린 시간만큼 기다림을 줄여 간격이 밀리지 않음). 결과는 잠금으로 보호한 최신 값 하나에만 쓰고 알림 메시지는 처리되기 전까지 하나만 보내므로, UI가 잠시 바빠도 메시지가 쌓이거나 메모리가 새지 않습니다. 화면 갱신은 UI 스레드에서만 합니다.
- **측정 실패**: 그 항목에 `측정 실패 (Windows 오류 n)`을 표시하고 막대를 비우며 그래프 선을 끊습니다. 상태 줄에도 `일부 값을 측정하지 못했습니다`가 붙습니다. 다음 측정이 성공하면 자동으로 회복합니다. 측정 스레드를 만들지 못하면 상태 줄에 사유를 표시합니다.
- 창을 닫으면 측정 스레드에 종료를 알리고 끝날 때까지 기다린 뒤 패널을 해제합니다.

## 디렉터리 구조

```
ByteSieve/
├── CMakeLists.txt          빌드·테스트 정의
├── READEME.md
├── src/
│   ├── path.c/.h           경로 결합·정규화, \\?\ 확장 경로 변환
│   ├── scanner.c/.h        디렉터리 탐색(DFS·병렬 미리 열거), 집계·디스크 할당 크기 분석, 오류 분류, 취소 훅
│   ├── scan_model.c/.h     폴더 트리 모델, 진행률, 정렬, 용량 순위
│   ├── scan_job.c/.h       백그라운드 스레드 스캔, 진행률 사본, 취소
│   ├── scan_history.c/.h   폴더별 용량 스냅숏 저장·읽기·비교(변화 추적)
│   ├── cli.c/.h            인자 파싱, 텍스트 출력, 종료 코드
│   ├── jsonl.c/.h          JSON Lines 이벤트 출력
│   ├── drive.c/.h          논리 드라이브 목록·용량 조회, 오류 분류, 사용률
│   ├── size_format.c/.h    용량 표시 문자열("1.50 KiB")
│   ├── sysmon.c/.h         RAM·CPU 사용률 측정(GlobalMemoryStatusEx, GetSystemTimes), 이상값·실패 처리
│   ├── cleanup_rules.c/.h  정리 후보 이름 규칙, 개발 환경 분류, 프로젝트 표시 파일, 시스템 보호 경로
│   ├── cleanup_scan.c/.h   정리 후보 탐지 엔진(중첩·용량 집계·필터·정렬)
│   ├── cleanup_job.c/.h    탐지 작업 스레드, 진행률 사본, 취소
│   ├── cleanup_trash.c/.h  삭제 보호 판정, 직전 확인, 휴지통 이동(IFileOperation), 작업 스레드
│   ├── main.c              CLI 진입점(disk-scanner)
│   ├── bench.c             대용량 스캔 실행 시간·메모리 측정 도구(bytesieve-bench)
│   ├── gui_main.c          Win32 GUI 진입점(bytesieve-gui), 탭 전환
│   ├── gui_drives.c/.h     드라이브 대시보드 패널(목록·사용률 막대·새로고침)
│   ├── gui_cleanup.c/.h    정리 후보 패널(탐지·취소·유형 필터·체크 목록·확인·휴지통 이동·결과)
│   └── gui_monitor.c/.h    시스템 패널(RAM·CPU 사용률 막대·60초 그래프, 측정 스레드)
└── tests/
    ├── test_path.c         경로 유틸리티
    ├── test_scanner.c      스캐너(권한 거부·정션·긴 경로 등)
    ├── test_scan_model.c   트리·순위·진행률·취소
    ├── test_scan_job.c     스레드 스캔·취소
    ├── test_scan_alloc.c   디스크 사용량 원인 분류(실제 압축·스파스 파일), 병렬 미리 열거의 훅 순서 보장
    ├── test_scan_history.c 스냅숏 저장·읽기, 변화 비교(늘어남·줄어듦·새 폴더·삭제·불확실·깊이), 손상 파일
    ├── test_jsonl.c        JSON 직렬화
    ├── test_cli.c          인자 파싱·출력 형식
    ├── test_drive.c        드라이브 조회·오류 분류·사용률 계산
    ├── test_size_format.c  용량 표시 문자열
    ├── test_sysmon.c       RAM·CPU 사용률 계산, 이상값, 측정 실패·회복(가짜 측정 함수), 실제 시스템
    ├── test_cleanup_rules.c 탐지 규칙·보호 경로 판정
    ├── test_cleanup_scan.c 탐지 엔진·필터·취소·작업 스레드(픽스처 폴더)
    ├── test_cleanup_trash.c 삭제 보호 판정, 실제 휴지통 이동·거부, 작업 스레드·취소
    ├── gui_trash_check.cmake GUI 휴지통 이동 자체 검사(픽스처 생성·결과 확인·휴지통 정리)
    ├── cli_track_check.cmake  CLI --track 기준 → 변경 → 비교, JSON Lines changes 검사
    ├── gui_history_check.cmake GUI 변화 순위 보기 검사(기준 스캔 → 변경 → --self-test-changes)
    └── cli_check.cmake     CLI 종료 코드·출력 검사 스크립트
```

CMake 대상은 다음과 같습니다. 정적 라이브러리는 `bytesieve_path` → `bytesieve_scanner` → `bytesieve_model` → `bytesieve_job` 순서로 의존하고, `bytesieve_history`(변화 추적)는 `bytesieve_model`에, `bytesieve_cli`와 `bytesieve_jsonl`은 `bytesieve_scanner`·`bytesieve_history`에 의존합니다. `bytesieve_drive`(드라이브 조회), `bytesieve_format`(용량 표시), `bytesieve_sysmon`(RAM·CPU 측정)은 다른 라이브러리에 의존하지 않습니다. 정리 후보 탐지는 `bytesieve_cleanup_rules`(→ `bytesieve_path`) ← `bytesieve_cleanup`(+ `bytesieve_scanner`) ← `bytesieve_cleanup_job` 순서로 의존하고, 휴지통 이동 `bytesieve_cleanup_trash`는 `bytesieve_cleanup`과 `ole32`·`shell32`·`uuid`에 의존합니다. 실행 파일은 `disk-scanner`(CLI), `bytesieve-gui`(GUI), `bytesieve-bench`(성능 측정)입니다.

## 빌드

**요구 사항**: Windows 10 이상, CMake 3.21 이상, Visual Studio(MSVC) 빌드 도구

```powershell
cmake -S . -B build
cmake --build build --config Debug      # 릴리스는 --config Release
```

결과물은 `build\Debug\disk-scanner.exe`와 `build\Debug\bytesieve-gui.exe`입니다.

## 실행

### CLI

```powershell
# 기본 요약
build\Debug\disk-scanner.exe C:\Users\me\Projects

# 대상 폴더 바로 아래만(깊이 0)
build\Debug\disk-scanner.exe --max-depth 0 C:\Users\me\Projects

# JSON Lines 출력
build\Debug\disk-scanner.exe --jsonl C:\Users\me\Projects > scan.jsonl

# 직전 --track 스캔과 폴더별 용량 비교(처음에는 기준만 저장)
build\Debug\disk-scanner.exe --track C:\Users\me\Projects

# 병렬 없이(메모리를 덜 씀)
build\Debug\disk-scanner.exe --threads 1 C:\Users\me\Projects

# 도움말
build\Debug\disk-scanner.exe --help
```

출력 예:

```
Target:         C:\Windows\Logs
Max depth:      unlimited
Files:          214
Directories:    21
Total size:     244.58 MiB (256465062 bytes)
On disk:        124.28 MiB (130318336 bytes)
  Slack:        +1.12 MiB (1177841 bytes) (cluster size 4.00 KiB)
  Compressed:   -121.43 MiB (127324567 bytes) in 8 file(s)
Reparse points: 0 (not descended)
Errors:         1 (access_denied 1)
Skipped dirs:   1
Aborted:        no
Status:         partial (1 directory could not be read; totals are incomplete)
```

`--track`으로 두 번째 스캔한 예:

```
Changes since 2026-10-31 02:05: +900 B (300 B -> 1.17 KiB)
  grown 1, shrunk 0, added 1, removed 1
       +1000 B  grown    a
        -200 B  removed  b
        +100 B  added    c
History:        saved to C:\Users\me\AppData\Local\ByteSieve\history\9835cdb3e1da6d91.snap
```

```
{"event":"start","target":"src"}
{"event":"result","target":"src","files":14,"dirs":0,"bytes":100451,"alloc_bytes":118784,"disk":{"slack":18333,"compressed_saved":0,"sparse_saved":0,"cloud_saved":0,"other_saved":0,"unknown_files":0},"reparse_points":0,"error_count":0,"errors":[]}
{"event":"complete","status":"complete","exit_code":0}
```

### GUI

```powershell
# 창을 열고 경로를 입력하거나 [찾아보기]로 선택한 뒤 [스캔]
build\Debug\bytesieve-gui.exe

# 폴더를 지정하면 바로 스캔을 시작
build\Debug\bytesieve-gui.exe C:\Users\me\Projects
```

정리 후보를 찾으려면 **정리 후보** 탭에서 경로(예: `C:\Users\me\Projects`)를 고르고 [탐지]를 누릅니다. 유형 콤보 상자로 원하는 유형만 볼 수 있습니다. 정리하려면 항목을 체크하고 **[휴지통으로 이동...]**을 누른 뒤 확인 창의 경로·유형·용량을 보고 [예]를 누릅니다. 옮긴 항목은 Windows 휴지통에서 복원할 수 있습니다. **추정** 표시가 붙은 항목은 프로젝트 파일이 옆에 없어 빌드 결과물이 아닐 수 있으니 특히 확인하세요.

자동 검사용 옵션도 있습니다. 창을 숨긴 채 스캔하고 화면 상태를 검증한 뒤 종료 코드로 결과를 알립니다(0 성공, 1 실패).

```powershell
build\Debug\bytesieve-gui.exe --self-test C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cancel C:\some\dir   # 시작 직후 취소
build\Debug\bytesieve-gui.exe --self-test-drives               # 드라이브 탭 조회 후 목록 검증
build\Debug\bytesieve-gui.exe --self-test-cleanup C:\some\dir         # 정리 후보 탐지 후 목록·필터 검증
build\Debug\bytesieve-gui.exe --self-test-cleanup-cancel C:\some\dir  # 탐지 시작 직후 취소
build\Debug\bytesieve-gui.exe --self-test-trash C:\some\dir          # 주의: 옮길 수 있는 항목을 모두 휴지통으로 옮김
build\Debug\bytesieve-gui.exe --self-test-monitor             # 시스템 탭에서 4번 측정한 뒤 화면·갱신 간격·실패 표시 검증
build\Debug\bytesieve-gui.exe --self-test-changes C:\some\dir  # 직전 스냅숏과 비교한 변화 보기 검증, 변화 목록을 stdout 에 출력
```

`--self-test-trash`는 확인 창 없이 실제로 휴지통으로 옮기므로 테스트용 폴더에만 쓰세요. `gui_trash` 테스트는 빌드 폴더 안에 전용 픽스처를 만들어 이 옵션을 실행합니다.
폴더 스캔 자체 검사도 변화 기록을 저장하므로, 테스트는 `BYTESIEVE_HISTORY_DIR`을 빌드 폴더로 지정해 사용자 기록 폴더를 건드리지 않습니다.

## 성능 측정

`bytesieve-bench`로 대용량 폴더 스캔의 실행 시간·CPU 시간·최대 메모리를 잽니다. 최대 메모리는 프로세스 전체 기준이므로
모드마다 따로 실행하고, 첫 실행은 디스크 캐시가 차지 않았을 수 있어 두 번째 실행부터 봅니다.

```powershell
cmake --build build --config Release --target bytesieve-bench
build\Release\bytesieve-bench.exe --mode tree    --runs 3 C:\Windows   # 집계만
build\Release\bytesieve-bench.exe --mode model   --runs 3 C:\Windows   # GUI 폴더 스캔과 같은 트리 구성
build\Release\bytesieve-bench.exe --mode cleanup --runs 3 C:\Windows   # 정리 후보 탐지
build\Release\bytesieve-bench.exe --mode model --threads 1 C:\Windows # 병렬 없이
```

측정 결과(Windows 11, 논리 프로세서 12개, `C:\Windows`: 파일 21만·폴더 16만, 디스크 캐시가 찬 상태, Release):

| 모드 | 개선 전 | 개선 후(자동, 8 스레드) | 개선 후(`--threads 1`) |
|---|---|---|---|
| tree | 16.4 s · 최대 전용 메모리 12 MiB | **3.2 s** · 55 MiB | 12.6 s · 36 MiB |
| model | 16.4 s · 64 MiB(해제 10 ms) | **3.1 s** · 98 MiB(해제 3 ms) | 12.6 s · 73 MiB |
| cleanup | 16.6 s · 12 MiB | **3.3 s** · 57 MiB | 13.0 s · 36 MiB |

병목 분석과 개선:
1. **병목은 커널의 폴더 열기·열거**였습니다. 개선 전 CPU 시간의 97%가 커널 시간이었고(폴더 하나에 약 100 µs, 보안 검사와
   파일 시스템 필터 비용), 모델 구성·정리 규칙 같은 사용자 코드는 거의 차지하지 않았습니다.
2. 열거 API 비교(같은 폴더, 한 스레드): `FindFirstFileW` 16.3 s → `FindFirstFileExW`(기본 정보·대용량 읽기) 14.0 s →
   **폴더 핸들 + `GetFileInformationByHandleEx`** 12.4 s. 마지막 방식은 할당 크기까지 함께 주므로 디스크 사용량 분석을
   추가 비용 없이 얻습니다.
3. 커널 작업은 폴더마다 독립이라 **여러 스레드로 나누면 거의 비례해 빨라집니다**(시험 구현: 1→2→4→8 스레드에서
   12.8→6.9→4.1→3.1 s). 다만 훅을 깊이 우선 순서로 불러야 해서, 처음 구현(호출 스레드가 하위 폴더를 발견한 뒤에야 넘김)은
   11.5 s에 그쳤습니다. 열거한 쪽이 하위 폴더를 바로 넘기고, 작업 스레드가 깊이 우선 처리 순서 키로 **가장 먼저 필요한
   폴더부터** 읽게 하자 거의 모든 폴더를 작업 스레드가 읽어 3.1 s가 되었습니다. 큰 폴더(`WinSxS`, 하위 폴더 3만 개) 하나를
   읽는 동안 다른 스레드가 뒤쪽 하위 트리를 앞서 읽어 두기 때문에, 미리 읽은 결과의 상한을 너무 작게(4,096개·8~16 MiB)
   잡으면 상한에 걸려 다시 느려졌습니다. 그래서 32 MiB로 정했습니다.
4. 메모리: 미리 읽은 폴더 목록과 하위 폴더 작업을 붙잡아 두는 만큼 **최대 메모리가 늘었습니다**(tree 12 → 55 MiB).
   작업 구조·경로·순서 키를 한 번에 할당하고, 첫 묶음은 필요한 만큼만 버퍼를 잡아 75 → 55 MiB로 줄였습니다.
   메모리가 중요하면 `--threads 1`을 쓰면 됩니다. 이때도 큰 폴더 하나의 목록을 한꺼번에 담기 때문에 개선 전보다는 많이 씁니다.
   모델 자체는 이름 저장소 덕분에 스캔 후 메모리가 약간 줄고(47 → 45 MiB) 해제가 3배 빨라졌습니다.

## 테스트

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

현재 테스트는 47개입니다.
- 단위 테스트: `path`, `scanner`, `scan_alloc`, `scan_model`, `scan_history`, `scan_job`, `jsonl`, `cli`, `drive`, `size_format`, `sysmon`, `cleanup_rules`, `cleanup_scan`, `cleanup_trash`
- CLI 종료 코드·출력 검사: `cli_*`(변화 추적 `cli_track` 포함) 및 인자 검사 테스트들
- GUI 자체 검사: `gui_scan`, `gui_cancel`, `gui_missing_dir`, `gui_drives`, `gui_cleanup`, `gui_cleanup_cancel`, `gui_trash`, `gui_monitor`, `gui_history`

`scan_alloc` 테스트는 원인 분류 규칙(클러스터 낭비, 압축, 스파스, 클라우드 전용이 압축·스파스보다 우선, 기타, 할당 크기
모름)을 확인하고, 임시 폴더에 **실제 파일**을 만들어 10,000 B 파일의 할당 크기가 클러스터 단위로 반올림되는지,
NTFS 압축한 1 MiB 파일과 64 MiB 스파스 파일이 압축·스파스로 분류되고 절약량이 맞는지, 전체 합계 식이 맞는지 봅니다.
또 폴더 186개짜리 트리를 작업 스레드 1개와 8개로 각각 5번씩 스캔해 **훅 호출 기록(순서·인자)과 집계가 완전히 같은지**
확인합니다(깊이 제한, 미리 읽은 폴더를 훅이 제외하는 경우, 취소 지점 포함).

`scan_history` 테스트는 스냅숏 내용(깊이 4 제한, 한글 이름), 저장·읽기 왕복, 변화 비교(늘어남·줄어듦·새 폴더와 그 하위
생략·삭제 폴더와 그 하위 생략·깊이 4 아래의 변화가 조상에 반영·변화량 순서·같은 스냅숏은 변화 없음·다른 대상과 비교
거부·대소문자와 끝 구분자 무시), 불확실 표시, 깊이가 다른 스냅숏, 기록 파일 이름·기본 위치·폴더 만들기, 손상된 파일을
확인합니다. `cli_track`과 `gui_history`는 실제 실행 파일로 기준 스캔 → 픽스처 변경 → 비교를 확인합니다.

`cleanup_scan` 테스트는 기존 픽스처와 별도로 개발 환경 픽스처를 만들어 환경마다의 분류(표시 파일이 폴더보다 뒤에
열거되는 경우 포함), 표시 파일이 없는 `bin`·`.gradle`·`target`이 후보가 아니고 그 안의 로그는 탐지되는지, 환경별
개수·합계, 환경 필터와 유형 × 환경 필터를 확인합니다. `gui_cleanup`은 환경 콤보 상자와 개발 환경 열도 검증합니다.

`drive` 테스트는 가짜 조회 함수로 이동식 드라이브 오류·분리·잘못된 값을 재현하고, 실제 시스템에서는 Windows가 설치된 드라이브가 정상 조회되는지와 없는 드라이브 문자가 실패 상태로 처리되는지 확인합니다.

`sysmon` 테스트는 RAM 사용률(내림·경계값·아주 큰 값·사용 가능 > 전체), CPU 사용률(반올림·0%·100%·idle 과다 보고·증가분 0·카운터 역행·큰 누적값)을 확인하고, 가짜 측정 함수로 RAM·CPU API 실패와 오류 코드 기록, CPU 실패 후 `pending` → 회복, 역행 후 재계산을 재현합니다. 실제 시스템에서는 두 번 측정해 값의 범위와 `GlobalMemoryStatusEx` 전체 용량 일치를 확인합니다. `gui_monitor`는 숨긴 창에서 4번 측정한 뒤 문구·막대 위치·경고 상태·그래프 기록이 최근 측정값과 맞는지, 평균 갱신 간격이 1초 근처(0.7~2초)인지 확인하고, 실패 측정값을 넣어 오류 문구·빈 막대·그래프 끊김을 확인합니다.

`cleanup_scan` 테스트는 임시 폴더에 픽스처를 만들어 중첩된 `node_modules`·`dist`, 확인됨/추정, 비슷하지만 해당하지 않는 이름(`report.log.bak`, `buildx`), 정션과 정션 순환, 접근 거부 폴더, 260자를 넘는 경로, 보호 경로 제외와 보호 경로 안에서의 경고, 유형 필터와 정렬, 취소, 작업 스레드를 확인합니다.

`cleanup_trash` 테스트는 보호 판정 규칙(보호 경로 아래, 중요 경로와 그 상위, 탐지 경로 밖·자체, 이름만 비슷한 형제 폴더, 일부 누락, UNC·상대·긴 경로, 대소문자·끝 구분자)과 현재 시스템의 보호 목록(Windows 폴더, 사용자 프로필, 테스트 실행 파일 폴더)을 확인합니다. 이어서 임시 폴더 픽스처의 탐지 결과를 **실제로 휴지통으로 옮겨** 원래 자리에 없고 휴지통 안에 있는지 확인하고, 보호 경로·종류가 바뀐 항목·정션 항목·정션을 거친 경로·없는 항목이 손대지 않은 채 거부되는지, 작업 스레드의 결과 순서·집계·취소를 확인합니다. `gui_trash`는 접근 거부 폴더가 든 `node_modules`(일부 누락 → 보호)를 포함한 픽스처로 GUI의 선택·이동·결과 표시를 확인합니다. 두 테스트 모두 **자신이 휴지통에 넣은 항목만** 지워 사용자 휴지통을 원래대로 둡니다.

`ctest`가 PATH에 없으면 Visual Studio 개발자 PowerShell에서 실행하거나 Visual Studio에 포함된 CMake 경로(`...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`)를 사용하세요.
