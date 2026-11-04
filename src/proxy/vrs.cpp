// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
//
// Foveated rendering with NVIDIA Variable Rate Shading (D3D11, through NVAPI without the SDK lib).
// Ported from the shared VR-mod toolkit (Star Ocean: The Divine Force VR / GranblueVR vrs.cpp).
//
// A shading-rate image (R8_UINT, one texel per 16x16 pixel tile) per eye selects one of three rates from a
// per-viewport table: 0 = full rate (centre), 1 = mid ring, 2 = outer ring. It is bound while an eye's scene
// is being rendered (inside one of the two RenderView passes, render target at the eye render size with a
// depth buffer bound) and unbound for everything else: shadow maps, the HUD/overlays, our own blits.
// The ring centre is the eye's optical axis in the image (from the OpenXR FOV), or the eye-gaze direction.
#include "vrs.h"
#include "settings.h"
#include "stereo.h"
#include "xrlink.h"
#include "globals.h"
#include "d3dhooks.h"
#include <cmath>
#include <vector>
#include <cstring>

namespace avr {
namespace {

// ---- NVAPI subset (layouts from the public NVAPI headers; identical for 32- and 64-bit apart from pointers)
using NvAPI_Status = int;
constexpr NvAPI_Status NVAPI_OK = 0;
using PFN_QueryInterface = void*(__cdecl*)(unsigned);
using PFN_Initialize = NvAPI_Status(__cdecl*)();

enum NvPixelShadingRate : int {
    NV_PSR_NULL = 0, NV_PSR_16X, NV_PSR_8X, NV_PSR_4X, NV_PSR_2X, NV_PSR_1X_PER_PIXEL,
    NV_PSR_1X_PER_2X1, NV_PSR_1X_PER_1X2, NV_PSR_1X_PER_2X2, NV_PSR_1X_PER_4X2, NV_PSR_1X_PER_2X4, NV_PSR_1X_PER_4X4,
};
struct NvViewportShadingRateDesc {          // NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1
    bool enableVariablePixelShadingRate;
    NvPixelShadingRate shadingRateTable[16];
};
struct NvViewportsShadingRateDesc {         // NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1
    unsigned version;
    unsigned numViewports;
    NvViewportShadingRateDesc* pViewports;
};
struct NvShadingRateResourceViewDesc {      // NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1
    unsigned version;
    DXGI_FORMAT Format;
    unsigned ViewDimension;                 // NV_SRRV_DIMENSION_TEXTURE2D = 4
    unsigned MipSlice, pad0, pad1;
};
struct ID3D11NvShadingRateResourceView;     // IUnknown-derived
using PFN_CreateSRRV = NvAPI_Status(__cdecl*)(ID3D11Device*, ID3D11Resource*, NvShadingRateResourceViewDesc*, ID3D11NvShadingRateResourceView**);
using PFN_SetSRRV = NvAPI_Status(__cdecl*)(IUnknown*, ID3D11NvShadingRateResourceView*);
using PFN_SetRates = NvAPI_Status(__cdecl*)(IUnknown*, NvViewportsShadingRateDesc*);

PFN_CreateSRRV p_CreateSRRV = nullptr;
PFN_SetSRRV p_SetSRRV = nullptr;
PFN_SetRates p_SetRates = nullptr;
bool g_nvTried = false, g_nvOk = false;

bool InitNvapi() {
    if (g_nvTried) return g_nvOk;
    g_nvTried = true;
#ifdef _WIN64
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
#else
    HMODULE m = LoadLibraryW(L"nvapi.dll");
#endif
    if (!m) { LOGI("foveated rendering: NVAPI not found (not an NVIDIA GPU?) - off"); return false; }
    auto qi = (PFN_QueryInterface)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) { LOGW("foveated rendering: nvapi_QueryInterface missing - off"); return false; }
    auto init = (PFN_Initialize)qi(0x0150e828);
    p_CreateSRRV = (PFN_CreateSRRV)qi(0x99ca2dff);
    p_SetSRRV = (PFN_SetSRRV)qi(0x1b0c2f83);
    p_SetRates = (PFN_SetRates)qi(0x34f7938f);
    if (!init || !p_CreateSRRV || !p_SetSRRV || !p_SetRates) { LOGW("foveated rendering: NVAPI VRS entry points missing - off"); return false; }
    NvAPI_Status s = init();
    if (s != NVAPI_OK) { LOGW("foveated rendering: NvAPI_Initialize failed (%d) - off", s); return false; }
    g_nvOk = true;
    LOGI("foveated rendering: NVAPI VRS available");
    return true;
}

struct EyePattern {
    ID3D11Texture2D* tex = nullptr;
    ID3D11NvShadingRateResourceView* srrv = nullptr;
    UINT tw = 0, th = 0;
    float key[8] = {};                       // inputs the current contents were built from
};
EyePattern g_eye[2];
bool g_bound = false;
int g_boundEye = -1;
int g_boundStrength = -1;
bool g_failed = false;
unsigned long long g_sceneBinds = 0, g_otherBinds = 0, g_eyeBinds[2] = {};

template <class T> void Rel(T*& p) { if (p) { reinterpret_cast<IUnknown*>(p)->Release(); p = nullptr; } }

void RatesFor(int strength, NvPixelShadingRate& mid, NvPixelShadingRate& outer) {
    switch (strength) {
    case 0:  mid = NV_PSR_1X_PER_2X1; outer = NV_PSR_1X_PER_2X2; break;   // Low
    case 2:  mid = NV_PSR_1X_PER_2X2; outer = NV_PSR_1X_PER_4X4; break;   // High
    default: mid = NV_PSR_1X_PER_2X2; outer = NV_PSR_1X_PER_4X2; break;   // Medium
    }
}

void SetRates(ID3D11DeviceContext* ctx, bool on) {
    NvViewportShadingRateDesc vp[16];
    NvPixelShadingRate mid, outer;
    RatesFor(VS().foveationStrength, mid, outer);
    for (auto& v : vp) {
        v.enableVariablePixelShadingRate = on;
        for (auto& r : v.shadingRateTable) r = NV_PSR_1X_PER_PIXEL;
        v.shadingRateTable[1] = mid;
        v.shadingRateTable[2] = outer;
    }
    NvViewportsShadingRateDesc d{};
    d.version = sizeof(NvViewportsShadingRateDesc) | (1u << 16);
    d.numViewports = 16;
    d.pViewports = vp;
    NvAPI_Status s = p_SetRates(ctx, &d);
    static int errs = 0;
    if (s != NVAPI_OK && errs++ < 3) LOGW("foveated rendering: RSSetViewportsPixelShadingRates failed (%d)", s);
}

// Builds or refreshes eye e's pattern for a w x h render target.
bool EnsurePattern(ID3D11DeviceContext* ctx, int e, UINT w, UINT h) {
    EyePattern& p = g_eye[e];
    const VrSettings& vs = VS();
    float t[4];
    float cx = 0.5f, cy = 0.5f, aspect = float(w) / float(h ? h : 1);
    const float tw0 = float((w + 15) / 16), th0 = float((h + 15) / 16);
    if (StereoEyeTangents(e, t) && t[1] > t[0] && t[2] > t[3]) {   // left, right, up, down
        float gx = 0.0f, gy = 0.0f;
        if (vs.foveation == 2 && XrGaze(&gx, &gy, 500)) {
            // keep the sharp area fully inside the image, then snap to whole tiles to avoid needless uploads
            gx = std::fmin(std::fmax(gx, t[0] * 0.8f), t[1] * 0.8f);
            gy = std::fmin(std::fmax(gy, t[3] * 0.8f), t[2] * 0.8f);
        }
        cx = (gx - t[0]) / (t[1] - t[0]);
        cy = (t[2] - gy) / (t[2] - t[3]);
        cx = std::floor(cx * tw0 + 0.5f) / tw0;
        cy = std::floor(cy * th0 + 0.5f) / th0;
        aspect = (t[1] - t[0]) / (t[2] - t[3]);   // tangent-space aspect, so the rings are round in angle
    }
    float key[8] = {float(w), float(h), cx, cy, vs.foveationInner, vs.foveationOuter, aspect, 0};
    UINT tw = (w + 15) / 16, th = (h + 15) / 16;
    if (p.tex && p.tw == tw && p.th == th && memcmp(key, p.key, sizeof(key)) == 0) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!p.tex || p.tw != tw || p.th != th) {
        Rel(p.srrv); Rel(p.tex);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UINT;
        d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &p.tex));
        if (ok) {
            NvShadingRateResourceViewDesc v{};
            v.version = sizeof(NvShadingRateResourceViewDesc) | (1u << 16);
            v.Format = DXGI_FORMAT_R8_UINT; v.ViewDimension = 4; v.MipSlice = 0;
            NvAPI_Status s = p_CreateSRRV(dev, p.tex, &v, &p.srrv);
            if (s != NVAPI_OK || !p.srrv) { LOGW("foveated rendering: CreateShadingRateResourceView failed (%d) - off (needs an RTX / GTX 16xx GPU)", s); ok = false; }
        }
        if (!ok) { Rel(p.srrv); Rel(p.tex); dev->Release(); g_failed = true; return false; }
        p.tw = tw; p.th = th;
    }
    dev->Release();
    std::vector<uint8_t> px(size_t(tw) * th);
    float inner = vs.foveationInner, outer = vs.foveationOuter;
    if (outer < inner) outer = inner;
    for (UINT y = 0; y < th; ++y)
        for (UINT x = 0; x < tw; ++x) {
            float u = (x + 0.5f) * 16.0f / w, v = (y + 0.5f) * 16.0f / h;
            float dx = (u - cx) * 2.0f, dy = (v - cy) * 2.0f / aspect;
            float r = std::sqrt(dx * dx + dy * dy);
            px[size_t(y) * tw + x] = r < inner ? 0 : (r < outer ? 1 : 2);
        }
    {
        InternalScope s;
        ctx->UpdateSubresource(p.tex, 0, nullptr, px.data(), tw, 0);
    }
    memcpy(p.key, key, sizeof(key));
    static unsigned uploads = 0, perEye[2] = {};
    ++uploads;
    if (++perEye[e] <= 3 || uploads % 3000 == 0)
        LOGI("foveated rendering: eye %d pattern %ux%u tiles, centre (%.3f, %.3f), rings %.2f / %.2f, strength %d, %s (upload %u)",
             e, tw, th, cx, cy, inner, outer, vs.foveationStrength, vs.foveation == 2 ? "eye-tracked" : "fixed", uploads);
    return true;
}

} // namespace

