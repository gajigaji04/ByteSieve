#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_rules.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void test_classify_dir(void)
{
    CHECK(cleanup_classify_dir(L"node_modules") == CLEANUP_NODE_MODULES);
    CHECK(cleanup_classify_dir(L"Node_Modules") == CLEANUP_NODE_MODULES);
    CHECK(cleanup_classify_dir(L"dist") == CLEANUP_BUILD);
    CHECK(cleanup_classify_dir(L"BUILD") == CLEANUP_BUILD);
    CHECK(cleanup_classify_dir(L"out") == CLEANUP_BUILD);
    CHECK(cleanup_classify_dir(L".cache") == CLEANUP_CACHE);
    CHECK(cleanup_classify_dir(L".next") == CLEANUP_CACHE);
    CHECK(cleanup_classify_dir(L".vite") == CLEANUP_CACHE);
    CHECK(cleanup_classify_dir(L".tmp") == CLEANUP_TEMP);
    CHECK(cleanup_classify_dir(L".TEMP") == CLEANUP_TEMP);

    /* 정확히 같은 이름만 */
    CHECK(cleanup_classify_dir(L"node_modules2") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"builds") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"output") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"cache") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"tmp") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"temp") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"logs") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(L"") == CLEANUP_NONE);
    CHECK(cleanup_classify_dir(NULL) == CLEANUP_NONE);
}

static void test_classify_file(void)
{
    CHECK(cleanup_classify_file(L"a.tmp") == CLEANUP_TEMP);
    CHECK(cleanup_classify_file(L"A.TMP") == CLEANUP_TEMP);
    CHECK(cleanup_classify_file(L"server.log") == CLEANUP_LOG);
    CHECK(cleanup_classify_file(L"npm-debug.LOG") == CLEANUP_LOG);
    CHECK(cleanup_classify_file(L".log") == CLEANUP_LOG);
    CHECK(cleanup_classify_file(L"한글.log") == CLEANUP_LOG);

    CHECK(cleanup_classify_file(L"x.tmp.bak") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"report.log.1") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"catalog") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"log") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"node_modules") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(NULL) == CLEANUP_NONE);

    /* LevelDB 쓰기 로그(숫자 6자리 이상)는 데이터라 로그가 아니다 */
    CHECK(cleanup_classify_file(L"000003.log") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"1234567.LOG") == CLEANUP_NONE);
    CHECK(cleanup_classify_file(L"12345.log") == CLEANUP_LOG);
    CHECK(cleanup_classify_file(L"000003a.log") == CLEANUP_LOG);
    CHECK(cleanup_classify_file(L"app-000003.log") == CLEANUP_LOG);

    CHECK(cleanup_is_leveldb_marker(L"CURRENT"));
    CHECK(cleanup_is_leveldb_marker(L"LOCK"));
    CHECK(cleanup_is_leveldb_marker(L"MANIFEST-000001"));
    CHECK(!cleanup_is_leveldb_marker(L"current.txt"));
    CHECK(!cleanup_is_leveldb_marker(L"server.log"));
    CHECK(!cleanup_is_leveldb_marker(NULL));

    /* 볼륨 루트의 Windows 시스템 파일 */
    CHECK(cleanup_is_root_system_file(L"C:\\", L"DumpStack.log.tmp"));
    CHECK(cleanup_is_root_system_file(L"\\\\?\\D:\\", L"pagefile.sys"));
    CHECK(cleanup_is_root_system_file(L"C:", L"dumpstack.log"));
    CHECK(!cleanup_is_root_system_file(L"C:\\work", L"DumpStack.log.tmp"));
    CHECK(!cleanup_is_root_system_file(L"C:\\", L"session.tmp"));
    CHECK(!cleanup_is_root_system_file(NULL, L"pagefile.sys"));
}

