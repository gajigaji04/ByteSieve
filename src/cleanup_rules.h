#ifndef BYTESIEVE_CLEANUP_RULES_H
#define BYTESIEVE_CLEANUP_RULES_H

#include <stddef.h>
#include <wchar.h>

/*
 * 정리 후보 탐지 규칙. 이름과 경로 문자열만 보며 파일 시스템에 접근하지 않는다.
 * 이름 비교는 Windows 파일 이름 규칙대로 대소문자를 구분하지 않는다.
 */

typedef enum {
    CLEANUP_NONE = 0,
    CLEANUP_NODE_MODULES, /* node_modules */
    CLEANUP_BUILD,        /* dist, build, out, target, bin, obj, cmake-build-* */
    CLEANUP_CACHE,        /* .cache, .next, .vite, __pycache__, .gradle, .vs 등 */
    CLEANUP_TEMP,         /* .tmp, .temp 폴더, *.tmp 파일 */
    CLEANUP_LOG,          /* *.log 파일 */
    CLEANUP_CATEGORY_COUNT
} CleanupCategory;

/* 정리 후보가 속한 개발 환경. */
typedef enum {
    CLEANUP_ECO_GENERIC = 0, /* 특정 환경으로 정할 수 없음(임시·로그, 표시 파일 없는 dist 등) */
    CLEANUP_ECO_NODE,        /* Node.js·JavaScript */
    CLEANUP_ECO_PYTHON,
    CLEANUP_ECO_RUST,
    CLEANUP_ECO_JAVA,        /* Java·Kotlin(Gradle·Maven) */
    CLEANUP_ECO_DOTNET,      /* .NET·Visual Studio */
    CLEANUP_ECO_CPP,         /* C/C++(CMake·Make) */
    CLEANUP_ECO_GO,
    CLEANUP_ECO_COUNT
} CleanupEco;

#define CLEANUP_ECO_BIT(eco) (1u << (unsigned)(eco))
#define CLEANUP_ECO_MASK_ALL ((1u << (unsigned)CLEANUP_ECO_COUNT) - 1u)

/* 폴더 이름 규칙 하나. */
typedef struct {
    CleanupCategory category;
    CleanupEco eco;   /* 이름으로 정해지는 환경. GENERIC 이면 같은 폴더의 표시 파일로 정한다 */
    unsigned require; /* 0 이 아니면 같은 폴더에 이 환경(비트)의 표시 파일이 있어야만 후보다 */
    const CleanupEco *prefer; /* eco 가 GENERIC 일 때 표시 파일로 고르는 순서(GENERIC 으로 끝남) */
} CleanupDirRule;

/* 유형 필터 비트. */
#define CLEANUP_MASK(category) (1u << (unsigned)(category))
#define CLEANUP_MASK_ALL                                                          \
    (CLEANUP_MASK(CLEANUP_NODE_MODULES) | CLEANUP_MASK(CLEANUP_BUILD) |           \
     CLEANUP_MASK(CLEANUP_CACHE) | CLEANUP_MASK(CLEANUP_TEMP) | CLEANUP_MASK(CLEANUP_LOG))

/*
 * 폴더 이름 규칙을 찾는다. 맞으면 1 이고 rule 을 채운다(NULL 허용).
 *   - 이름만으로 환경이 정해지는 폴더: node_modules·.next·.nuxt·.vite·.parcel-cache·.turbo·
 *     .svelte-kit·.angular(Node.js), __pycache__·.pytest_cache·.mypy_cache·.ruff_cache·.tox·
 *     .nox·.hypothesis(Python), cmake-build-*(C/C++)
 *   - 흔한 이름이라 표시 파일로 환경을 고르는 폴더: dist·build·out·.cache
 *   - 이름이 너무 흔하거나 설정이 섞여 있어 표시 파일이 있을 때만 후보인 폴더:
 *     target(Cargo.toml·pom.xml), bin·obj·.vs(*.csproj·*.fsproj·*.vbproj·*.sln),
 *     .gradle(Gradle·Maven 파일). 사용자 폴더의 bin, 홈의 .gradle(설정·인증 정보) 보호.
 */
int cleanup_dir_rule(const wchar_t *name, CleanupDirRule *rule);

/* 폴더 이름이 규칙에 맞으면 유형, 아니면 CLEANUP_NONE(표시 파일 조건은 보지 않는다). */
CleanupCategory cleanup_classify_dir(const wchar_t *name);

/*
 * 파일 이름이 규칙(*.tmp, *.log)에 맞으면 유형, 아니면 CLEANUP_NONE.
 * 이름이 숫자 6자리 이상뿐인 *.log("000003.log")는 LevelDB(Chrome·Edge·VS Code 등 Chromium/
 * Electron 앱의 저장소)의 쓰기 로그라 아직 합쳐지지 않은 데이터가 들어 있으므로 로그로 보지 않는다.
 */
