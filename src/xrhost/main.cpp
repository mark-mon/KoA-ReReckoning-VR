// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
// AmalurVR_XrHost.exe - 64-bit OpenXR compositor bridge for the Amalur VR mod.
// The 32-bit game renders both eyes + HUD into shared D3D11 textures; this process owns the
// OpenXR session, publishes predicted eye poses, and submits the game's frames to the runtime.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>


#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "../common/ipc.h"
#include "../common/log.h"
#include "../common/version.h"
#include "../../third_party/vrkit/vrkit_menu_placement.h"   // QuadPlacer (needs openxr.h first)

using namespace avr;

// ------------------------------------------------------------------ helpers
#define XRC(x) do { XrResult _r = (x); if (XR_FAILED(_r)) { LOGE("%s failed: %d (line %d)", #x, (int)_r, __LINE__); return false; } } while (0)

static IpcShared* g_shm = nullptr;
static HANDLE g_game = nullptr;
static HANDLE g_evtFrame = nullptr, g_evtHostFrame = nullptr;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
static Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat QConj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
static Vec3 QRot(Quat q, Vec3 v) {
    Quat p{v.x, v.y, v.z, 0};
    Quat r = QMul(QMul(q, p), QConj(q));
    return {r.x, r.y, r.z};
}
static XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {   // a * b : b expressed in a's frame
    Quat qa{a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w};
    Quat qb{b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w};
    Vec3 pb = QRot(qa, {b.position.x, b.position.y, b.position.z});
    Quat q = QMul(qa, qb);
    XrPosef r;
    r.orientation = {q.x, q.y, q.z, q.w};
    r.position = {a.position.x + pb.x, a.position.y + pb.y, a.position.z + pb.z};
    return r;
}
static XrPosef PoseInv(const XrPosef& a) {
    Quat qi = QConj({a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w});
    Vec3 p = QRot(qi, {-a.position.x, -a.position.y, -a.position.z});
    XrPosef r;
    r.orientation = {qi.x, qi.y, qi.z, qi.w};
    r.position = {p.x, p.y, p.z};
    return r;
}
static XrPosef Identity() { XrPosef p{}; p.orientation.w = 1; return p; }

// ------------------------------------------------------------------ state
struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    uint32_t w = 0, h = 0;
};

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static XrViewConfigurationView g_vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
static Swap g_eye[2], g_hud;
static int64_t g_swapFormat = 0;

struct SharedTex { ID3D11Texture2D* tex = nullptr; IDXGIKeyedMutex* mutex = nullptr; };
static SharedTex g_tex[kSlots];
static uint32_t g_texGen = 0, g_texW = 0, g_texH = 0;

static XrPosef g_base = Identity();       // recenter reference (in LOCAL space)
static uint32_t g_lastRecenter = 0;
static bool g_needRecenter = true;

static bool g_haveFrame = false;          // swapchains hold a valid image
static IpcPose g_shownPose[2];
static IpcFov g_shownFov[2];
static uint32_t g_shownFlags = 0;
static uint32_t g_lastSubmitSeq = 0;
static struct { uint32_t frames, shouldRender, fetched, busy, layersProj, layersQuad, panel, gaze; DWORD t0; } g_stats{};

// In-headset settings menu panel (texture shared by the game)
static struct {
    uint32_t gen = 0, seq = 0, w = 0, h = 0;
    SharedTex tex;
    Swap swap;
    bool hasImage = false;
    vrkit::menu::QuadPlacer placer;
} g_panel;

// Eye gaze (XR_EXT_eye_gaze_interaction) for eye-tracked foveated rendering
static bool g_gazeExt = false, g_gazeSupported = false;
static XrActionSet g_gazeSet = XR_NULL_HANDLE;
static XrAction g_gazeAction = XR_NULL_HANDLE;
static XrSpace g_gazeSpace = XR_NULL_HANDLE;
static float g_gazeX = 0, g_gazeY = 0;

// ------------------------------------------------------------------ D3D11 device on the runtime's adapter
static bool CreateDevice(LUID luid) {
    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) { chosen = ad; LOGI("Using adapter: %S", d.Description); break; }
        ad->Release();
    }
    fac->Release();
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { LOGE("D3D11CreateDevice failed %08X", hr); return false; }
    return true;
}

