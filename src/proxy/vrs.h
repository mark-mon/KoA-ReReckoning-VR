// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <d3d11.h>

namespace avr {
// Foveated rendering (NVIDIA Variable Rate Shading through NVAPI). On every render-target bind of the
// immediate context: reduced shading in the periphery while an eye's scene is rendered, off otherwise.
void VrsOnSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
// Turn VRS off now (before overlays / HUD / our own passes, end of frame).
void VrsUnbind(ID3D11DeviceContext* ctx);
void VrsEndOfFrame(ID3D11DeviceContext* ctx);
const char* VrsStatus();   // for the menu: "off", "on", "unavailable", ...
}
