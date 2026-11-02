// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// D3D11 / DXGI hooks and the one-frame capture tracer.
#include "d3dhooks.h"
#include "globals.h"
#include "hookutil.h"
#include "vtbl.h"
#include "dds.h"
#include "game.h"
#include "input.h"
#include "stereo.h"
#include "vrs.h"
#include "xrlink.h"
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <d3dcommon.h>

namespace avr {

thread_local int t_internal = 0;

// ---------------------------------------------------------------- originals
#define ORIG(name, ret, ...) typedef ret(STDMETHODCALLTYPE* PFN_##name)(__VA_ARGS__); static PFN_##name o_##name = nullptr;
ORIG(D3D11CreateDevice_, HRESULT, IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**)
ORIG(CreateSwapChain, HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**)
ORIG(CreateSwapChainForHwnd, HRESULT, IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**)
ORIG(Present, HRESULT, IDXGISwapChain*, UINT, UINT)
ORIG(ResizeBuffers, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT)
ORIG(CreateVertexShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**)
ORIG(CreatePixelShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**)
ORIG(CreateDeferredContext, HRESULT, ID3D11Device*, UINT, ID3D11DeviceContext**)
ORIG(Draw, void, ID3D11DeviceContext*, UINT, UINT)
ORIG(DrawIndexed, void, ID3D11DeviceContext*, UINT, UINT, INT)
ORIG(DrawIndexedInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT)
ORIG(DrawInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, UINT)
ORIG(Map, HRESULT, ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*)
ORIG(Unmap, void, ID3D11DeviceContext*, ID3D11Resource*, UINT)
ORIG(UpdateSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT)
ORIG(VSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG(PSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG(VSSetShader, void, ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT)
ORIG(PSSetShader, void, ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)
ORIG(PSSetShaderResources, void, ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*)
ORIG(OMSetRenderTargets, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*)
ORIG(OMSetRenderTargetsAndUnorderedAccessViews, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*)
ORIG(RSSetViewports, void, ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)
ORIG(ClearRenderTargetView, void, ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*)
ORIG(ClearDepthStencilView, void, ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8)
ORIG(CopyResource, void, ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*)
ORIG(CopySubresourceRegion, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*)
ORIG(ResolveSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT)
ORIG(Dispatch, void, ID3D11DeviceContext*, UINT, UINT, UINT)
ORIG(CSSetShader, void, ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT)
ORIG(OMSetBlendState, void, ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT)
ORIG(OMSetDepthStencilState, void, ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT)
ORIG(ExecuteCommandList, void, ID3D11DeviceContext*, ID3D11CommandList*, BOOL)
ORIG(FinishCommandList, HRESULT, ID3D11DeviceContext*, BOOL, ID3D11CommandList**)
#undef ORIG

// ---------------------------------------------------------------- capture state
static std::mutex g_shaderMutex;
static std::unordered_map<void*, std::vector<uint8_t>> g_shaderCode; // shader object -> bytecode

struct Capture {
    bool requested = false;
    bool active = false;
    int index = 0;
    std::wstring dir;
    std::string text;
    int event = 0;
    std::unordered_map<void*, int> ids;
    std::unordered_set<void*> usedShaders;
    std::unordered_map<void*, void*> mapped; // buffer -> mapped ptr
    ID3D11RenderTargetView* curRTV[8] = {};
    ID3D11DepthStencilView* curDSV = nullptr;
    int drawsSinceBind = 0;
    int dumps = 0;
    int draws = 0;
};
static Capture g_cap;
static int g_captureKey = VK_F11;
static bool g_dumpTargets = true;

static volatile LONG g_deferredDraws = 0, g_deferredCalls = 0;
static bool CapturingOn(ID3D11DeviceContext* c) {
    if (!g_cap.active || t_internal != 0) return false;
    if (c != G().context) { InterlockedIncrement(&g_deferredCalls); return false; }
    return true;
}

static void Tr(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_cap.text += buf;
}

static void TrCallers(void* addrOfRet) {
    uint32_t rva[8];
    int n = ScanGameReturnAddresses(addrOfRet, rva, 8, 2048);
    Tr("  @");
    for (int i = 0; i < n; ++i) Tr(" %06X", rva[i]);
    Tr("\n");
}

static std::string DescribeTex(ID3D11Resource* r) {
    if (!r) return "null";
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    char b[256];
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        snprintf(b, sizeof(b), "tex2d %ux%u %s ms%u arr%u mips%u bind%X", d.Width, d.Height, FormatName(d.Format),
                 d.SampleDesc.Count, d.ArraySize, d.MipLevels, d.BindFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        snprintf(b, sizeof(b), "buffer %u bytes bind%X usage%u", d.ByteWidth, d.BindFlags, d.Usage);
    } else snprintf(b, sizeof(b), "resource dim%d", (int)dim);
    return b;
}

static int Id(void* p, const char* kind) {
    if (!p) return 0;
    auto it = g_cap.ids.find(p);
    if (it != g_cap.ids.end()) return it->second;
    int id = (int)g_cap.ids.size() + 1;
    g_cap.ids[p] = id;
    // describe once
    std::string desc;
    if (!strcmp(kind, "rtv")) {
        ID3D11Resource* r = nullptr; ((ID3D11RenderTargetView*)p)->GetResource(&r);
        D3D11_RENDER_TARGET_VIEW_DESC vd; ((ID3D11RenderTargetView*)p)->GetDesc(&vd);
        desc = DescribeTex(r) + " view=" + FormatName(vd.Format);
        char b[32]; snprintf(b, sizeof(b), " res=%p", (void*)r); desc += b;
        if (r) r->Release();
    } else if (!strcmp(kind, "dsv")) {
        ID3D11Resource* r = nullptr; ((ID3D11DepthStencilView*)p)->GetResource(&r);
        desc = DescribeTex(r);
        if (r) r->Release();
    } else if (!strcmp(kind, "srv")) {
        ID3D11Resource* r = nullptr; ((ID3D11ShaderResourceView*)p)->GetResource(&r);
        desc = DescribeTex(r);
        char b[32]; snprintf(b, sizeof(b), " res=%p", (void*)r); desc += b;
        if (r) r->Release();
    } else if (!strcmp(kind, "buf") || !strcmp(kind, "res")) {
        desc = DescribeTex((ID3D11Resource*)p);
    }
    Tr("    [obj %s#%d %p %s]\n", kind, id, p, desc.c_str());
    return id;
}

static void DumpRT(ID3D11RenderTargetView* rtv, const char* tag) {
    if (!g_dumpTargets || !rtv || g_cap.dumps >= 300) return;
    InternalScope s;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_RENDER_TARGET_VIEW_DESC vd; rtv->GetDesc(&vd);
        wchar_t name[128];
        swprintf(name, 128, L"rt_%05d_%S_rtv%d.dds", g_cap.event, tag, g_cap.ids.count(rtv) ? g_cap.ids[rtv] : 0);
        if (SaveTextureDDS(G().device, G().context, (ID3D11Texture2D*)r, vd.Format, g_cap.dir + name)) {
            Tr("    [dump %S]\n", name);
            g_cap.dumps++;
        }
    }
    r->Release();
}

static void DumpBoundTargets(const char* tag) {
    if (g_cap.drawsSinceBind == 0) return;
    for (int i = 0; i < 8; ++i) if (g_cap.curRTV[i]) DumpRT(g_cap.curRTV[i], tag);
    g_cap.drawsSinceBind = 0;
}

typedef HRESULT(WINAPI* PFN_D3DDisassemble)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);

static void WriteShaders() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryW(L"d3dcompiler_43.dll");
    auto dis = dc ? (PFN_D3DDisassemble)GetProcAddress(dc, "D3DDisassemble") : nullptr;
    std::wstring sdir = g_cap.dir + L"shaders\\";
    CreateDirectoryW(sdir.c_str(), nullptr);
    std::lock_guard<std::mutex> lk(g_shaderMutex);
    for (void* s : g_cap.usedShaders) {
        auto it = g_shaderCode.find(s);
        if (it == g_shaderCode.end()) continue;
        int id = g_cap.ids.count(s) ? g_cap.ids[s] : 0;
        wchar_t name[64];
        swprintf(name, 64, L"sh%d.bin", id);
        FILE* f = _wfopen((sdir + name).c_str(), L"wb");
        if (f) { fwrite(it->second.data(), 1, it->second.size(), f); fclose(f); }
        if (dis) {
            ID3DBlob* b = nullptr;
            if (SUCCEEDED(dis(it->second.data(), it->second.size(), 0, nullptr, &b)) && b) {
                swprintf(name, 64, L"sh%d.asm", id);
                f = _wfopen((sdir + name).c_str(), L"wb");
                if (f) { fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() ? b->GetBufferSize() - 1 : 0, f); fclose(f); }
                b->Release();
            }
        }
    }
}

