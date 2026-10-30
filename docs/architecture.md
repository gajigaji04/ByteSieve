# 아키텍처와 설계

## 기술 스택

- **언어·빌드**: C11, CMake 3.21+, MSVC(`/W4 /utf-8`), 유니코드(`UNICODE`) 빌드
- **Windows API**: 폴더 열거 `GetFileInformationByHandleEx(FileFullDirectoryInfo)`(미지원 파일 시스템은 `FindFirstFileExW`),
  긴 경로 `\\?\`, 휴지통 이동 `IFileOperation`, 드라이브 `GetLogicalDrives`·`GetDiskFreeSpaceExW`,
  메모리·CPU `GlobalMemoryStatusEx`·`GetSystemTimes`, GUI는 Win32 + Common Controls v6
- **구조**: UI와 무관한 정적 라이브러리 위에 CLI·GUI를 얹었습니다. 무거운 작업(스캔·탐지·휴지통 이동·측정)은 모두 작업 스레드에서
  실행하고, GUI 스레드는 `PostMessage`로 결과만 받아 화면을 갱신합니다.

```
                    disk-scanner (CLI)                bytesieve-gui (GUI)
                     │  cli · jsonl                     │  폴더 스캔 │ 드라이브 │ 정리 후보 │ 시스템
                     │                                  │
   scan_history ── scan_model ── scan_job        cleanup_job ── cleanup_trash      drive   sysmon
   (변화 추적)     (폴더 트리)   (스캔 스레드)   (탐지 스레드)   (휴지통 이동)    (드라이브) (RAM·CPU)
                        │                             │
                        └────────── scanner ──────────┘   cleanup_scan ── cleanup_rules
                         (DFS 열거·병렬 미리 열거·      (탐지 엔진)     (이름 규칙·개발 환경·보호 경로)
                          할당 크기 분석·오류 분류)
                                     │
                                    path (경로 결합·\\?\ 변환)
```

| 라이브러리(CMake 대상) | 역할 |
|---|---|
| `bytesieve_path` | 경로 결합·정규화, `\\?\` 확장 경로 변환 |
| `bytesieve_scanner` | 폴더 열거(DFS), 집계·디스크 할당 크기 분석, 오류 분류, 취소·제외 훅, 병렬 미리 열거 |
| `bytesieve_model` | 폴더 트리 모델, 정렬, 용량·디스크 사용 순위, 진행률 |
| `bytesieve_job` | 스캔 작업 스레드, 진행률 사본, 취소 |
| `bytesieve_history` | 폴더별 용량 스냅숏 저장·읽기·비교 |
| `bytesieve_cleanup_rules` / `bytesieve_cleanup` / `bytesieve_cleanup_job` | 정리 후보 규칙, 탐지 엔진, 탐지 스레드 |
| `bytesieve_cleanup_trash` | 삭제 보호 판정, 직전 확인, 휴지통 이동 |
| `bytesieve_drive` / `bytesieve_sysmon` / `bytesieve_format` | 드라이브 조회, RAM·CPU 측정, 용량 표시 문자열 |
| `bytesieve_cli` / `bytesieve_jsonl` | CLI 인자 해석·출력, JSON Lines 직렬화 |

실행 파일은 `disk-scanner`(CLI), `bytesieve-gui`(GUI), `bytesieve-bench`(성능 측정 도구)입니다.

## 오류 처리

- 오류가 나도 스캔을 멈추지 않고 경로와 Windows 오류 코드를 기록합니다. 종류: 접근 거부, 경로 없음, 잘못된 경로, 경로 너무 김, 메모리 부족, 기타.
- 결과 상태는 `complete`(모두 읽음), `partial`(일부 폴더를 읽지 못했거나 취소), `fatal`(대상 폴더 자체를 열지 못함) 세 가지입니다.
- `\\?\` 확장 경로로 260자를 넘는 경로(최대 32,767자)도 읽습니다.
- 정션·심볼릭 링크는 개수만 세고 따라가지 않아 순환을 막습니다.
- 다 읽지 못한 폴더는 트리에서 `[읽기 실패]`·`[일부 누락]`, 변화 추적에서 `(?)`, 정리 후보에서 '일부 누락'으로 표시합니다.

## 취소와 응답성

- 취소 요청은 폴더를 열기 직전과 폴더 하나를 읽는 도중 256개 항목마다 확인합니다.
- 진행률 알림은 100ms 간격으로 제한하고, 작업 중 창을 닫으면 작업을 취소하고 스레드가 끝날 때까지 기다린 뒤 종료합니다.

## 병렬 미리 열거

- 폴더를 열거한 쪽이 곧바로 하위 폴더 작업을 큐에 넣고, 작업 스레드(기본 논리 프로세서 수, 최대 8개)는 깊이 우선 처리 순서 키로 정렬한 힙에서
  **가장 먼저 필요한 폴더부터** 읽습니다.
- 훅은 언제나 호출 스레드에서 병렬이 아닐 때와 **같은 순서**로 불리므로, 결과·오류 순서가 스레드 수와 상관없이 같습니다(테스트로 확인).
- 미리 읽은 결과는 32 MiB(또는 65,536개 폴더)로 제한하고, 상한에 닿으면 호출 스레드가 직접 읽습니다.

측정 결과와 병목 분석은 [performance.md](performance.md)에 있습니다.

## 디스크 사용량 분석

할당 크기는 폴더 열거 결과에 들어 있어 파일을 열지 않습니다. 압축·스파스 파일만 `GetCompressedFileSizeW`로 다시 구하고,
클라우드 자리표시자는 내려받기를 일으키지 않도록 건드리지 않습니다. 원인은 클라우드 전용 > 압축 > 스파스 > 기타 순으로 하나에만 넣으므로
`디스크 사용 = 논리 크기 + 클러스터 낭비 - (압축 + 스파스 + 클라우드 + 기타)`가 항상 성립합니다.

## 변화 추적

폴더 트리를 깊이 4까지 저장하며(그 아래 변화는 조상 폴더 용량에 반영), 새 폴더·삭제된 폴더 안의 하위 폴더는 따로 나열하지 않습니다.
스냅숏은 임시 파일에 쓴 뒤 바꿔치기하고, 대상 폴더마다 직전 스냅숏 하나만 남깁니다.
저장 위치는 환경 변수 `BYTESIEVE_HISTORY_DIR`, 없으면 `%LOCALAPPDATA%\ByteSieve\history`입니다.

## 파일 삭제 안전성

정리 후보 탐지 규칙과 휴지통 이동 보호 조건은 [cleanup.md](cleanup.md)에 있습니다.
