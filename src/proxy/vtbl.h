// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KoA-ReReckoning-VR contributors
#pragma once
/* Vtable slot indices, computed in vtbl.c from the SDK C declarations. */

#define VT_CTX_LIST(X) \
    X(VSSetConstantBuffers) X(PSSetShaderResources) X(PSSetShader) X(VSSetShader) \
    X(DrawIndexed) X(Draw) X(Map) X(Unmap) X(PSSetConstantBuffers) X(DrawIndexedInstanced) \
    X(DrawInstanced) X(OMSetRenderTargets) X(OMSetRenderTargetsAndUnorderedAccessViews) \
    X(RSSetViewports) X(CopyResource) X(CopySubresourceRegion) X(UpdateSubresource) \
    X(ClearRenderTargetView) X(ClearDepthStencilView) X(ResolveSubresource) X(Dispatch) \
    X(OMSetBlendState) X(OMSetDepthStencilState) X(RSSetState) X(VSSetShaderResources) \
    X(IASetVertexBuffers) X(IASetPrimitiveTopology) X(GSSetShader) X(CSSetShader) \
    X(DrawAuto) X(DrawIndexedInstancedIndirect) X(DrawInstancedIndirect) X(RSSetScissorRects) \
    X(OMGetRenderTargets) X(RSGetViewports) X(VSGetConstantBuffers) X(PSGetConstantBuffers) \
    X(ClearState) X(PSSetSamplers) X(IASetInputLayout) X(IASetIndexBuffer) \
    X(ExecuteCommandList) X(FinishCommandList)

#define VT_DEV_LIST(X) \
    X(CreateBuffer) X(CreateTexture2D) X(CreateShaderResourceView) X(CreateRenderTargetView) \
    X(CreateDepthStencilView) X(CreateVertexShader) X(CreatePixelShader) X(CreateDeferredContext) \
    X(GetImmediateContext)

#define VT_SC_LIST(X) X(Present) X(ResizeBuffers) X(GetBuffer) X(ResizeTarget) X(SetFullscreenState)

#define VT_FAC_LIST(X) X(CreateSwapChain)
#define VT_FAC2_LIST(X) X(CreateSwapChainForHwnd)

#ifdef __cplusplus
extern "C" {
#endif
#define AVR_DECL_CTX(m) extern const int VT_CTX_##m;
#define AVR_DECL_DEV(m) extern const int VT_DEV_##m;
#define AVR_DECL_SC(m)  extern const int VT_SC_##m;
#define AVR_DECL_FAC(m) extern const int VT_FAC_##m;
#define AVR_DECL_FAC2(m) extern const int VT_FAC2_##m;
VT_CTX_LIST(AVR_DECL_CTX)
VT_DEV_LIST(AVR_DECL_DEV)
VT_SC_LIST(AVR_DECL_SC)
VT_FAC_LIST(AVR_DECL_FAC)
VT_FAC2_LIST(AVR_DECL_FAC2)
#ifdef __cplusplus
}
#endif
