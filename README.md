# KoA-ReReckoning-VR

A VR mod for **Kingdoms of Amalur: Re-Reckoning** (Steam, PC). It turns the game into a seated / standing
gamepad VR experience through **OpenXR**, with **native stereo rendering** — the engine renders the scene
separately for each eye every frame (no alternate-eye rendering) — and the HUD and menus on their own
floating panels.

> Unofficial fan project. Not affiliated with THQ Nordic, Kaiko, 38 Studios or Big Huge Games.
> You need your own copy of the game. No game files are included or modified on disk.

## Features
- **True stereo**: both eyes rendered by the game's own renderer each frame, using the headset's exact
  per-eye field of view and your IPD. Characters, vegetation and props are re-projected per eye.
- **Head tracking** on top of the game camera; the horizon is kept level (optional).
- **HUD on a separate layer** (health, compass, prompts, dialog) shown as a floating panel; **menus and
  loading screens** on a large flat panel with nothing behind it.
- **OpenXR**: works with SteamVR, Virtual Desktop (VDXR), PimaxXR, Oculus/Meta and other OpenXR runtimes.
  The game is 32-bit, so a small 64-bit helper (`AmalurVR_XrHost.exe`) owns the OpenXR session and
  receives the game's frames through shared GPU textures.
- Normal gamepad controls. Paced to the headset refresh rate.
- `config.txt` for render size, world scale, HUD placement, logging, etc.

## Install
1. Download the latest release zip from the [Releases](../../releases) page.
2. Extract `dinput8.dll`, `AmalurVR_XrHost.exe` and `config.txt` into the game folder, next to `koa.exe`
   (e.g. `...\steamapps\common\Kingdoms of Amalur Re-Reckoning\`).
3. Start your OpenXR runtime (e.g. SteamVR) with the headset connected.
4. Launch the game from Steam. The VR helper starts automatically.

### Using it with other mods
The mod's DLL works under two names: `dinput8.dll` or `winmm.dll` (it detects its own file name).
- **Another mod already uses `dinput8.dll`?** Either rename ours to `winmm.dll`, or rename the other
  mod's file to `dinput8-2.dll` and keep ours as `dinput8.dll`. Ours loads `dinput8-2.dll` automatically
  and passes DirectInput through it, so the other mod keeps working.
- Same idea for winmm: if ours is `winmm.dll`, another mod's `winmm.dll` can be renamed to `winmm-2.dll`.
- More DLLs can be loaded with `chainload` in `config.txt` (comma-separated, `none` to disable).
- Mods that also change the game's rendering, resolution or camera may conflict with VR.

### Controls
| Action | Gamepad |
|---|---|
| Recenter view | hold **Back/View** + click **right stick** (also recenters at startup) |
| Everything else | normal game controls |

### Uninstall
Set `vr_enabled = 0` in `config.txt` and start the game once (this restores your desktop resolution),
then delete `dinput8.dll` (or `winmm.dll`), `AmalurVR_XrHost.exe`, `config.txt` and the `AmalurVR` folder.

## Configuration (`config.txt`)
| Key | Default | Meaning |
|---|---|---|
| `vr_enabled` | 1 | 0 = mod does nothing (restores desktop resolution) |
| `chainload` | auto | other DLLs to load; `auto` = `dinput8-2.dll` / `winmm-2.dll` if present |
| `render_scale` | 1.0 | multiplier on the headset's recommended per-eye size |
| `render_width` / `render_height` | 0 | fixed per-eye size (0 = automatic) |
| `min_render_aspect` | 1.333 | widen the render to at least 4:3 so the game's UI (dialog) fits |
| `world_scale` | 1.0 | larger = the world looks smaller |
| `ipd_scale` | 1.0 | extra stereo separation multiplier |
| `camera_pitch_lock` | 1 | keep the horizon level (recommended) |
| `game_fov_vertical` | 80 | the game's own FOV (wider = less pop-in at the edges when turning your head) |
| `hud_distance` / `hud_width` / `hud_offset_y` | 1.6 / 1.6 / -0.05 | HUD panel placement in meters |
| `hud_follow_head` | 0 | 1 = HUD follows your head |
| `menu_distance` / `menu_width` | 2.0 / 2.4 | menu panel placement in meters |
| `desktop_mirror` | 1 | show the left eye + HUD in the game window |
| `log_level` | 2 | 0 = errors … 4 = trace |

**Note:** the mod writes the per-eye render size into `%APPDATA%\kaiko\koa\personal.ini`
(`Display Width/Height`) and turns the game's vsync off (the headset paces the game). Your original
resolution is saved in `AmalurVR\personal_ini_display_backup.txt` and restored when `vr_enabled = 0`.

## Troubleshooting
Logs are written to the `AmalurVR` folder in the game directory:
- `amalurvr.log` — game side (rendering, hooks)
- `xrhost.log` — OpenXR side (runtime, session, frame statistics)

Press **F11** in game to save a one-frame capture (`AmalurVR\capture_*`, `atlas.dds` = left eye | right eye | HUD).
Please attach both logs when opening an issue.

Known limitations:
- Only the Steam version of Re-Reckoning (build `cs:15032`) is supported; other builds log a signature
  mismatch and fall back to mono.
- Objects the game culls outside its own view can pop in at the edges when you turn your head far.
- The game and the headset must use the same GPU.

## Building from source
Requirements: Visual Studio 2022 (Desktop C++), CMake 3.21+, Git.
```bat
cmake -B build32 -A Win32 && cmake --build build32 --config Release
cmake -B build64 -A x64   && cmake --build build64 --config Release
```
Outputs: `build32\Release\dinput8.dll` and `build64\Release\AmalurVR_XrHost.exe`. The OpenXR loader is
fetched and linked statically by CMake. See [docs/BUILDING.md](docs/BUILDING.md) for a Linux
cross-compile setup and [docs/TECHNICAL.md](docs/TECHNICAL.md) for how the mod works.

## License
GPL-3.0-or-later — see [LICENSE](LICENSE).
Third-party components: [MinHook](third_party/minhook/LICENSE.txt) (BSD-2-Clause),
[OpenXR SDK](third_party/openxr/LICENSE) (Apache-2.0).
