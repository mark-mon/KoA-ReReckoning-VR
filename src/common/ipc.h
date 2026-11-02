// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
// Shared-memory protocol between the 32-bit game mod (dinput8.dll) and the 64-bit OpenXR host.
// All fields are naturally aligned and fixed-size so both bitnesses agree on the layout.
#include <cstdint>

namespace avr {

constexpr uint32_t kIpcMagic = 0x52564141;   // 'AAVR'
constexpr uint32_t kIpcVersion = 4;
constexpr int kSlots = 3;                     // texture ring size
#define AVR_SHM_NAME L"Local\\AmalurVR_Shared_v4"
#define AVR_EVT_FRAME L"Local\\AmalurVR_FrameReady_v4"      // game -> host: a slot was submitted
#define AVR_EVT_HOSTFRAME L"Local\\AmalurVR_HostFrame_v4"     // host -> game: new poses for a new display frame

struct IpcPose { float px, py, pz; float qx, qy, qz, qw; };   // OpenXR convention (RH, -Z forward, meters)
struct IpcFov { float left, right, up, down; };               // radians (left/down negative)

enum HostState : uint32_t { HOST_NONE = 0, HOST_STARTING = 1, HOST_READY = 2, HOST_RUNNING = 3, HOST_FOCUSED = 4, HOST_ERROR = 100 };
enum SlotFlags : uint32_t { SLOT_HUD_VISIBLE = 1, SLOT_MENU_MODE = 2 };

struct alignas(8) IpcShared {
    uint32_t magic;
    uint32_t version;
    uint32_t gamePid;
    uint32_t hostPid;

    // ---- host -> game
    volatile uint32_t hostState;
    volatile uint32_t hostHeartbeat;          // incremented every host frame
    uint32_t recWidth, recHeight;              // recommended per-eye size
    uint64_t adapterLuid;                      // GPU the runtime wants
    volatile uint32_t poseSeq;                 // seqlock: odd while writing
    uint32_t pad0;
    IpcPose eyePose[2];                        // predicted eye poses relative to the recentered reference
    IpcFov eyeFov[2];
    int64_t predictedTime;
    char runtimeName[64];
    char systemName[64];
    char hostError[128];

    // ---- game -> host
    volatile uint32_t gameState;               // 1 = textures created
    uint32_t texWidth, texHeight;              // per-eye size; atlas is 3*texWidth x texHeight [L | R | HUD]
    uint32_t texFormat;                        // DXGI_FORMAT
    uint64_t sharedHandle[kSlots];             // legacy shared handles (D3D11 keyed mutex), 32-bit values
    volatile uint32_t submitSeq;               // incremented after each submitted slot
    volatile int32_t lastSlot;
    IpcPose slotPose[kSlots][2];               // poses the slot was rendered with
    IpcFov slotFov[kSlots][2];
    int64_t slotTime[kSlots];
    uint32_t slotFlags[kSlots];

    // ---- control / settings (game -> host)
    volatile uint32_t recenterRequest;         // game increments; host recenters when it changes
    float hudDistance, hudWidth, hudOffsetY;
    uint32_t hudAnchor;                        // 0 = world (fixed at the recentre point), 1 = head-locked
    float menuDistance, menuWidth;             // panel for the game's own full-screen menus
    uint32_t hostLogLevel;
    volatile uint32_t quitRequest;

    // ---- in-headset settings menu (vrkit), game -> host
    uint64_t panelHandle;                      // legacy shared handle, R8G8B8A8_UNORM, keyed mutex (key 0 both sides)
    uint32_t panelW, panelH;
    volatile uint32_t panelGen;                // changes when the texture is (re)created
    volatile uint32_t panelSeq;                // incremented after each update of its contents
    volatile uint32_t panelVisible;
    uint32_t panelAnchor;                      // 0 = world (placed where you looked when it opened), 1 = head
    float panelDistance, panelWidth, panelOffsetY;

    // ---- eye tracking (XR_EXT_eye_gaze_interaction), host -> game
    volatile uint32_t gazeSupported;           // the runtime reports eye tracking
    volatile uint32_t gazeSeq;                 // incremented on every valid sample
    float gazeTanX, gazeTanY;                  // gaze direction relative to the head, as tangents (x right, y up)
    uint32_t debugFakeGaze;                    // game -> host: synthesise a circling gaze (testing without a tracker)
};

} // namespace avr