static void LogContextVtable(const char* tag);
static void BeginCapture() {
    LogContextVtable("capture start");
    g_cap = Capture();
    g_deferredDraws = 0; g_deferredCalls = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"capture_%04d%02d%02d_%02d%02d%02d\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_cap.dir = G().outDir + name;
    CreateDirectoryW(G().outDir.c_str(), nullptr);
    CreateDirectoryW(g_cap.dir.c_str(), nullptr);
    g_cap.text.reserve(8 << 20);
    Tr("# Amalur VR frame capture. frame=%llu gameBase=%p\n", (unsigned long long)G().frame, (void*)GameModule().base);
    Tr("# caller list: RVAs of plausible return addresses into koa.exe (add 0x400000 for VA)\n");
    g_cap.active = true;
    GameOnCaptureBegin();
    LOGI("Capture started -> %S", g_cap.dir.c_str());
}

static void EndCapture(IDXGISwapChain* sc) {
    // dump whatever is bound + the backbuffer
    DumpBoundTargets("final");
    {
        InternalScope s;
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            SaveTextureDDS(G().device, G().context, bb, DXGI_FORMAT_UNKNOWN, g_cap.dir + L"backbuffer.dds");
            bb->Release();
        }
    }
    g_cap.active = false;
    Tr("# end of frame: %d immediate draws, %ld deferred-context draws, %ld deferred calls, %d events\n", g_cap.draws, g_deferredDraws, g_deferredCalls, g_cap.event);
    FILE* f = _wfopen((g_cap.dir + L"trace.txt").c_str(), L"wb");
    if (f) { fwrite(g_cap.text.data(), 1, g_cap.text.size(), f); fclose(f); }
    WriteShaders();
    GameOnCaptureEnd(g_cap.dir.c_str());
    LOGI("Capture written: %d draws, %d events, %d target dumps", g_cap.draws, g_cap.event, g_cap.dumps);
    g_cap.text.clear(); g_cap.text.shrink_to_fit();
}