static void CreateGaze();
// ------------------------------------------------------------------ OpenXR setup
static bool InitXr() {
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (auto& e : props) if (!strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) g_gazeExt = true;
        if (g_gazeExt) exts.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        LOGI("OpenXR eye gaze extension: %s", g_gazeExt ? "available" : "not offered by the runtime");
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "Amalur VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "AmalurVR");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    XRC(xrCreateInstance(&ci, &g_inst));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    strncpy_s(g_shm->runtimeName, ip.runtimeName, _TRUNCATE);

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (int tries = 0;; ++tries) {
        XrResult r = xrGetSystem(g_inst, &sg, &g_sys);
        if (XR_SUCCEEDED(r)) break;
        if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE && tries < 60) { if (tries == 0) LOGW("Headset not available yet, waiting..."); Sleep(1000); continue; }
        LOGE("xrGetSystem failed: %d", (int)r);
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    if (g_gazeExt) sp.next = &gp;
    xrGetSystemProperties(g_inst, g_sys, &sp);
    g_gazeSupported = g_gazeExt && gp.supportsEyeGazeInteraction;
    g_shm->gazeSupported = g_gazeSupported ? 1 : 0;
    LOGI("Eye tracking: %s", g_gazeSupported ? "supported" : "not available");
    LOGI("System: %s (max swapchain %ux%u)", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);
    strncpy_s(g_shm->systemName, sp.systemName, _TRUNCATE);

    uint32_t n = 0;
    XRC(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, g_vcv));
    LOGI("Recommended per-eye %ux%u (max %ux%u)", g_vcv[0].recommendedImageRectWidth, g_vcv[0].recommendedImageRectHeight,
         g_vcv[0].maxImageRectWidth, g_vcv[0].maxImageRectHeight);

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRC(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRC(getReq(g_inst, g_sys, &req));
    g_shm->adapterLuid = ((uint64_t)(uint32_t)req.adapterLuid.HighPart << 32) | req.adapterLuid.LowPart;
    if (!CreateDevice(req.adapterLuid)) return false;

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    XRC(xrCreateSession(g_inst, &sci, &g_sess));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = Identity();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRC(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRC(xrCreateReferenceSpace(g_sess, &rs, &g_view));
    CreateGaze();

    // swapchain format: need the RGBA8 family so the game's atlas can be copied directly
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &fc, nullptr);
    std::vector<int64_t> fmts(fc);
    xrEnumerateSwapchainFormats(g_sess, fc, &fc, fmts.data());
    for (int64_t pref : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM}) {
        for (auto f : fmts) if (f == pref) { g_swapFormat = f; break; }
        if (g_swapFormat) break;
    }
    if (!g_swapFormat) { LOGE("Runtime offers no R8G8B8A8 swapchain format"); return false; }
    LOGI("Swapchain format %lld", (long long)g_swapFormat);

    g_shm->recWidth = g_vcv[0].recommendedImageRectWidth;
    g_shm->recHeight = g_vcv[0].recommendedImageRectHeight;
    MemoryBarrier();
    g_shm->hostState = HOST_READY;
    return true;
}

static bool CreateSwap(Swap& s, uint32_t w, uint32_t h) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_swapFormat;
    ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XRC(xrCreateSwapchain(g_sess, &ci, &s.sc));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRC(xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader*)s.images.data()));
    s.w = w; s.h = h;
    return true;
}

static void DestroySwap(Swap& s) { if (s.sc) xrDestroySwapchain(s.sc); s = Swap(); }