static void test_markers(void)
{
    CHECK(cleanup_needs_marker(CLEANUP_NODE_MODULES));
    CHECK(cleanup_needs_marker(CLEANUP_BUILD));
    CHECK(!cleanup_needs_marker(CLEANUP_CACHE));
    CHECK(!cleanup_needs_marker(CLEANUP_TEMP));
    CHECK(!cleanup_needs_marker(CLEANUP_LOG));

    CHECK(cleanup_is_project_marker(L"package.json"));
    CHECK(cleanup_is_project_marker(L"cmakelists.txt"));
    CHECK(cleanup_is_project_marker(L"Makefile"));
    CHECK(cleanup_is_project_marker(L"Cargo.toml"));
    CHECK(cleanup_is_project_marker(L"build.gradle.kts"));
    CHECK(cleanup_is_project_marker(L"App.csproj"));
    CHECK(cleanup_is_project_marker(L"ByteSieve.sln"));
    CHECK(!cleanup_is_project_marker(L"package.json.bak"));
    CHECK(!cleanup_is_project_marker(L"readme.md"));
    CHECK(!cleanup_is_project_marker(NULL));
}

static void test_dir_rules(void)
{
    CleanupDirRule rule;

    /* 이름만으로 환경이 정해지는 폴더 */
    CHECK(cleanup_dir_rule(L"node_modules", &rule) && rule.eco == CLEANUP_ECO_NODE &&
          rule.require == 0);
    CHECK(cleanup_dir_rule(L".Next", &rule) && rule.category == CLEANUP_CACHE &&
          rule.eco == CLEANUP_ECO_NODE);
    CHECK(cleanup_dir_rule(L".svelte-kit", &rule) && rule.eco == CLEANUP_ECO_NODE);
    CHECK(cleanup_dir_rule(L"__pycache__", &rule) && rule.category == CLEANUP_CACHE &&
          rule.eco == CLEANUP_ECO_PYTHON && rule.require == 0);
    CHECK(cleanup_dir_rule(L".pytest_cache", &rule) && rule.eco == CLEANUP_ECO_PYTHON);
    CHECK(cleanup_dir_rule(L".tox", &rule) && rule.eco == CLEANUP_ECO_PYTHON);
    CHECK(cleanup_dir_rule(L"cmake-build-debug", &rule) && rule.category == CLEANUP_BUILD &&
          rule.eco == CLEANUP_ECO_CPP);
    CHECK(cleanup_dir_rule(L"CMake-Build-Release", &rule) && rule.eco == CLEANUP_ECO_CPP);
    CHECK(!cleanup_dir_rule(L"cmake-build-", &rule)); /* 접두사만 */

    /* 표시 파일이 있어야만 후보인 폴더 */
    CHECK(cleanup_dir_rule(L"target", &rule) && rule.category == CLEANUP_BUILD &&
          rule.require == (CLEANUP_ECO_BIT(CLEANUP_ECO_RUST) | CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA)));
    CHECK(cleanup_dir_rule(L"bin", &rule) && rule.require == CLEANUP_ECO_BIT(CLEANUP_ECO_DOTNET));
    CHECK(cleanup_dir_rule(L"OBJ", &rule) && rule.eco == CLEANUP_ECO_DOTNET);
    CHECK(cleanup_dir_rule(L".vs", &rule) && rule.category == CLEANUP_CACHE &&
          rule.require == CLEANUP_ECO_BIT(CLEANUP_ECO_DOTNET));
    CHECK(cleanup_dir_rule(L".gradle", &rule) && rule.eco == CLEANUP_ECO_JAVA &&
          rule.require == CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA));
    CHECK(cleanup_classify_dir(L"target") == CLEANUP_BUILD);

    /* 표시 파일로 환경 고르기 */
    CHECK(cleanup_dir_rule(L"build", &rule));
    CHECK(cleanup_resolve_eco(&rule, 0) == CLEANUP_ECO_GENERIC);
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_NODE)) == CLEANUP_ECO_NODE);
    /* build 는 Gradle 이 C/C++·Node.js 보다 앞선다. */
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_NODE) |
                                         CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA)) == CLEANUP_ECO_JAVA);
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_CPP) |
                                         CLEANUP_ECO_BIT(CLEANUP_ECO_NODE)) == CLEANUP_ECO_CPP);
    /* 고를 수 있는 환경이 아니면 일반 */
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_RUST)) == CLEANUP_ECO_GENERIC);
    CHECK(cleanup_dir_rule(L"dist", &rule));
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_PYTHON)) == CLEANUP_ECO_PYTHON);
    CHECK(cleanup_dir_rule(L"target", &rule));
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA)) == CLEANUP_ECO_JAVA);
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_RUST) |
                                         CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA)) == CLEANUP_ECO_RUST);
    /* 이름으로 정해진 환경은 표시 파일과 상관없다. */
    CHECK(cleanup_dir_rule(L"node_modules", &rule));
    CHECK(cleanup_resolve_eco(&rule, CLEANUP_ECO_BIT(CLEANUP_ECO_PYTHON)) == CLEANUP_ECO_NODE);
    CHECK(cleanup_resolve_eco(NULL, 0) == CLEANUP_ECO_GENERIC);

    /* 표시 파일 → 환경 */
    CHECK(cleanup_marker_ecos(L"package.json") == CLEANUP_ECO_BIT(CLEANUP_ECO_NODE));
    CHECK(cleanup_marker_ecos(L"requirements.txt") == CLEANUP_ECO_BIT(CLEANUP_ECO_PYTHON));
    CHECK(cleanup_marker_ecos(L"PYPROJECT.TOML") == CLEANUP_ECO_BIT(CLEANUP_ECO_PYTHON));
    CHECK(cleanup_marker_ecos(L"Cargo.toml") == CLEANUP_ECO_BIT(CLEANUP_ECO_RUST));
    CHECK(cleanup_marker_ecos(L"pom.xml") == CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA));
    CHECK(cleanup_marker_ecos(L"settings.gradle.kts") == CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA));
    CHECK(cleanup_marker_ecos(L"App.fsproj") == CLEANUP_ECO_BIT(CLEANUP_ECO_DOTNET));
    CHECK(cleanup_marker_ecos(L"x.sln") == CLEANUP_ECO_BIT(CLEANUP_ECO_DOTNET));
    CHECK(cleanup_marker_ecos(L".sln") == 0);
    CHECK(cleanup_marker_ecos(L"Makefile") == CLEANUP_ECO_BIT(CLEANUP_ECO_CPP));
    CHECK(cleanup_marker_ecos(L"go.mod") == CLEANUP_ECO_BIT(CLEANUP_ECO_GO));
    CHECK(cleanup_marker_ecos(L"go.sum") == 0);
    CHECK(cleanup_marker_ecos(NULL) == 0);
    CHECK(cleanup_is_project_marker(L"setup.py"));

    CHECK(wcscmp(cleanup_eco_name(CLEANUP_ECO_PYTHON), L"python") == 0);
    CHECK(wcscmp(cleanup_eco_name(CLEANUP_ECO_DOTNET), L"dotnet") == 0);
    CHECK(wcscmp(cleanup_eco_name(CLEANUP_ECO_GENERIC), L"generic") == 0);
    CHECK(CLEANUP_ECO_MASK_ALL == 0xFFu);
}

