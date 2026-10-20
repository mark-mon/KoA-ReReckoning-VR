// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include <string>

namespace avr {

void InstallD3DHooks();      // patches koa.exe IAT for D3D11CreateDevice
void RequestCapture();        // capture the next full frame
const std::wstring* ActiveCaptureDir(); // non-null while a capture frame is being recorded

// Set while the mod itself issues D3D calls so hooks pass straight through.
extern thread_local int t_internal;
struct InternalScope { InternalScope() { ++t_internal; } ~InternalScope() { --t_internal; } };

} // namespace avr
