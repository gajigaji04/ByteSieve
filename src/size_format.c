#include "size_format.h"

void format_size(uint64_t bytes, wchar_t *buf, size_t len)
{
    static const wchar_t *units[] = {L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
    double value = (double)bytes;
    int unit = -1;

    if (buf == NULL || len == 0) {
        return;
    }
    if (bytes < 1024) {
        swprintf(buf, len, L"%llu B", (unsigned long long)bytes);
        return;
    }
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    swprintf(buf, len, value < 10.0 ? L"%.2f %ls" : L"%.1f %ls", value, units[unit]);
}
