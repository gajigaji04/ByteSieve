#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

enum {
    EXIT_COMPLETE = 0,
    EXIT_PARTIAL = 1,
    EXIT_FATAL = 2
};

static void print_usage(FILE *out)
{
    fwprintf(out,
             L"Usage: disk-scanner <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories and total size under <directory>.\n");
}

static int is_help(const wchar_t *arg)
{
    return wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0;
}

int wmain(int argc, wchar_t **argv)
{
    /* 이후 wide 출력 함수만 사용한다(printf와 혼용 금지). */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    if (argc != 2) {
        print_usage(stderr);
        return EXIT_FATAL;
    }

    if (is_help(argv[1])) {
        print_usage(stdout);
        return EXIT_COMPLETE;
    }

    if (argv[1][0] == L'-') {
        fwprintf(stderr, L"error: unknown option: %ls\n", argv[1]);
        print_usage(stderr);
        return EXIT_FATAL;
    }

    /* 스캔은 이후 단계에서 구현한다. */
    wprintf(L"Target: %ls\n", argv[1]);
    return EXIT_COMPLETE;
}
