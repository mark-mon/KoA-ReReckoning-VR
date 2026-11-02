// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
//
// In-headset settings menu (vrkit, Dear ImGui) for Amalur VR: registers the config.txt settings that can be
// changed from inside the game, and hands the menu texture to the XR host, which shows it as its own panel.
// Open: hold L3 + R3 (both stick clicks) or press Insert. See third_party/vrkit/README.md.
#include "menu_options.h"
#include "settings.h"
#include "globals.h"
#include "d3dhooks.h"
#include "xrlink.h"
#include "vrs.h"
#include "dds.h"
#include "../common/version.h"
#include "../../third_party/vrkit/vrkit_options.h"
#include "../../third_party/vrkit/vrkit_menu.h"
#include "../../third_party/minhook/include/MinHook.h"
#include <string>
#include <cstdio>

namespace avr {
namespace {

bool g_inited = false, g_failed = false;
std::wstring g_cfgPath;
std::string g_hudAnchorStr, g_panelAnchorStr, g_fovStr, g_fovStrengthStr, g_chordStr;

// shared copy of the menu texture for the host
ID3D11Texture2D* g_shared = nullptr;
IDXGIKeyedMutex* g_sharedMutex = nullptr;
bool g_wasOpen = false;

double g_fps = 0;
LARGE_INTEGER g_last{}, g_qpf{};
int g_frames = 0;

void LogFn(const char* m) { LOGI("%s", m); }
const char* AnchorStr(int a) { return a == 1 ? "head" : "world"; }

std::string Status() {
    UINT w = 0, h = 0;
    if (G().swapchain) { DXGI_SWAP_CHAIN_DESC d; if (SUCCEEDED(G().swapchain->GetDesc(&d))) { w = d.BufferDesc.Width; h = d.BufferDesc.Height; } }
    char b[160];
    snprintf(b, sizeof(b), "%.0f fps | %ux%u per eye | foveation %s", g_fps, w, h, VrsStatus());
    return b;
}

void Push() { SettingsPushToHost(); }

void Register() {
    auto& o = vrkit::Options::Get();
    if (!o.All().empty()) return;
    VrSettings& s = VS();

    // ---- View
    o.AddFloat("View", "world_scale", "World scale", &s.worldScale, 0.25f, 4.0f, 0.05f, 2,
               "Larger = the world looks smaller (more stereo depth, larger head movement).");
    o.AddFloat("View", "ipd_scale", "Eye separation", &s.ipdScale, 0.5f, 2.0f, 0.05f, 2,
               "Multiplier on your headset's eye distance (1 = real).");
    o.AddBool("View", "camera_pitch_lock", "Level horizon", &s.pitchLock,
              "Remove the game camera's up/down tilt; look up and down with your head.\nRecommended against motion sickness.");
    o.AddInt("View", "game_fov_vertical", "Game field of view", &s.gameFovVertical, 40, 80, 5,
             "The game's own camera FOV. Wider = less pop-in at the edges when you turn your head.").needsRestart = true;
    o.AddAction("View", "Recentre view", [] { XrRequestRecenter(); },
                "Makes your current head position and direction the centre.\nAlso: hold Back/View + click the right stick.");

    // ---- Display
    o.AddFloat("Display", "render_scale", "Render scale", &s.renderScale, 0.5f, 2.0f, 0.05f, 2,
               "Multiplies the headset's recommended per-eye resolution.\nLower for more fps, higher for sharpness.").needsRestart = true;
    o.AddFloat("Display", "min_render_aspect", "Minimum image width", &s.minRenderAspect, 0.0f, 1.8f, 0.05f, 3,
               "Widens the render to at least this width/height ratio so the game's dialog UI fits\n(1.333 = 4:3, 0 = off).").needsRestart = true;
    o.AddBool("Display", "desktop_mirror", "Mirror on the monitor", &s.desktopMirror,
              "Show the left eye and the HUD in the game window.");

    // ---- Foveated rendering
    g_fovStr = s.foveation == 2 ? "gaze" : s.foveation == 1 ? "fixed" : "off";
    auto& fv = o.AddChoice("Foveation", "foveation", "Foveated rendering", &g_fovStr, {"off", "fixed", "gaze"},
        "Shade the outer part of each eye at a reduced rate for more fps (NVIDIA RTX).\nEye-tracked: the sharp area follows your eyes.",
        [] { VS().foveation = g_fovStr == "gaze" ? 2 : g_fovStr == "fixed" ? 1 : 0; });
    fv.choiceLabels = {"Off", "Fixed (centre of each lens)", "Eye-tracked"};
    static const char* kStr[] = {"low", "medium", "high"};
    g_fovStrengthStr = kStr[s.foveationStrength < 0 || s.foveationStrength > 2 ? 1 : s.foveationStrength];
    auto& fs = o.AddChoice("Foveation", "foveation_strength", "Strength", &g_fovStrengthStr, {"low", "medium", "high"},
        "How coarsely the outer areas are shaded. Higher = more fps, blurrier edges.",
        [] { VS().foveationStrength = g_fovStrengthStr == "low" ? 0 : g_fovStrengthStr == "high" ? 2 : 1; });
    fs.choiceLabels = {"Low", "Medium", "High"};
    fs.visible = [] { return VS().foveation != 0; };
    o.AddFloat("Foveation", "foveation_inner", "Full-quality radius", &s.foveationInner, 0.1f, 1.5f, 0.05f, 2,
               "Radius of the full-quality centre (1.0 = half the image width).").visible = [] { return VS().foveation != 0; };
    o.AddFloat("Foveation", "foveation_outer", "Lowest-rate radius", &s.foveationOuter, 0.1f, 2.0f, 0.05f, 2,
               "Beyond this radius the lowest rate is used; between the two a middle rate.").visible = [] { return VS().foveation != 0; };

    // ---- HUD panel
    o.AddFloat("HUD", "hud_distance", "Distance (m)", &s.hudDistance, 0.3f, 5.0f, 0.05f, 2, "How far away the HUD panel is.", Push);
    o.AddFloat("HUD", "hud_width", "Width (m)", &s.hudWidth, 0.2f, 5.0f, 0.05f, 2, "Width of the HUD panel; its height follows the image shape.", Push);
    o.AddFloat("HUD", "hud_offset_y", "Height offset (m)", &s.hudOffsetY, -1.5f, 1.5f, 0.02f, 2, "Up (+) or down (-) from eye level.", Push);
    g_hudAnchorStr = AnchorStr(s.hudAnchor);
    auto& ha = o.AddChoice("HUD", "hud_anchor", "Anchor", &g_hudAnchorStr, {"world", "head"},
        "World: fixed in space in front of the recentre point (Recentre moves it).\nHead: follows your head.\nAlso: hold Back/View + click the left stick.",
        [] { VS().hudAnchor = g_hudAnchorStr == "head" ? 1 : 0; Push(); });
    ha.choiceLabels = {"World (fixed in space)", "Head (follows)"};
    o.AddFloat("HUD", "menu_distance", "Game menus: distance (m)", &s.menuDistance, 0.5f, 6.0f, 0.05f, 2,
               "Distance of the large panel used for the game's own menus and loading screens.", Push);
    o.AddFloat("HUD", "menu_width", "Game menus: width (m)", &s.menuWidth, 0.5f, 6.0f, 0.05f, 2,
               "Width of the panel for the game's own menus and loading screens.", Push);

    // ---- This settings panel
    o.AddFloat("Settings panel", "settings_distance", "Distance (m)", &s.panel.distance, 0.4f, 3.0f, 0.05f, 2, "How far away this panel is.", Push);
    o.AddFloat("Settings panel", "settings_width", "Width (m)", &s.panel.width, 0.3f, 2.0f, 0.05f, 2,
               "Width of this panel. Narrower is easier to read on headsets with soft edges.", Push);
    o.AddFloat("Settings panel", "settings_offset_y", "Height offset (m)", &s.panel.offsetY, -1.0f, 1.0f, 0.02f, 2, "Up (+) or down (-) from eye level.", Push);
    g_panelAnchorStr = AnchorStr(s.panel.anchor);
    auto& pa = o.AddChoice("Settings panel", "settings_anchor", "Anchor", &g_panelAnchorStr, {"world", "head"},
        "World: stays where it opened, so you can turn your head to read it.\nHead: follows your head.",
        [] { VS().panel.anchor = g_panelAnchorStr == "head" ? 1 : 0; Push(); });
    pa.choiceLabels = {"World (where you looked)", "Head (follows)"};
    g_chordStr = s.menuChord == "ltrt" ? "ltrt" : s.menuChord == "none" ? "none" : "l3r3";
    auto& ch = o.AddChoice("Settings panel", "menu_chord", "Open with", &g_chordStr, {"l3r3", "ltrt", "none"},
        "Gamepad buttons to hold to open this menu (Insert on the keyboard always works).");
    ch.choiceLabels = {"Hold both stick clicks", "Hold both triggers", "Keyboard only"};
    ch.needsRestart = true;
}

void EnsureShared(ID3D11Device* dev, UINT w, UINT h) {
    if (g_shared) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_shared))) { LOGE("settings menu: shared texture creation failed"); return; }
    g_shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_sharedMutex);
    HANDLE handle = nullptr;
    IDXGIResource* r = nullptr;
    if (SUCCEEDED(g_shared->QueryInterface(__uuidof(IDXGIResource), (void**)&r))) { r->GetSharedHandle(&handle); r->Release(); }
    if (!handle || !g_sharedMutex) { LOGE("settings menu: shared handle failed"); return; }
    XrPublishPanel(w, h, (uint64_t)(uintptr_t)handle);
}

} // namespace

