// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Amalur VR - loaded by koa.exe as dinput8.dll (proxy to the system dinput8.dll).
#include "globals.h"
#include "d3dhooks.h"
#include "hookutil.h"
#include "game.h"
#include "input.h"
#include "stereo.h"
#include "xrlink.h"
#include "../common/version.h"

namespace avr {
Globals& G() { static Globals g; return g; }
}
using namespace avr;

static HMODULE g_realDinput = nullptr;

static FARPROC RealProc(const char* name) {
    if (!g_realDinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(path);
        if (!g_realDinput) LOGE("Could not load system dinput8.dll");
    }
    return g_realDinput ? GetProcAddress(g_realDinput, name) : nullptr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, const IID& riid, void** out, IUnknown* outer) {
    typedef HRESULT(WINAPI* F)(HINSTANCE, DWORD, const IID&, void**, IUnknown*);
    auto f = (F)RealProc("DirectInput8Create");
    return f ? f(h, v, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    typedef HRESULT(WINAPI* F)();
    auto f = (F)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(const IID& c, const IID& i, void** o) {
    typedef HRESULT(WINAPI* F)(const IID&, const IID&, void**);
    auto f = (F)RealProc("DllGetClassObject");
    return f ? f(c, i, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer() { return E_NOTIMPL; }
HRESULT WINAPI Proxy_DllUnregisterServer() { return E_NOTIMPL; }
void* WINAPI Proxy_GetdfDIJoystick() {
    typedef void*(WINAPI* F)();
    auto f = (F)RealProc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

static void Init(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    G().gameDir = dir;
    G().outDir = dir + L"AmalurVR\\";
    CreateDirectoryW(G().outDir.c_str(), nullptr);

    std::wstring cfgPath = dir + L"config.txt";
    WriteDefaultConfig(cfgPath.c_str());
    G().cfg.Load(cfgPath.c_str());
    LogInit((G().outDir + L"amalurvr.log").c_str(), G().cfg.GetInt("log_level", LOG_INFO));
    LOGI("Amalur VR " AVR_VERSION " (" __DATE__ " " __TIME__ ") loaded into %S (base %p)", path, (void*)GameModule().base);
    G().cfg.Dump();
    StereoInit();
    PatchPersonalIni();
    InstallScreenSpoof();
    InstallD3DHooks();
    InstallGameHooks();
    InstallInputHooks();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Init(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        XrLinkShutdown();
        LOGI("Unloading");
        LogFlush();
    }
    return TRUE;
}
