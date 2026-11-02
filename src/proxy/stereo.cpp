// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// Native stereo rendering: renders the game's view twice per frame (one per eye) by re-running
// RsSceneRenderController::RenderView with a per-eye camera, and separates the HUD into its own layer.
#include "stereo.h"
#include "globals.h"
#include "d3dhooks.h"
#include "dds.h"
#include "xrlink.h"
#include "input.h"
#include "settings.h"
#include "vrs.h"
#include "menu_options.h"
#include "../common/vmath.h"
#include "../common/ipc.h"
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <cstdint>
#include <cmath>
#include <array>
#include <map>
#include <algorithm>
#include <string>

namespace avr {

// --------------------------------------------------------------------- camera layout (render-side camera)
namespace camoff {
constexpr int Pos = 0x04, Target = 0x14, Near = 0x24, Far = 0x28, FovH = 0x2C, TanH = 0x38, TanV = 0x3C;
constexpr int View = 0x44, ViewRotT = 0x84, Proj = 0xC4, ViewProj = 0x104, ViewProj2 = 0x144;
constexpr int NearMin = 0x184, NearMax = 0x190, Right = 0x19C, Up = 0x1A8, Fwd = 0x1B4, Up2 = 0x1C0;
constexpr int Planes = 0x1CC, AbsNormals = 0x28C, PlaneCount = 0x31C;
constexpr int Size = 0x360;
}

struct Settings {
    bool enabled = true;
    bool nativeStereo = true;
    float unitsPerMeter = 100.0f;
    bool rollLock = true;
    UINT width = 0, height = 0;
    float debugFovDeg = 100.0f;
    float debugIpd = 0.064f;
    float nearOverride = 0.0f;
    int mirrorEye = 0;
};
static Settings S;

// --------------------------------------------------------------------- state
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static UINT g_w = 0, g_h = 0;                 // per-eye size (= backbuffer size)

struct Atlas {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE shared = nullptr;
};
static Atlas g_local;                         // used when no host is attached
static Atlas g_slots[kSlots];
static bool g_slotsReady = false;
static int g_curSlot = -1;                    // slot acquired for the current frame (-1 = local)
static Atlas* g_cur = nullptr;

static int g_eyePass = -1;                    // -1 not in stereo pass, else eye index
static M4 g_origVP, g_eyeVP;                  // camera view-projection: as the game built it / for the current eye
static volatile LONG g_fixedWVP = 0, g_skippedWVP = 0;
static volatile bool g_hudPhase = false;      // HUD is being drawn onto the transparent backbuffer
static std::map<ID3D11BlendState*, ID3D11BlendState*> g_hudBlend;
static bool g_eyesRendered[2] = {};
static bool g_eyeWroteBB[2] = {};             // the engine rendered the scene into the backbuffer this eye pass
static ID3D11Resource* g_bbRes = nullptr;
static bool g_frameHasWorld = false;
static IpcPose g_framePose[2];
static IpcFov g_frameFov[2];
static int64_t g_frameTime = 0;

// blit resources
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11SamplerState* g_samp = nullptr;
static ID3D11BlendState* g_blendOpaque = nullptr;
static ID3D11BlendState* g_blendAlpha = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;
static ID3D11DepthStencilState* g_ds = nullptr;
static ID3DDeviceContextState* g_ourState = nullptr;

// --------------------------------------------------------------------- settings
void StereoInit() {
    auto& c = G().cfg;
    S.enabled = c.GetBool("vr_enabled", true);
    S.nativeStereo = c.GetBool("stereo_native", true);
    S.unitsPerMeter = c.GetFloat("units_per_meter", 100.0f);
    S.rollLock = true;
    S.width = (UINT)c.GetInt("render_width", 0);
    S.height = (UINT)c.GetInt("render_height", 0);
    S.debugFovDeg = c.GetFloat("debug_fov", 100.0f);
    S.nearOverride = c.GetFloat("near_clip", 0.0f);
    LOGI("stereo: enabled=%d native=%d unitsPerMeter=%.1f worldScale=%.2f pitchLock=%d", S.enabled, S.nativeStereo,
         S.unitsPerMeter, VS().worldScale, VS().pitchLock);
}

bool StereoWantsSwapchainSize(UINT* w, UINT* h) {
    if (!S.enabled) return false;
    UINT rw = S.width, rh = S.height;
    if (!rw || !rh) {
        UINT hw = 0, hh = 0;
        if (XrWaitRecommendedSize(&hw, &hh, 15000)) {
            rw = (UINT)(hw * VS().renderScale + 0.5f);
            rh = (UINT)(hh * VS().renderScale + 0.5f);
        }
    }
    if (!rw || !rh) return false;
    // The game lays out its UI for the screen aspect; headsets are ~0.9:1, which clips wide UI (dialog).
    // Widen the render (projection still uses the exact headset FOV) to at least min_render_aspect.
    float minAspect = VS().minRenderAspect;
    if (minAspect > 0 && (float)rw / rh < minAspect) rw = (UINT)(rh * minAspect + 0.5f);
    rw &= ~1u; rh &= ~1u;
    if (rw * 3 > 16384) { float k = 16384.0f / (rw * 3); rw = (UINT)(rw * k) & ~1u; rh = (UINT)(rh * k) & ~1u; LOGW("stereo: clamped size for atlas limit"); }
    *w = rw; *h = rh;
    return true;
}

// --------------------------------------------------------------------- resources
static const char* kShader = R"(
cbuffer CB : register(b0) { float4 srcRect; float4 dstRect; float4 params; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSO VS(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);
    VSO o;
    float2 d = dstRect.xy + t * dstRect.zw;
    o.pos = float4(d * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = srcRect.xy + t * srcRect.zw;
    return o;
}
float4 PS(VSO i) : SV_Target {
    if (params.y > 0.5 && (any(i.uv < 0) || any(i.uv > 1))) discard;   // clip to the destination rectangle
    float4 c = tex.Sample(samp, i.uv);
    if (params.x > 0.5) c.a = 1;
    return c;
}
)";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static bool CreateBlitResources() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("stereo: d3dcompiler_47 not available - desktop mirror disabled"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err))) {
        LOGE("stereo: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    InternalScope s;
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{ 48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_samp);
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = 0xF;
    g_dev->CreateBlendState(&b, &g_blendOpaque);
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;            // HUD is premultiplied (blended over black)
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    g_dev->CreateBlendState(&b, &g_blendAlpha);
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&r, &g_rs);
    D3D11_DEPTH_STENCIL_DESC ds{};
    g_dev->CreateDepthStencilState(&ds, &g_ds);
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1))) {
        D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_ourState);
        d1->Release();
    }
    LOGI("stereo: blit resources ready (contextState=%p)", g_ourState);
    return true;
}

