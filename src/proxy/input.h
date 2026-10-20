// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once

namespace avr {

enum { COMBO_RECENTER = 1, COMBO_HUD_TOGGLE = 2 };

void InstallInputHooks();
void InputOnPresent();
int TakeCombos(); // returns and clears COMBO_* bits seen since last call

} // namespace avr
