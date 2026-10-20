#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux cross-compile (see docs/BUILDING.md). Windows users: use CMake instead.
#   AVR_TOOLCHAIN  folder with MSVC + Windows SDK headers/libs (default ./toolchain)
#   AVR_SDK_VER    Windows SDK version folder (default 10.0.28000.0)
#   AVR_CLANG      clang driver command (default: "zig clang" from the ziglang pip package)
set -e
cd "$(dirname "$0")"
TC=$(realpath "${AVR_TOOLCHAIN:-$PWD/toolchain}")
SDKV=${AVR_SDK_VER:-10.0.28000.0}
CLANG=${AVR_CLANG:-"python3 -m ziglang clang"}
SDK=$TC/Include/$SDKV
ZIGINC=$(python3 -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'lib','include'))" 2>/dev/null || true)
[ -f "$TC/vfs.yaml" ] || python3 - "$TC" "$SDKV" <<'PY'
import os,json,sys
tc,v=sys.argv[1],sys.argv[2]
def walk(d):
    return [{'type':'directory','name':n,'contents':walk(os.path.join(d,n))} if os.path.isdir(os.path.join(d,n))
            else {'type':'file','name':n,'external-contents':os.path.abspath(os.path.join(d,n))} for n in sorted(os.listdir(d))]
roots=[{'type':'directory','name':os.path.abspath(os.path.join(tc,r)),'contents':walk(os.path.join(tc,r))} for r in ['include','Include/'+v]]
json.dump({'version':0,'case-sensitive':'false','roots':roots},open(os.path.join(tc,'vfs.yaml'),'w'))
PY
cc() { # arch out args...
  local A=$1 OUT=$2; shift 2
  local T=$([ $A = x86 ] && echo i686-pc-windows-msvc || echo x86_64-pc-windows-msvc)
  $CLANG --driver-mode=cl --target=$T /nologo /MT /EHsc /std:c++20 -fuse-ld=lld-link /O2 \
    -Wno-unused-command-line-argument -Wno-microsoft -Wno-pragma-pack -Wno-unknown-pragmas \
    -vfsoverlay $TC/vfs.yaml ${ZIGINC:+/imsvc$ZIGINC} /imsvc$TC/include /imsvc$SDK/ucrt /imsvc$SDK/um /imsvc$SDK/shared \
    -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX /Fe$OUT "$@" \
    /libpath:$TC/lib/$A /libpath:$TC/Lib/$SDKV/um/$A /libpath:$TC/Lib/$SDKV/ucrt/$A
}
mkdir -p build/x86 build/x64 dist
cc x86 dist/dinput8.dll /arch:SSE2 /LD /Fobuild/x86/ \
  src/proxy/*.cpp src/proxy/vtbl.c src/common/log.cpp src/common/config.cpp \
  third_party/minhook/src/{buffer.c,hook.c,trampoline.c} third_party/minhook/src/hde/hde32.c \
  /link /DEF:src/proxy/dinput8.def user32.lib dxgi.lib d3d11.lib dxguid.lib advapi32.lib
cc x64 dist/AmalurVR_XrHost.exe /Fobuild/x64/ -DXR_USE_PLATFORM_WIN32 -DXR_USE_GRAPHICS_API_D3D11 /Ithird_party/openxr/include \
  src/xrhost/main.cpp src/common/log.cpp src/common/config.cpp \
  /link third_party/openxr/lib/openxr_loader_static.lib user32.lib d3d11.lib dxgi.lib dxguid.lib advapi32.lib \
  shlwapi.lib pathcch.lib ole32.lib shell32.lib /SUBSYSTEM:WINDOWS
cp config/config.txt dist/
ls -la dist
