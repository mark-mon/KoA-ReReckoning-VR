// vrkit - renderer-independent part of the in-game settings menu (used by the D3D11 and D3D9 backends).
// Owns the ImGui context, input handling (open/close, navigation) and the menu layout. A backend creates its
// device objects, calls InitImGui once, and each frame: UpdateInput(); if open: <backend>_NewFrame(), BuildFrame(),
// then renders ImGui::GetDrawData().
#pragma once
#include <string>
#include <functional>
#include "vrkit_input.h"

namespace vrkit { namespace menu {

struct Settings {
    std::string title = "VR mod";
    unsigned texW = 1280, texH = 960;   // menu size (layout units = texture pixels for D3D11)
    float fontPx = 34.f;                // text size in layout units
    float fontOversample = 1.f;         // D3D9 backend: rasterise the font this much larger (the menu is scaled up when drawn)
    float backgroundAlpha = 0.94f;      // panel opacity (1 = nothing behind the menu shows through)
    std::wstring configPath;            // where Options::Save writes on close (empty: not saved)
    std::function<std::string()> statusLine;   // optional line under the title (fps, version...)
    input::Settings input;
};

namespace core {
bool InitImGui(const Settings& s);    // ImGui context, font, style (also configures + hooks input)
void ShutdownImGui();
const Settings& Get();
// Poll input, handle open/close and navigation. Returns true while the menu is open.
bool UpdateInput();
// Build this frame's menu (ImGui::NewFrame ... ImGui::Render). Call after the backend's NewFrame.
void BuildFrame();
bool IsOpen();
void SetOpen(bool open);
} // namespace core

} } // namespace vrkit::menu
