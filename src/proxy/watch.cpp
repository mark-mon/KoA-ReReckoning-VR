// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Hardware write-watchpoints (debug registers) for reverse engineering: logs which code writes
// selected memory addresses, with a scan of return addresses for context.
#include "watch.h"
#include "globals.h"
#include "hookutil.h"
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <map>
#include <mutex>

namespace avr {

static uintptr_t g_addr[4];
static const char* g_label[4];
static int g_n = 0;
static PVOID g_veh = nullptr;
static std::mutex g_mx;
struct Hit { int count; uint32_t stack[8]; int nstack; DWORD tid; };
static std::map<std::pair<int, uint32_t>, Hit> g_hits; // (watch idx, eip rva) -> hit
static volatile LONG g_active = 0;

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_active) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t eip = (uint32_t)(c->Eip - GameModule().base);
    for (int i = 0; i < 4; ++i) {
        if (!(dr6 & (1u << i))) continue;
        std::lock_guard<std::mutex> lk(g_mx);
        auto& h = g_hits[{i, eip}];
        if (h.count++ == 0) {
            h.tid = GetCurrentThreadId();
            h.nstack = ScanGameReturnAddresses((void*)c->Esp, h.stack, 8, 1024);
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ApplyToThread(HANDLE th, bool set) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return;
    DWORD dr7 = 0;
    if (set) {
        for (int i = 0; i < g_n; ++i) {
            (&ctx.Dr0)[i] = g_addr[i];
            dr7 |= (1u << (i * 2));                 // local enable
            dr7 |= (1u << (16 + i * 4));            // RW=01 write
            dr7 |= (3u << (18 + i * 4));            // LEN=11 (4 bytes)
        }
    } else {
        ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
    }
    ctx.Dr7 = dr7;
    ctx.Dr6 = 0;
    SetThreadContext(th, &ctx);
}

static void ApplyAll(bool set) {
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (te.th32ThreadID != self) SuspendThread(th);
        ApplyToThread(th, set);
        if (te.th32ThreadID != self) ResumeThread(th);
        CloseHandle(th);
        n++;
    }
    CloseHandle(snap);
    LOGI("watch: %s debug registers on %d threads", set ? "set" : "cleared", n);
}

void WatchStart(const uintptr_t* addrs, const char** labels, int n) {
    if (g_active) return;
    g_n = n > 4 ? 4 : n;
    for (int i = 0; i < g_n; ++i) { g_addr[i] = addrs[i]; g_label[i] = labels[i]; }
    g_hits.clear();
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    g_active = 1;
    ApplyAll(true);
}

void WatchStop(const wchar_t* outFile) {
    if (!g_active) return;
    ApplyAll(false);
    g_active = 0;
    FILE* f = _wfopen(outFile, L"w");
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& kv : g_hits) {
        const Hit& h = kv.second;
        fprintf(f, "[%s] eip=%06X hits=%d tid=%lu stack:", g_label[kv.first.first], kv.first.second, h.count, h.tid);
        for (int i = 0; i < h.nstack; ++i) fprintf(f, " %06X", h.stack[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    LOGI("watch: wrote %zu unique writers", g_hits.size());
}

bool WatchActive() { return g_active != 0; }

} // namespace avr
