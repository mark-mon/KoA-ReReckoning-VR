// vrkit - where the settings menu panel appears in the headset (OpenXR quad layer), and the menu options that
// let the player change it. Header-only. Placement + AddPlacementOptions need only vrkit_options.h, so a config
// header can hold a Placement; QuadPlacer is available when openxr.h is included before this header.
//
//   World anchor (default): the panel is placed in front of where the player was looking when the menu opened
//     and then stays fixed in space, so they can turn their head to read its edges (good for headsets with
//     soft edge clarity).
//   Head anchor: the panel follows the headset (classic head-locked quad).
//
// Host usage (XR thread, once per frame, before xrEndFrame):
//   static vrkit::menu::QuadPlacer placer;
//   bool shown = menuVisible && haveSwapchainImage;
//   XrPosef head; bool headOk = LocateHead(predictedDisplayTime, head);   // VIEW in LOCAL space
//   placer.Update(shown, headOk, head);
//   if (shown) { placer.Apply(cfg.menu, localSpace, viewSpace, texW, texH, quad); layers[n++] = &quad; }
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include "vrkit_options.h"

namespace vrkit { namespace menu {

struct Placement {
    float distance = 1.0f;   // metres in front of the eyes
    float width = 0.75f;     // metres; the height follows the menu texture's aspect
    float offsetY = -0.05f;  // metres, up (+) or down (-) from eye level
    int   anchor = 0;        // 0 = World (fixed where it opened), 1 = Head (follows the headset)
};

#ifdef XR_CURRENT_API_VERSION
class QuadPlacer {
public:
    // Call every frame. Captures the head's heading and position on the frame the menu becomes visible.
    void Update(bool shown, bool headValid, const XrPosef& headInLocal) {
        if (shown && !wasShown_) {
            haveAnchor_ = headValid;
            if (headValid) {
                const XrQuaternionf& q = headInLocal.orientation;
                float fx = -2.0f * (q.x * q.z + q.w * q.y);              // forward (-Z) rotated by q
                float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                yaw_ = std::atan2(-fx, -fz);                              // heading only: the panel stays upright
                pos_ = headInLocal.position;
            }
        }
        wasShown_ = shown;
    }
    // Forget the anchor (e.g. after a recentre) so the next Update while shown re-captures it.
    void Reset() { wasShown_ = false; haveAnchor_ = false; }

    // Fills space, pose and size of the quad layer (the caller sets swapchain, image rect, flags).
    void Apply(const Placement& p, XrSpace localSpace, XrSpace viewSpace, uint32_t texW, uint32_t texH,
               XrCompositionLayerQuad& q) const {
        const float d = p.distance;
        if (p.anchor == 0 && haveAnchor_) {
            q.space = localSpace;
            q.pose.orientation = {0, std::sin(yaw_ * 0.5f), 0, std::cos(yaw_ * 0.5f)};
            q.pose.position = {pos_.x - std::sin(yaw_) * d, pos_.y + p.offsetY, pos_.z - std::cos(yaw_) * d};
        } else {
            q.space = viewSpace ? viewSpace : localSpace;
            q.pose.orientation = {0, 0, 0, 1};
            q.pose.position = {0, p.offsetY, -d};
        }
        q.size = {p.width, texW ? p.width * float(texH) / float(texW) : p.width};
    }

private:
    bool wasShown_ = false, haveAnchor_ = false;
    float yaw_ = 0;
    XrVector3f pos_{0, 0, 0};
};
#endif // XR_CURRENT_API_VERSION

// Adds the panel's own settings to the menu (a "Menu" page by default). Keys match config.txt entries
// MenuDistance / MenuWidth / MenuOffsetY / MenuAnchor (World | Head). Changes apply immediately.
inline void AddPlacementOptions(Options& o, Placement& p, const char* page = "Menu") {
    static std::string anchorStr;
    anchorStr = p.anchor == 1 ? "Head" : "World";
    static Placement* target = nullptr;
    target = &p;
    o.AddFloat(page, "MenuDistance", "Distance (m)", &p.distance, 0.4f, 3.0f, 0.05f, 2, "How far away this menu panel is.");
    o.AddFloat(page, "MenuWidth", "Width (m)", &p.width, 0.3f, 2.0f, 0.05f, 2,
               "Width of this menu panel. Narrower is easier to read on headsets with soft edges.");
    o.AddFloat(page, "MenuOffsetY", "Height offset (m)", &p.offsetY, -1.0f, 1.0f, 0.02f, 2, "Up (+) or down (-) from eye level.");
    auto& a = o.AddChoice(page, "MenuAnchor", "Anchor", &anchorStr, {"World", "Head"},
                          "World: stays where it opened, so you can turn your head to read it.\nHead: follows your head.",
                          [] { if (target) target->anchor = anchorStr == "Head" ? 1 : 0; });
    a.choiceLabels = {"World (fixed)", "Head (follows)"};
}

}} // namespace vrkit::menu