static void test_names(void)
{
    CHECK(wcscmp(cleanup_category_name(CLEANUP_NODE_MODULES), L"node_modules") == 0);
    CHECK(wcscmp(cleanup_category_name(CLEANUP_LOG), L"log") == 0);
    CHECK(wcscmp(cleanup_category_name(CLEANUP_NONE), L"none") == 0);
    CHECK(CLEANUP_MASK_ALL == 0x3Eu);
}

static void test_protect_match(void)
{
    CleanupProtect p;

    cleanup_protect_init(&p);
    CHECK(cleanup_protect_add(&p, L"C:\\Windows\\") == 0);
    CHECK(cleanup_protect_add(&p, L"\\\\?\\C:\\Program Files") == 0);
    CHECK(cleanup_protect_add(&p, L"c:\\windows") == 0); /* 중복은 무시 */
    CHECK(p.len == 2);
    CHECK(wcscmp(p.paths[0], L"C:\\Windows") == 0);
    CHECK(wcscmp(p.paths[1], L"C:\\Program Files") == 0);
    CHECK(cleanup_protect_add(&p, L"") == -1);
    CHECK(cleanup_protect_add(&p, NULL) == -1);

    CHECK(cleanup_protect_match(&p, L"C:\\", L"Windows"));
    CHECK(cleanup_protect_match(&p, L"\\\\?\\C:\\", L"windows"));
    CHECK(cleanup_protect_match(&p, L"\\\\?\\C:", L"Program Files"));
    CHECK(!cleanup_protect_match(&p, L"C:\\", L"Windows2"));
    CHECK(!cleanup_protect_match(&p, L"C:\\", L"Win"));
    CHECK(!cleanup_protect_match(&p, L"C:\\Windows", L"System32")); /* 같은 경로만 */
    CHECK(!cleanup_protect_match(&p, L"D:\\", L"Windows"));
    CHECK(!cleanup_protect_match(&p, L"C:\\dev", L"Windows"));
    CHECK(!cleanup_protect_match(&p, L"\\\\?\\UNC\\srv\\share", L"Windows"));

    /* 볼륨 루트 바로 아래의 시스템 폴더 이름(어느 드라이브든) */
    CHECK(cleanup_protect_match(&p, L"D:\\", L"$RECYCLE.BIN"));
    CHECK(cleanup_protect_match(NULL, L"\\\\?\\E:\\", L"System Volume Information"));
    CHECK(cleanup_protect_match(NULL, L"C:\\", L"Recovery"));
    CHECK(cleanup_protect_match(NULL, L"C:\\", L"Config.Msi"));
    CHECK(!cleanup_protect_match(NULL, L"C:\\Users\\me", L"Recovery"));
    CHECK(!cleanup_protect_match(NULL, L"C:\\", L"Users"));
    cleanup_protect_free(&p);
}