void RequestCapture() { g_cap.requested = true; }
const std::wstring* ActiveCaptureDir() { return g_cap.active ? &g_cap.dir : nullptr; }

// ---------------------------------------------------------------- context hooks
#define EV(fmt, ...) do { Tr("%05d " fmt "\n", ++g_cap.event, __VA_ARGS__); TrCallers(_AddressOfReturnAddress()); } while (0)

static void TrDraw(ID3D11DeviceContext* c, const char* kind, UINT count, UINT inst, void* ret) {
    g_cap.draws++; g_cap.drawsSinceBind++;
    Tr("%05d %s count=%u inst=%u\n", ++g_cap.event, kind, count, inst);
    TrCallers(ret);
}

static void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT s) {
    if (g_cap.active && c != G().context) InterlockedIncrement(&g_deferredDraws);
    if (CapturingOn(c)) TrDraw(c, "Draw", n, 1, _AddressOfReturnAddress());
    o_Draw(c, n, s);
}
static void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT si, INT bv) {
    if (g_cap.active && c != G().context) InterlockedIncrement(&g_deferredDraws);
    if (CapturingOn(c)) TrDraw(c, "DrawIndexed", n, 1, _AddressOfReturnAddress());
    o_DrawIndexed(c, n, si, bv);
}
static void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT si, INT bv, UINT sinst) {
    if (g_cap.active && c != G().context) InterlockedIncrement(&g_deferredDraws);
    if (CapturingOn(c)) TrDraw(c, "DrawIndexedInstanced", n, inst, _AddressOfReturnAddress());
    o_DrawIndexedInstanced(c, n, inst, si, bv, sinst);
}
static void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT sv, UINT sinst) {
    if (g_cap.active && c != G().context) InterlockedIncrement(&g_deferredDraws);
    if (CapturingOn(c)) TrDraw(c, "DrawInstanced", n, inst, _AddressOfReturnAddress());
    o_DrawInstanced(c, n, inst, sv, sinst);
}

static HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = o_Map(c, r, sub, type, flags, m);
    if (CapturingOn(c) && SUCCEEDED(hr) && m && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
            if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) g_cap.mapped[r] = m->pData;
        }
    }
    return hr;
}

static void TrFloats(const void* data, UINT bytes) {
    const float* f = (const float*)data;
    UINT n = bytes / 16; if (n > 64) n = 64;
    for (UINT i = 0; i < n; ++i)
        Tr("      c%-2u %12.5f %12.5f %12.5f %12.5f\n", i, f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]);
}

static void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (CapturingOn(c)) {
        auto it = g_cap.mapped.find(r);
        if (it != g_cap.mapped.end()) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
            int id = Id(r, "buf");
            EV("CBUpdate(map) buf#%d %u bytes", id, d.ByteWidth);
            TrFloats(it->second, d.ByteWidth);
            g_cap.mapped.erase(it);
        }
    }
    o_Unmap(c, r, sub);
}

static void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (CapturingOn(c) && r && data) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
            if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                int id = Id(r, "buf");
                EV("CBUpdate(update) buf#%d %u bytes", id, d.ByteWidth);
                TrFloats(data, box ? (box->right - box->left) : d.ByteWidth);
            }
        } else {
            int id = Id(r, "res");
            EV("UpdateSubresource res#%d", id);
        }
    }
    o_UpdateSubresource(c, r, sub, box, data, rp, dp);
}

static void TrCBs(const char* stage, UINT start, UINT n, ID3D11Buffer* const* b, void* ret) {
    char line[256]; int o = snprintf(line, sizeof(line), "%sSetConstantBuffers start=%u:", stage, start);
    for (UINT i = 0; i < n && o < 200; ++i) o += snprintf(line + o, sizeof(line) - o, " buf#%d", Id(b ? b[i] : nullptr, "buf"));
    Tr("%05d %s\n", ++g_cap.event, line);
    TrCallers(ret);
}
static void STDMETHODCALLTYPE H_VSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (CapturingOn(c)) TrCBs("VS", s, n, b, _AddressOfReturnAddress());
    o_VSSetConstantBuffers(c, s, n, b);
}
static void STDMETHODCALLTYPE H_PSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (CapturingOn(c)) TrCBs("PS", s, n, b, _AddressOfReturnAddress());
    o_PSSetConstantBuffers(c, s, n, b);
}

static void STDMETHODCALLTYPE H_VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (CapturingOn(c)) {
        g_cap.usedShaders.insert(s);
        int id = Id(s, "vs");
        Tr("%05d VSSetShader sh%d\n", ++g_cap.event, id);
    }
    o_VSSetShader(c, s, ci, n);
}
static void STDMETHODCALLTYPE H_PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (CapturingOn(c)) {
        g_cap.usedShaders.insert(s);
        int id = Id(s, "ps");
        Tr("%05d PSSetShader sh%d\n", ++g_cap.event, id);
    }
    o_PSSetShader(c, s, ci, n);
}
static void STDMETHODCALLTYPE H_PSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (CapturingOn(c)) {
        char line[256]; int o = snprintf(line, sizeof(line), "PSSetShaderResources start=%u:", s);
        for (UINT i = 0; i < n && o < 200; ++i) o += snprintf(line + o, sizeof(line) - o, " srv#%d", Id(v ? v[i] : nullptr, "srv"));
        Tr("%05d %s\n", ++g_cap.event, line);
    }
    o_PSSetShaderResources(c, s, n, v);
}