// (Re)open the game's shared atlases when the game publishes a new generation.
static bool SyncSharedTextures() {
    uint32_t gen = g_shm->gameState;
    if (gen == g_texGen) return g_tex[0].tex != nullptr;
    for (auto& t : g_tex) { if (t.mutex) t.mutex->Release(); if (t.tex) t.tex->Release(); t = SharedTex(); }
    g_texGen = gen;
    if (!gen) return false;
    g_texW = g_shm->texWidth; g_texH = g_shm->texHeight;
    for (int i = 0; i < kSlots; ++i) {
        HANDLE h = (HANDLE)(uintptr_t)g_shm->sharedHandle[i];
        if (FAILED(g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&g_tex[i].tex))) {
            LOGE("OpenSharedResource(slot %d, handle %p) failed - is the game on the same GPU as the headset?", i, h);
            return false;
        }
        g_tex[i].tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_tex[i].mutex);
    }
    DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud);
    bool ok = CreateSwap(g_eye[0], g_texW, g_texH) && CreateSwap(g_eye[1], g_texW, g_texH) && CreateSwap(g_hud, g_texW, g_texH);
    g_haveFrame = false;
    LOGI("Opened game atlases %ux%u (gen %u), swapchains %s", g_texW * 3, g_texH, gen, ok ? "ok" : "FAILED");
    return ok;
}


// ------------------------------------------------------------------ eye gaze
static void CreateGaze() {
    if (!g_gazeSupported) return;
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "amalurvr_gaze"); strcpy_s(asi.localizedActionSetName, "Eye gaze");
    if (XR_FAILED(xrCreateActionSet(g_inst, &asi, &g_gazeSet))) { LOGW("eye gaze: action set failed"); return; }
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ai.actionName, "gaze_pose"); strcpy_s(ai.localizedActionName, "Gaze pose");
    XrPath profile = 0, gazePath = 0;
    bool ok = XR_SUCCEEDED(xrCreateAction(g_gazeSet, &ai, &g_gazeAction)) &&
              XR_SUCCEEDED(xrStringToPath(g_inst, "/interaction_profiles/ext/eye_gaze_interaction", &profile)) &&
              XR_SUCCEEDED(xrStringToPath(g_inst, "/user/eyes_ext/input/gaze_ext/pose", &gazePath));
    if (ok) {
        XrActionSuggestedBinding b{g_gazeAction, gazePath};
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = profile; sb.countSuggestedBindings = 1; sb.suggestedBindings = &b;
        ok = XR_SUCCEEDED(xrSuggestInteractionProfileBindings(g_inst, &sb));
    }
    if (ok) {
        XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        at.countActionSets = 1; at.actionSets = &g_gazeSet;
        ok = XR_SUCCEEDED(xrAttachSessionActionSets(g_sess, &at));
    }
    if (ok) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = g_gazeAction; sci.poseInActionSpace.orientation.w = 1.0f;
        ok = XR_SUCCEEDED(xrCreateActionSpace(g_sess, &sci, &g_gazeSpace));
    }
    LOGI("eye gaze: %s", ok ? "ready" : "setup failed");
    if (!ok) { if (g_gazeSet) xrDestroyActionSet(g_gazeSet); g_gazeSet = XR_NULL_HANDLE; g_gazeAction = XR_NULL_HANDLE; g_gazeSpace = XR_NULL_HANDLE; }
}

// Once per headset frame: gaze direction relative to the head as tangents (x right, y up) -> shared memory.
static void UpdateGaze(XrTime t) {
    if (g_shm->debugFakeGaze) {   // testing without a tracker: a slow circle
        double a = double(GetTickCount64() % 6000) / 6000.0 * 6.2831853;
        g_shm->gazeTanX = float(0.35 * cos(a)); g_shm->gazeTanY = float(0.25 * sin(a));
        g_shm->gazeSeq = g_shm->gazeSeq + 1;
        g_stats.gaze++;
        return;
    }
    if (!g_gazeSpace) return;
    XrActiveActionSet aas{g_gazeSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_sess, &si))) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_gazeSpace, g_view, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const XrQuaternionf& q = loc.pose.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y);              // forward = q * (0, 0, -1)
    float fy = -2.0f * (q.y * q.z - q.w * q.x);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    if (fz > -0.2f) return;
    float tx = fx / -fz, ty = fy / -fz, k = 0.5f;            // light smoothing
    g_gazeX += (tx - g_gazeX) * k; g_gazeY += (ty - g_gazeY) * k;
    g_shm->gazeTanX = g_gazeX; g_shm->gazeTanY = g_gazeY;
    MemoryBarrier();
    g_shm->gazeSeq = g_shm->gazeSeq + 1;
    g_stats.gaze++;
}

