// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <cstdint>

namespace avr {
void WatchStart(const uintptr_t* addrs, const char** labels, int n);
void WatchStop(const wchar_t* outFile);
bool WatchActive();
}