static void test_protect_contains(void)
{
    CleanupProtect p;

    cleanup_protect_init(&p);
    cleanup_protect_add(&p, L"C:\\Windows");
    CHECK(cleanup_protect_contains(&p, L"C:\\Windows"));
    CHECK(cleanup_protect_contains(&p, L"c:\\windows\\"));
    CHECK(cleanup_protect_contains(&p, L"C:\\Windows\\System32\\drivers"));
    CHECK(cleanup_protect_contains(&p, L"\\\\?\\C:\\Windows\\Temp"));
    CHECK(!cleanup_protect_contains(&p, L"C:\\Windows2"));
    CHECK(!cleanup_protect_contains(&p, L"C:\\"));
    CHECK(!cleanup_protect_contains(&p, L"C:\\Users"));
    CHECK(cleanup_protect_contains(NULL, L"D:\\$Recycle.Bin\\S-1-5"));
    CHECK(cleanup_protect_contains(NULL, L"C:\\System Volume Information"));
    CHECK(!cleanup_protect_contains(NULL, L"C:\\dev\\Recovery"));
    CHECK(!cleanup_protect_contains(NULL, NULL));
    cleanup_protect_free(&p);
}

/* 실제 시스템의 Windows 폴더는 보호 목록에 들어가야 한다. */
static void test_protect_system(void)
{
    CleanupProtect p;
    wchar_t windir[MAX_PATH], sys32[MAX_PATH + 16];

    cleanup_protect_init(&p);
    CHECK(cleanup_protect_add_system(&p) >= 2);
    CHECK(GetWindowsDirectoryW(windir, MAX_PATH) > 0);
    swprintf(sys32, sizeof(sys32) / sizeof(sys32[0]), L"%ls\\System32", windir);
    CHECK(cleanup_protect_contains(&p, windir));
    CHECK(cleanup_protect_contains(&p, sys32));
    /* 두 번 불러도 중복되지 않는다 */
    CHECK(cleanup_protect_add_system(&p) == 0);
    cleanup_protect_free(&p);
}