void VrsUnbind(ID3D11DeviceContext* ctx) {
    if (!g_bound || !ctx) return;
    SetRates(ctx, false);
    p_SetSRRV(ctx, nullptr);
    g_bound = false; g_boundEye = -1;
}

void VrsOnSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (!ctx) return;
    int eye = StereoEyePass();
    bool want = VS().foveation != 0 && !g_failed && eye >= 0 && !StereoHudPhase();
    if (!want) { if (g_bound) VrsUnbind(ctx); return; }
    if (!InitNvapi()) { g_failed = true; return; }
    // Scene pass: RT0 at the eye render size, with a depth buffer.
    bool scene = false;
    UINT w = 0, h = 0, rw = 0, rh = 0;
    StereoRenderSize(&rw, &rh);
    if (n && rtv && rtv[0] && dsv && rw) {
        ID3D11Resource* r = nullptr;
        rtv[0]->GetResource(&r);
        if (r) {
            D3D11_RESOURCE_DIMENSION dim;
            r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d;
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                scene = d.Width == rw && d.Height == rh && d.SampleDesc.Count == 1;
                w = d.Width; h = d.Height;
            }
            r->Release();
        }
    }
    if (!scene) { g_otherBinds++; VrsUnbind(ctx); return; }
    g_sceneBinds++;
    if (!EnsurePattern(ctx, eye, w, h)) { VrsUnbind(ctx); return; }
    int strength = VS().foveationStrength;
    if (g_bound && g_boundEye == eye && g_boundStrength == strength) return;
    g_eyeBinds[eye]++;
    p_SetSRRV(ctx, g_eye[eye].srrv);
    SetRates(ctx, true);
    g_bound = true; g_boundEye = eye; g_boundStrength = strength;
}

void VrsEndOfFrame(ID3D11DeviceContext* ctx) {
    VrsUnbind(ctx);
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last >= 10000) {
        last = now;
        if (VS().foveation || g_sceneBinds)
            LOGI("foveated rendering: %s, scene binds %llu (pattern binds left %llu, right %llu), other binds %llu (last 10s)",
                 VrsStatus(), g_sceneBinds, g_eyeBinds[0], g_eyeBinds[1], g_otherBinds);
        g_sceneBinds = g_otherBinds = g_eyeBinds[0] = g_eyeBinds[1] = 0;
    }
}

const char* VrsStatus() {
    if (!VS().foveation) return "off";
    if (g_failed) return g_nvTried && !g_nvOk ? "unavailable (needs an NVIDIA RTX GPU)" : "failed (see log)";
    float gx, gy;
    if (VS().foveation == 2) return XrGaze(&gx, &gy, 500) ? "on, eye-tracked" : "on (no eye-tracking data: fixed)";
    return "on, fixed";
}

} // namespace avr
