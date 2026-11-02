# vrkit — reusable in-game settings menu for VR game mods

> Shared copy (D:\vrconversions\sharedtools\vrkit). The GranblueVR adapter referred to below as `src/menu_options.cpp` is included here as `example_adapter/granbluevr_menu_options.cpp`. The master copy lives in the GranblueVR source tree (`vrkit/`). Background on the other reusable VR-mod parts (VRS, quad views, eye gaze, replay stereo): `../vr-mod-toolkit.md`.

vrkit is a small, game-independent library for D3D11 + OpenXR game mods (typically injected as a proxy
DLL such as `dinput8.dll`). It gives a mod an **in-headset settings menu** that edits the mod's own
`key = value` config file live, driven by a gamepad or the keyboard. It was built for the Granblue Fantasy
Relink VR mod (`src/menu_options.cpp` is that game's adapter) and is meant to be copied unchanged into
the next mod.

Status: v1, first written for GranblueVR 0.11.0. It compiles; it had not been tested in a game when
this was written. Check the mod's changelog / log for what has been verified since.

## What it does

* **Option registry** (`vrkit_options.h`): the mod registers its settings once. Each entry points
  straight at a field in the mod's own config struct, with a range, a step, help text and an optional
  `onChange` callback that applies the new value right away.
* **Menu** (`vrkit_menu.h`): Dear ImGui draws pages of options into an off-screen texture (premultiplied
  alpha). The host copies that texture into an OpenXR swapchain and shows it as a head-locked quad layer.
  When the menu closes, changed values are written back into the config file, keeping its comments,
  layout and line order.
* **Input** (`vrkit_input.h`): reads the pad through XInput and the keyboard through `GetAsyncKeyState`.
  It hooks the game's `XInputGetState` / `XInputGetStateEx` so that while the menu is open the game sees a
  neutral pad and does not also react to the button presses.

vrkit does **not** create OpenXR objects or own the swapchain. The host already has an OpenXR session, a
D3D11 device and a frame loop, so the host adds one quad layer. That keeps vrkit independent of how each
mod structures its rendering.

## Controls (defaults)

| Action | Gamepad | Keyboard |
|---|---|---|
| Open / close | hold **L3 + R3** (click both sticks) for 300 ms | **Insert** |
| Select row | D-pad / left stick up-down | ↑ ↓ |
| Change value | D-pad / left stick left-right (auto-repeat) | ← → |
| Toggle / next / run action | A | Enter |
| Undo: back to the value at start-up | Y | Delete |
| Previous / next page | LB / RB | PgUp / PgDn |
| Close | B | Backspace |

Esc is deliberately unused, because keyboard input is not blocked from the game and most games open
their own pause menu on Esc. The chord (`chordButtons` mask, or `chordTriggers` for LT + RT), hold time, keyboard key and trigger threshold are set through
`input::Settings`.

## Files

```
vrkit/
  vrkit_options.h/.cpp   option registry, value formatting, config-file writer, logging hook
  vrkit_input.h/.cpp     XInput hooks (MinHook), chord detection, keyboard, auto-repeat
  vrkit_menu.h/.cpp      ImGui context, menu layout and drawing, texture, open/close/save logic
  third_party/imgui/     Dear ImGui v1.91.5 core + imgui_impl_dx11 (MIT, see LICENSE)
```

Dependencies: D3D11 (`d3d11.lib`, `d3dcompiler.lib` for the ImGui DX11 backend), `user32`, MinHook
(already in most hook-based mods; the host must call `MH_Initialize()` before `menu::Init`).

Build (clang-cl, as in this repo's `build.sh`): add `-Ivrkit -Ivrkit/third_party/imgui` and compile
`vrkit/*.cpp` plus `imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_impl_dx11.cpp`.

## Integrating into a new mod (checklist)

1. **Copy `vrkit/`** into the mod and add it to the build (see above).

2. **Write the adapter**, one `.cpp` per game (example: `src/menu_options.cpp`):
   ```cpp
   #include "vrkit_options.h"
   #include "vrkit_menu.h"
   void MenuOptions_Init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
       vrkit::SetLog([](const char* m) { MyLog("%s", m); });
       auto& o = vrkit::Options::Get();
       o.AddFloat("Display", "render_scale", "Render scale", &cfg.renderScale, 0.25f, 3.f, 0.05f, 2,
                  "Per-eye resolution.", [] { ApplyRenderScale(); });
       auto& aa = o.AddChoice("Anti-aliasing", "aa_mode", "Mode", &cfg.aaMode, { "off", "fxaa", "taa" });
       aa.choiceLabels = { "Off", "FXAA", "TAA" };
       o.AddBool("View", "symmetric_fov", "Symmetric FOV", &cfg.symmetricFov).needsRestart = true;
       o.AddAction("Display", "Recenter view", [] { Recenter(); });
       vrkit::menu::Settings s;
       s.title = "My Game VR 1.0";
       s.configPath = modDir + L"config.txt";
       s.statusLine = [] { return FpsText(); };          // optional
       s.input.chordHoldMs = cfg.menuHoldMs;              // optional
       vrkit::menu::Init(dev, imm, s);
   }
   ```
   * The **key** must be exactly the config-file key: `Save()` rewrites that line.
   * Pointers must stay valid (point into a global config struct).
   * `onChange` runs on the render thread (inside the Present hook), right after the value changes.
     Make it cheap and idempotent: set a flag, invalidate a cache, rebuild a small table.
   * Options that cannot change live get `.needsRestart = true` (shown as "restart"; still saved).
   * `.visible = [] { return cfg.aaMode == "taa"; }` hides options that do not apply.
   * Pages appear in the order they are first used.
   * Note: `Add*` returns a reference into a vector. Set extra fields immediately and do not keep the
     reference.

3. **Call `MenuOptions_Init`** once the game's D3D11 device exists (e.g. in the first Present), after
   `MH_Initialize()`.

4. **Each Present** (on the thread that owns the immediate context):
   ```cpp
   if (vrkit::menu::Update()) {                // true while open; draws into the texture
       acquire menu swapchain image;
       ctx->CopyResource(swapImage, vrkit::menu::Texture());
       release; menuVisible = true;
   } else menuVisible = false;
   ```
   Create the swapchain once with the menu's size (`menu::Width()/Height()`), format
   `R8G8B8A8_UNORM_SRGB` (or `R8G8B8A8_UNORM`), usage `TRANSFER_DST | SAMPLED`.
   `CopyResource` works because the texture is `R8G8B8A8_TYPELESS`.

5. **Add the quad layer** to `xrEndFrame` while visible, last in the list so it is drawn on top:
   ```cpp
   q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied (the default)
   q.space = viewSpace;                       // XR_REFERENCE_SPACE_TYPE_VIEW: head-locked
   q.pose = { {0,0,0,1}, {0, 0, -1.0f} };     // 1 m ahead
   q.size = { 0.9f, 0.9f * h / w };
   ```
   Remember to enlarge the layer array, and to add the layer in every frame path (for example both the
   stereo path and a flat "theatre" path).

6. **Keep hotkeys in sync**: if the mod also has hotkeys that change settings, make them write the same
   config field, so the menu shows the true value and `Save()` writes it.

7. Optional config keys worth exposing: `menu_chord` (l3r3 / ltrt / none), `menu_hold_ms`, `menu_key`, `menu_distance`, `menu_size`.

## How it works (details for maintainers)

* **Drawing without disturbing the game.** `Draw()` swaps in a private `ID3DDeviceContextState`
  (`SwapDeviceContextState`), so the game's bound state is untouched. It renders with
  `ImGui_ImplDX11_RenderDrawData` into its RTV, then swaps the game's state back. ImGui's standard blend
  (SrcAlpha / InvSrcAlpha on colour, One / InvSrcAlpha on alpha) onto a cleared (0,0,0,0) target produces
  premultiplied colour, which is what OpenXR expects by default.
* **Layout.** Custom drawing on one full-texture ImGui window: title and status line, page tabs, rows of
  `label ........ < value >`, help text and config key for the selected row, and a controls footer. ImGui
  navigation is not used; `Navigate()` maps vrkit buttons to `Options::Step / Activate / Reset`. The font
  is Segoe UI (falling back to Arial, then ImGui's built-in font) at `fontPx`.
* **Input blocking.** One MinHook trampoline per XInput DLL (`xinput1_4`, `xinput1_3`, `xinput9_1_0`),
  for both `XInputGetState` and ordinal 100 (`XInputGetStateEx`). The hooks return a zeroed state with an
  increasing packet number while the menu is open. They also do so while the open chord is still held
  after firing, and after closing until every button and trigger is released, so the game never sees a
  half-press. While all chord buttons are down together they are also masked out of the game's state, so
  pressing L3 + R3 does not trigger the game's own L3 / R3 actions (only a stick clicked noticeably earlier
  than the other can reach the game). vrkit reads the pad through the original (trampoline) functions. `InstallHooks()` is retried
  every 300 frames, in case the game loads XInput late.
* **Saving.** `Options::Save` reads the file, replaces the value of each registered key on its existing
  line (CRLF/LF preserved), and appends keys that are missing under `# --- set from the in-game menu ---`.
  It only runs on close, and only if something changed.
* **Reset (Y).** This restores the value the option had when it was registered, which is the value loaded
  from the config at start-up, not a compiled-in default.

## Known limitations / ideas

* Gamepad input is blocked only for **XInput**. Games that read the pad through DirectInput, Raw Input,
  Windows.Gaming.Input or Steam Input's own API will still see the presses. Add a hook for that API in
  `vrkit_input.cpp` if needed.
* Keyboard input is never blocked: arrow keys move the menu *and* reach the game. Use the pad in VR.
* No pointer or laser control with VR motion controllers yet. The OpenXR action set would live in the
  host; vrkit would need a "hover row" input.
* There is no text entry and no key-binding editor. Keys stay config-file only.
* A single global menu (one `Options` singleton, one ImGui context).