static bool CreateAtlas(Atlas& a, bool shared) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = g_w * 3; d.Height = g_h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;
    InternalScope s;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &a.tex))) { LOGE("stereo: atlas create failed (%ux%u)", d.Width, d.Height); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(a.tex, &sv, &a.srv);
    if (shared) {
        a.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&a.mutex);
        IDXGIResource* r = nullptr;
        if (SUCCEEDED(a.tex->QueryInterface(__uuidof(IDXGIResource), (void**)&r))) { r->GetSharedHandle(&a.shared); r->Release(); }
    }
    return true;
}

static void ReleaseAtlas(Atlas& a) {
    if (a.srv) a.srv->Release();
    if (a.mutex) a.mutex->Release();
    if (a.tex) a.tex->Release();
    a = Atlas();
}

static void CreateSizeDependent() {
    ReleaseAtlas(g_local);
    for (auto& s : g_slots) ReleaseAtlas(s);
    g_slotsReady = false;
    if (!g_w || !g_h) return;
    CreateAtlas(g_local, false);
    bool ok = true;
    for (auto& s : g_slots) ok = ok && CreateAtlas(s, true);
    if (ok) {
        uint64_t handles[kSlots];
        for (int i = 0; i < kSlots; ++i) handles[i] = (uint64_t)(uintptr_t)g_slots[i].shared;
        XrPublishTextures(g_w, g_h, DXGI_FORMAT_R8G8B8A8_TYPELESS, handles);
        g_slotsReady = true;
    }
    LOGI("stereo: atlases %ux%u created (shared=%d)", g_w * 3, g_h, ok);
}

void StereoOnDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    g_dev = dev; g_ctx = ctx;
    if (S.enabled) CreateBlitResources();
}

