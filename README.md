# ByteSieve

Windows용 디스크 사용량 분석 도구입니다. 지정한 폴더 아래의 파일 수·폴더 수·총 용량을 집계하고,
CLI(텍스트·JSON Lines)와 Win32 GUI(진행률·폴더 트리·용량 순위)로 결과를 보여 줍니다.
GUI의 드라이브 탭에서는 시스템의 모든 논리 드라이브 용량과 사용률을 한눈에 볼 수 있고,
정리 후보 탭에서는 `node_modules`·빌드 결과물·개발 캐시·임시·로그 파일을 찾아 예상 확보 용량을 보여 줍니다.
C11과 Win32 API만 사용하며 외부 라이브러리 의존성은 없습니다.

## 진행 현황

| 단계 | 내용 | 상태 |
|---|---|---|
| 1 | CLI 스캐너: 지정 디렉터리 탐색, 파일 수·폴더 수·총 용량 집계 | 완료 |
| 2 | 정렬 및 캐시 분류: 상위 용량 폴더와 개발자 캐시 확인 | 용량 순위만 완료(캐시 분류 미구현) |
| 3 | 오류 및 취소 처리: 권한 거부, 긴 경로, 재분석 지점, 중단 처리 | 완료 |
| 4 | JSON Lines 출력: 시작·진행·결과·완료 이벤트 | 완료 |
| 5 | GUI 연동: 스캔 진행률, 폴더 트리, 용량 순위 화면 | 완료 |
| 6 | 고급 분석: 실제 디스크 점유량, BitLocker 정보, 메모리 최적화 | 예정 |

| 대시보드 | 내용 | 상태 |
|---|---|---|
| Phase 1 | 드라이브 사용량: 논리 드라이브 목록, 전체·사용·여유 용량, 사용률 막대, 새로고침 | 완료 |
| Phase 2 | 정리 후보 탐지: 개발 캐시·빌드 결과물·임시·로그, 예상 확보 용량, 유형 필터, 취소 (읽기 전용) | 완료 |
| Phase 3 | 정리 후보 삭제 | 예정 |

## 구현된 기능

### 스캐너 (`scanner`)
- 동적 스택 기반 DFS로 하위 폴더 전체를 탐색합니다(재귀 호출 없음). `--max-depth`로 깊이를 제한할 수 있습니다.
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

### 데이터 모델 (`scan_model`)
- 스캔 결과를 폴더 트리로 만듭니다. 파일은 노드로 만들지 않고 소속 폴더에만 집계합니다.
- 폴더마다 바로 아래 파일 용량과 하위 트리 전체 용량을 계산합니다.
- 자식 폴더를 열거 순서·용량순·이름순으로 정렬할 수 있습니다.
- 용량 순위를 하위 포함 용량 또는 바로 아래 파일 용량 기준으로 구합니다.
- 진행률 콜백을 제공합니다. 진행률은 다 읽은 폴더 수 ÷ (다 읽은 폴더 + 남은 폴더)로 추정합니다.
- 폴더마다 상태 표시가 붙습니다: 읽기 실패, 일부 누락, 취소됨, 링크, 깊이 제한.

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
**읽기 전용입니다.** 디렉터리 열거(`FindFirstFileW`/`FindNextFileW`)로 얻은 이름·크기·속성만 쓰며, 파일을 열거나 내용을 읽지 않고, 삭제·이동·수정하지 않습니다. 실제 삭제는 다음 단계에서 따로 구현합니다.

탐지 규칙(이름은 대소문자 무시, 정확히 일치):

| 유형 | 폴더 | 파일 |
|---|---|---|
| Node.js | `node_modules` | – |
| 빌드 결과물 | `dist`, `build`, `out` | – |
| 개발 캐시 | `.cache`, `.next`, `.vite` | – |
| 임시 파일 | `.tmp`, `.temp` | `*.tmp` |
| 로그 | – | `*.log` |

