// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include "../common/config.h"
#include "../common/log.h"

namespace avr {

struct Globals {
    std::wstring gameDir;   // folder containing koa.exe, trailing backslash
    std::wstring outDir;    // gameDir + L"AmalurVR\\"
    Config cfg;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;   // immediate context
    IDXGISwapChain* swapchain = nullptr;
    HWND hwnd = nullptr;
    uint64_t frame = 0;
};

Globals& G();

} // namespace avr
