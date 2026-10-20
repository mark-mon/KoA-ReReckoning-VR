// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
// Game-specific addresses (RVAs relative to koa.exe base) and engine hooks.
#include <windows.h>
#include <cstdint>

namespace avr {

// koa.exe (Steam, Re-Reckoning, version cs:15032)
namespace rva {
constexpr uint32_t RenderView = 0x91F280;        // __thiscall Renderer::RenderView(a1..a5), ret 0x14
constexpr uint32_t SetupViewConstants = 0x8FE260; // uploads per-view constant buffer
constexpr uint32_t FillViewCB = 0x8FADB0;        // writes camera data into CB
constexpr uint32_t CopyObjectCBCall = 0x89669A;  // call memcpy(mappedCB4, packetConstants, n) in the draw-packet executor
constexpr uint32_t RenderOverlays = 0x912B20;    // __thiscall, 5 args, ret 0x14: in-world/screen overlays drawn after tonemap
}

inline uintptr_t GameBase() { return (uintptr_t)GetModuleHandleW(nullptr); }
template <class T> inline T GameAddr(uint32_t r) { return (T)(GameBase() + r); }

const char* RttiName(const void* obj); // class name of a polymorphic object, or nullptr
void InstallGameHooks();
void PatchPersonalIni();
void InstallScreenSpoof();
void GameOnCaptureBegin();
void GameOnCaptureEnd(const wchar_t* dir);
extern int g_renderViewCallsThisFrame;
void GameOnPresent();

} // namespace avr
