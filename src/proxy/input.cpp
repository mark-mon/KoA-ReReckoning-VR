// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// XInput interception: test automation (auto-load the last save) and VR control combos.
#include "input.h"
#include "globals.h"
#include "hookutil.h"
#include "d3dhooks.h"
#include <xinput.h>

namespace avr {

typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
static PFN_XInputGetState o_XInputGetState = nullptr;

static bool g_autoload = false;
static DWORD g_autoStart = 0;       // tick when autoload begins pressing
static DWORD g_autoEnd = 0;
static DWORD g_autoCaptureAt = 0;
static DWORD g_packet = 1;
static int g_autoButton = 0;
static DWORD g_autoButtonAt = 0;
static volatile LONG g_combo = 0;   // bitmask of combos seen since last poll
static WORD g_lastButtons = 0;

static bool AutoPressing(DWORD now) {
    if (!g_autoload || !g_autoStart || now < g_autoStart || now > g_autoEnd) return false;
    // press A for 150ms every 2s
    return ((now - g_autoStart) % 2000) < 150;
}

static DWORD WINAPI H_XInputGetState(DWORD idx, XINPUT_STATE* st) {
    DWORD r = o_XInputGetState ? o_XInputGetState(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (idx == 0 && st) {
        DWORD now = GetTickCount();
        if (r == ERROR_SUCCESS) {
            WORD b = st->Gamepad.wButtons;
            // Recenter: hold Back + click right stick
            if ((b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_RIGHT_THUMB) && !(g_lastButtons & XINPUT_GAMEPAD_RIGHT_THUMB))
                InterlockedOr(&g_combo, COMBO_RECENTER);
            // Toggle HUD placement: hold Back + click left stick
            if ((b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_LEFT_THUMB) && !(g_lastButtons & XINPUT_GAMEPAD_LEFT_THUMB))
                InterlockedOr(&g_combo, COMBO_HUD_TOGGLE);
            g_lastButtons = b;
        }
        if (g_autoload && g_autoStart) {
            // test automation: present a virtual pad (kept connected afterwards so the game doesn't pause)
            if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); r = ERROR_SUCCESS; st->dwPacketNumber = g_packet; }
            if (now <= g_autoEnd) {
                st->dwPacketNumber = ++g_packet;
                if (AutoPressing(now)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
            } else if (g_autoButton && g_autoButtonAt && now >= g_autoButtonAt && now < g_autoButtonAt + 150) {
                st->dwPacketNumber = ++g_packet;
                st->Gamepad.wButtons |= (WORD)g_autoButton;   // e.g. open a menu before the auto capture
            }
        }
    }
    return r;
}

int TakeCombos() { return (int)InterlockedExchange(&g_combo, 0); }

void InputOnPresent() {
    DWORD now = GetTickCount();
    if (g_autoload && !g_autoStart) {
        int delay = G().cfg.GetInt("autoload_delay_ms", 6000);
        int dur = G().cfg.GetInt("autoload_duration_ms", 40000);
        g_autoStart = now + delay;
        g_autoEnd = g_autoStart + dur;
        int cap = G().cfg.GetInt("autoload_capture_after_ms", 0);
        if (cap > 0) g_autoCaptureAt = g_autoEnd + cap;
        g_autoButton = G().cfg.GetInt("autoload_button", 0);
        if (g_autoButton) g_autoButtonAt = g_autoEnd + G().cfg.GetInt("autoload_button_after_ms", 3000);
        LOGI("autoload: pressing A from +%dms for %dms (capture at +%dms)", delay, dur, cap);
    }
    if (g_autoCaptureAt && now >= g_autoCaptureAt) {
        g_autoCaptureAt = 0;
        LOGI("autoload: requesting capture");
        RequestCapture();
    }
}

void InstallInputHooks() {
    g_autoload = G().cfg.GetBool("autoload", false);
    void* p = HookIATOrdinal(GetModuleHandleW(nullptr), "xinput1_3.dll", 2, (void*)&H_XInputGetState);
    if (p) { o_XInputGetState = (PFN_XInputGetState)p; LOGI("Hooked XInputGetState (ordinal 2)"); }
    else LOGW("XInputGetState import not found");
}

} // namespace avr