void StereoOnSwapchain(IDXGISwapChain* sc) {
    g_sc = sc;
    if (g_bbRes) { g_bbRes->Release(); g_bbRes = nullptr; }
    sc->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&g_bbRes);
    DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
    if (d.BufferDesc.Width != g_w || d.BufferDesc.Height != g_h) {
        g_w = d.BufferDesc.Width; g_h = d.BufferDesc.Height;
        if (S.enabled) CreateSizeDependent();
    }
}

// --------------------------------------------------------------------- per-eye camera
static M4 LoadM(const uint8_t* p) { M4 m; memcpy(m.m, p, 64); return m; }
static void StoreM(uint8_t* p, const M4& m) { memcpy(p, m.m, 64); }
static V3 LoadV(const uint8_t* p) { V3 v; memcpy(&v, p, 12); return v; }
static void StoreV(uint8_t* p, V3 v) { memcpy(p, &v, 12); }
static float& F(uint8_t* p, int off) { return *(float*)(p + off); }

static void ApplyEye(uint8_t* cam, const uint8_t* orig, const IpcPose& pose, const IpcFov& fov) {
    using namespace camoff;
    float n = *(const float*)(orig + Near), f = *(const float*)(orig + Far);
    if (S.nearOverride > 0) n = S.nearOverride;
    M4 V = LoadM(orig + View);
    M4 C = InverseRigid(V);                 // rows: right, up, fwd, position (world)
    if (VS().pitchLock) {
        V3 fw = C.Row(2); fw.z = 0;
        if (Len(fw) > 1e-3f) {
            fw = Norm(fw);
            V3 up = {0, 0, 1};
            V3 rt = Norm(Cross(up, fw));
            C.SetRow(0, rt, 0); C.SetRow(1, up, 0); C.SetRow(2, fw, 0);
        }
    }
    // eye pose (OpenXR RH meters) -> game view space (LH, game units)
    float upm = S.unitsPerMeter * VS().worldScale;
    M4 E = FromQuat(pose.qx, pose.qy, pose.qz, pose.qw);
    E.m[0][2] = -E.m[0][2]; E.m[1][2] = -E.m[1][2]; E.m[2][0] = -E.m[2][0]; E.m[2][1] = -E.m[2][1];
    E.m[3][0] = pose.px * upm * VS().ipdScale; E.m[3][1] = pose.py * upm; E.m[3][2] = -pose.pz * upm; E.m[3][3] = 1;
    M4 Ce = Mul(E, C);
    M4 Ve = InverseRigid(Ce);
    float tl = tanf(fov.left), tr = tanf(fov.right), tu = tanf(fov.up), td = tanf(fov.down);
    M4 P = ProjFromTangents(tl, tr, tu, td, n, f);
    M4 VP = Mul(Ve, P);

    V3 pos = Ce.Row(3), rt = Ce.Row(0), up = Ce.Row(1), fw = Ce.Row(2);
    float dist = Len(LoadV(orig + Target) - LoadV(orig + Pos));
    StoreV(cam + Pos, pos);
    StoreV(cam + Target, pos + fw * dist);
    F(cam, Near) = n;
    float hf = atanf(tr) - atanf(tl), vf = atanf(tu) - atanf(td);
    F(cam, FovH) = hf * 57.29578f;
    F(cam, TanH) = tanf(hf * 0.5f);
    F(cam, TanV) = tanf(vf * 0.5f);
    StoreM(cam + View, Ve);
    M4 RT = Ve;                              // mimic the engine's second matrix: transposed rotation + view translation
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) RT.m[i][j] = Ve.m[j][i];
    StoreM(cam + ViewRotT, RT);
    StoreM(cam + Proj, P);
    StoreM(cam + ViewProj, VP);
    StoreM(cam + ViewProj2, VP);
    g_origVP = LoadM(orig + ViewProj);
    g_eyeVP = VP;
    StoreV(cam + NearMin, {n * tl, n * td, n});
    StoreV(cam + NearMax, {n * tr, n * tu, n});
    StoreV(cam + Right, rt); StoreV(cam + Up, up); StoreV(cam + Fwd, fw); StoreV(cam + Up2, up);
    // frustum planes (inward normals, n.p + d >= 0 inside): top, bottom, left, right, near, far
    auto col = [&](int j) { return std::array<float, 4>{VP.m[0][j], VP.m[1][j], VP.m[2][j], VP.m[3][j]}; };
    std::array<float, 4> c0 = col(0), c1 = col(1), c2 = col(2), c3 = col(3);
    std::array<float, 4> pl[6];
    for (int k = 0; k < 4; ++k) {
        pl[0][k] = c3[k] - c1[k]; pl[1][k] = c3[k] + c1[k];
        pl[2][k] = c3[k] + c0[k]; pl[3][k] = c3[k] - c0[k];
        pl[4][k] = c2[k];         pl[5][k] = c3[k] - c2[k];
    }
    for (int i = 0; i < 6; ++i) {
        float l = sqrtf(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
        for (int k = 0; k < 4; ++k) pl[i][k] /= l;
        memcpy(cam + Planes + i * 16, pl[i].data(), 16);
        V3 an = {fabsf(pl[i][0]), fabsf(pl[i][1]), fabsf(pl[i][2])};
        StoreV(cam + AbsNormals + i * 12, an);
    }
}

