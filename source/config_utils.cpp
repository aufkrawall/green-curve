#include "app_shared.h"

void trim_ascii(char* s) {
    if (!s) return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;

    if (start > 0 && end > start) {
        memmove(s, s + start, (size_t)(end - start));
    }

    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;

    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || *end != 0) return false;
    if (v < -2147483647L - 1L || v > 2147483647L) return false;

    *out = (int)v;
    return true;
}

void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;

    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;

    char buf[64] = {};
    if (text) StringCchCopyA(buf, ARRAY_COUNT(buf), text);
    trim_ascii(buf);
    if (buf[0] == 0 || streqi_ascii(buf, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (value < 0 || value > 100) return false;

    *isAuto = false;
    *pct = value;
    return true;
}

bool config_section_has_keys(const char* path, const char* section) {
    if (!path || !section) return false;

    char keys[256] = {};
    DWORD n = GetPrivateProfileStringA(section, nullptr, "", keys, ARRAY_COUNT(keys), path);
    return n > 0 && keys[0] != 0;
}

int get_config_int(const char* path, const char* section, const char* key, int defaultVal) {
    if (!path || !section || !key) return defaultVal;

    char buf[32] = {};
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return defaultVal;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return defaultVal;
    return value;
}

bool set_config_int(const char* path, const char* section, const char* key, int value) {
    if (!path || !section || !key) return false;

    char buf[32] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", value);
    return WritePrivateProfileStringA(section, key, buf, path) != FALSE;
}