ID3D11Texture2D* MenuOnPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_failed || !dev || !ctx) return nullptr;
    InternalScope internal;
    if (!g_inited) {
        g_inited = true;
        g_cfgPath = G().gameDir + L"config.txt";
        MH_STATUS mh = MH_Initialize();
        if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) { g_failed = true; LOGE("settings menu: MinHook init failed"); return nullptr; }
        vrkit::SetLog(&LogFn);
        Register();
        vrkit::menu::Settings s;
        s.title = "Kingdoms of Amalur VR " AVR_VERSION;
        s.configPath = g_cfgPath;
        s.statusLine = &Status;
        const VrSettings& v = VS();
        s.input.chordHoldMs = v.menuHoldMs;
        if (v.menuChord == "ltrt") { s.input.chordButtons = 0; s.input.chordTriggers = true; }
        else if (v.menuChord == "none") { s.input.chordButtons = 0; s.input.chordTriggers = false; }
        if (!vrkit::menu::Init(dev, ctx, s)) { g_failed = true; LOGE("settings menu: init failed"); return nullptr; }
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_last);
        LOGI("settings menu ready (hold L3+R3 or press Insert)");
    }
    ++g_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - g_last.QuadPart) / double(g_qpf.QuadPart);
    if (dt >= 0.5) { g_fps = g_frames / dt; g_frames = 0; g_last = now; }

    static int retry = 0;
    if (++retry % 300 == 1) vrkit::input::InstallHooks();   // in case XInput is loaded late
    bool open = vrkit::menu::Update();
    if (open != g_wasOpen) { g_wasOpen = open; LOGI("settings menu %s", open ? "opened" : "closed"); }
    if (!open) { XrPanelUpdated(false); return nullptr; }
    ID3D11Texture2D* tex = vrkit::menu::Texture();
    EnsureShared(dev, vrkit::menu::Width(), vrkit::menu::Height());
    // Diagnostic (debug_menu_dump = 1): save the menu image a second after it opens.
    static int openFrames = 0;
    openFrames = g_wasOpen ? openFrames + 1 : 0;
    if (openFrames == 60 && G().cfg.GetBool("debug_menu_dump", false)) {
        SaveTextureDDS(dev, ctx, tex, DXGI_FORMAT_R8G8B8A8_UNORM, G().outDir + L"menu.dds");
        LOGI("settings menu: saved menu.dds");
    }
    if (g_sharedMutex && g_sharedMutex->AcquireSync(0, 5) == S_OK) {
        ctx->CopyResource(g_shared, tex);
        g_sharedMutex->ReleaseSync(0);
        XrPanelUpdated(true);
    }
    return tex;
}

void MenuToggleHudAnchor() {
    for (auto& o : vrkit::Options::Get().All()) {
        if (o.key != "hud_anchor") continue;
        vrkit::Options::Get().Activate(o);    // next choice + onChange
        if (!g_cfgPath.empty()) vrkit::Options::Get().Save(g_cfgPath);
        LOGI("HUD anchor -> %s", g_hudAnchorStr.c_str());
        return;
    }
    VS().hudAnchor ^= 1;                       // menu not initialised yet
    SettingsPushToHost();
    LOGI("HUD anchor -> %s (not saved)", AnchorStr(VS().hudAnchor));
}

} // namespace avr
