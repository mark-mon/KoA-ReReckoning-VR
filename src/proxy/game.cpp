// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Engine-level hooks for Kingdoms of Amalur (koa.exe).
#include "game.h"
#include "globals.h"
#include "hookutil.h"
#include "watch.h"
#include "stereo.h"
#include "xrlink.h"
#include "../../third_party/minhook/include/MinHook.h"
#include <cstdio>
#include <string>

namespace avr {

int g_renderViewCallsThisFrame = 0;
static int g_renderViewLastFrame = 0;

const char* RttiName(const void* obj) {
    __try {
        if (!obj) return nullptr;
        const uintptr_t* vt = *(const uintptr_t* const*)obj;
        const auto& g = GameModule();
        if ((uintptr_t)vt < g.base || (uintptr_t)vt >= g.end) return nullptr;
        const uint8_t* col = (const uint8_t*)vt[-1];
        if ((uintptr_t)col < g.base || (uintptr_t)col >= g.end) return nullptr;
        const uint8_t* td = *(const uint8_t* const*)(col + 12);
        if ((uintptr_t)td < g.base || (uintptr_t)td >= g.end) return nullptr;
        return (const char*)(td + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------------------------------------------------------------- probe of RenderView
typedef void(__fastcall* PFN_RenderView)(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5);
static PFN_RenderView o_RenderView = nullptr;

static bool g_probeArmed = false;
static std::string g_probe;

static void P(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    g_probe += b;
}

static void DumpMem(const char* label, const void* p, int bytes) {
    P("%s @ %p  rtti=%s\n", label, p, RttiName(p) ? RttiName(p) : "-");
    if (!p) return;
    __try {
        const uint8_t* b = (const uint8_t*)p;
        for (int off = 0; off < bytes; off += 16) {
            const float* f = (const float*)(b + off);
            const uint32_t* u = (const uint32_t*)(b + off);
            P("  +%03X: %08X %08X %08X %08X | %12.5g %12.5g %12.5g %12.5g\n", off, u[0], u[1], u[2], u[3], f[0], f[1], f[2], f[3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        P("  <fault>\n");
    }
}

static void* g_lastCamera = nullptr;
static int g_watchFramesLeft = 0;

static void __fastcall H_RenderView(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5) {
    g_renderViewCallsThisFrame++;
    __try { g_lastCamera = ((void**)a3)[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_probeArmed) {
        P("RenderView #%d this=%p a1=%p a2=%p a3=%p a4=%p a5=%p ret=%06X\n", g_renderViewCallsThisFrame, self, a1, a2, a3, a4, a5,
          (unsigned)((uintptr_t)_ReturnAddress() - GameBase()));
        P("  this rtti=%s  [this+4]=%p rtti=%s\n", RttiName(self) ? RttiName(self) : "-", *(void**)((uint8_t*)self + 4),
          RttiName(*(void**)((uint8_t*)self + 4)) ? RttiName(*(void**)((uint8_t*)self + 4)) : "-");
        __try {
            void* cam = ((void**)a3)[0];
            void* other = ((void**)a3)[1];
            DumpMem("a2", a2, 0x80);
            DumpMem("a3", a3, 0x60);
            DumpMem("camera = a3[0]", cam, 0x360);
            DumpMem("a3[1]", other, 0x200);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            P("  <fault reading args>\n");
        }
    }
    StereoRenderView((PFN_EngineFn5)o_RenderView, self, a1, a2, a3, a4, a5);
}

static PFN_RenderView o_Overlays = nullptr;
static void __fastcall H_Overlays(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5) {
    StereoOverlay((PFN_EngineFn5)o_Overlays, self, a1, a2, a3, a4, a5);
}

void GameOnCaptureBegin() {
    g_probe.clear(); g_probeArmed = true;
    if (G().cfg.GetBool("watch_camera", false) && g_lastCamera && !WatchActive()) {
        uintptr_t c = (uintptr_t)g_lastCamera;
        int sel = G().cfg.GetInt("watch_set", 0);
        uintptr_t a[4]; const char* l[4];
        if (sel == 0) { a[0] = c + 0x04; l[0] = "pos.x"; a[1] = c + 0x44; l[1] = "view.m00"; a[2] = c + 0x104; l[2] = "viewproj.m00"; a[3] = c + 0x1C0; l[3] = "plane0"; }
        else { a[0] = c + 0x2C; l[0] = "fov"; a[1] = c + 0xC4; l[1] = "proj.m00"; a[2] = c + 0x14; l[2] = "target.x"; a[3] = c + 0x190; l[3] = "vec190"; }
        WatchStart(a, l, 4);
        g_watchFramesLeft = 90;
    }
}

void GameOnCaptureEnd(const wchar_t* dir) {
    g_probeArmed = false;
    std::wstring p = std::wstring(dir) + L"probe.txt";
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (f) { fwrite(g_probe.data(), 1, g_probe.size(), f); fclose(f); }
}

void GameOnPresent() {
    if (g_watchFramesLeft > 0 && --g_watchFramesLeft == 0) WatchStop((G().outDir + L"watch.txt").c_str());
    static int logged = 0;
    if (g_renderViewCallsThisFrame != g_renderViewLastFrame && logged < 20) {
        LOGI("RenderView calls per frame: %d", g_renderViewCallsThisFrame);
        logged++;
    }
    g_renderViewLastFrame = g_renderViewCallsThisFrame;
    g_renderViewCallsThisFrame = 0;
}

void InstallIniHooks();
void InstallGameHooks() {
    InstallIniHooks();
    void* target = GameAddr<void*>(rva::RenderView);
    // sanity: expect "sub esp, 0xc8" (81 EC C8 00 00 00)
    const uint8_t* b = (const uint8_t*)target;
    if (!(b[0] == 0x81 && b[1] == 0xEC && b[2] == 0xC8)) {
        LOGE("RenderView signature mismatch at %p (%02X %02X %02X) - wrong game version?", target, b[0], b[1], b[2]);
        return;
    }
    MH_Initialize();
    if (MH_CreateHook(target, (void*)&H_RenderView, (void**)&o_RenderView) == MH_OK && MH_EnableHook(target) == MH_OK)
        LOGI("Hooked RenderView at %p", target);
    else
        LOGE("Failed to hook RenderView");
    // redirect the per-object constant copy (call rel32) to our wrapper
    {
        uint8_t* site = GameAddr<uint8_t*>(rva::CopyObjectCBCall);
        uintptr_t tgt = (uintptr_t)site + 5 + *(int32_t*)(site + 1);
        if (site[0] == 0xE8 && tgt - GameBase() == 0xDBDD20) {
            DWORD old;
            VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
            *(int32_t*)(site + 1) = (int32_t)((uintptr_t)&StereoCopyObjectConstants - ((uintptr_t)site + 5));
            VirtualProtect(site, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), site, 5);
            LOGI("Patched object-constant copy at %p", site);
        } else {
            LOGE("Object-constant copy site mismatch at %p (%02X) - characters/props will not be stereo", site, site[0]);
        }
    }
    void* ov = GameAddr<void*>(rva::RenderOverlays);
    const uint8_t* ob = (const uint8_t*)ov;
    if (ob[0] == 0x81 && ob[1] == 0xEC && ob[2] == 0xBC && MH_CreateHook(ov, (void*)&H_Overlays, (void**)&o_Overlays) == MH_OK && MH_EnableHook(ov) == MH_OK)
        LOGI("Hooked RenderOverlays at %p", ov);
    else
        LOGE("Failed to hook RenderOverlays (%02X %02X %02X)", ob[0], ob[1], ob[2]);
}

} // namespace avr

// ---------------------------------------------------------------- personal.ini overrides
namespace avr {

typedef DWORD(WINAPI* PFN_GetPrivateProfileStringW)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
typedef BOOL(WINAPI* PFN_WritePrivateProfileStringW)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
static PFN_GetPrivateProfileStringW o_GetPPS = nullptr;
static PFN_WritePrivateProfileStringW o_WritePPS = nullptr;

static bool IniOverride(LPCWSTR key, wchar_t* out, size_t cap) {
    if (!key) return false;
    UINT w = 0, h = 0;
    if (!_wcsicmp(key, L"Display Width") || !_wcsicmp(key, L"Display Height")) {
        XrLinkInit();
        if (!StereoWantsSwapchainSize(&w, &h)) return false;
        swprintf(out, cap, L"%u", !_wcsicmp(key, L"Display Width") ? w : h);
        return true;
    }
    if (!G().cfg.GetBool("vr_enabled", true)) return false;
    if (!_wcsicmp(key, L"Display Fullscreen") && G().cfg.GetBool("force_windowed", false)) { wcscpy_s(out, cap, L"0"); return true; }
    if (!_wcsicmp(key, L"Display Vsync")) { wcscpy_s(out, cap, L"0"); return true; }
    if (wcsstr(key, L"camera_fov_vertical") == key) {
        int fov = G().cfg.GetInt("game_fov_vertical", 0);
        if (fov > 0) { swprintf(out, cap, L"%d", fov); return true; }
    }
    return false;
}

static DWORD WINAPI H_GetPPS(LPCWSTR app, LPCWSTR key, LPCWSTR def, LPWSTR ret, DWORD size, LPCWSTR file) {
    wchar_t v[64];
    if (ret && size && IniOverride(key, v, 64)) {
        wcsncpy_s(ret, size, v, _TRUNCATE);
        LOGI("ini override: %S = %S", key, ret);
        return (DWORD)wcslen(ret);
    }
    return o_GetPPS(app, key, def, ret, size, file);
}

static BOOL WINAPI H_WritePPS(LPCWSTR app, LPCWSTR key, LPCWSTR val, LPCWSTR file) {
    wchar_t v[64];
    if (key && IniOverride(key, v, 64)) { LOGD("ini write blocked: %S", key); return TRUE; }
    return o_WritePPS(app, key, val, file);
}

void InstallIniHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    void* p = HookIAT(exe, "kernel32.dll", "GetPrivateProfileStringW", (void*)&H_GetPPS);
    if (p) o_GetPPS = (PFN_GetPrivateProfileStringW)p;
    p = HookIAT(exe, "kernel32.dll", "WritePrivateProfileStringW", (void*)&H_WritePPS);
    if (p) o_WritePPS = (PFN_WritePrivateProfileStringW)p;
    LOGI("ini hooks: get=%p write=%p", o_GetPPS, o_WritePPS);
}

} // namespace avr

// ---------------------------------------------------------------- personal.ini file patch (before the engine parses it)
namespace avr {

static std::wstring PersonalIniPath() {
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return L"";
    return std::wstring(appdata) + L"\\kaiko\\koa\\personal.ini";
}

static bool ReadIniKey(const std::wstring& path, const wchar_t* key, int* out) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    char line[512]; bool found = false;
    char k[128]; snprintf(k, sizeof(k), "%S=", key);
    while (fgets(line, sizeof(line), f)) if (!strncmp(line, k, strlen(k))) { *out = atoi(line + strlen(k)); found = true; }
    fclose(f);
    return found;
}

static void WriteIniKeys(const std::wstring& path, int w, int h) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    std::string all, line;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        if (!strncmp(buf, "Display Width=", 14)) snprintf(buf, sizeof(buf), "Display Width=%d\n", w);
        else if (!strncmp(buf, "Display Height=", 15)) snprintf(buf, sizeof(buf), "Display Height=%d\n", h);
        all += buf;
    }
    fclose(f);
    f = _wfopen(path.c_str(), L"w");
    if (f) { fputs(all.c_str(), f); fclose(f); }
}

// Called from DllMain: makes the engine create its swapchain and render targets at the per-eye size.
void PatchPersonalIni() {
    std::wstring ini = PersonalIniPath();
    if (ini.empty() || GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { LOGW("personal.ini not found"); return; }
    std::wstring backup = G().outDir + L"personal_ini_display_backup.txt";
    int curW = 0, curH = 0;
    ReadIniKey(ini, L"Display Width", &curW);
    ReadIniKey(ini, L"Display Height", &curH);
    // remember the user's own (desktop) values once
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* b = _wfopen(backup.c_str(), L"w");
        if (b) { fprintf(b, "%d %d\n", curW, curH); fclose(b); }
    }
    if (!G().cfg.GetBool("vr_enabled", true)) {
        FILE* b = _wfopen(backup.c_str(), L"r");
        int bw = 0, bh = 0;
        if (b) { if (fscanf(b, "%d %d", &bw, &bh) != 2) bw = bh = 0; fclose(b); }
        if (bw && bh && (bw != curW || bh != curH)) { WriteIniKeys(ini, bw, bh); LOGI("personal.ini: restored desktop resolution %dx%d", bw, bh); }
        return;
    }
    UINT w = 0, h = 0;
    XrLinkInit();
    if (!StereoWantsSwapchainSize(&w, &h)) { LOGW("personal.ini: per-eye size unknown, leaving %dx%d", curW, curH); return; }
    if ((int)w != curW || (int)h != curH) {
        WriteIniKeys(ini, w, h);
        LOGI("personal.ini: Display %dx%d -> %ux%u (per-eye render size)", curW, curH, w, h);
    }
}

} // namespace avr

// ---------------------------------------------------------------- screen-size spoofing (engine resolution = per-eye size)
namespace avr {

typedef int(WINAPI* PFN_GetSystemMetrics)(int);
typedef BOOL(WINAPI* PFN_GetClientRect)(HWND, LPRECT);
static PFN_GetSystemMetrics o_GetSystemMetrics = nullptr;
static PFN_GetClientRect o_GetClientRect = nullptr;
static UINT g_spoofW = 0, g_spoofH = 0;

static int WINAPI H_GetSystemMetrics(int i) {
    if (g_spoofW && (i == SM_CXSCREEN || i == SM_CXFULLSCREEN || i == SM_CXVIRTUALSCREEN)) return (int)g_spoofW;
    if (g_spoofH && (i == SM_CYSCREEN || i == SM_CYFULLSCREEN || i == SM_CYVIRTUALSCREEN)) return (int)g_spoofH;
    return o_GetSystemMetrics(i);
}

static BOOL WINAPI H_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = o_GetClientRect(h, r);
    if (ok && r && g_spoofW && (h == G().hwnd || G().hwnd == nullptr)) {
        // only spoof top-level windows owned by this process (the game window)
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && !GetParent(h)) { r->left = r->top = 0; r->right = (LONG)g_spoofW; r->bottom = (LONG)g_spoofH; }
    }
    return ok;
}

void InstallScreenSpoof() {
    if (!G().cfg.GetBool("vr_enabled", true)) return;
    UINT w, h;
    if (!StereoWantsSwapchainSize(&w, &h)) return;
    g_spoofW = w; g_spoofH = h;
    HMODULE exe = GetModuleHandleW(nullptr);
    void* p = HookIAT(exe, "user32.dll", "GetSystemMetrics", (void*)&H_GetSystemMetrics);
    if (p) o_GetSystemMetrics = (PFN_GetSystemMetrics)p;
    p = HookIAT(exe, "user32.dll", "GetClientRect", (void*)&H_GetClientRect);
    if (p) o_GetClientRect = (PFN_GetClientRect)p;
    LOGI("screen spoof %ux%u (GetSystemMetrics=%p GetClientRect=%p)", w, h, o_GetSystemMetrics, o_GetClientRect);
}

} // namespace avr