CleanupCategory cleanup_classify_file(const wchar_t *name);

/*
 * LevelDB 데이터베이스 폴더의 표시 파일인지(CURRENT, LOCK, MANIFEST-*). 정리 후보 탐지는 이런
 * 파일이 있는 폴더의 *.log 를 후보로 삼지 않는다.
 */
int cleanup_is_leveldb_marker(const wchar_t *name);

/*
 * 이름이 흔해서 프로젝트 폴더 안에 있는지로 신뢰도를 매기는 유형인지
 * (node_modules, dist/build/out).
 */
int cleanup_needs_marker(CleanupCategory category);

/* 프로젝트 표시 파일(package.json, CMakeLists.txt, *.csproj 등)인지. */
int cleanup_is_project_marker(const wchar_t *name);

/*
 * 표시 파일이 가리키는 개발 환경 비트(CLEANUP_ECO_BIT). 표시 파일이 아니면 0.
 *   package.json(Node.js), pyproject.toml·setup.py·setup.cfg·requirements.txt·Pipfile·
 *   tox.ini(Python), Cargo.toml(Rust), pom.xml·build.gradle(.kts)·settings.gradle(.kts)(Java),
 *   *.csproj·*.fsproj·*.vbproj·*.sln(.NET), CMakeLists.txt·Makefile(C/C++), go.mod(Go)
 */
unsigned cleanup_marker_ecos(const wchar_t *name);

/*
 * 같은 폴더에서 찾은 표시 파일 비트(markers)로 폴더 항목의 환경을 정한다.
 * 이름으로 정해진 환경이 있으면 그것, 없으면 rule->prefer 순서로 처음 맞는 환경, 없으면 GENERIC.
 */
CleanupEco cleanup_resolve_eco(const CleanupDirRule *rule, unsigned markers);

/* 식별자 이름("generic", "node", "python", "rust", "java", "dotnet", "cpp", "go"). */
const wchar_t *cleanup_eco_name(CleanupEco eco);

/* 식별자 이름("node_modules", "build", "cache", "temp", "log", "none"). */
const wchar_t *cleanup_category_name(CleanupCategory category);

/*
 * 시스템·프로그램 보호 경로 목록. 탐색 중 이 경로와 같은 폴더를 만나면 들어가지 않으므로
 * 그 아래의 파일·폴더(실행 파일, 설치 폴더 안의 node_modules·__pycache__ 등)는 후보가 되지 않는다.
 * 경로는 확장 접두사 없이, 끝 구분자 없이 저장한다("C:\Windows").
 */
typedef struct {
    wchar_t **paths;
    size_t len;
    size_t cap;
} CleanupProtect;

void cleanup_protect_init(CleanupProtect *p);
void cleanup_protect_free(CleanupProtect *p);

/* 경로 하나를 추가한다(확장 접두사·끝 구분자는 정리한다). 성공 0, 실패 -1. */
int cleanup_protect_add(CleanupProtect *p, const wchar_t *path);

/*
 * 현재 시스템의 보호 경로를 추가한다:
 *   - Windows 폴더, %ProgramFiles%·%ProgramFiles(x86)%·%ProgramW6432%, %ProgramData%
 *   - %LOCALAPPDATA%\Programs(사용자별 설치 프로그램: VS Code 사용자 설치, Python 등)
 *   - %USERPROFILE%\.vscode\extensions, \.vscode-insiders\extensions(설치한 VS Code 확장)
 *   - 프로그램 제거 정보(레지스트리 Uninstall, HKLM 64·32비트와 HKCU)에 등록된 Visual Studio·
 *     VS Code 설치 경로(InstallLocation). 기본 위치가 아닌 곳(D:\VS 등)에 설치한 경우를 위한 것이며,
 *     cleanup_protect_add_install 의 조건을 통과한 경로만 넣는다.
 *   - PEP 514 로 등록된 Python 설치 경로(HKLM 64·32비트와 HKCU 의 Software\Python\<회사>\<태그>\
 *     InstallPath). Program Files 밖(C:\Python314 등)에 설치한 경우를 위한 것이며 같은 조건을 거친다.
 *     테스트용으로 환경 변수 BYTESIEVE_TEST_PYTHON_KEY 가 있으면 HKCU 의 그 키도 같은 구조로 읽는다.
 * 찾지 못한 항목은 건너뛴다. 추가한 개수를 반환한다.
 */
size_t cleanup_protect_add_system(CleanupProtect *p);