탐지 정책:
- **중첩**: 위에서부터 처음 맞은 폴더가 그 아래 전체를 차지합니다. `node_modules\pkg\dist`나 `build\app.log`는 따로 나오지 않고 바깥 항목 용량에 포함되므로, 항목 용량의 합(예상 확보 용량)에 중복이 없습니다.
- **용량**: 폴더는 하위 전체 파일의 논리 크기 합이며 스캐너와 같은 기준입니다(실제 디스크 점유량은 6단계).
- **신뢰도**: `node_modules`·`dist`·`build`·`out`은 흔한 이름이라, 같은 폴더에 프로젝트 표시 파일(`package.json`, `CMakeLists.txt`, `Makefile`, `Cargo.toml`, `pom.xml`, `build.gradle(.kts)`, `pyproject.toml`, `go.mod`, `*.csproj`, `*.sln`)이 있으면 **확인됨**, 없으면 **추정**으로 표시합니다.
- **시스템 보호 경로**: Windows 폴더, `Program Files`, `Program Files (x86)`, `ProgramData`와 볼륨 루트의 `$Recycle.Bin`, `System Volume Information`, `Recovery`, `Config.Msi`는 탐색 중 만나면 들어가지 않습니다. 사용자가 보호 경로 안을 직접 고르면 탐지는 하되 빨간 경고를 띄우고 모든 항목에 **시스템 경로** 표시를 붙입니다. `AppData`는 캐시·임시 파일이 많은 곳이라 제외하지 않습니다.
- **링크**: 정션·심볼릭 링크는 폴더든 파일이든 따라가지 않고 항목으로도 만들지 않습니다(순환 방지, pnpm 링크 중복 계산 방지).
- **선택한 경로 자체**는 탐색 범위일 뿐 항목이 되지 않습니다.
- **오류·취소**: 스캐너 정책 그대로입니다(접근 거부·긴 경로 등은 기록하고 계속). 하위를 다 읽지 못한 항목은 **일부 누락**으로 표시하며, 취소하면 그때까지 찾은 결과를 보여 줍니다.
- 탐지는 별도 스레드(`cleanup_job`)에서 하며 진행률 알림은 100ms 간격으로 제한합니다. 폴더마다 메모리를 잡지 않고 후보 항목만 저장합니다.

### CLI (`disk-scanner`)
- 사람이 읽는 요약을 출력합니다. 오류는 종류별 개수와 함께 최대 20개까지 나열합니다.
- `--jsonl`을 주면 표준 출력에 UTF-8 JSON Lines 이벤트를 출력합니다. 이벤트 순서는 `start` → `progress`(1,000개 항목마다) → `result` → `complete`입니다.
- 종료 코드: `0` 완료, `1` 일부 실패, `2` 스캔 불가 또는 잘못된 인자.

### GUI (`bytesieve-gui`)
창은 **폴더 스캔**, **드라이브**, **정리 후보** 세 탭으로 나뉩니다.

**폴더 스캔 탭**
- 경로 입력, 찾아보기, 스캔, 취소 버튼이 있습니다.
- 진행률 표시줄과 상태 줄에 파일·폴더 수, 용량, 오류 수, 현재 폴더를 보여 줍니다.
- **폴더 트리(TreeView)**: 용량이 큰 순서로 정렬하고, 폴더를 펼칠 때 하위 항목을 채웁니다. 각 항목에 용량과 전체 대비 비율, 상태 표시가 붙습니다.
- **용량 순위(ListView)**: 상위 100개 폴더를 순위·경로·용량·파일 수·비율로 보여 줍니다. 항목을 더블클릭하거나 Enter를 누르면 트리에서 그 폴더를 펼쳐 선택합니다.
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
- **유형** 콤보 상자로 전체·Node.js·빌드 결과물·개발 캐시·임시 파일·로그 중 하나만 볼 수 있습니다. 요약 줄에 `표시 n개 · 용량 / 전체 n개 · 예상 확보 용량`을 보여 줍니다.
- 탐지 중에 창을 닫으면 탐지를 취소하고 스레드가 끝날 때까지 기다린 뒤 종료합니다.

## 디렉터리 구조

```
ByteSieve/
├── CMakeLists.txt          빌드·테스트 정의
├── READEME.md
├── src/
│   ├── path.c/.h           경로 결합·정규화, \\?\ 확장 경로 변환
│   ├── scanner.c/.h        디렉터리 탐색(DFS), 집계, 오류 분류, 취소 훅
│   ├── scan_model.c/.h     폴더 트리 모델, 진행률, 정렬, 용량 순위
│   ├── scan_job.c/.h       백그라운드 스레드 스캔, 진행률 사본, 취소
│   ├── cli.c/.h            인자 파싱, 텍스트 출력, 종료 코드
│   ├── jsonl.c/.h          JSON Lines 이벤트 출력
│   ├── drive.c/.h          논리 드라이브 목록·용량 조회, 오류 분류, 사용률
│   ├── size_format.c/.h    용량 표시 문자열("1.50 KiB")
│   ├── cleanup_rules.c/.h  정리 후보 이름 규칙, 프로젝트 표시 파일, 시스템 보호 경로
│   ├── cleanup_scan.c/.h   정리 후보 탐지 엔진(중첩·용량 집계·필터·정렬)
│   ├── cleanup_job.c/.h    탐지 작업 스레드, 진행률 사본, 취소
│   ├── main.c              CLI 진입점(disk-scanner)
│   ├── gui_main.c          Win32 GUI 진입점(bytesieve-gui), 탭 전환
│   ├── gui_drives.c/.h     드라이브 대시보드 패널(목록·사용률 막대·새로고침)
│   └── gui_cleanup.c/.h    정리 후보 패널(탐지·취소·유형 필터·용량순 목록)
└── tests/
    ├── test_path.c         경로 유틸리티
    ├── test_scanner.c      스캐너(권한 거부·정션·긴 경로 등)
    ├── test_scan_model.c   트리·순위·진행률·취소
    ├── test_scan_job.c     스레드 스캔·취소
    ├── test_jsonl.c        JSON 직렬화
    ├── test_cli.c          인자 파싱·출력 형식
    ├── test_drive.c        드라이브 조회·오류 분류·사용률 계산
    ├── test_size_format.c  용량 표시 문자열
    ├── test_cleanup_rules.c 탐지 규칙·보호 경로 판정
    ├── test_cleanup_scan.c 탐지 엔진·필터·취소·작업 스레드(픽스처 폴더)
    └── cli_check.cmake     CLI 종료 코드·출력 검사 스크립트
```