/* 레지스트리 등에서 받은 설치 경로는 로컬 절대 경로만, 볼륨 루트는 받지 않는다. */
static void test_protect_install(void)
{
    CleanupProtect p;

    cleanup_protect_init(&p);
    CHECK(cleanup_protect_add_install(&p, L" \"D:\\VS\\2022\\Community\\\" ") == 0);
    CHECK(cleanup_protect_add_install(&p, L"E:/Tools/./Microsoft VS Code/") == 0);
    CHECK(cleanup_protect_add_install(&p, L"F:\\a\\..\\Code") == 0);
    CHECK(p.len == 3);
    CHECK(p.len == 3 && wcscmp(p.paths[0], L"D:\\VS\\2022\\Community") == 0);
    CHECK(p.len == 3 && wcscmp(p.paths[1], L"E:\\Tools\\Microsoft VS Code") == 0);
    CHECK(p.len == 3 && wcscmp(p.paths[2], L"F:\\Code") == 0);
    CHECK(cleanup_protect_add_install(&p, L"d:\\vs\\2022\\community") == 0); /* 중복 */
    CHECK(p.len == 3);

    CHECK(cleanup_protect_add_install(&p, L"D:\\") == -1);
    CHECK(cleanup_protect_add_install(&p, L"\"D:\\\"") == -1);
    CHECK(cleanup_protect_add_install(&p, L"D:") == -1);
    CHECK(cleanup_protect_add_install(&p, L"D:\\x\\..") == -1);
    CHECK(cleanup_protect_add_install(&p, L"D:\\.") == -1);
    CHECK(cleanup_protect_add_install(&p, L"D:VS") == -1);
    CHECK(cleanup_protect_add_install(&p, L"VS\\2022") == -1);
    CHECK(cleanup_protect_add_install(&p, L"\\VS\\2022") == -1);
    CHECK(cleanup_protect_add_install(&p, L"\\\\srv\\share\\VS") == -1);
    CHECK(cleanup_protect_add_install(&p, L"\\\\?\\C:\\VS") == -1);
    CHECK(cleanup_protect_add_install(&p, L"") == -1);
    CHECK(cleanup_protect_add_install(&p, L"\"\"") == -1);
    CHECK(cleanup_protect_add_install(&p, NULL) == -1);
    CHECK(cleanup_protect_add_install(NULL, L"D:\\VS") == -1);
    CHECK(p.len == 3);

    /* 설치 폴더 안의 실행 파일과 하위 폴더 */
    CHECK(cleanup_protect_contains(&p, L"D:\\VS\\2022\\Community\\Common7\\IDE\\devenv.exe"));
    CHECK(cleanup_protect_contains(&p, L"D:\\VS\\2022\\Community\\Common7\\IDE\\Extensions"));
    CHECK(cleanup_protect_contains(&p, L"E:\\Tools\\Microsoft VS Code\\Code.exe"));
    CHECK(cleanup_protect_match(&p, L"E:\\Tools", L"Microsoft VS Code"));
    CHECK(!cleanup_protect_contains(&p, L"D:\\VS\\2022"));
    CHECK(!cleanup_protect_contains(&p, L"D:\\VS\\2022\\Community2\\x"));
    cleanup_protect_free(&p);
}

static void test_dev_tool_product(void)
{
    CHECK(cleanup_is_dev_tool_product(L"Microsoft Visual Studio Code"));
    CHECK(cleanup_is_dev_tool_product(L"Microsoft Visual Studio Code (User)"));
    CHECK(cleanup_is_dev_tool_product(L"Microsoft Visual Studio Code Insiders"));
    CHECK(cleanup_is_dev_tool_product(L"Microsoft Visual Studio Installer"));
    CHECK(cleanup_is_dev_tool_product(L"microsoft visual studio community 2015"));
    CHECK(cleanup_is_dev_tool_product(L"Visual Studio Community 2022"));
    CHECK(cleanup_is_dev_tool_product(L"Visual Studio Build Tools 2022"));
    CHECK(cleanup_is_dev_tool_product(L"Visual Studio Enterprise 2026"));
    CHECK(!cleanup_is_dev_tool_product(L"Microsoft Visual C++ 2015-2022 Redistributable (x64)"));
    CHECK(!cleanup_is_dev_tool_product(L"Visual Studio"));
    CHECK(!cleanup_is_dev_tool_product(L"VisualStudio Community"));
    CHECK(!cleanup_is_dev_tool_product(L"Notepad++"));
    CHECK(!cleanup_is_dev_tool_product(L""));
    CHECK(!cleanup_is_dev_tool_product(NULL));
}

/* 환경 변수 값을 저장한다(없으면 빈 문자열과 0). */
static int save_env(const wchar_t *name, wchar_t *buf, DWORD cap)
{
    DWORD n = GetEnvironmentVariableW(name, buf, cap);

    if (n == 0 || n >= cap) {
        buf[0] = L'\0';
        return 0;
    }
    return 1;
}

/*
 * 개발 도구 설치 경로: VS Code(사용자·시스템 설치, 확장), Visual Studio 기본 위치.
 * 사용자 폴더를 가짜 값으로 돌려 실제 시스템과 무관하게 확인한다(파일 시스템은 보지 않음).
 */
