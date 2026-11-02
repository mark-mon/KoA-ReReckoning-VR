// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#include "settings.h"
#include "globals.h"
#include "xrlink.h"
#include <algorithm>
#include <cctype>

namespace avr {

VrSettings& VS() { static VrSettings s; return s; }

static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static int Anchor(const Config& c, const char* key, int def) {
    std::string v = Lower(c.GetStr(key, def ? "head" : "world"));
    return v == "head" || v == "1" ? 1 : 0;
}

void SettingsLoad() {
    auto& c = G().cfg;
    VrSettings& s = VS();
    s.worldScale = c.GetFloat("world_scale", 1.0f);
    s.ipdScale = c.GetFloat("ipd_scale", 1.0f);
    s.pitchLock = c.GetBool("camera_pitch_lock", true);
    s.gameFovVertical = c.GetInt("game_fov_vertical", 80);
    s.renderScale = c.GetFloat("render_scale", 1.0f);
    s.minRenderAspect = c.GetFloat("min_render_aspect", 1.333f);
    s.desktopMirror = c.GetBool("desktop_mirror", true);
    s.hudDistance = c.GetFloat("hud_distance", 1.6f);
    s.hudWidth = c.GetFloat("hud_width", 1.6f);
    s.hudOffsetY = c.GetFloat("hud_offset_y", -0.05f);
    // hud_anchor (world | head); older configs used hud_follow_head = 0 / 1
    s.hudAnchor = Anchor(c, "hud_anchor", c.GetBool("hud_follow_head", false) ? 1 : 0);
    s.menuDistance = c.GetFloat("menu_distance", 2.0f);
    s.menuWidth = c.GetFloat("menu_width", 2.4f);
    s.panel.distance = c.GetFloat("settings_distance", 1.0f);
    s.panel.width = c.GetFloat("settings_width", 0.8f);
    s.panel.offsetY = c.GetFloat("settings_offset_y", -0.05f);
    s.panel.anchor = Anchor(c, "settings_anchor", 0);
    std::string fv = Lower(c.GetStr("foveation", "off"));
    s.foveation = (fv == "gaze" || fv == "2") ? 2 : (fv == "fixed" || fv == "1") ? 1 : 0;
    std::string st = Lower(c.GetStr("foveation_strength", "medium"));
    s.foveationStrength = (st == "low" || st == "0") ? 0 : (st == "high" || st == "2") ? 2 : 1;
    s.foveationInner = c.GetFloat("foveation_inner", 0.45f);
    s.foveationOuter = c.GetFloat("foveation_outer", 0.80f);
    s.debugFakeGaze = c.GetBool("debug_fake_gaze", false);
    s.menuChord = Lower(c.GetStr("menu_chord", "l3r3"));
    s.menuHoldMs = c.GetInt("menu_hold_ms", 300);
}

void SettingsPushToHost() { XrPushSettings(VS()); }

} // namespace avr
