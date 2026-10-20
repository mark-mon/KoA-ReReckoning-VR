// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Game-side link to AmalurVR_XrHost.exe (64-bit OpenXR process) via shared memory.
#include "xrlink.h"
#include "globals.h"
#include <cstdio>

namespace avr {

static HANDLE g_map = nullptr, g_evt = nullptr, g_hostProc = nullptr, g_evtHost = nullptr;
static IpcShared* g_shm = nullptr;
static uint32_t g_lastBeat = 0;
static DWORD g_lastBeatTick = 0;

void XrLinkInit() {
    static bool done = false;
    if (done) return;
    done = true;
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(IpcShared), AVR_SHM_NAME);
    if (!g_map) { LOGE("xrlink: CreateFileMapping failed %lu", GetLastError()); return; }
    g_shm = (IpcShared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcShared));
    g_evt = CreateEventW(nullptr, FALSE, FALSE, AVR_EVT_FRAME);
    g_evtHost = CreateEventW(nullptr, FALSE, FALSE, AVR_EVT_HOSTFRAME);
    if (!g_shm) return;
    ZeroMemory(g_shm, sizeof(IpcShared));
    auto& c = G().cfg;
    g_shm->version = kIpcVersion;
    g_shm->gamePid = GetCurrentProcessId();
    g_shm->hudDistance = c.GetFloat("hud_distance", 1.6f);
    g_shm->hudWidth = c.GetFloat("hud_width", 1.6f);
    g_shm->hudOffsetY = c.GetFloat("hud_offset_y", -0.05f);
    g_shm->hudFollowHead = c.GetBool("hud_follow_head", false);
    g_shm->menuDistance = c.GetFloat("menu_distance", 2.0f);
    g_shm->menuWidth = c.GetFloat("menu_width", 2.4f);
    g_shm->hostLogLevel = c.GetInt("log_level", 2);
    g_shm->lastSlot = -1;
    MemoryBarrier();
    g_shm->magic = kIpcMagic;

    if (!c.GetBool("vr_enabled", true)) return;
    std::wstring exe = G().gameDir + L"AmalurVR_XrHost.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { LOGW("xrlink: %S not found - running without VR (debug stereo)", exe.c_str()); return; }
    wchar_t cmd[MAX_PATH * 2];
    swprintf(cmd, MAX_PATH * 2, L"\"%s\" --pid %lu --dir \"%s\"", exe.c_str(), GetCurrentProcessId(), G().gameDir.c_str());
    // strip trailing backslash inside quotes
    std::wstring cs = cmd;
    size_t bs = cs.rfind(L"\\\"");
    if (bs != std::wstring::npos) cs.erase(bs, 1);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cs[0], nullptr, nullptr, FALSE, 0, nullptr, G().gameDir.c_str(), &si, &pi)) {
        g_hostProc = pi.hProcess;
        CloseHandle(pi.hThread);
        LOGI("xrlink: launched XR host pid %lu", pi.dwProcessId);
    } else {
        LOGE("xrlink: failed to launch XR host (%lu)", GetLastError());
    }
}

void XrLinkShutdown() {
    if (g_shm) g_shm->quitRequest = 1;
}

bool XrHostConnected() {
    if (!g_shm || g_shm->hostState < HOST_READY || g_shm->hostState >= HOST_ERROR) return false;
    uint32_t b = g_shm->hostHeartbeat;
    DWORD now = GetTickCount();
    if (b != g_lastBeat) { g_lastBeat = b; g_lastBeatTick = now; }
    return now - g_lastBeatTick < 2000;
}

bool XrWaitRecommendedSize(unsigned* w, unsigned* h, unsigned timeoutMs) {
    if (!g_shm || !g_hostProc) return false;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeoutMs) {
        if (g_shm->hostState == HOST_ERROR) { LOGE("xrlink: host error: %s", g_shm->hostError); return false; }
        if (g_shm->recWidth && g_shm->recHeight) {
            *w = g_shm->recWidth; *h = g_shm->recHeight;
            LOGI("xrlink: host recommends %ux%u per eye (runtime '%s', system '%s')", *w, *h, g_shm->runtimeName, g_shm->systemName);
            return true;
        }
        if (WaitForSingleObject(g_hostProc, 50) == WAIT_OBJECT_0) { LOGE("xrlink: XR host exited early"); return false; }
    }
    LOGW("xrlink: timed out waiting for the XR host");
    return false;
}

bool XrGetLatestPoses(IpcPose pose[2], IpcFov fov[2], int64_t* time) {
    if (!XrHostConnected()) return false;
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t s0 = g_shm->poseSeq;
        if (s0 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(pose, g_shm->eyePose, sizeof(IpcPose) * 2);
        memcpy(fov, g_shm->eyeFov, sizeof(IpcFov) * 2);
        *time = g_shm->predictedTime;
        MemoryBarrier();
        if (g_shm->poseSeq == s0) return fov[0].right != 0;
    }
    return false;
}

void XrPublishTextures(unsigned w, unsigned h, unsigned format, const uint64_t handles[kSlots]) {
    if (!g_shm) return;
    g_shm->texWidth = w; g_shm->texHeight = h; g_shm->texFormat = format;
    for (int i = 0; i < kSlots; ++i) g_shm->sharedHandle[i] = handles[i];
    MemoryBarrier();
    g_shm->gameState = g_shm->gameState + 1;   // generation
    LOGI("xrlink: published %d shared atlases %ux%u", kSlots, w * 3, h);
}

void XrSubmitSlot(int slot, const IpcPose pose[2], const IpcFov fov[2], int64_t time, uint32_t flags) {
    if (!g_shm) return;
    memcpy(g_shm->slotPose[slot], pose, sizeof(IpcPose) * 2);
    memcpy(g_shm->slotFov[slot], fov, sizeof(IpcFov) * 2);
    g_shm->slotTime[slot] = time;
    g_shm->slotFlags[slot] = flags;
    MemoryBarrier();
    g_shm->lastSlot = slot;
    g_shm->submitSeq = g_shm->submitSeq + 1;
    if (g_evt) SetEvent(g_evt);
}

int XrLastSubmittedSlot() { return g_shm ? g_shm->lastSlot : -1; }

void XrRequestRecenter() {
    if (g_shm) { g_shm->recenterRequest = g_shm->recenterRequest + 1; LOGI("xrlink: recenter requested"); }
}

} // namespace avr

namespace avr {
bool XrWaitHostFrame(unsigned ms) {
    if (!g_evtHost || !XrHostConnected() || g_shm->hostState < HOST_RUNNING) return false;
    return WaitForSingleObject(g_evtHost, ms) == WAIT_OBJECT_0;
}
}