static void test_protect_dev_tools(void)
{
    CleanupProtect p;
    wchar_t local[MAX_PATH], home[MAX_PATH], pf[MAX_PATH], pf86[MAX_PATH], buf[2 * MAX_PATH];
    int had_local = save_env(L"LOCALAPPDATA", local, MAX_PATH);
    int had_home = save_env(L"USERPROFILE", home, MAX_PATH);
    int had_pf = save_env(L"ProgramFiles", pf, MAX_PATH);
    int had_pf86 = save_env(L"ProgramFiles(x86)", pf86, MAX_PATH);

    SetEnvironmentVariableW(L"LOCALAPPDATA", L"Q:\\fx\\home\\AppData\\Local");
    SetEnvironmentVariableW(L"USERPROFILE", L"Q:\\fx\\home");
    cleanup_protect_init(&p);
    cleanup_protect_add_system(&p);

    /* %LOCALAPPDATA%\Programs: VS Code 사용자 설치의 실행 파일·하위 폴더 */
    CHECK(cleanup_protect_match(&p, L"Q:\\fx\\home\\AppData\\Local", L"Programs"));
    CHECK(cleanup_protect_contains(
        &p, L"Q:\\fx\\home\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe"));
    CHECK(cleanup_protect_contains(&p, L"Q:\\fx\\home\\AppData\\Local\\Programs\\Microsoft VS "
                                       L"Code\\resources\\app\\node_modules"));
    CHECK(cleanup_protect_contains(
        &p, L"Q:\\fx\\home\\AppData\\Local\\Programs\\Python\\Python312\\Lib\\__pycache__"));
    CHECK(!cleanup_protect_contains(&p, L"Q:\\fx\\home\\AppData\\Local\\Temp\\x.tmp"));
    CHECK(!cleanup_protect_contains(&p, L"Q:\\fx\\home\\AppData\\Local\\Programs2"));

    /* 설치한 VS Code 확장(안에 node_modules·package.json·out 이 있음) */
    CHECK(cleanup_protect_match(&p, L"Q:\\fx\\home\\.vscode", L"extensions"));
    CHECK(cleanup_protect_contains(
        &p, L"Q:\\fx\\home\\.vscode\\extensions\\ms-python.python-2026.1.0\\node_modules"));
    CHECK(cleanup_protect_contains(
        &p, L"Q:\\fx\\home\\.vscode-insiders\\extensions\\x.y-1.0.0\\out"));
    CHECK(!cleanup_protect_contains(&p, L"Q:\\fx\\home\\.vscode\\argv.json"));
    CHECK(!cleanup_protect_contains(&p, L"Q:\\fx\\home\\proj\\.vscode\\extensions"));

    /* VS Code 시스템 설치·Visual Studio 기본 위치(Program Files 아래) */
    if (had_pf) {
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%ls\\Microsoft VS Code\\Code.exe", pf);
        CHECK(cleanup_protect_contains(&p, buf));
        swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                 L"%ls\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\devenv.exe", pf);
        CHECK(cleanup_protect_contains(&p, buf));
        swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                 L"%ls\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin", pf);
        CHECK(cleanup_protect_contains(&p, buf));
    }
    if (had_pf86) {
        swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                 L"%ls\\Microsoft Visual Studio\\Installer\\setup.exe", pf86);
        CHECK(cleanup_protect_contains(&p, buf));
    }
    /* 레지스트리에서 읽은 설치 경로도 볼륨 루트는 없다(드라이브 전체 보호 방지). */
    {
        size_t i;
        for (i = 0; i < p.len; i++) {
            CHECK(wcslen(p.paths[i]) > 2);
        }
    }
    cleanup_protect_free(&p);

    SetEnvironmentVariableW(L"LOCALAPPDATA", had_local ? local : NULL);
    SetEnvironmentVariableW(L"USERPROFILE", had_home ? home : NULL);
}

int main(void)
{
    test_classify_dir();
    test_classify_file();
    test_markers();
    test_dir_rules();
    test_names();
    test_protect_match();
    test_protect_contains();
    test_protect_system();
    test_protect_install();
    test_dev_tool_product();
    test_protect_dev_tools();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cleanup_rules tests passed\n");
    return 0;
}
