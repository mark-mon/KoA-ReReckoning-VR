// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
// Live user settings. Loaded from config.txt at start-up (SettingsLoad), edited by the in-headset menu
// (menu_options.cpp) and read every frame by the code that uses them, so most changes apply immediately.
#include <string>
#include "../../third_party/vrkit/vrkit_menu_placement.h"

namespace avr {

struct VrSettings {
    // View
    float worldScale = 1.0f;
    float ipdScale = 1.0f;
    bool pitchLock = true;
    int gameFovVertical = 80;          // applied by the game when it reads its settings (restart)
    // Display
    float renderScale = 1.0f;          // restart
    float minRenderAspect = 1.333f;    // restart
    bool desktopMirror = true;
    // HUD panel
    float hudDistance = 1.6f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    int hudAnchor = 0;                 // 0 = World, 1 = Head
    // Panel for the game's own full-screen menus
    float menuDistance = 2.0f, menuWidth = 2.4f;
    // Settings menu panel (vrkit)
    vrkit::menu::Placement panel{1.0f, 0.8f, -0.05f, 0};
    // Foveated rendering (NVIDIA VRS)
    int foveation = 0;                 // 0 = off, 1 = fixed, 2 = eye-tracked
    int foveationStrength = 1;         // 0 = low, 1 = medium, 2 = high
    float foveationInner = 0.45f;      // full-rate radius (1.0 = half the image width)
    float foveationOuter = 0.80f;      // lowest-rate radius
    bool debugFakeGaze = false;
    // Settings menu input
    std::string menuChord = "l3r3";    // l3r3 | ltrt | none
    int menuHoldMs = 300;
};

VrSettings& VS();
void SettingsLoad();
void SettingsPushToHost();   // HUD / panel placement etc. -> shared memory (cheap; call after a change)

} // namespace avr