// --------------------------------------------------------------------- frame flow
static void GetEyeViews(IpcPose pose[2], IpcFov fov[2], int64_t* t) {
    if (XrGetLatestPoses(pose, fov, t)) return;
    // debug fallback: fixed IPD, symmetric FOV matching the backbuffer aspect
    float hf = S.debugFovDeg * 0.5f * 3.14159265f / 180.0f;
    float aspect = g_h ? (float)g_w / g_h : 1.0f;
    float vf = atanf(tanf(hf) / aspect);
    for (int e = 0; e < 2; ++e) {
        pose[e] = {(e == 0 ? -0.5f : 0.5f) * S.debugIpd, 0, 0, 0, 0, 0, 1};
        fov[e] = {-hf, hf, vf, -vf};
    }
    *t = 0;
}

static void AcquireFrameTarget() {
    if (g_cur) return;
    g_curSlot = -1;
    g_cur = &g_local;
    if (g_slotsReady && XrHostConnected()) {
        int start = (XrLastSubmittedSlot() + 1) % kSlots;
        for (int k = 0; k < kSlots; ++k) {
            int s = (start + k) % kSlots;
            if (!g_slots[s].mutex) continue;
            if (g_slots[s].mutex->AcquireSync(0, 0) == S_OK || g_slots[s].mutex->AcquireSync(1, 0) == S_OK) {
                g_curSlot = s; g_cur = &g_slots[s];
                break;
            }
        }
    }
}

static uint32_t ReadPixel(ID3D11Texture2D* t, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) return 0xDEADBEEF;
    D3D11_BOX b{x, y, 0, x + 1, y + 1, 1};
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, &b);
    uint32_t v = 0xBAADF00D;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { v = *(uint32_t*)m.pData; g_ctx->Unmap(st, 0); }
    st->Release();
    return v;
}

static void CopyBackbufferTo(int region) {
    if (!g_cur || !g_cur->tex || !g_sc) return;
    InternalScope s;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_BOX box{0, 0, 0, g_w, g_h, 1};
    g_ctx->CopySubresourceRegion(g_cur->tex, 0, region * g_w, 0, 0, bb, 0, &box);
    static int diag = 0;
    if (diag < 6 && g_eyePass >= 0) {
        diag++;
        D3D11_TEXTURE2D_DESC bd, ad; bb->GetDesc(&bd); g_cur->tex->GetDesc(&ad);
        LOGI("diag copy region %d: bb=%p %ux%u fmt%d -> atlas=%p %ux%u fmt%d misc%X | bb(400,300)=%08X atlas=%08X eyePass=%d",
             region, bb, bd.Width, bd.Height, bd.Format, g_cur->tex, ad.Width, ad.Height, ad.Format, ad.MiscFlags,
             ReadPixel(bb, 400, 300), ReadPixel(g_cur->tex, region * g_w + 400, 300), g_eyePass);
    }
    bb->Release();
}

static void ClearBackbufferTransparent() {
    InternalScope s;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_dev->CreateRenderTargetView(bb, &rd, &rtv))) {
        const float z[4] = {0, 0, 0, 0};
        g_ctx->ClearRenderTargetView(rtv, z);
        rtv->Release();
    }
    bb->Release();
}

