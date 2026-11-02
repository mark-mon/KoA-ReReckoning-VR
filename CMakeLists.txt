# SPDX-License-Identifier: GPL-3.0-or-later
# KoA-ReReckoning-VR - Windows build with Visual Studio 2022 (MSVC).
#
# The game is 32-bit and the OpenXR host is 64-bit, so configure twice:
#   cmake -B build32 -A Win32   && cmake --build build32 --config Release   -> dinput8.dll
#   cmake -B build64 -A x64     && cmake --build build64 --config Release   -> AmalurVR_XrHost.exe
cmake_minimum_required(VERSION 3.21)
project(KoAReReckoningVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no redistributable needed
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)

set(COMMON_SRC src/common/log.cpp src/common/config.cpp)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # ---------------------------------------------------------------- 32-bit: in-game proxy (dinput8.dll)
    add_library(minhook STATIC
        third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde32.c)
    target_include_directories(minhook PUBLIC third_party/minhook/include)

    add_library(vrkit STATIC
        third_party/vrkit/vrkit_input.cpp third_party/vrkit/vrkit_menu.cpp third_party/vrkit/vrkit_menu_core.cpp
        third_party/vrkit/vrkit_options.cpp
        third_party/vrkit/imgui/imgui.cpp third_party/vrkit/imgui/imgui_draw.cpp third_party/vrkit/imgui/imgui_tables.cpp
        third_party/vrkit/imgui/imgui_widgets.cpp third_party/vrkit/imgui/imgui_impl_dx11.cpp)
    target_include_directories(vrkit PUBLIC third_party/vrkit third_party/vrkit/imgui)
    target_link_libraries(vrkit PUBLIC minhook d3d11 d3dcompiler)

    add_library(dinput8 SHARED
        src/proxy/dllmain.cpp src/proxy/d3dhooks.cpp src/proxy/hookutil.cpp src/proxy/dds.cpp
        src/proxy/game.cpp src/proxy/input.cpp src/proxy/watch.cpp src/proxy/stereo.cpp src/proxy/xrlink.cpp src/proxy/proxy.cpp
        src/proxy/settings.cpp src/proxy/menu_options.cpp src/proxy/vrs.cpp
        src/proxy/vtbl.c src/proxy/proxy.def ${COMMON_SRC})
    target_link_libraries(dinput8 PRIVATE vrkit minhook user32 dxgi d3d11 dxguid advapi32)
    target_compile_options(dinput8 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/arch:SSE2>)
    set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
    target_link_options(dinput8 PRIVATE /DELAYLOAD:d3dcompiler_47.dll)   # only needed once the settings menu starts
    target_link_libraries(dinput8 PRIVATE delayimp)
    install(TARGETS dinput8 RUNTIME DESTINATION .)
else()
    # ---------------------------------------------------------------- 64-bit: OpenXR host (AmalurVR_XrHost.exe)
    include(FetchContent)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openxr
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.65
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(openxr)
    # match our static CRT (the SDK defaults its static loader to the DLL CRT)
    set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_executable(AmalurVR_XrHost WIN32 src/xrhost/main.cpp ${COMMON_SRC})
    target_compile_definitions(AmalurVR_XrHost PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
    target_link_libraries(AmalurVR_XrHost PRIVATE OpenXR::openxr_loader d3d11 dxgi dxguid shell32 user32)
    install(TARGETS AmalurVR_XrHost RUNTIME DESTINATION .)
endif()

install(FILES config/config.txt DESTINATION .)
