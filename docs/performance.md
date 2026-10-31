# 성능 측정

`bytesieve-bench`로 실행 시간·CPU 시간·최대 메모리를 측정합니다.

```powershell
cmake --build build --config Release --target bytesieve-bench
build\Release\bytesieve-bench.exe --mode model --runs 3 C:\Windows     # --mode tree|model|cleanup, --threads N
```

최대 메모리는 프로세스 전체 기준이므로 모드마다 따로 실행하고, 첫 실행은 디스크 캐시가 차지 않았을 수 있어 두 번째 실행부터 봅니다.

## 측정 결과

Windows 11, 논리 프로세서 12개, `C:\Windows` 파일 21만·폴더 16만, 디스크 캐시가 찬 상태, Release:

| 모드 | 개선 전 | 개선 후(자동, 8 스레드) | 개선 후(`--threads 1`) |
|---|---|---|---|
| tree(집계만) | 16.4 s · 최대 12 MiB | **3.2 s** · 55 MiB | 12.6 s · 36 MiB |
| model(GUI 트리 구성) | 16.4 s · 64 MiB | **3.1 s** · 98 MiB | 12.6 s · 73 MiB |
| cleanup(정리 후보 탐지) | 16.6 s · 12 MiB | **3.3 s** · 57 MiB | 13.0 s · 36 MiB |

## 병목 분석과 개선

- 개선 전 CPU 시간의 97%가 커널의 폴더 열기·열거였습니다. 열거 API를 `FindFirstFileW`에서 폴더 핸들 + `GetFileInformationByHandleEx`로
  바꿔 한 스레드 기준 16.3초 → 12.4초로 줄였고(할당 크기도 함께 얻음), 병렬 미리 열거로 3초대가 되었습니다.
- 대가로 최대 메모리가 늘었습니다. 메모리가 중요하면 `--threads 1`을 쓰세요.
- 모델은 폴더 이름을 하나의 문자열 저장소에 담아 해제 시간이 10ms → 3ms로 줄었습니다.

병렬 미리 열거의 동작 방식은 [architecture.md](architecture.md#병렬-미리-열거)를 참고하세요.