void StereoRenderView(PFN_EngineFn5 orig, void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uint8_t* cam = nullptr;
    __try { cam = ((uint8_t**)a3)[0]; } __except (EXCEPTION_EXECUTE_HANDLER) { cam = nullptr; }
    if (!S.enabled || !cam || !g_w || g_eyePass >= 0) { orig(self, nullptr, a1, a2, a3, a4, a5); return; }

    if (!g_cur) XrWaitHostFrame(30);   // start the frame right after the headset publishes fresh poses
    AcquireFrameTarget();
    GetEyeViews(g_framePose, g_frameFov, &g_frameTime);
    uint8_t saved[camoff::Size];
    memcpy(saved, cam, sizeof(saved));
    int eyes = S.nativeStereo ? 2 : 1;
    static int alt = 0;
    for (int i = 0; i < eyes; ++i) {
        int eye = S.nativeStereo ? i : (alt ^= 1);
        g_eyePass = eye;
        ApplyEye(cam, saved, g_framePose[eye], g_frameFov[eye]);
        orig(self, nullptr, a1, a2, a3, a4, a5);
        VrsUnbind(g_ctx);
        if (!g_eyesRendered[eye] && g_eyeWroteBB[eye]) {   // overlays skipped but the scene was rendered -> copy now
            CopyBackbufferTo(eye);
            g_eyesRendered[eye] = true;
        }
        memcpy(cam, saved, sizeof(saved));
    }
    g_eyePass = -1;
    // Menus still call RenderView (e.g. inventory character preview) without drawing the world to the screen.
    g_frameHasWorld = g_eyesRendered[0] && (!S.nativeStereo || g_eyesRendered[1]);
    g_eyeWroteBB[0] = g_eyeWroteBB[1] = false;
}

void StereoReleaseBackbuffer() { if (g_bbRes) { g_bbRes->Release(); g_bbRes = nullptr; } }

void StereoNoteRenderTargets(UINT n, ID3D11RenderTargetView* const* rtv) {
    if (g_eyePass < 0 || !rtv || !g_bbRes) return;
    for (UINT i = 0; i < n; ++i) {
        if (!rtv[i]) continue;
        ID3D11Resource* r = nullptr;
        rtv[i]->GetResource(&r);
        if (r) { if (r == g_bbRes) g_eyeWroteBB[g_eyePass] = true; r->Release(); }
    }
}

void StereoOverlay(PFN_EngineFn5 orig, void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!S.enabled || g_eyePass < 0) { orig(self, nullptr, a1, a2, a3, a4, a5); return; }
    if (!g_eyeWroteBB[g_eyePass]) { orig(self, nullptr, a1, a2, a3, a4, a5); return; }   // no scene this frame (menu)
    // The scene for this eye is complete in the backbuffer: grab it. Overlays never use reduced shading.
    VrsUnbind(g_ctx);
    CopyBackbufferTo(g_eyePass);
    g_eyesRendered[g_eyePass] = true;
    bool lastEye = !S.nativeStereo || g_eyePass == 1;
    if (!lastEye) return;   // skip overlays for the first eye
    // Last eye: overlays (and later the UI) go onto a transparent backbuffer -> HUD layer.
    ClearBackbufferTransparent();
    g_hudPhase = true;
    orig(self, nullptr, a1, a2, a3, a4, a5);
}

bool StereoHudPhase() { return g_hudPhase; }

// Per-object constants (g_dynamicbuffer: world @0, worldVector @64, worldViewProjection @128) are baked into
// draw packets with the game's camera before RenderView runs. During eye passes, re-derive WVP for this eye.
void* __cdecl StereoCopyObjectConstants(void* dst, const void* src, size_t n) {
    memcpy(dst, src, n);
    if (g_eyePass < 0 || n < 192) return dst;
    const float* f = (const float*)src;
    double pred[4][4], fix[4][4];
    double err = 0, mag = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double a = 0, b = 0;
            for (int k = 0; k < 4; ++k) {
                a += (double)f[i * 4 + k] * g_origVP.m[k][j];
                b += (double)f[i * 4 + k] * g_eyeVP.m[k][j];
            }
            pred[i][j] = a; fix[i][j] = b;
            double w = f[32 + i * 4 + j];
            err = std::max(err, fabs(a - w));
            mag = std::max(mag, fabs(w));
        }
    if (err > 1e-3 * (mag + 1.0)) { InterlockedIncrement(&g_skippedWVP); return dst; }   // not camera-relative (shadow, special)
    float* d = (float*)dst + 32;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) d[i * 4 + j] = (float)fix[i][j];
    InterlockedIncrement(&g_fixedWVP);
    return dst;
}