static void TrSetRT(UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, void* ret, const char* kind) {
    DumpBoundTargets("before_rebind");
    char line[256]; int o = snprintf(line, sizeof(line), "%s n=%u:", kind, n);
    for (UINT i = 0; i < n && o < 200; ++i) o += snprintf(line + o, sizeof(line) - o, " rtv#%d", Id(rtv ? rtv[i] : nullptr, "rtv"));
    snprintf(line + o, sizeof(line) - o, " dsv#%d", Id(dsv, "dsv"));
    Tr("%05d %s\n", ++g_cap.event, line);
    TrCallers(ret);
    for (int i = 0; i < 8; ++i) g_cap.curRTV[i] = (rtv && (UINT)i < n) ? rtv[i] : nullptr;
    g_cap.curDSV = dsv;
}
static void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (t_internal == 0 && n) StereoNoteRenderTargets(n, rtv);
    if (t_internal == 0 && c == G().context) VrsOnSetRT(c, n, rtv, dsv);
    if (CapturingOn(c)) TrSetRT(n, rtv, dsv, _AddressOfReturnAddress(), "OMSetRenderTargets");
    o_OMSetRenderTargets(c, n, rtv, dsv);
}
static void STDMETHODCALLTYPE H_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ic) {
    if (t_internal == 0 && c == G().context && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) VrsOnSetRT(c, n, rtv, dsv);
    if (CapturingOn(c) && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) TrSetRT(n, rtv, dsv, _AddressOfReturnAddress(), "OMSetRenderTargetsAndUAV");
    o_OMSetRenderTargetsAndUnorderedAccessViews(c, n, rtv, dsv, us, un, u, ic);
}
static void STDMETHODCALLTYPE H_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (CapturingOn(c) && n && v) EV("RSSetViewports n=%u [%.0f %.0f %.0f %.0f z%.2f-%.2f]", n, v[0].TopLeftX, v[0].TopLeftY, v[0].Width, v[0].Height, v[0].MinDepth, v[0].MaxDepth);
    o_RSSetViewports(c, n, v);
}
static void STDMETHODCALLTYPE H_ClearRenderTargetView(ID3D11DeviceContext* c, ID3D11RenderTargetView* r, const FLOAT* col) {
    if (CapturingOn(c)) EV("ClearRTV rtv#%d (%.2f %.2f %.2f %.2f)", Id(r, "rtv"), col[0], col[1], col[2], col[3]);
    o_ClearRenderTargetView(c, r, col);
}
static void STDMETHODCALLTYPE H_ClearDepthStencilView(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT z, UINT8 s) {
    if (CapturingOn(c)) EV("ClearDSV dsv#%d flags=%u z=%.3f s=%u", Id(d, "dsv"), f, z, s);
    o_ClearDepthStencilView(c, d, f, z, s);
}
static void STDMETHODCALLTYPE H_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) {
    if (CapturingOn(c)) EV("CopyResource res#%d <- res#%d", Id(d, "res"), Id(s, "res"));
    o_CopyResource(c, d, s);
}
static void STDMETHODCALLTYPE H_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b) {
    if (CapturingOn(c)) EV("CopySubresourceRegion res#%d <- res#%d", Id(d, "res"), Id(s, "res"));
    o_CopySubresourceRegion(c, d, ds, x, y, z, s, ss, b);
}
static void STDMETHODCALLTYPE H_ResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f) {
    if (CapturingOn(c)) EV("ResolveSubresource res#%d <- res#%d %s", Id(d, "res"), Id(s, "res"), FormatName(f));
    o_ResolveSubresource(c, d, ds, s, ss, f);
}
static void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (CapturingOn(c)) EV("Dispatch %u %u %u", x, y, z);
    o_Dispatch(c, x, y, z);
}
static void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (CapturingOn(c)) Tr("%05d CSSetShader cs%p\n", ++g_cap.event, (void*)s);
    o_CSSetShader(c, s, ci, n);
}
static void STDMETHODCALLTYPE H_OMSetBlendState(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT* f, UINT m) {
    if (t_internal == 0 && b && c == G().context && StereoHudPhase()) b = StereoHudBlend(b);
    if (CapturingOn(c) && b) {
        D3D11_BLEND_DESC d; b->GetDesc(&d);
        Tr("%05d OMSetBlendState blend=%d src=%d dst=%d mask=%X\n", ++g_cap.event, d.RenderTarget[0].BlendEnable,
           d.RenderTarget[0].SrcBlend, d.RenderTarget[0].DestBlend, d.RenderTarget[0].RenderTargetWriteMask);
    }
    o_OMSetBlendState(c, b, f, m);
}
static void STDMETHODCALLTYPE H_OMSetDepthStencilState(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref) {
    if (CapturingOn(c) && s) {
        D3D11_DEPTH_STENCIL_DESC d; s->GetDesc(&d);
        Tr("%05d OMSetDepthStencilState depth=%d write=%d func=%d\n", ++g_cap.event, d.DepthEnable, d.DepthWriteMask, d.DepthFunc);
    }
    o_OMSetDepthStencilState(c, s, ref);
}

