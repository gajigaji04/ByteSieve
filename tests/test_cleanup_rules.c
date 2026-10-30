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

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cleanup_rules tests passed\n");
    return 0;
}
