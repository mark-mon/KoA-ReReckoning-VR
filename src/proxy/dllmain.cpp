// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Amalur VR - loaded by koa.exe as dinput8.dll or winmm.dll (proxy to the system DLL, see proxy.cpp).
#include "globals.h"
#include "d3dhooks.h"
#include "hookutil.h"
#include "game.h"
#include "input.h"
#include "stereo.h"
#include "xrlink.h"
#include "proxy.h"
#include "../common/version.h"

namespace avr {
Globals& G() { static Globals g; return g; }
}
using namespace avr;

static bool g_primary = false;

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
    ProxyChainload(G().cfg);
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
        if (ProxySetup(h)) { g_primary = true; Init(h); }   // a second copy of the mod only forwards
    } else if (reason == DLL_PROCESS_DETACH && g_primary) {
        XrLinkShutdown();
        LOGI("Unloading");
        LogFlush();
    }
    return TRUE;
}
