// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_str.h"

namespace Libraries::LibcInternal {

s32 PS4_SYSV_ABI internal_strcpy_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcpy_s(dest, dest_size, src);
#else
    std::strcpy(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcat_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcat_s(dest, dest_size, src);
#else
    std::strcat(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcmp(const char* str1, const char* str2) {
    return std::strcmp(str1, str2);
}

s32 PS4_SYSV_ABI internal_strncmp(const char* str1, const char* str2, size_t num) {
    return std::strncmp(str1, str2, num);
}

char* PS4_SYSV_ABI internal_strcpy(char* dest, const char* src) {
    return std::strcpy(dest, src);
}

size_t PS4_SYSV_ABI internal_strlen(const char* str) {
    return std::strlen(str);
}

size_t PS4_SYSV_ABI internal_wcslen(const u16* str) {
    const u16* end = str;
    while (*end != 0) {
        ++end;
    }
    return static_cast<size_t>(end - str);
}

char* PS4_SYSV_ABI internal_strncpy(char* dest, const char* src, std::size_t count) {
    return std::strncpy(dest, src, count);
}

s32 PS4_SYSV_ABI internal_strncpy_s(char* dest, size_t destsz, const char* src, size_t count) {
#ifdef _WIN64
    return strncpy_s(dest, destsz, src, count);
#else
    std::strcpy(dest, src);
    return 0;
#endif
}

char* PS4_SYSV_ABI internal_strcat(char* dest, const char* src) {
    return std::strcat(dest, src);
}

const char* PS4_SYSV_ABI internal_strchr(const char* str, int c) {
    return std::strchr(str, c);
}

#ifndef LUMEN_GEN4_DIRECT_FEX
const char* PS4_SYSV_ABI internal_strrchr(const char* str, int c) { return std::strrchr(str, c); }
const char* PS4_SYSV_ABI internal_strstr(const char* str, const char* needle) { return std::strstr(str, needle); }
size_t PS4_SYSV_ABI internal_strspn(const char* str, const char* accept) { return std::strspn(str, accept); }
size_t PS4_SYSV_ABI internal_strcspn(const char* str, const char* reject) { return std::strcspn(str, reject); }
const char* PS4_SYSV_ABI internal_strpbrk(const char* str, const char* accept) { return std::strpbrk(str, accept); }
#endif

void RegisterlibSceLibcInternalStr(Core::Loader::SymbolsResolver* sym) {
#ifndef LUMEN_GEN4_DIRECT_FEX
    LIB_FUNCTION("kiZSXIWd9vg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcpy);
    LIB_FUNCTION("9yDWMxEFdJU", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strrchr);
    LIB_FUNCTION("viiwFMaNamA", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strstr);
    LIB_FUNCTION("-kU6bB4M-+k", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strspn);
    LIB_FUNCTION("q0F6yS-rCms", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcspn);
    LIB_FUNCTION("kDZvoVssCgQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strpbrk);
#endif

    LIB_FUNCTION("5Xa2ACNECdo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcpy_s);
    LIB_FUNCTION("K+gcnFFJKVc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat_s);
    LIB_FUNCTION("Ovb2dSJOAuE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcmp);
    LIB_FUNCTION("aesyjrHVWy4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncmp);
    LIB_FUNCTION("j4ViWNHEgww", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strlen);
    LIB_FUNCTION("6sJWiWSRuqk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy);
    LIB_FUNCTION("YNzNkJzYqEg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy_s);
    LIB_FUNCTION("Ls4tzzhimqQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat);
    LIB_FUNCTION("ob5xAW4ln-0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strchr);
}

void RegisterFexLibcStrAliases(Core::Loader::SymbolsResolver* sym) {
#ifndef LUMEN_GEN4_DIRECT_FEX
    LIB_FUNCTION("9yDWMxEFdJU", "libc", 1, "libc", internal_strrchr);
    LIB_FUNCTION("viiwFMaNamA", "libc", 1, "libc", internal_strstr);
    LIB_FUNCTION("-kU6bB4M-+k", "libc", 1, "libc", internal_strspn);
    LIB_FUNCTION("q0F6yS-rCms", "libc", 1, "libc", internal_strcspn);
    LIB_FUNCTION("kDZvoVssCgQ", "libc", 1, "libc", internal_strpbrk);
    LIB_FUNCTION("aesyjrHVWy4", "libc", 1, "libc", internal_strncmp);
    LIB_FUNCTION("6sJWiWSRuqk", "libc", 1, "libc", internal_strncpy);
    LIB_FUNCTION("ob5xAW4ln-0", "libc", 1, "libc", internal_strchr);
#endif

    LIB_FUNCTION("kiZSXIWd9vg", "libc", 1, "libc", internal_strcpy);
    LIB_FUNCTION("Ls4tzzhimqQ", "libc", 1, "libc", internal_strcat);
    LIB_FUNCTION("j4ViWNHEgww", "libc", 1, "libc", internal_strlen);
    LIB_FUNCTION("WkkeywLJcgU", "libc", 1, "libc", internal_wcslen);
    LIB_FUNCTION("Ovb2dSJOAuE", "libc", 1, "libc", internal_strcmp);
}

} // namespace Libraries::LibcInternal