// ------------------------------------------------------------------ settings menu panel
static void ReleasePanelTex() {
    if (g_panel.tex.mutex) g_panel.tex.mutex->Release();
    if (g_panel.tex.tex) g_panel.tex.tex->Release();
    g_panel.tex = SharedTex();
}

// True when the panel has an image to show this frame.
static bool UpdatePanel() {
    if (!g_shm->panelVisible) return false;
    uint32_t gen = g_shm->panelGen;
    if (gen != g_panel.gen) {
        ReleasePanelTex();
        g_panel.gen = gen;
        g_panel.hasImage = false;
        g_panel.seq = 0;
        HANDLE h = (HANDLE)(uintptr_t)g_shm->panelHandle;
        if (!h || FAILED(g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&g_panel.tex.tex))) {
            LOGE("settings panel: OpenSharedResource failed");
            return false;
        }
        g_panel.tex.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_panel.tex.mutex);
        uint32_t w = g_shm->panelW, h2 = g_shm->panelH;
        if (!g_panel.swap.sc || g_panel.swap.w != w || g_panel.swap.h != h2) {
            DestroySwap(g_panel.swap);
            if (!CreateSwap(g_panel.swap, w, h2)) { LOGE("settings panel: swapchain failed"); return false; }
            LOGI("settings panel swapchain %ux%u", w, h2);
        }
    }
    if (!g_panel.tex.tex || !g_panel.swap.sc) return false;
    uint32_t sq = g_shm->panelSeq;
    if (sq != g_panel.seq && g_panel.tex.mutex && g_panel.tex.mutex->AcquireSync(0, 4) == S_OK) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_panel.swap.sc, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = 100000000;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(g_panel.swap.sc, &wi))) {
                g_ctx->CopyResource(g_panel.swap.images[idx].texture, g_panel.tex.tex);
                g_panel.hasImage = true;
                g_panel.seq = sq;
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(g_panel.swap.sc, &ri);
        }
        g_panel.tex.mutex->ReleaseSync(0);
    }
    return g_panel.hasImage;
}

// ------------------------------------------------------------------ frame
static void Recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    // keep only yaw (rotation about +Y) and position
    Quat q{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    Vec3 fwd = QRot(q, {0, 0, -1});
    float yaw = atan2f(-fwd.x, -fwd.z);
    g_base.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_base.position = loc.pose.position;
    LOGI("Recentered: yaw %.1f deg, pos (%.2f %.2f %.2f)", yaw * 57.2958f, g_base.position.x, g_base.position.y, g_base.position.z);
}

static void PublishPoses(const XrView views[2], XrTime t) {
    XrPosef inv = PoseInv(g_base);
    g_shm->poseSeq = g_shm->poseSeq + 1;   // odd: writing
    MemoryBarrier();
    for (int e = 0; e < 2; ++e) {
        XrPosef p = PoseMul(inv, views[e].pose);
        g_shm->eyePose[e] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        g_shm->eyeFov[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown};
    }
    g_shm->predictedTime = t;
    MemoryBarrier();
    g_shm->poseSeq = g_shm->poseSeq + 1;
}

static void CopyRegion(Swap& s, ID3D11Texture2D* src, int region) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(s.sc, &wi))) {
        D3D11_BOX box{(UINT)region * g_texW, 0, 0, (UINT)(region + 1) * g_texW, g_texH, 1};
        g_ctx->CopySubresourceRegion(s.images[idx].texture, 0, 0, 0, 0, src, 0, &box);
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.sc, &ri);
}