static void STDMETHODCALLTYPE H_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore) {
    if (CapturingOn(c)) EV("ExecuteCommandList list=%p restore=%d (deferred draws so far %ld)", (void*)l, restore, g_deferredDraws);
    o_ExecuteCommandList(c, l, restore);
}
static HRESULT STDMETHODCALLTYPE H_FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** l) {
    HRESULT hr = o_FinishCommandList(c, restore, l);
    if (g_cap.active) LOGI("capture: FinishCommandList on ctx %p thread %lu -> %p", (void*)c, GetCurrentThreadId(), l ? (void*)*l : nullptr);
    return hr;
}

#define HOOKCTX(obj, name) do { void* p = HookVtable(obj, VT_CTX_##name, (void*)&H_##name); if (p) o_##name = (PFN_##name)p; } while (0)

static void InstallContextHooks(ID3D11DeviceContext* c) {
    HOOKCTX(c, Draw); HOOKCTX(c, DrawIndexed); HOOKCTX(c, DrawIndexedInstanced); HOOKCTX(c, DrawInstanced);
    HOOKCTX(c, Map); HOOKCTX(c, Unmap); HOOKCTX(c, UpdateSubresource);
    HOOKCTX(c, VSSetConstantBuffers); HOOKCTX(c, PSSetConstantBuffers);
    HOOKCTX(c, VSSetShader); HOOKCTX(c, PSSetShader); HOOKCTX(c, PSSetShaderResources);
    HOOKCTX(c, OMSetRenderTargets); HOOKCTX(c, OMSetRenderTargetsAndUnorderedAccessViews);
    HOOKCTX(c, RSSetViewports); HOOKCTX(c, ClearRenderTargetView); HOOKCTX(c, ClearDepthStencilView);
    HOOKCTX(c, CopyResource); HOOKCTX(c, CopySubresourceRegion); HOOKCTX(c, ResolveSubresource);
    HOOKCTX(c, Dispatch); HOOKCTX(c, CSSetShader); HOOKCTX(c, OMSetBlendState); HOOKCTX(c, OMSetDepthStencilState);
    HOOKCTX(c, ExecuteCommandList); HOOKCTX(c, FinishCommandList);
    LOGI("Context hooks installed (vtable %p)", *(void**)c);
    LogContextVtable("after hook");
}

