# 테스트

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

CTest에 등록된 테스트는 57개입니다.

| 분류 | 테스트 | 확인 내용 |
|---|---|---|
| 단위 테스트(19개) | `path`, `scanner`, `scan_alloc`, `scan_parallel`, `hardlink`, `scan_model`, `scan_history`, `export`, `scan_job`, `jsonl`, `cli`, `drive`, `sysmon`, `size_format`, `cleanup_rules`, `cleanup_scan`, `cleanup_trash`, `dev_cache`, `quarantine` | 아래 참고 |
| CLI·도구 검사(25개) | `no_args`, `help`, `scan_*`, `cli_*`, `bench_smoke` | 종료 코드, 텍스트·JSONL 출력, 인자 오류, 접근 거부 픽스처, `--track` 기준 → 변경 → 비교(`cli_track`), 성능 측정 도구의 스캐너 진단 출력(`bench_smoke`) |
| GUI 자체 검사(13개) | `gui_scan`, `gui_cancel`, `gui_missing_dir`, `gui_drives`, `gui_cleanup`, `gui_cleanup_cancel`, `gui_trash`, `gui_quarantine`, `gui_history`, `gui_precise`, `gui_export`, `gui_cache`, `gui_monitor` | 숨긴 창에서 실제 작업을 실행하고 화면 상태를 검증 |

## 주요 단위 테스트 범위

- `scanner`: 권한 거부, 정션과 정션 순환, 260자를 넘는 경로, 5 GiB 넘는 크기, 깊이 제한, 취소, 제외 훅
- `scan_alloc`: 디스크 사용량 원인 분류, 실제 NTFS 압축·스파스 파일, 작업 스레드 1개와 8개의 훅 호출 기록이 같은지(제외·취소 포함)
- `scan_parallel`: 하위 폴더 2,500개 픽스처에서 동시에 살아 있는 폴더 작업 수가 상한 안인지(예전 방식은 2,500개를 넘음, 1 스레드는 깊이만큼),
  작업 스레드 1개·8개의 집계·훅 호출(순서 포함)이 같은지, 직접 열거 + 미리 열거 − 버림 = 처리한 폴더 수(빠뜨리거나 두 번 쓰지 않음),
  제외 훅이 미리 열거한 하위 트리를 정리하는지, 취소 뒤 폴더를 새로 시작하지 않고 남은 작업을 버리는지.
  최적화 전후의 시간·메모리 비교는 `bytesieve-bench`로 합니다([performance.md](performance.md))
- `hardlink`: 실제 하드 링크 픽스처(같은 폴더·다른 폴더·크기 0 파일)로 기본 스캔은 그대로(중복 0), 정밀 스캔의 중복 경로 수·용량·할당,
  실제 파일마다 한 경로만 세는지, 작업 스레드 1개와 8개의 같은 결과, 모델의 폴더별 중복·부모 합계·순위(중복 제외·중복)·진행률
- `scan_history`: 스냅숏 저장·읽기, 늘어남·줄어듦·새 폴더·삭제·불확실 판정, 손상된 파일,
  자동 저장 설정(기본값·저장 후 다시 읽기·손상된 설정·쓰기 실패), 기록 삭제(형식에 맞는 스냅숏만 삭제, 사용 중인 파일이 있어도 나머지 계속, 설정·다른 파일 보존)
- `export`: CSV(UTF-8 BOM, CRLF, 머리글·행 수, 크기 내림차순, 쉼표가 든 이름, `=`로 시작하는 이름의 수식 방지, 64 KiB 쓰기 버퍼를 넘는 큰 결과),
  HTML(구역, 이름 이스케이프, 순위 표 100행 제한, 통계), 실패(없는 폴더, 빈·실패한 스캔, 다른 프로그램이 연 파일에서 기존 내용 보존·임시 파일 없음)
- `cleanup_scan`·`cleanup_rules`: 중첩 항목, 확인됨/추정, 개발 환경 분류, 표시 파일이 없는 `bin`·`.gradle`, 보호 경로, 필터, 취소
- `cleanup_trash`: 보호 판정 규칙과 **실제 휴지통 이동·거부**(테스트가 넣은 휴지통 항목만 지워 사용자 휴지통은 그대로 둠)
- `dev_cache`: `USERPROFILE`·`LOCALAPPDATA`·`APPDATA`를 임시 픽스처로 돌리고 도구 환경 변수를 지워 실제 캐시를 보지 않는다.
  기본 위치(없음·있음, NuGet `v3-cache`·`http-cache` 둘 다), 용량·파일 수, 도구 폴더 자체·`.cargo\bin`·`gradle.properties` 제외,
  환경 변수·`.npmrc`(`${VAR}`)·`pip.ini`(구역·상대 경로)·`NuGet.Config`(상대 경로), 캐시 구조가 아닌 폴더·파일·정션·바꾸지 못한 변수,
  세기 전·취소 뒤 정리 거부, 중요 경로 거부, 항목별 탐지 경로로 판정하는 휴지통 작업(밖·없음), 작업 스레드·취소
