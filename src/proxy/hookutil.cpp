// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#include "hookutil.h"
#include <cstring>
#include <cstdint>
#include "../../third_party/minhook/include/MinHook.h"
#include "../common/log.h"

namespace avr {

void* HookVtableRaw(void** vtbl, int slot, void* fn) {
    void* orig = vtbl[slot];
    if (orig == fn) return nullptr;
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return orig;
}

// Inline-detours the function that the vtable slot points to (catches every caller,
// including other vtables sharing the implementation). Returns trampoline or nullptr.
void* HookVtable(void* obj, int slot, void* fn) {
    static bool init = [] { MH_STATUS s = MH_Initialize(); return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED; }();
    if (!init) return nullptr;
    void* target = (*(void***)obj)[slot];
    void* orig = nullptr;
    MH_STATUS st = MH_CreateHook(target, fn, &orig);
    if (st != MH_OK) {
        if (st != MH_ERROR_ALREADY_CREATED) LOGE("MH_CreateHook slot %d target %p failed: %d", slot, target, (int)st);
        return nullptr;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) { LOGE("MH_EnableHook slot %d failed: %d", slot, (int)st); return nullptr; }
    return orig;
}

void* HookIAT(HMODULE module, const char* dllName, const char* funcName, void* fn) {
    auto base = (uint8_t*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dllName) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto orig = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto name = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
            if (strcmp((char*)name->Name, funcName) != 0) continue;
            void* prev = (void*)thunk->u1.Function;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

void* HookIATOrdinal(HMODULE module, const char* dllName, WORD ordinal, void* fn) {
    auto base = (uint8_t*)module;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dllName) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto orig = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (!IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal) || IMAGE_ORDINAL(orig->u1.Ordinal) != ordinal) continue;
            void* prev = (void*)thunk->u1.Function;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

const ModuleRange& GameModule() {
    static ModuleRange r = [] {
        ModuleRange m{};
        auto base = (uint8_t*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        m.base = (uintptr_t)base;
        m.end = m.base + nt->OptionalHeader.SizeOfImage;
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (memcmp(sec->Name, ".text", 5) == 0) {
                m.textBase = m.base + sec->VirtualAddress;
                m.textEnd = m.textBase + sec->Misc.VirtualSize;
            }
        }
        return m;
    }();
    return r;
}

static bool LooksLikeReturnAddress(uintptr_t a) {
    const auto& g = GameModule();
    if (a < g.textBase + 6 || a >= g.textEnd) return false;
    const uint8_t* p = (const uint8_t*)a;
    if (p[-5] == 0xE8) return true;                         // call rel32
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;  // call [reg] / call reg
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;  // call [reg+disp8]
    if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;  // call [reg+disp32] / [abs]
    if (p[-4] == 0xFF && ((p[-3] >> 3) & 7) == 2) return true;  // call [sib+disp8]
    if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;  // call [sib+disp32]
    return false;
}

int ScanGameReturnAddresses(const void* stackStart, uint32_t* outRva, int max, int scanBytes) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t lo = (uintptr_t)stackStart & ~(uintptr_t)3;
    uintptr_t hi = (uintptr_t)tib->StackBase;
    if (lo + scanBytes < hi) hi = lo + scanBytes;
    int n = 0;
    for (uintptr_t p = lo; p + sizeof(uintptr_t) <= hi && n < max; p += sizeof(uintptr_t)) {
        uintptr_t v = *(uintptr_t*)p;
        if (LooksLikeReturnAddress(v)) outRva[n++] = (uint32_t)(v - GameModule().base);
    }
    return n;
}

} // namespace avr
