# Changelog

## 0.2.0
- **In-headset settings menu** (vrkit / Dear ImGui): hold L3 + R3 or press Insert. Pages for View, Display,
  Foveation, HUD and the settings panel itself. Most settings apply live; all are saved to `config.txt`
  (comments kept). The panel stays where it opened (or follows your head) and is previewed on the monitor.
- **Foveated rendering** with NVIDIA Variable Rate Shading: `fixed` (centre of each lens) or `gaze`
  (eye-tracked, `XR_EXT_eye_gaze_interaction`), three strengths, adjustable radii. Only the eyes' scene
  passes are affected; the HUD, overlays and shadow maps are always full rate.
- **HUD anchor**: `hud_anchor = world | head` (replaces `hud_follow_head`, which is still read), switchable
  in the menu or with Back + left stick click; HUD placement changes apply live.
- The XR host enables eye tracking when the runtime offers it.

## 0.1.1
- The mod's DLL can be installed as `dinput8.dll` or `winmm.dll` (it detects its own name), for setups where
  another mod already uses `dinput8.dll`.
- Chainloading: `dinput8-2.dll` / `winmm-2.dll` are loaded automatically and our exports forward to them;
  more DLLs via `chainload` in `config.txt`.
- Only one copy of the mod runs if it is installed under both names.

## 0.1.0 — 2026-10-20
First public release.
- Native stereo rendering (two `RenderView` passes per frame) with per-eye OpenXR FOV and IPD.
- Per-eye re-projection of pre-baked object constants (characters, vegetation, props).
- HUD on a separate transparent quad layer; menus/loading screens on a flat opaque panel.
- 64-bit OpenXR host with shared keyed-mutex textures; pacing to the headset refresh rate.
- Engine runs at the per-eye resolution (min 4:3 so the game UI fits); vsync forced off.
- Recenter (Back + right stick), level horizon option, `config.txt`, logs, F11 frame capture.