- `quarantine`: 임시 폴더를 가짜 볼륨으로 써서 격리 이동·목록, 같은 이름 여러 번 격리, 이동 실패(사용 중·없음·다른 볼륨)
  뒤 원래 항목 보존, 격리 폴더가 정션일 때 거부, 복원(같은 이름은 덮어쓰지 않음, 다른 이름 복원, 없어진 상위 폴더,
  드라이브 문자 변경), 기록 불일치(파일 없음·기록 손상·`moving` 상태·볼륨 밖 상대 경로·사용자가 넣은 파일)에서 파일 보존,
  영구 삭제(읽기 전용 파일, 안의 정션은 대상 보존, 격리 폴더 밖을 가리키게 꾸민 항목 거부), 정리·탐지·폴더 스캔 연동
- `drive`·`sysmon`: 가짜 조회 함수로 이동식 드라이브 오류·API 실패·이상값 재현

## GUI 자체 검사 옵션

직접 실행할 수도 있습니다(종료 코드 0 성공, 1 실패).

```powershell
build\Debug\bytesieve-gui.exe --self-test C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cancel C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-changes C:\some\dir   # 직전 스냅숏 필요, 변화 목록을 stdout에 출력
build\Debug\bytesieve-gui.exe --self-test-precise C:\some\dir   # 정밀 스캔, 기본·중복 제외 집계를 stdout에 출력
build\Debug\bytesieve-gui.exe --self-test-no-autosave C:\some\dir    # 주의: 자동 저장 설정을 끔(처음 설정을 stdout에 출력)
build\Debug\bytesieve-gui.exe --self-test-clear-history C:\some\dir  # 주의: 저장된 기록을 확인 창 없이 모두 삭제
build\Debug\bytesieve-gui.exe --self-test-export C:\some\dir C:\out\result   # result.csv·result.html 저장
build\Debug\bytesieve-gui.exe --self-test-drives
build\Debug\bytesieve-gui.exe --self-test-cleanup C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cleanup-cancel C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-trash C:\some\dir     # 주의: 옮길 수 있는 항목을 모두 휴지통으로 옮김
$env:BYTESIEVE_QUARANTINE_ROOT = "C:\some\dir"                # 이 폴더 안을 휴지통 없는 드라이브로 다룸
build\Debug\bytesieve-gui.exe --self-test-quarantine C:\some\dir C:\some\dir\app\server.log
                                                                 # 주의: 격리 → 복원 → 남은 항목 영구 삭제
$env:USERPROFILE = "C:\fx\home"; $env:LOCALAPPDATA = "C:\fx\local"; $env:APPDATA = "C:\fx\roaming"
build\Debug\bytesieve-gui.exe --self-test-cache C:\fx             # 주의: 찾은 캐시를 확인 창 없이 휴지통으로
build\Debug\bytesieve-gui.exe --self-test-monitor
```

`--self-test-cache`는 찾은 캐시가 **하나라도** 지정한 폴더 밖이면(실제 사용자 캐시를 볼 수 있으므로) 아무것도 옮기지 않고 실패합니다.
사용자 폴더 환경 변수를 테스트 폴더로 돌리고 `npm_config_cache`·`PIP_CACHE_DIR`·`GRADLE_USER_HOME`·`CARGO_HOME`·`NUGET_PACKAGES` 등을 지운 뒤
실행하세요(`gui_cache`가 그렇게 합니다).

`--self-test-trash`는 확인 창 없이 실제로 휴지통으로 옮기므로 테스트용 폴더에만 쓰세요. `--self-test-quarantine`도 확인 창 없이
옮기고 영구 삭제하므로 테스트용 폴더에만 쓰세요(격리 폴더 탭은 이 폴더만 보여 주어 사용자의 실제 격리 항목은 건드리지 않습니다).
`BYTESIEVE_QUARANTINE_ROOT`는 테스트용 환경 변수로, 지정한 폴더 안의 항목을 휴지통이 없는 드라이브처럼 그 폴더의 격리 폴더로 옮깁니다. 폴더 스캔 자체 검사도 스냅숏을 저장하므로,
CTest는 `BYTESIEVE_HISTORY_DIR`을 빌드 폴더로 지정해 사용자 기록 폴더를 건드리지 않습니다.
`--self-test-no-autosave`·`--self-test-clear-history`는 기록 폴더의 설정·기록을 바꾸므로 `BYTESIEVE_HISTORY_DIR`을 테스트용 폴더로 지정해 실행하세요.
`gui_history`는 기준 스캔 → 변화 비교 → 자동 저장 끄기(스냅숏 그대로, 재실행 뒤에도 꺼짐) → 기록 삭제(스냅숏과 남은 임시 파일만 지우고
설정·다른 파일 보존, 변화 열·변화 순위·분석 줄·버튼에 반영) 순으로 확인합니다.
