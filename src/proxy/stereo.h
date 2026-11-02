// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace avr {

// Called from the D3D hooks / game hooks.
void StereoInit();                                           // after config load
bool StereoWantsSwapchainSize(UINT* w, UINT* h);              // override swapchain size (per-eye render size)
void StereoOnDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx);
void StereoOnSwapchain(IDXGISwapChain* sc);
void StereoBeforePresent(IDXGISwapChain* sc);

// Engine integration (called from game.cpp hooks)
typedef void(__fastcall* PFN_EngineFn5)(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5);
void StereoRenderView(PFN_EngineFn5 orig, void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
void StereoOverlay(PFN_EngineFn5 orig, void* self, void* a1, void* a2, void* a3, void* a4, void* a5);

void StereoCaptureDump(const wchar_t* dir);
int StereoEyePass();                                   // eye being rendered (0/1), -1 outside the eye passes
bool StereoEyeTangents(int eye, float t[4]);           // this frame's eye FOV as tangents: left, right, up, down
void StereoRenderSize(UINT* w, UINT* h);               // per-eye render size
bool StereoHudPhase();
void StereoReleaseBackbuffer();
void StereoNoteRenderTargets(UINT n, ID3D11RenderTargetView* const* rtv);
void* __cdecl StereoCopyObjectConstants(void* dst, const void* src, size_t n);
ID3D11BlendState* StereoHudBlend(ID3D11BlendState* b);                  // save eye/HUD images

} // namespace avr
