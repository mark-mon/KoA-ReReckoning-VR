# Changelog

## 0.1.0 — 2026-10-20
First public release.
- Native stereo rendering (two `RenderView` passes per frame) with per-eye OpenXR FOV and IPD.
- Per-eye re-projection of pre-baked object constants (characters, vegetation, props).
- HUD on a separate transparent quad layer; menus/loading screens on a flat opaque panel.
- 64-bit OpenXR host with shared keyed-mutex textures; pacing to the headset refresh rate.
- Engine runs at the per-eye resolution (min 4:3 so the game UI fits); vsync forced off.
- Recenter (Back + right stick), level horizon option, `config.txt`, logs, F11 frame capture.
