# 아키텍처와 설계

## 기술 스택

- **언어·빌드**: C11, CMake 3.21+, MSVC(`/W4 /utf-8`), 유니코드(`UNICODE`) 빌드
- **Windows API**: 폴더 열거 `GetFileInformationByHandleEx(FileFullDirectoryInfo)`(미지원 파일 시스템은 `FindFirstFileExW`),
  긴 경로 `\\?\`, 휴지통 이동 `IFileOperation`, 격리 폴더 이동·복원 `MoveFileExW`(같은 볼륨, 덮어쓰기 없음), 드라이브 `GetLogicalDrives`·`GetDiskFreeSpaceExW`,
  메모리·CPU `GlobalMemoryStatusEx`·`GetSystemTimes`, GUI는 Win32 + Common Controls v6
- **구조**: UI와 무관한 정적 라이브러리 위에 CLI·GUI를 얹었습니다. 무거운 작업(스캔·탐지·휴지통 이동·측정)은 모두 작업 스레드에서
  실행하고, GUI 스레드는 `PostMessage`로 결과만 받아 화면을 갱신합니다.

```
                    disk-scanner (CLI)                bytesieve-gui (GUI)
                     │  cli · jsonl                     │  폴더 스캔 │ 드라이브 │ 정리 후보 │ 공용 캐시 │ 격리 폴더 │ 시스템
                     │                                  │
   scan_history ── scan_model ── scan_job        cleanup_job ── cleanup_trash ── dev_cache   drive   sysmon
   (변화 추적)     (폴더 트리)   (스캔 스레드)   (탐지 스레드)   (휴지통 이동)    (공용 캐시)  (드라이브) (RAM·CPU)
                                                                        │
                                                                    quarantine
                                                                    (격리 폴더)
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
| `bytesieve_export` | 스캔 결과 CSV·HTML 보고서 내보내기(임시 파일 → 바꿔치기, 64 KiB 버퍼로 나눠 쓰기) |
| `bytesieve_cleanup_rules` / `bytesieve_cleanup` / `bytesieve_cleanup_job` | 정리 후보 규칙, 탐지 엔진, 탐지 스레드 |
| `bytesieve_cleanup_trash` | 삭제 보호 판정, 직전 확인, 휴지통 이동(쓸 수 없으면 격리 폴더로). 항목마다 탐지 경로를 따로 받는 작업도 지원 |
| `bytesieve_dev_cache` | 공용 개발 캐시(npm·pip·Gradle·Cargo·NuGet) 위치 탐지(환경 변수·설정 파일·기본 위치, 캐시 구조 확인), 용량 세기, 작업 스레드 |
| `bytesieve_quarantine` | 드라이브별 격리 폴더: 같은 볼륨 안 이동, 기록(원자적 쓰기), 목록·불일치 판정, 복원, 영구 삭제 |
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

## 정밀 스캔(하드 링크 중복 보정)

`scan_tree_opts(..., SCAN_OPT_HARDLINKS)`(모델은 `scan_model_build_opts`, 작업은 `scan_job_start_ex`)로 켭니다.

- 열거 정보를 `FileFullDirectoryInfo` 대신 `FileIdExtdDirectoryInfo`(128비트 파일 ID)로 받고, 안 되면 `FileIdBothDirectoryInfo`(64비트),
  그것도 안 되면 ID 없이 받습니다. 볼륨 일련번호는 폴더 핸들의 `FileIdInfo`로 구합니다. 파일을 열지 않으므로 추가 I/O가 거의 없습니다.
- 파일 ID는 정밀 스캔일 때만 열거 기록(`EntryRec`)에 16바이트로 붙어, 기본 스캔의 메모리·속도는 그대로입니다.
- 같은 실제 파일인지는 호출 스레드가 훅 순서대로 (볼륨, 파일 ID) 집합을 확인해 정하므로, 병렬 미리 열거와 상관없이 같은 경로가 "처음"이 됩니다.
  처음이 아닌 경로는 `ScanEntry.hardlink_dup`로 알리고 `ScanReport.hardlink_dup_*`, `ScanNode.*_dup_*`에 따로 더합니다.
  경로마다 센 기본 값(`total_bytes`·`alloc_bytes` 등)은 바꾸지 않으므로 기본 스캔과 정밀 스캔의 차이를 한 번의 정밀 스캔으로 보여 줄 수 있습니다.
- 탐색 시작 볼륨이 `FILE_SUPPORTS_HARD_LINKS`가 아니면(FAT·exFAT) ID를 모으지 않습니다(FAT의 파일 ID는 믿을 수 없음). ID가 0이거나 없는 파일과,
  집합을 늘릴 메모리가 없을 때의 파일은 `hardlink_unknown_files`로 세고 경로마다 셉니다.
- 예: `C:\Windows`(파일 21만 개)는 경로마다 35.5 GB이지만 그중 5만 7천여 경로·10.9 GB가 하드 링크 중복이라 중복 제외 용량은 24.6 GB입니다.

## 변화 추적

폴더 트리를 깊이 4까지 저장하며(그 아래 변화는 조상 폴더 용량에 반영), 새 폴더·삭제된 폴더 안의 하위 폴더는 따로 나열하지 않습니다.
스냅숏은 임시 파일에 쓴 뒤 바꿔치기하고, 대상 폴더마다 직전 스냅숏 하나만 남깁니다.
저장 위치는 환경 변수 `BYTESIEVE_HISTORY_DIR`, 없으면 `%LOCALAPPDATA%\ByteSieve\history`입니다.
같은 폴더의 `settings.txt`에 GUI 자동 저장 설정을 둡니다(임시 파일에 쓴 뒤 바꿔치기, 없거나 손상되면 켬). CLI `--track`은 명시적 요청이므로 이 설정을 보지 않습니다.
기록 삭제는 `<16자리 16진수>.snap`(과 남은 `.snap.tmp`) 형식의 일반 파일만 지우고, 하나가 실패해도 나머지를 계속 지운 뒤 지운·실패 개수를 알려 줍니다.

## 파일 삭제 안전성

정리 후보 탐지 규칙과 휴지통 이동 보호 조건은 [cleanup.md](cleanup.md)에 있습니다.