CMake 대상은 다음과 같습니다. 정적 라이브러리는 `bytesieve_path` → `bytesieve_scanner` → `bytesieve_model` → `bytesieve_job` 순서로 의존하고, `bytesieve_cli`와 `bytesieve_jsonl`은 `bytesieve_scanner`에 의존합니다. `bytesieve_drive`(드라이브 조회)와 `bytesieve_format`(용량 표시)은 다른 라이브러리에 의존하지 않습니다. 정리 후보 탐지는 `bytesieve_cleanup_rules`(→ `bytesieve_path`) ← `bytesieve_cleanup`(+ `bytesieve_scanner`) ← `bytesieve_cleanup_job` 순서로 의존합니다. 실행 파일은 `disk-scanner`(CLI)와 `bytesieve-gui`(GUI)입니다.

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

# 도움말
build\Debug\disk-scanner.exe --help
```

출력 예:

```
Target:         src
Max depth:      unlimited
Files:          14
Directories:    0
Total size:     98.10 KiB (100451 bytes)
Reparse points: 0 (not descended)
Errors:         0
Skipped dirs:   0
Aborted:        no
Status:         complete (all directories scanned)
```

```
{"event":"start","target":"src"}
{"event":"result","target":"src","files":14,"dirs":0,"bytes":100451,"reparse_points":0,"error_count":0,"errors":[]}
{"event":"complete","status":"complete","exit_code":0}
```

### GUI

```powershell
# 창을 열고 경로를 입력하거나 [찾아보기]로 선택한 뒤 [스캔]
build\Debug\bytesieve-gui.exe

# 폴더를 지정하면 바로 스캔을 시작
build\Debug\bytesieve-gui.exe C:\Users\me\Projects
```

정리 후보를 찾으려면 **정리 후보** 탭에서 경로(예: `C:\Users\me\Projects`)를 고르고 [탐지]를 누릅니다. 유형 콤보 상자로 원하는 유형만 볼 수 있습니다. 이 단계에서는 아무것도 삭제하지 않으므로, 정리는 목록을 보고 직접 하세요. **추정**이나 **시스템 경로** 표시가 붙은 항목은 특히 조심해야 합니다.

자동 검사용 옵션도 있습니다. 창을 숨긴 채 스캔하고 화면 상태를 검증한 뒤 종료 코드로 결과를 알립니다(0 성공, 1 실패).

```powershell
build\Debug\bytesieve-gui.exe --self-test C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cancel C:\some\dir   # 시작 직후 취소
build\Debug\bytesieve-gui.exe --self-test-drives               # 드라이브 탭 조회 후 목록 검증
build\Debug\bytesieve-gui.exe --self-test-cleanup C:\some\dir         # 정리 후보 탐지 후 목록·필터 검증
build\Debug\bytesieve-gui.exe --self-test-cleanup-cancel C:\some\dir  # 탐지 시작 직후 취소
```

## 테스트

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

현재 테스트는 39개입니다.
- 단위 테스트: `path`, `scanner`, `scan_model`, `scan_job`, `jsonl`, `cli`, `drive`, `size_format`, `cleanup_rules`, `cleanup_scan`
- CLI 종료 코드·출력 검사: `cli_*` 및 인자 검사 테스트들
- GUI 자체 검사: `gui_scan`, `gui_cancel`, `gui_missing_dir`, `gui_drives`, `gui_cleanup`, `gui_cleanup_cancel`

`drive` 테스트는 가짜 조회 함수로 이동식 드라이브 오류·분리·잘못된 값을 재현하고, 실제 시스템에서는 Windows가 설치된 드라이브가 정상 조회되는지와 없는 드라이브 문자가 실패 상태로 처리되는지 확인합니다.

`cleanup_scan` 테스트는 임시 폴더에 픽스처를 만들어 중첩된 `node_modules`·`dist`, 확인됨/추정, 비슷하지만 해당하지 않는 이름(`report.log.bak`, `buildx`), 정션과 정션 순환, 접근 거부 폴더, 260자를 넘는 경로, 보호 경로 제외와 보호 경로 안에서의 경고, 유형 필터와 정렬, 취소, 작업 스레드를 확인합니다.

`ctest`가 PATH에 없으면 Visual Studio 개발자 PowerShell에서 실행하거나 Visual Studio에 포함된 CMake 경로(`...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`)를 사용하세요.