/*
 * 정리 후보 탐지에서 들어가지 않을 앱 데이터 폴더를 추가한다: %LOCALAPPDATA%·%APPDATA%·
 * %USERPROFILE%\AppData\LocalLow 바로 아래의 앱별 폴더(브라우저·편집기의 데이터베이스·설정이
 * 있어 이름이 *.log 여도 데이터일 수 있음). 각 폴더의 Temp 와 이미 목록에 있는 경로는 넣지 않는다.
 * 지금 있는 하위 폴더를 읽어 넣으므로 탐지 직전에 부른다. 정리 후보 탐지 전용이며, 공용 캐시
 * (npm·pip 캐시가 %LOCALAPPDATA% 아래에 있음)는 이 목록을 쓰지 않는다. 추가한 개수를 반환한다.
 */
size_t cleanup_protect_add_app_data(CleanupProtect *p);

/*
 * 정리 후보 탐지에서 들어가지 않을 다른 사용자 프로필을 추가한다: 프로필 폴더(레지스트리
 * ProfileList\ProfilesDirectory, 보통 C:\Users) 바로 아래에서 현재 사용자 프로필(%USERPROFILE% 과
 * 실제 프로필 위치 FOLDERID_Profile)이 아닌 폴더(Default·Public·다른 계정 등). 드라이브나 C:\Users 를
 * 통째로 탐지할 때 남의 파일·새 계정의 틀(Default)을 후보로 삼지 않는다. 프로필 폴더가 볼륨 루트면
 * 하지 않는다. 테스트용으로 환경 변수 BYTESIEVE_TEST_PROFILES_DIR 이 있으면 그 폴더를 프로필 폴더로 쓴다.
 * 지금 있는 폴더를 읽어 넣으므로 탐지 직전에 부른다. 추가한 개수를 반환한다.
 */
size_t cleanup_protect_add_other_profiles(CleanupProtect *p);

/*
 * dir 가 볼륨 루트이고 name 이 Windows 시스템 파일(DumpStack.log·DumpStack.log.tmp·pagefile.sys·
 * hiberfil.sys·swapfile.sys)인지. 정리 후보 탐지는 이 파일을 후보로 삼지 않는다.
 */
int cleanup_is_root_system_file(const wchar_t *dir, const wchar_t *name);

/*
 * 프로그램 설치 경로 하나를 추가한다. 레지스트리 등 외부 값을 받으므로 cleanup_protect_add 보다
 * 엄격하다: 앞뒤 공백·따옴표를 벗기고 "X:\폴더..." 꼴의 로컬 절대 경로만 받으며, 볼륨 루트는
 * 받지 않는다(드라이브 전체가 보호되어 탐지가 멈추는 것을 막음). "."·".."·'/' 는 정규화한다.
 * 성공 0, 받지 않은 경로·실패 -1.
 */
int cleanup_protect_add_install(CleanupProtect *p, const wchar_t *path);

/*
 * 프로그램 제거 정보의 표시 이름이 보호할 개발 도구인지(대소문자 무시):
 * "Microsoft Visual Studio"(VS Code·Installer·이전 버전 포함) 또는 "Visual Studio "로 시작
 * (Visual Studio Community/Professional/Enterprise/Build Tools 2017 이후).
 */
int cleanup_is_dev_tool_product(const wchar_t *display_name);

/*
 * dir 바로 아래의 name 폴더가 보호 대상인지.
 * dir 는 확장 경로("\\?\C:\x")여도 된다. 다음 중 하나면 1:
 *   - dir\name 이 목록의 경로와 같다
 *   - dir 가 볼륨 루트이고 name 이 $Recycle.Bin, System Volume Information,
 *     Recovery, Config.Msi 중 하나다
 */
int cleanup_protect_match(const CleanupProtect *p, const wchar_t *dir, const wchar_t *name);

/* path 가 보호 경로와 같거나 그 아래에 있는지(탐지 시작 경로 경고용). */
int cleanup_protect_contains(const CleanupProtect *p, const wchar_t *path);

/*
 * 휴지통이 없는 드라이브에서 정리한 항목을 옮겨 두는 격리 폴더 이름(볼륨 루트 아래
 * "X:\.bytesieve-trash", quarantine.h). 폴더 스캔·정리 후보 탐지는 이 이름의 폴더에 들어가지
 * 않고, 정리 보호 판정은 이 폴더 안의 항목을 거부한다.
 */
#define CLEANUP_QUARANTINE_DIR L".bytesieve-trash"

/* name 이 격리 폴더 이름인지(대소문자 무시). */
int cleanup_is_quarantine_name(const wchar_t *name);

/* path 의 구성 요소 중 하나가 격리 폴더 이름인지(격리 폴더 자체이거나 그 안). */
int cleanup_in_quarantine(const wchar_t *path);

#endif /* BYTESIEVE_CLEANUP_RULES_H */
