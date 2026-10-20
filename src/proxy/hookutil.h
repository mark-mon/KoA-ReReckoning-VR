// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <windows.h>
#include <cstdint>

namespace avr {

// Replace a vtable slot. Returns original function pointer. Safe to call repeatedly
// (if the slot already holds 'fn', returns nullptr and leaves it).
void* HookVtable(void* obj, int slot, void* fn);
void* HookVtableRaw(void** vtbl, int slot, void* fn);

// Patch an IAT entry of 'module' importing 'dllName'!'funcName'. Returns original.
void* HookIAT(HMODULE module, const char* dllName, const char* funcName, void* fn);

void* HookIATOrdinal(HMODULE module, const char* dllName, WORD ordinal, void* fn);

// Info about the main executable image.
struct ModuleRange { uintptr_t base, end, textBase, textEnd; };
const ModuleRange& GameModule();

// Return-address helpers: scan the stack for plausible return addresses into the game .text.
// Writes up to 'max' RVAs. Returns count.
int ScanGameReturnAddresses(const void* stackStart, uint32_t* outRva, int max, int scanBytes = 4096);

} // namespace avr
