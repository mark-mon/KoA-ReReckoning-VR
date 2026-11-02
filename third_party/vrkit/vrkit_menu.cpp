#include "vrkit_menu.h"
#include "vrkit_options.h"
#include <windows.h>
#include <d3d11_1.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"

namespace vrkit { namespace menu {

namespace {
ID3D11Device* g_dev = nullptr; ID3D11DeviceContext* g_ctx = nullptr; ID3D11DeviceContext1* g_ctx1 = nullptr;
ID3DDeviceContextState* g_state = nullptr;
ID3D11Texture2D* g_tex = nullptr; ID3D11RenderTargetView* g_rtv = nullptr;
bool g_ok = false;

void Draw() {
    const Settings& s = core::Get();
    ImGui_ImplDX11_NewFrame();
    core::BuildFrame();
    // draw into our texture with our own pipeline state (the game's state is untouched)
    ID3DDeviceContextState* prev = nullptr;
    if (g_ctx1 && g_state) g_ctx1->SwapDeviceContextState(g_state, &prev);
    const float clear[4] = { 0, 0, 0, 0 };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)s.texW, (float)s.texH, 0, 1 }; g_ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_ctx1 && g_state) { g_ctx1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); }
}
} // namespace

bool Init(ID3D11Device* dev, ID3D11DeviceContext* imm, const Settings& s) {
    if (g_ok) return true;
    g_dev = dev; g_ctx = imm;
    imm->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1);
    ID3D11Device1* dev1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen;
        dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &g_state);
        dev1->Release();
    }
    D3D11_TEXTURE2D_DESC d = {}; d.Width = s.texW; d.Height = s.texH; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex))) { Log("vrkit: menu texture failed"); return false; }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(g_tex, &rd, &g_rtv))) { Log("vrkit: menu RTV failed"); return false; }
    if (!core::InitImGui(s)) return false;
    if (!ImGui_ImplDX11_Init(dev, imm)) { Log("vrkit: ImGui D3D11 init failed"); return false; }
    g_ok = true;
    Log("vrkit: menu ready (%zu options on %zu pages)", Options::Get().All().size(), Options::Get().Pages().size());
    return true;
}

void Shutdown() {
    if (g_ok) ImGui_ImplDX11_Shutdown();
    core::ShutdownImGui();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_state) { g_state->Release(); g_state = nullptr; }
    if (g_ctx1) { g_ctx1->Release(); g_ctx1 = nullptr; }
    g_ok = false;
}

bool Update() {
    if (!g_ok || !core::UpdateInput()) return false;
    Draw();
    return true;
}
bool IsOpen() { return core::IsOpen(); }
void SetOpen(bool open) { core::SetOpen(open); }
ID3D11Texture2D* Texture() { return g_tex; }
UINT Width() { return core::Get().texW; }
UINT Height() { return core::Get().texH; }

} } // namespace vrkit::menu
