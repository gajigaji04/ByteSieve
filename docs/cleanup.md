# 정리 후보 탐지와 안전한 정리

## 탐지 규칙

| 유형 | 폴더 | 파일 |
|---|---|---|
| Node.js | `node_modules` | – |
| 빌드 결과물 | `dist`, `build`, `out`, `cmake-build-*`, `target`\*, `bin`\*, `obj`\* | – |
| 개발 캐시 | `.cache`, `.next`, `.nuxt`, `.vite`, `.parcel-cache`, `.turbo`, `.svelte-kit`, `.angular`, `__pycache__`, `.pytest_cache`, `.mypy_cache`, `.ruff_cache`, `.tox`, `.nox`, `.hypothesis`, `.gradle`\*, `.vs`\* | – |
| 임시 파일 | `.tmp`, `.temp` | `*.tmp` |
| 로그 | – | `*.log` |

\* 같은 폴더에 해당 프로젝트 표시 파일이 있을 때만 후보입니다(`target`: `Cargo.toml`·`pom.xml`, `bin`·`obj`·`.vs`: `*.csproj`·`*.fsproj`·`*.vbproj`·`*.sln`,
`.gradle`: Gradle·Maven 파일). 그래서 사용자 폴더의 `bin`이나 홈의 `.gradle`(설정·인증 정보 포함)은 후보가 되지 않습니다.

개발 환경은 폴더 이름(예: `__pycache__` → Python)이나 같은 폴더의 표시 파일(`package.json`, `pyproject.toml`·`requirements.txt`,
`Cargo.toml`, `pom.xml`·`build.gradle`, `*.csproj`·`*.sln`, `CMakeLists.txt`·`Makefile`, `go.mod`)로 정하고, 정할 수 없으면 '일반'입니다.
`node_modules`와 빌드 결과물은 표시 파일이 있으면 **확인됨**, 없으면 **추정**으로 표시합니다.
먼저 맞은 폴더가 그 아래 전체를 차지하므로(`node_modules\pkg\dist`는 따로 나오지 않음) 항목 용량의 합에 중복이 없습니다.

## 파일 삭제 안전성

- 탐지는 이름·크기·속성만 읽고 파일을 열거나 수정하지 않습니다. 영구 삭제 코드 경로는 없습니다.
- 휴지통 이동은 `IFileOperation`(`FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE`)으로 항목마다 실행합니다. 셸이 영구 삭제로 바꾸려 하면
  (휴지통 용량 초과 등) 진행 싱크에서 거부합니다.
- 다음 중 하나라도 해당하면 그 항목은 건드리지 않습니다.

| 시점 | 거부 조건 |
|---|---|
| 경로 판정 | UNC·상대 경로, 260자 이상, 탐지 경로 밖이거나 탐지 경로 자체 |
| | Windows·`Program Files`·`ProgramData`, 볼륨 루트의 `$Recycle.Bin`·`System Volume Information` 등과 같거나 그 아래 |
| | 볼륨 루트, `Users`, 사용자·공용 프로필, 바탕 화면·문서·다운로드 등 알려진 폴더, `AppData`, 실행 중인 프로그램 폴더, 그리고 이들의 상위 |
| | 하위를 다 읽지 못한 항목(접근 거부·취소로 일부 누락) |
| 옮기기 직전 | 고정 디스크가 아님(이동식·네트워크 드라이브), 이미 없음 |
| | 탐지 때와 종류가 다름, 링크로 바뀜, `GetFinalPathNameByHandleW`로 구한 실제 위치가 표시 경로와 다름(중간 폴더가 정션으로 바뀐 경우 등) |