void StereoLogStats() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 10000) return;
    last = now;
    LOGI("stereo: object WVP fixed %ld, left alone %ld (last 10s)", InterlockedExchange(&g_fixedWVP, 0), InterlockedExchange(&g_skippedWVP, 0));
}

// While drawing the HUD layer: keep the game's colour blending but accumulate alpha as coverage
// (ONE, INV_SRC_ALPHA) so the layer is correctly premultiplied for the compositor.
ID3D11BlendState* StereoHudBlend(ID3D11BlendState* b) {
    if (!b || !g_dev) return b;
    auto it = g_hudBlend.find(b);
    if (it != g_hudBlend.end()) return it->second;
    D3D11_BLEND_DESC d; b->GetDesc(&d);
    ID3D11BlendState* r = b;
    auto& rt = d.RenderTarget[0];
    if (rt.BlendEnable && rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) {
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.IndependentBlendEnable = FALSE;
        InternalScope s;
        if (FAILED(g_dev->CreateBlendState(&d, &r))) r = b;
    } else if (rt.BlendEnable && rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_ONE) {
        // additive glow: don't let it add coverage beyond what it lights up
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        InternalScope s;
        if (FAILED(g_dev->CreateBlendState(&d, &r))) r = b;
    }
    g_hudBlend[b] = r;
    return r;
}

static void Blit(ID3D11ShaderResourceView* srv, float sx, float sy, float sw, float sh, ID3D11RenderTargetView* rtv,
                 ID3D11BlendState* blend, bool forceOpaque) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = (float*)m.pData;
        f[0] = sx; f[1] = sy; f[2] = sw; f[3] = sh;
        f[4] = 0; f[5] = 0; f[6] = 1; f[7] = 1;
        f[8] = forceOpaque ? 1.0f : 0.0f; f[9] = f[10] = f[11] = 0;
        g_ctx->Unmap(g_cb, 0);
    }
    D3D11_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0, 1};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs);
    g_ctx->OMSetDepthStencilState(g_ds, 0);
    g_ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nul);
}

static void DrawMirror(bool world) {
    if (!VS().desktopMirror || !g_vs || !g_cur || !g_cur->srv) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, &rd, &rtv);
    bb->Release();
    if (!rtv) return;
    ID3D11Device1* d1 = nullptr;
    ID3D11DeviceContext1* c1 = nullptr;
    ID3DDeviceContextState* prev = nullptr;
    if (g_ourState && SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)))
        c1->SwapDeviceContextState(g_ourState, &prev);
    float third = 1.0f / 3.0f;
    if (world) {
        Blit(g_cur->srv, S.mirrorEye * third, 0, third, 1, rtv, g_blendOpaque, true);   // eye
        Blit(g_cur->srv, 2 * third, 0, third, 1, rtv, g_blendAlpha, false);            // HUD over it
    }
    if (c1) { c1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); c1->Release(); }
    (void)d1;
    rtv->Release();
}