// Pull the newest finished game frame into the swapchains (if there is one).
static void FetchGameFrame() {
    if (!SyncSharedTextures()) return;
    uint32_t seq = g_shm->submitSeq;
    if (seq == g_lastSubmitSeq && g_haveFrame) return;
    int slot = g_shm->lastSlot;
    if (slot < 0 || slot >= kSlots || !g_tex[slot].mutex) return;
    if (g_tex[slot].mutex->AcquireSync(1, 4) != S_OK) { g_stats.busy++; return; }   // game still owns it (or it was recycled)
    g_stats.fetched++;
    IpcPose pose[2]; IpcFov fov[2];
    memcpy(pose, g_shm->slotPose[slot], sizeof(pose));
    memcpy(fov, g_shm->slotFov[slot], sizeof(fov));
    uint32_t flags = g_shm->slotFlags[slot];
    if (!(flags & SLOT_MENU_MODE)) { CopyRegion(g_eye[0], g_tex[slot].tex, 0); CopyRegion(g_eye[1], g_tex[slot].tex, 1); }
    CopyRegion(g_hud, g_tex[slot].tex, 2);
    g_tex[slot].mutex->ReleaseSync(0);
    memcpy(g_shownPose, pose, sizeof(pose));
    memcpy(g_shownFov, fov, sizeof(fov));
    g_shownFlags = flags;
    g_lastSubmitSeq = seq;
    g_haveFrame = true;
}

static bool RunFrame() {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XRC(xrWaitFrame(g_sess, &fwi, &fs));
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    XRC(xrBeginFrame(g_sess, &fbi));

    if (g_needRecenter || g_shm->recenterRequest != g_lastRecenter) {
        g_lastRecenter = g_shm->recenterRequest;
        g_needRecenter = false;
        Recenter(fs.predictedDisplayTime);
    }
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews(g_sess, &vli, &vs, 2, &vc, views)) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        PublishPoses(views, fs.predictedDisplayTime);
    UpdateGaze(fs.predictedDisplayTime);
    g_shm->hostHeartbeat = g_shm->hostHeartbeat + 1;
    if (g_evtHostFrame) SetEvent(g_evtHostFrame);   // pace the game to the headset

    FetchGameFrame();

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender && g_haveFrame) {
        bool menu = (g_shownFlags & SLOT_MENU_MODE) != 0;
        if (!menu) {
            for (int e = 0; e < 2; ++e) {
                const IpcPose& p = g_shownPose[e];
                XrPosef rel{{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}};
                pv[e].pose = PoseMul(g_base, rel);   // back to LOCAL space
                pv[e].fov = {g_shownFov[e].left, g_shownFov[e].right, g_shownFov[e].up, g_shownFov[e].down};
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_texW, (int32_t)g_texH}};
            }
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);
        }
        if (g_shownFlags & SLOT_HUD_VISIBLE) {
            float dist = menu ? g_shm->menuDistance : g_shm->hudDistance;
            float width = menu ? g_shm->menuWidth : g_shm->hudWidth;
            quad.layerFlags = menu ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.subImage.swapchain = g_hud.sc;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_texW, (int32_t)g_texH}};
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.size = {width, width * (float)g_texH / (float)g_texW};
            if (!menu && g_shm->hudAnchor == 1) {
                quad.space = g_view;
                quad.pose = Identity();
                quad.pose.position = {0, g_shm->hudOffsetY, -dist};
            } else {
                quad.space = g_local;
                XrPosef off = Identity();
                off.position = {0, menu ? 0.0f : g_shm->hudOffsetY, -dist};
                quad.pose = PoseMul(g_base, off);
            }
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }
    }
    // Settings menu panel, on top of everything.
    XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool panelShown = fs.shouldRender && UpdatePanel();
    {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        bool headOk = panelShown && XR_SUCCEEDED(xrLocateSpace(g_view, g_local, fs.predictedDisplayTime, &head)) &&
                      (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        g_panel.placer.Update(panelShown, headOk, head.pose);
    }
    if (panelShown) {
        vrkit::menu::Placement pl;
        pl.distance = g_shm->panelDistance; pl.width = g_shm->panelWidth; pl.offsetY = g_shm->panelOffsetY;
        pl.anchor = (int)g_shm->panelAnchor;
        panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
        panel.subImage.swapchain = g_panel.swap.sc;
        panel.subImage.imageRect = {{0, 0}, {(int32_t)g_panel.swap.w, (int32_t)g_panel.swap.h}};
        panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        g_panel.placer.Apply(pl, g_local, g_view, g_panel.swap.w, g_panel.swap.h, panel);
        layers.push_back((XrCompositionLayerBaseHeader*)&panel);
        g_stats.panel++;
    }
    g_stats.frames++;
    if (fs.shouldRender) g_stats.shouldRender++;
    for (auto* l : layers) { if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) g_stats.layersProj++; else g_stats.layersQuad++; }
    DWORD now = GetTickCount();
    if (!g_stats.t0) g_stats.t0 = now;
    if (now - g_stats.t0 >= 10000) {
        float sec = (now - g_stats.t0) / 1000.0f;
        LOGI("stats: %.1f host fps, shouldRender %u, game frames %.1f/s, slot busy %u, proj layers %u, quad layers %u (settings panel %u), gaze samples %u, state %d",
             g_stats.frames / sec, g_stats.shouldRender, g_stats.fetched / sec, g_stats.busy, g_stats.layersProj, g_stats.layersQuad,
             g_stats.panel, g_stats.gaze, (int)g_state);
        g_stats = {}; g_stats.t0 = now;
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)layers.size();
    fei.layers = layers.data();
    XRC(xrEndFrame(g_sess, &fei));
    return true;
}

