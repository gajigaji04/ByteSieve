# ByteSieve

Windows용 디스크 사용량 분석·정리 도구입니다. C11과 Win32 API만으로 작성했으며 외부 라이브러리 의존성이 없습니다.
CLI(`disk-scanner`)와 Win32 GUI(`bytesieve-gui`)를 제공합니다.

## 주요 기능

| 기능 | 내용 |
|---|---|
| 폴더 스캔 | 파일 수·폴더 수·총 용량 집계, 폴더 트리와 용량 순위 |
| 디스크 사용량 분석 | 논리 크기와 실제 할당 크기(size on disk)의 차이를 클러스터 낭비·NTFS 압축·스파스·클라우드 전용으로 분류 |
| 폴더별 용량 변화 추적 | 스냅숏을 저장해 다음 스캔 때 늘어남·줄어듦·새 폴더·삭제된 폴더를 비교 |
| 정리 후보 탐지 | `node_modules`·빌드 결과물·개발 캐시·임시·로그를 개발 환경(Node.js·Python·Rust·Java·.NET·C/C++·Go)별로 분류 |
| 안전한 정리 | 고르고 확인한 항목만 **Windows 휴지통으로** 이동(영구 삭제 없음), 시스템·중요 폴더 보호 |
| 격리 폴더 | 휴지통이 없는 드라이브(USB 등)는 같은 드라이브의 격리 폴더(`.bytesieve-trash`)로 옮기고, GUI에서 복원(덮어쓰기 없음)하거나 두 번 확인한 뒤 영구 삭제 |
| 드라이브·시스템 | 드라이브별 사용률, RAM·CPU 사용률 1초 갱신과 60초 그래프 |
| 병렬 스캔 | 폴더 목록을 여러 스레드로 미리 읽어 `C:\Windows` 스캔을 16.4초 → 3.2초로 단축 |
| JSON Lines 출력 | 다른 도구가 읽을 수 있는 UTF-8 이벤트 스트림(`--jsonl`) |

## 빌드

**요구 사항**: Windows 10 이상, CMake 3.21 이상, Visual Studio(MSVC) 빌드 도구

```powershell
cmake -S . -B build
cmake --build build --config Debug      # 릴리스는 --config Release
```

결과물은 `build\Debug\` 아래의 `disk-scanner.exe`, `bytesieve-gui.exe`, `bytesieve-bench.exe`입니다.

## 빠른 시작

```powershell
# CLI: 요약 출력
build\Debug\disk-scanner.exe C:\Users\me\Projects

# CLI: 직전 --track 스캔과 폴더별 용량 비교, JSON Lines 출력
build\Debug\disk-scanner.exe --track C:\Users\me\Projects
build\Debug\disk-scanner.exe --jsonl C:\Users\me\Projects > scan.jsonl

# GUI: 폴더를 지정하면 바로 스캔
build\Debug\bytesieve-gui.exe C:\Users\me\Projects
```

```
Total size:     12.21 KiB (12503 bytes)
On disk:        12.50 KiB (12800 bytes)
  Slack:        +297 bytes (cluster size 4.00 KiB)
Status:         complete (all directories scanned)
```

GUI는 **폴더 스캔 / 드라이브 / 정리 후보 / 격리 폴더 / 시스템** 다섯 탭으로 나뉩니다. 정리 후보 탭에서 [탐지] → 항목 체크 →
[휴지통으로 이동...] → 확인 창에서 [예] 순서로 정리하며, 옮긴 항목은 Windows 휴지통에서 복원할 수 있습니다.

## 테스트

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

단위 테스트 15개, CLI 검사 24개, GUI 자체 검사 10개(총 49개)가 등록되어 있습니다.

## 문서

| 문서 | 내용 |
|---|---|
| [docs/cli.md](docs/cli.md) | CLI 옵션 전체, 종료 코드, 텍스트·`--track`·JSON Lines 출력 예 |
| [docs/gui.md](docs/gui.md) | 탭별 사용법, 스크린샷 안내 |
| [docs/cleanup.md](docs/cleanup.md) | 정리 후보 탐지 규칙, 개발 환경 분류, 휴지통 이동 보호 조건 |
| [docs/architecture.md](docs/architecture.md) | 기술 스택, 모듈 구조, 오류·취소 처리, 병렬 미리 열거, 디스크 분석·변화 추적 설계 |
| [docs/performance.md](docs/performance.md) | `bytesieve-bench` 사용법, 측정 결과, 병목 분석 |
| [docs/testing.md](docs/testing.md) | 테스트 목록과 범위, GUI 자체 검사 옵션 |

## 디렉터리 구조

```
ByteSieve/
├── CMakeLists.txt     빌드·테스트 정의
├── docs/              상세 문서
├── src/
│   ├── scanner, scan_model, scan_job, scan_history   폴더 열거·트리 모델·스캔 스레드·변화 추적
│   ├── cleanup_rules, cleanup_scan, cleanup_job, cleanup_trash   정리 후보 탐지·휴지통 이동
│   ├── quarantine                                    격리 폴더(이동·기록·복원·영구 삭제)
│   ├── drive, sysmon, path, size_format              드라이브·RAM/CPU 측정·경로·용량 표시
│   ├── cli, jsonl, main.c                            CLI(disk-scanner)
│   ├── gui_main.c, gui_drives, gui_cleanup, gui_quarantine, gui_monitor   GUI(bytesieve-gui)
│   └── bench.c                                       성능 측정 도구(bytesieve-bench)
└── tests/             단위 테스트(test_*.c), CLI·GUI 검사 스크립트(*.cmake)
```

## 구현 상태와 향후 계획

CLI 스캐너, JSON Lines 출력, GUI 다섯 탭, 정리 후보 탐지·휴지통 이동, 휴지통 없는 드라이브의 격리 폴더(복원·영구 삭제), 디스크 사용량 분석, 폴더별 변화 추적, 병렬 스캔이 구현되어 있습니다.

### 향후 개선 계획(미구현)

아래 항목은 아직 구현되지 않았습니다.

**기능**
- 트리맵 시각화: 폴더별 용량을 사각형 크기로 표현
- 결과 내보내기: CSV·HTML 형식 지원
- 공용 개발 캐시 탐지: 프로젝트 밖의 npm·pip·Gradle·Cargo·NuGet 전역 캐시 탐지
- 스캔 기록 관리: GUI의 스냅숏 자동 저장을 끄는 설정, 저장된 기록 삭제
- BitLocker 등 볼륨 암호화 정보 표시

**정확도·성능**
- 하드 링크 중복 집계 보정: 선택해서 켜는 정밀 스캔 모드(지금은 링크마다 용량을 셈)
- 병렬 스캔 메모리 최적화: 불필요한 작업 생성을 제한해 최대 메모리 사용량 절감

**UI·버그 수정**
- 조회 완료 표시 개선: 조회 후 사라지는 상단 로딩 바를 고치고, 완료·실패 상태를 명확하게 표시
- `gui_trash` 테스트 실패 원인 분석: 실패 재현과 원인 확인

**배포·문서**
- 설치 프로그램 배포: 설치형 exe 제공(설치·제거, 시작 메뉴 바로 가기)
- GUI 스크린샷 추가: 개인정보가 드러나지 않는 예제 화면(`docs/screenshots/`)

## 라이선스

[MIT License](LICENSE) © 2026 gajigaji04
