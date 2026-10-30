# 테스트

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

CTest에 등록된 테스트는 47개입니다.

| 분류 | 테스트 | 확인 내용 |
|---|---|---|
| 단위 테스트(14개) | `path`, `scanner`, `scan_alloc`, `scan_model`, `scan_history`, `scan_job`, `jsonl`, `cli`, `drive`, `sysmon`, `size_format`, `cleanup_rules`, `cleanup_scan`, `cleanup_trash` | 아래 참고 |
| CLI 검사(24개) | `no_args`, `help`, `scan_*`, `cli_*` | 종료 코드, 텍스트·JSONL 출력, 인자 오류, 접근 거부 픽스처, `--track` 기준 → 변경 → 비교(`cli_track`) |
| GUI 자체 검사(9개) | `gui_scan`, `gui_cancel`, `gui_missing_dir`, `gui_drives`, `gui_cleanup`, `gui_cleanup_cancel`, `gui_trash`, `gui_history`, `gui_monitor` | 숨긴 창에서 실제 작업을 실행하고 화면 상태를 검증 |

## 주요 단위 테스트 범위

- `scanner`: 권한 거부, 정션과 정션 순환, 260자를 넘는 경로, 5 GiB 넘는 크기, 깊이 제한, 취소, 제외 훅
- `scan_alloc`: 디스크 사용량 원인 분류, 실제 NTFS 압축·스파스 파일, 작업 스레드 1개와 8개의 훅 호출 기록이 같은지(제외·취소 포함)
- `scan_history`: 스냅숏 저장·읽기, 늘어남·줄어듦·새 폴더·삭제·불확실 판정, 손상된 파일
- `cleanup_scan`·`cleanup_rules`: 중첩 항목, 확인됨/추정, 개발 환경 분류, 표시 파일이 없는 `bin`·`.gradle`, 보호 경로, 필터, 취소
- `cleanup_trash`: 보호 판정 규칙과 **실제 휴지통 이동·거부**(테스트가 넣은 휴지통 항목만 지워 사용자 휴지통은 그대로 둠)
- `drive`·`sysmon`: 가짜 조회 함수로 이동식 드라이브 오류·API 실패·이상값 재현

## GUI 자체 검사 옵션

직접 실행할 수도 있습니다(종료 코드 0 성공, 1 실패).

```powershell
build\Debug\bytesieve-gui.exe --self-test C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cancel C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-changes C:\some\dir   # 직전 스냅숏 필요, 변화 목록을 stdout에 출력
build\Debug\bytesieve-gui.exe --self-test-drives
build\Debug\bytesieve-gui.exe --self-test-cleanup C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-cleanup-cancel C:\some\dir
build\Debug\bytesieve-gui.exe --self-test-trash C:\some\dir     # 주의: 옮길 수 있는 항목을 모두 휴지통으로 옮김
build\Debug\bytesieve-gui.exe --self-test-monitor
```

`--self-test-trash`는 확인 창 없이 실제로 휴지통으로 옮기므로 테스트용 폴더에만 쓰세요. 폴더 스캔 자체 검사도 스냅숏을 저장하므로,
CTest는 `BYTESIEVE_HISTORY_DIR`을 빌드 폴더로 지정해 사용자 기록 폴더를 건드리지 않습니다.
