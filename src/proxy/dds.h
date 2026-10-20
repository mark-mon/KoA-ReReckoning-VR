// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
#include <d3d11.h>
#include <string>

namespace avr {

// Copies a 2D texture (subresource 0) to staging and writes it as a DX10-header DDS.
// 'viewFormat' overrides typeless resource formats (pass DXGI_FORMAT_UNKNOWN to use the texture's).
// MSAA textures are resolved first. Returns true on success.
bool SaveTextureDDS(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                    DXGI_FORMAT viewFormat, const std::wstring& path);

const char* FormatName(DXGI_FORMAT f);

} // namespace avr
