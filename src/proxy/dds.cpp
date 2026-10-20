// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#include "dds.h"
#include "../common/log.h"
#include <cstdio>
#include <cstdint>
#include <vector>

namespace avr {

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
#define F(x) case DXGI_FORMAT_##x: return #x;
        F(UNKNOWN) F(R32G32B32A32_TYPELESS) F(R32G32B32A32_FLOAT) F(R32G32B32_FLOAT)
        F(R16G16B16A16_TYPELESS) F(R16G16B16A16_FLOAT) F(R16G16B16A16_UNORM)
        F(R32G32_FLOAT) F(R32G32_TYPELESS) F(R10G10B10A2_TYPELESS) F(R10G10B10A2_UNORM) F(R11G11B10_FLOAT)
        F(R8G8B8A8_TYPELESS) F(R8G8B8A8_UNORM) F(R8G8B8A8_UNORM_SRGB) F(R16G16_TYPELESS)
        F(R16G16_FLOAT) F(R16G16_UNORM) F(R32_TYPELESS) F(D32_FLOAT) F(R32_FLOAT) F(R24G8_TYPELESS)
        F(D24_UNORM_S8_UINT) F(R24_UNORM_X8_TYPELESS) F(R8G8_UNORM) F(R16_TYPELESS) F(R16_FLOAT)
        F(D16_UNORM) F(R16_UNORM) F(R8_UNORM) F(A8_UNORM) F(B8G8R8A8_UNORM) F(B8G8R8X8_UNORM)
        F(B8G8R8A8_TYPELESS) F(B8G8R8A8_UNORM_SRGB) F(B8G8R8X8_TYPELESS) F(B8G8R8X8_UNORM_SRGB)
        F(R32G8X24_TYPELESS) F(D32_FLOAT_S8X24_UINT) F(BC1_UNORM) F(BC3_UNORM) F(R8_TYPELESS)
#undef F
    default: return "?";
    }
}

#pragma pack(push, 1)
struct DDS_PIXELFORMAT { uint32_t size, flags, fourCC, rgbBits, r, g, b, a; };
struct DDS_HEADER {
    uint32_t size, flags, height, width, pitch, depth, mips, reserved1[11];
    DDS_PIXELFORMAT pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DDS_HEADER_DXT10 { uint32_t dxgiFormat, dim, misc, arraySize, misc2; };
#pragma pack(pop)

static DXGI_FORMAT ResolveTypeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return f;
    }
}

bool SaveTextureDDS(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                    DXGI_FORMAT viewFormat, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    DXGI_FORMAT fmt = viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : d.Format;
    fmt = ResolveTypeless(fmt);
    if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = DXGI_FORMAT_R8G8B8A8_UNORM; // same bits; keep plain
    if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    ID3D11Texture2D* src = tex;
    ID3D11Texture2D* resolved = nullptr;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = d;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
        rd.MipLevels = 1; rd.ArraySize = 1;
        rd.BindFlags = 0; rd.MiscFlags = 0; rd.CPUAccessFlags = 0; rd.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolved))) return false;
        ctx->ResolveSubresource(resolved, 0, tex, 0, fmt);
        src = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.BindFlags = 0; sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.Usage = D3D11_USAGE_STAGING;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        if (resolved) resolved->Release();
        return false;
    }
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, nullptr);
    if (resolved) resolved->Release();

    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        // bytes per row: derive from bpp table
        uint32_t bpp = 4;
        switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: bpp = 16; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: bpp = 8; break;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_D16_UNORM: bpp = 2; break;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: bpp = 1; break;
        default: bpp = 4; break;
        }
        uint32_t rowBytes = d.Width * bpp;
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            uint32_t magic = 0x20534444;
            DDS_HEADER h{};
            h.size = 124; h.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8;
            h.height = d.Height; h.width = d.Width; h.pitch = rowBytes; h.mips = 1;
            h.pf.size = 32; h.pf.flags = 0x4; h.pf.fourCC = 0x30315844; // 'DX10'
            h.caps = 0x1000;
            DDS_HEADER_DXT10 x{ (uint32_t)fmt, 3, 0, 1, 0 };
            fwrite(&magic, 4, 1, f); fwrite(&h, sizeof(h), 1, f); fwrite(&x, sizeof(x), 1, f);
            for (uint32_t y = 0; y < d.Height; ++y) fwrite((uint8_t*)m.pData + (size_t)y * m.RowPitch, rowBytes, 1, f);
            fclose(f);
            ok = true;
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

} // namespace avr