static void InstallContextHooks(ID3D11DeviceContext* c);
// ---------------------------------------------------------------- device hooks
static HRESULT STDMETHODCALLTYPE H_CreateVertexShader(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11VertexShader** out) {
    HRESULT hr = o_CreateVertexShader(d, code, len, l, out);
    if (SUCCEEDED(hr) && out && *out) {
        std::lock_guard<std::mutex> lk(g_shaderMutex);
        g_shaderCode[*out].assign((const uint8_t*)code, (const uint8_t*)code + len);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreatePixelShader(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l, ID3D11PixelShader** out) {
    HRESULT hr = o_CreatePixelShader(d, code, len, l, out);
    if (SUCCEEDED(hr) && out && *out) {
        std::lock_guard<std::mutex> lk(g_shaderMutex);
        g_shaderCode[*out].assign((const uint8_t*)code, (const uint8_t*)code + len);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateDeferredContext(ID3D11Device* d, UINT f, ID3D11DeviceContext** out) {
    HRESULT hr = o_CreateDeferredContext(d, f, out);
    LOGI("Game created a deferred context %p (hr=%08X), vtable %p", out ? (void*)*out : nullptr, hr, out && *out ? *(void**)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out && G().context) {
        void** dv = *(void***)*out; void** iv = *(void***)G().context;
        LOGI("  deferred vs immediate: Draw %p/%p DrawIndexed %p/%p Map %p/%p (same vtable=%d)",
             dv[VT_CTX_Draw], iv[VT_CTX_Draw], dv[VT_CTX_DrawIndexed], iv[VT_CTX_DrawIndexed], dv[VT_CTX_Map], iv[VT_CTX_Map], dv == iv);
    }
    return hr;
}

// ---------------------------------------------------------------- swapchain hooks
static void PollCaptureTriggers() {
    static bool keyWas = false;
    bool key = (GetAsyncKeyState(g_captureKey) & 0x8000) != 0;
    if (key && !keyWas) RequestCapture();
    keyWas = key;
    if ((G().frame % 30) == 0) {
        std::wstring trig = G().gameDir + L"capture.now";
        if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trig.c_str());
            RequestCapture();
        }
    }
}

static HRESULT STDMETHODCALLTYPE H_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) StereoBeforePresent(sc);
    if (g_cap.active && t_internal == 0) EndCapture(sc);
    HRESULT hr = o_Present(sc, sync, flags);
    G().frame++;
    GameOnPresent();
    InputOnPresent();
    static DWORD lastT = GetTickCount(); static uint64_t lastF = 0;
    DWORD now = GetTickCount();
    if (now - lastT >= 10000) {
        LOGI("frame %llu, %.1f fps", (unsigned long long)G().frame, (G().frame - lastF) * 1000.0 / (now - lastT));
        lastT = now; lastF = G().frame;
    }
    PollCaptureTriggers();
    if (g_cap.requested && !(flags & DXGI_PRESENT_TEST)) { g_cap.requested = false; BeginCapture(); }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    UINT ew, eh;
    if (StereoWantsSwapchainSize(&ew, &eh)) { w = ew; h = eh; }
    LOGI("ResizeBuffers %u x %u fmt=%s count=%u", w, h, FormatName(f), n);
    StereoReleaseBackbuffer();
    HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, fl);
    if (SUCCEEDED(hr)) StereoOnSwapchain(sc);
    return hr;
}

static void InstallSwapchainHooks(IDXGISwapChain* sc) {
    G().swapchain = sc;
    void* p;
    if ((p = HookVtable(sc, VT_SC_Present, (void*)&H_Present))) o_Present = (PFN_Present)p;
    if ((p = HookVtable(sc, VT_SC_ResizeBuffers, (void*)&H_ResizeBuffers))) o_ResizeBuffers = (PFN_ResizeBuffers)p;
    DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
    G().hwnd = d.OutputWindow;
    LOGI("Swapchain %p: %ux%u %s buffers=%u windowed=%d swapEffect=%d msaa=%u hwnd=%p",
         sc, d.BufferDesc.Width, d.BufferDesc.Height, FormatName(d.BufferDesc.Format), d.BufferCount, d.Windowed,
         d.SwapEffect, d.SampleDesc.Count, d.OutputWindow);
    StereoOnSwapchain(sc);
}

