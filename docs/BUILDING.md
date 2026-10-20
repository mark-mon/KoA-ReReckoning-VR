# Building

## Windows (Visual Studio 2022)
```bat
cmake -B build32 -A Win32 && cmake --build build32 --config Release
cmake -B build64 -A x64   && cmake --build build64 --config Release
```
The 32-bit configuration builds `dinput8.dll` (game side). The 64-bit configuration downloads the
OpenXR SDK (`release-1.1.65`) and builds `AmalurVR_XrHost.exe` with the loader linked statically.
`cmake --install build32 --prefix <game folder>` / `build64` copies the outputs and `config.txt`.

GitHub Actions (`.github/workflows/build.yml`) builds both and uploads a zip on every push and tag.

## Linux cross-compile (how the releases were developed)
`build.sh` uses clang-cl (LLVM 22, e.g. from the `ziglang` pip package: `zig clang --driver-mode=cl`)
and lld-link with the Windows SDK + MSVC libraries copied from a Windows machine:
```
TC/
  include/                      MSVC include
  Include/10.0.xxxxx.0/{ucrt,um,shared,winrt}
  lib/{x86,x64}/                MSVC libs
  Lib/10.0.xxxxx.0/{um,ucrt}/{x86,x64}
```
Linux is case-sensitive, so a clang VFS overlay with `"case-sensitive": "false"` is used for the headers
and lowercase symlinks for the libraries. Set `AVR_TOOLCHAIN` and `AVR_SDK_VER` and run `./build.sh`.
The OpenXR loader for this path is compiled from the OpenXR-SDK sources (`src/loader`, `src/common`,
`src/external/jsoncpp`) into `third_party/openxr/lib/openxr_loader_static.lib`.
