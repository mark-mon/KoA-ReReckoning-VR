// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#include "log.h"
#include <cstdio>
#include <share.h>

namespace avr {

static FILE* g_file = nullptr;
static int g_level = LOG_INFO;
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static const char* kNames[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};

void LogInit(const wchar_t* path, int level) {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    g_level = level;
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path, L"w", _SH_DENYWR);
}

void LogSetLevel(int level) { g_level = level; }
int LogGetLevel() { return g_level; }

void LogWrite(int level, const char* fmt, ...) {
    if (level > g_level || !g_file) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    fprintf(g_file, "%02d:%02d:%02d.%03d [%s] [%5lu] %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, kNames[level < 0 ? 0 : (level > 4 ? 4 : level)], GetCurrentThreadId(), buf);
    if (level <= LOG_INFO) fflush(g_file);
    LeaveCriticalSection(&g_cs);
}

void LogFlush() {
    if (g_file) { EnterCriticalSection(&g_cs); fflush(g_file); LeaveCriticalSection(&g_cs); }
}

} // namespace avr