static HRESULT STDMETHODCALLTYPE H_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    UINT ew, eh;
    if (d && StereoWantsSwapchainSize(&ew, &eh)) {
        LOGI("CreateSwapChain: overriding %ux%u -> %ux%u (per-eye render size)", d->BufferDesc.Width, d->BufferDesc.Height, ew, eh);
        d->BufferDesc.Width = ew; d->BufferDesc.Height = eh;
    }
    HRESULT hr = o_CreateSwapChain(f, dev, d, out);
    LOGI("CreateSwapChain hr=%08X", hr);
    if (SUCCEEDED(hr) && out && *out) InstallSwapchainHooks(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, h, d, fd, o, out);
    LOGI("CreateSwapChainForHwnd hr=%08X", hr);
    if (SUCCEEDED(hr) && out && *out) InstallSwapchainHooks(*out);
    return hr;
}

static void InstallFactoryHooks(ID3D11Device* dev) {
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return;
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC ade; ad->GetDesc(&ade);
        LOGI("Adapter: %S (VRAM %llu MB)", ade.Description, (unsigned long long)(ade.DedicatedVideoMemory >> 20));
        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac))) {
            void* p;
            if ((p = HookVtable(fac, VT_FAC_CreateSwapChain, (void*)&H_CreateSwapChain))) o_CreateSwapChain = (PFN_CreateSwapChain)p;
            IDXGIFactory2* f2 = nullptr;
            if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
                if ((p = HookVtable(f2, VT_FAC2_CreateSwapChainForHwnd, (void*)&H_CreateSwapChainForHwnd))) o_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)p;
                f2->Release();
            }
            fac->Release();
        }
        ad->Release();
    }
    dxgiDev->Release();
}

static HRESULT WINAPI H_D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx) {
    LOGI("D3D11CreateDevice(driverType=%d flags=0x%X levels=%u)", dt, flags, nfl);
    // We need our own context pointer even if the game passes null.
    ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
    HRESULT hr = o_D3D11CreateDevice_(a, dt, sw, flags, fl, nfl, sdk, &d, outFl, &c);
    LOGI("  -> hr=%08X device=%p fl=%X", hr, d, outFl ? *outFl : 0);
    if (SUCCEEDED(hr) && d) {
        if (!G().device && dev && ctx) {
            G().device = d; G().context = c;
            void* p;
            if ((p = HookVtable(d, VT_DEV_CreateVertexShader, (void*)&H_CreateVertexShader))) o_CreateVertexShader = (PFN_CreateVertexShader)p;
            if ((p = HookVtable(d, VT_DEV_CreatePixelShader, (void*)&H_CreatePixelShader))) o_CreatePixelShader = (PFN_CreatePixelShader)p;
            if ((p = HookVtable(d, VT_DEV_CreateDeferredContext, (void*)&H_CreateDeferredContext))) o_CreateDeferredContext = (PFN_CreateDeferredContext)p;
            InstallContextHooks(c);
            InstallFactoryHooks(d);
            XrLinkInit();
            StereoOnDevice(d, c);
        }
    }
    if (dev) *dev = d; else if (d) d->Release();
    if (ctx) *ctx = c; else if (c) c->Release();
    return hr;
}

static void LogContextVtable(const char* tag) {
    if (!G().context) return;
    void** iv = *(void***)G().context;
    LOGI("%s: ctx %p vtable %p Draw=%p DrawIndexed=%p VSSetCB=%p", tag, (void*)G().context, (void*)iv, iv[VT_CTX_Draw], iv[VT_CTX_DrawIndexed], iv[VT_CTX_VSSetConstantBuffers]);
}

void InstallD3DHooks() {
    g_captureKey = G().cfg.GetInt("capture_key", VK_F11);
    g_dumpTargets = G().cfg.GetBool("capture_dump_targets", true);
    void* p = HookIAT(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", (void*)&H_D3D11CreateDevice);
    if (p) { o_D3D11CreateDevice_ = (PFN_D3D11CreateDevice_)p; LOGI("Hooked D3D11CreateDevice via IAT"); }
    else LOGE("Failed to hook D3D11CreateDevice IAT entry");
}

} // namespace avr