static void FitWindowToMonitor() {
    static bool done = false;
    HWND hw = G().hwnd;
    if (done || !hw || !G().cfg.GetBool("desktop_window_fit", true)) return;
    done = true;
    HMONITOR mon = MonitorFromWindow(hw, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top;
    float k = std::min(1.0f, std::min((float)mw / g_w, (float)mh / g_h));
    int ww = (int)(g_w * k), wh = (int)(g_h * k);
    SetWindowPos(hw, nullptr, mi.rcWork.left + (mw - ww) / 2, mi.rcWork.top + (mh - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    LOGI("desktop window fitted to %dx%d (render stays %ux%u)", ww, wh, g_w, g_h);
}

int StereoEyePass() { return g_eyePass; }
void StereoRenderSize(UINT* w, UINT* h) { *w = g_w; *h = g_h; }
bool StereoEyeTangents(int eye, float t[4]) {
    if (eye < 0 || eye > 1) return false;
    const IpcFov& f = g_frameFov[eye];
    if (f.right == 0) return false;
    t[0] = tanf(f.left); t[1] = tanf(f.right); t[2] = tanf(f.up); t[3] = tanf(f.down);
    return true;
}

// The settings menu on the monitor (the headset gets it as its own layer): centred, at most 80% of the window.
static void DrawMenuPreview(ID3D11Texture2D* tex) {
    if (!g_vs || !tex) return;
    static ID3D11Texture2D* srvTex = nullptr;
    static ID3D11ShaderResourceView* srv = nullptr;
    if (srvTex != tex) {
        if (srv) srv->Release();
        srv = nullptr; srvTex = tex;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(tex, &sv, &srv);
    }
    if (!srv) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, &rd, &rtv);
    bb->Release();
    if (!rtv) return;
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    float k = std::min(0.8f * g_w / td.Width, 0.8f * g_h / td.Height);
    float dw = td.Width * k / g_w, dh = td.Height * k / g_h;
    ID3D11DeviceContext1* c1 = nullptr;
    ID3DDeviceContextState* prev = nullptr;
    if (g_ourState && SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)))
        c1->SwapDeviceContextState(g_ourState, &prev);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = (float*)m.pData;
        f[0] = 0; f[1] = 0; f[2] = 1; f[3] = 1;
        f[4] = (1 - dw) * 0.5f; f[5] = (1 - dh) * 0.5f; f[6] = dw; f[7] = dh;
        f[8] = 0; f[9] = 1; f[10] = f[11] = 0;
        g_ctx->Unmap(g_cb, 0);
    }
    D3D11_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0, 1};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs);
    g_ctx->OMSetDepthStencilState(g_ds, 0);
    g_ctx->OMSetBlendState(g_blendAlpha, nullptr, 0xFFFFFFFF);   // the menu texture is premultiplied
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nul);
    if (c1) { c1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); c1->Release(); }
    rtv->Release();
}

void StereoBeforePresent(IDXGISwapChain* sc) {
    if (!S.enabled || !g_w) return;
    StereoLogStats();
    if (G().frame > 120) FitWindowToMonitor();
    InternalScope s;
    VrsEndOfFrame(g_ctx);
    int combos = TakeCombos();
    if (combos & COMBO_RECENTER) XrRequestRecenter();
    if (combos & COMBO_HUD_TOGGLE) MenuToggleHudAnchor();
    bool world = g_frameHasWorld;
    static int lastMode = -1;
    if ((int)world != lastMode) { lastMode = world; LOGI("stereo: frame mode -> %s", world ? "WORLD (stereo + HUD layer)" : "MENU (flat panel only)"); }
    if (!world) { XrWaitHostFrame(30); AcquireFrameTarget(); }
    // HUD (or full menu screen) = current backbuffer
    CopyBackbufferTo(2);
    if (const std::wstring* dir = ActiveCaptureDir()) {
        if (g_cur && g_cur->tex) SaveTextureDDS(g_dev, g_ctx, g_cur->tex, DXGI_FORMAT_R8G8B8A8_UNORM, *dir + L"atlas.dds");
    }
    if (world) DrawMirror(true);
    // Settings menu: drawn into its own texture and handed to the host; previewed on the monitor.
    if (ID3D11Texture2D* menuTex = MenuOnPresent(g_dev, g_ctx)) DrawMenuPreview(menuTex);
    if (g_curSlot >= 0) {
        uint32_t flags = world ? SLOT_HUD_VISIBLE : (SLOT_HUD_VISIBLE | SLOT_MENU_MODE);
        g_ctx->Flush();
        g_slots[g_curSlot].mutex->ReleaseSync(1);
        XrSubmitSlot(g_curSlot, g_framePose, g_frameFov, g_frameTime, flags);
    }
    g_cur = nullptr; g_curSlot = -1;
    g_frameHasWorld = false;
    g_hudPhase = false;
    g_eyesRendered[0] = g_eyesRendered[1] = false;
}

void StereoCaptureDump(const wchar_t* dir) {
    if (!g_local.tex && !g_cur) return;
    // dump the local atlas copy of the last frame (copy current backbuffer state is in it only if local)
    Atlas* a = g_cur ? g_cur : &g_local;
    SaveTextureDDS(g_dev, g_ctx, a->tex, DXGI_FORMAT_R8G8B8A8_UNORM, std::wstring(dir) + L"atlas.dds");
}

} // namespace avr
