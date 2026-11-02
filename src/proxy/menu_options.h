// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <d3d11.h>

namespace avr {
// Once per frame on the render thread (Present hook): polls the menu input, redraws it while open and hands
// it to the XR host. Returns the menu texture while the menu is open (for the monitor preview), else null.
ID3D11Texture2D* MenuOnPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx);
// Back + left stick click: switch the HUD panel between World and Head anchoring (saved to config.txt).
void MenuToggleHudAnchor();
}