static bool PollEvents(bool* quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g_state = e->state;
            LOGI("Session state -> %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XRC(xrBeginSession(g_sess, &bi));
                g_running = true;
                g_shm->hostState = HOST_READY;
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false;
                g_shm->hostState = HOST_READY;
            } else if (g_state == XR_SESSION_STATE_FOCUSED) {
                g_shm->hostState = HOST_FOCUSED;
            } else if (g_state == XR_SESSION_STATE_VISIBLE) {
                g_shm->hostState = HOST_RUNNING;
            } else if (g_state == XR_SESSION_STATE_SYNCHRONIZED) {
                g_shm->hostState = HOST_READY;   // not shown yet (runtime may throttle): game must not pace to us
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                *quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: *quit = true; break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: g_needRecenter = true; break;
        default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

// ------------------------------------------------------------------ main
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD pid = 0;
    std::wstring dir = L".\\";
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = argv[++i];
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    CreateDirectoryW((dir + L"AmalurVR").c_str(), nullptr);
    LogInit((dir + L"AmalurVR\\xrhost.log").c_str(), LOG_INFO);
    LOGI("Amalur VR XR host " AVR_VERSION " (" __DATE__ " " __TIME__ ") (game pid %lu)", pid);

    HANDLE map = nullptr;
    for (int i = 0; i < 100 && !map; ++i) { map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, AVR_SHM_NAME); if (!map) Sleep(50); }
    if (!map) { LOGE("Shared memory not found - start the game, not this exe"); return 1; }
    g_shm = (IpcShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcShared));
    if (!g_shm || g_shm->magic != kIpcMagic || g_shm->version != kIpcVersion) { LOGE("Shared memory version mismatch"); return 1; }
    LogSetLevel((int)g_shm->hostLogLevel);
    g_shm->hostPid = GetCurrentProcessId();
    g_shm->hostState = HOST_STARTING;
    g_evtFrame = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, AVR_EVT_FRAME);
    g_evtHostFrame = CreateEventW(nullptr, FALSE, FALSE, AVR_EVT_HOSTFRAME);
    if (pid) g_game = OpenProcess(SYNCHRONIZE, FALSE, pid ? pid : g_shm->gamePid);

    if (!InitXr()) {
        g_shm->hostState = HOST_ERROR;
        strcpy_s(g_shm->hostError, "OpenXR initialisation failed - see AmalurVR\\xrhost.log");
        LOGE("OpenXR init failed");
        return 2;
    }

    bool quit = false;
    while (!quit) {
        if (g_game && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) { LOGI("Game exited"); break; }
        if (g_shm->quitRequest) break;
        PollEvents(&quit);
        if (g_running) {
            if (!RunFrame()) { Sleep(10); }
        } else {
            Sleep(20);
        }
    }
    LOGI("Shutting down");
    if (g_running) xrEndSession(g_sess);
    DestroySwap(g_eye[0]); DestroySwap(g_eye[1]); DestroySwap(g_hud); DestroySwap(g_panel.swap);
    ReleasePanelTex();
    if (g_gazeSet) xrDestroyActionSet(g_gazeSet);
    if (g_sess) xrDestroySession(g_sess);
    if (g_inst) xrDestroyInstance(g_inst);
    g_shm->hostState = HOST_NONE;
    LogFlush();
    return 0;
}
