/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 KoA-ReReckoning-VR contributors */
/* Computes COM vtable slot indices from the SDK's C interface declarations,
   so hook slot numbers are never hand-maintained. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "vtbl.h"

#define CTX(m) const int VT_CTX_##m = (int)(offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void*));
#define DEV(m) const int VT_DEV_##m = (int)(offsetof(ID3D11DeviceVtbl, m) / sizeof(void*));
#define SC(m)  const int VT_SC_##m  = (int)(offsetof(IDXGISwapChainVtbl, m) / sizeof(void*));
#define FAC(m) const int VT_FAC_##m = (int)(offsetof(IDXGIFactoryVtbl, m) / sizeof(void*));
#define FAC2(m) const int VT_FAC2_##m = (int)(offsetof(IDXGIFactory2Vtbl, m) / sizeof(void*));

VT_CTX_LIST(CTX)
VT_DEV_LIST(DEV)
VT_SC_LIST(SC)
VT_FAC_LIST(FAC)
VT_FAC2_LIST(FAC2)
