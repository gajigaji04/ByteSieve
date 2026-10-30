# CLI 사용법 (`disk-scanner`)

```
disk-scanner [--max-depth N] [--jsonl] [--track [--history-dir DIR]] [--threads N] <directory>
disk-scanner -h | --help
```

| 옵션 | 설명 |
|---|---|
| `--max-depth N` | 대상 폴더 아래 N단계까지만 탐색(0 = 대상 폴더만, 기본: 제한 없음) |
| `--jsonl` | 표준 출력에 UTF-8 JSON Lines 이벤트 출력 |
| `--track` | 직전 `--track` 스캔과 폴더별 용량을 비교하고 새 스냅숏 저장 |
| `--history-dir DIR` | 스냅숏 폴더(`--track`과 함께만 사용). 기본: 환경 변수 `BYTESIEVE_HISTORY_DIR`, 없으면 `%LOCALAPPDATA%\ByteSieve\history` |
| `--threads N` | 폴더 열거 작업 스레드 수(0 = 자동, 1 = 병렬 없음, 최대 8) |
| `-h`, `--help` | 도움말(다른 인자보다 우선) |
| `--` | 다음 인자를 대상 폴더로 취급 |

옵션과 대상의 순서는 상관없고 `--max-depth=2`처럼 `=` 형식도 받습니다.
**종료 코드**: `0` 모든 폴더를 읽음, `1` 일부 폴더를 읽지 못함, `2` 대상 폴더를 스캔할 수 없거나 인자 오류.

```powershell
build\Debug\disk-scanner.exe C:\Users\me\Projects
build\Debug\disk-scanner.exe --max-depth 0 C:\Users\me\Projects
build\Debug\disk-scanner.exe --jsonl C:\Users\me\Projects > scan.jsonl
build\Debug\disk-scanner.exe --track C:\Users\me\Projects
build\Debug\disk-scanner.exe --threads 1 C:\Users\me\Projects   # 병렬 없이(메모리를 덜 씀)
```

## 텍스트 출력

```
Target:         readme_demo
Max depth:      unlimited
Files:          3
Directories:    4
Total size:     12.21 KiB (12503 bytes)
On disk:        12.50 KiB (12800 bytes)
  Slack:        +297 bytes (cluster size 4.00 KiB)
Reparse points: 0 (not descended)
Errors:         0
Skipped dirs:   0
Aborted:        no
Status:         complete (all directories scanned)
```

`On disk:` 아래에는 0이 아닌 원인(`Slack`·`Compressed`·`Sparse`·`Cloud-only`·`Other`·`Unknown`)만 표시합니다.
오류가 있으면 표준 오류에 경로·종류·Windows 오류 코드를 최대 20개까지 나열합니다.

## 변화 추적 (`--track`)

두 번째 스캔부터 직전 스캔 대비 변화를 보여 줍니다(첫 스캔은 `Changes: no previous snapshot (baseline)`).
변화량이 큰 폴더를 최대 20개 나열하며, 기록 저장 실패는 경고만 하고 종료 코드에는 영향을 주지 않습니다.

```
Changes since 2026-10-31 02:37: -8.01 KiB (12.2 KiB -> 4.20 KiB)
  grown 1, shrunk 1, added 1, removed 1
     -11.7 KiB  shrunk   app
     -11.7 KiB  removed  app\node_modules
     +2.93 KiB  grown    logs
        +800 B  added    dist
History:        saved to C:\Users\me\AppData\Local\ByteSieve\history\6f56c5d88fb9907f.snap
```

## JSON Lines 출력 (`--jsonl`)

이벤트 순서는 `start` → `progress`(1,000개 항목마다) → `result` → `complete`입니다.

```json
{"event":"start","target":"readme_demo"}
{"event":"result","target":"readme_demo","files":3,"dirs":4,"bytes":12503,"alloc_bytes":12800,"disk":{"slack":297,"compressed_saved":0,"sparse_saved":0,"cloud_saved":0,"other_saved":0,"unknown_files":0},"reparse_points":0,"error_count":0,"errors":[]}
{"event":"complete","status":"complete","exit_code":0}
```

`--jsonl --track`이면 `progress` 대신 `complete` 앞에 `changes` 이벤트가 붙습니다(`items`는 최대 100개, 나머지는 `more`).

```json
{"event":"changes","previous":true,"since":134378554553163683,"old_bytes":4303,"new_bytes":4303,"items":[],"more":0,"saved_to":"C:\\Users\\me\\AppData\\Local\\ByteSieve\\history\\6f56c5d88fb9907f.snap"}
```

`items`의 각 항목은 `{"path","kind":"grown|shrunk|added|removed","old","new","delta","uncertain"}`이고, `since`는 직전 스캔 시각(FILETIME, UTC)입니다.
