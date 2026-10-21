// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include "../common/config.h"

namespace avr {

// Called first thing in DLL_PROCESS_ATTACH (before the config/log exist). Works out which DLL this copy
// is installed as (dinput8.dll or winmm.dll, from its own file name), points every export at the real
// system DLL, and returns true if this copy should run the mod (false = another copy already does; this
// one only forwards).
bool ProxySetup(HMODULE self);

// Loads the chained DLLs from config.txt ("chainload") and forwards our exports to them where they
// provide them. Call after the config and log are initialised.
void ProxyChainload(const Config& cfg);

const wchar_t* ProxyName();   // e.g. L"dinput8.dll"

} // namespace avr
