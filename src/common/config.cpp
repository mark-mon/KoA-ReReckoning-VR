// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

namespace avr {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool Config::Load(const wchar_t* path) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        size_t c = l.find_first_of("#;");
        if (c != std::string::npos) l = l.substr(0, c);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Lower(Trim(l.substr(0, eq)));
        std::string v = Trim(l.substr(eq + 1));
        if (!k.empty()) m_[k] = v;
    }
    fclose(f);
    return true;
}

std::string Config::GetStr(const char* key, const char* def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? std::string(def) : it->second;
}
int Config::GetInt(const char* key, int def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (int)strtol(it->second.c_str(), nullptr, 0);
}
float Config::GetFloat(const char* key, float def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (float)atof(it->second.c_str());
}
bool Config::GetBool(const char* key, bool def) const {
    auto it = m_.find(Lower(key));
    if (it == m_.end()) return def;
    std::string v = Lower(it->second);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
void Config::Dump() const {
    for (auto& kv : m_) LOGI("config: %s = %s", kv.first.c_str(), kv.second.c_str());
}

static const char kDefaultConfig[] =
"# Amalur VR configuration\n"
"# Lines starting with # are comments. Restart the game after editing.\n"
"\n"
"# ---- General ----\n"
"# 0=error 1=warn 2=info 3=debug 4=trace\n"
"log_level = 2\n"
"# 1 = enable VR (starts AmalurVR_XrHost.exe). 0 = mod loads but does nothing (diagnostics only)\n"
"vr_enabled = 1\n"
"\n"
"# ---- Rendering ----\n"
"# Per-eye render size. 0 = use the headset's recommended size.\n"
"render_width = 0\n"
"render_height = 0\n"
"# Multiplier applied to the recommended size when render_width/height are 0\n"
"render_scale = 1.0\n"
"# Minimum width/height of the render. Headsets are ~0.9:1, which makes the game's UI (e.g. dialog) too narrow.\n"
"# 1.333 = 4:3 (the extra width only adds horizontal detail; the 3D view still uses the headset FOV). 0 = off\n"
"min_render_aspect = 1.333\n"
"# 1 = render both eyes natively each frame (recommended). 0 = alternate-eye fallback\n"
"stereo_native = 1\n"
"# World scale: larger = world looks smaller. 1.0 = game units as authored\n"
"world_scale = 1.0\n"
"# Extra IPD multiplier on top of the headset IPD\n"
"ipd_scale = 1.0\n"
"# Near clip plane override in game units (0 = keep game value)\n"
"near_clip = 0\n"
"# Game units per meter (the engine uses centimeters)\n"
"units_per_meter = 100\n"
"# 1 = keep the horizon level (removes the game camera's pitch; your head still looks up/down). Recommended.\n"
"camera_pitch_lock = 1\n"
"# Override the game's own vertical FOV setting (10-80, 0 = leave). 80 reduces objects popping in at the edges of your view.\n"
"game_fov_vertical = 80\n"
"# Shrink the desktop window to fit the monitor (rendering resolution is unaffected)\n"
"desktop_window_fit = 1\n"
"\n"
"# ---- HUD / UI ----\n"
"# Distance of the HUD panel in meters, width in meters\n"
"hud_distance = 1.6\n"
"hud_width = 1.6\n"
"# Vertical offset of the HUD panel in meters (negative = lower)\n"
"hud_offset_y = -0.05\n"
"# 1 = HUD follows head, 0 = HUD fixed in front of the recentered position\n"
"hud_follow_head = 0\n"
"# Menus / cutscene screens panel (shown instead of the world when the game is in a full-screen menu)\n"
"menu_distance = 2.0\n"
"menu_width = 2.4\n"
"\n"
"# ---- Controls ----\n"
"# Recenter the view: hold the gamepad Back/View button + click the right stick\n"
"recenter_on_start = 1\n"
"\n"
"# ---- Diagnostics ----\n"
"# Press this key (or create a file named capture.now in the game folder) to capture one frame\n"
"capture_key = 0x7A\n"
"# Save render-target images during a capture (slower capture, much more useful)\n"
"capture_dump_targets = 0\n"
"# Save the per-eye and HUD images every N seconds to the AmalurVR folder (0 = off)\n"
"debug_dump_interval = 0\n"
"# Show a desktop mirror window of the left eye\n"
"desktop_mirror = 1\n"
"# Test automation only (presses A to load the last save). Leave at 0.\n"
"autoload = 0\n";

void WriteDefaultConfig(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fputs(kDefaultConfig, f);
    fclose(f);
}

} // namespace avr
