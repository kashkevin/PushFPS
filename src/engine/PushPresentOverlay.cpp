// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "present" R1: see include/veyra/engine/PushPresentOverlay.h. Unverified on Windows hardware.
#include "veyra/engine/PushPresentOverlay.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/Log.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace veyra::engine {

void PushPresentOverlay::reset() {
    const auto device = failedDevice_;
    const auto format = failedFormat_;
    const auto mode = failedMode_;
    *this = PushPresentOverlay{};
    failedDevice_ = device;
    failedFormat_ = format;
    failedMode_ = mode;
}

bool PushPresentOverlay::create(ID3D12Device* device, DXGI_FORMAT format, Mode mode) {
    if (pso_ && device_ == device && format_ == format && mode_ == mode) return true;
    if (failedDevice_ == device && failedFormat_ == format && failedMode_ == mode) return false;
    reset();
    failedDevice_ = device;   // cleared below on success
    failedFormat_ = format;
    failedMode_ = mode;
    std::vector<uint8_t> vs, ps;
    if (!pipeline::loadShaderBytes("PushVideoOverlay_vs.dxil", vs) || !pipeline::loadShaderBytes("PushVideoOverlay_ps.dxil", ps)) {
        log::error("present-overlay", "shader files PushVideoOverlay_vs/ps.dxil missing: the in-picture readouts stay cut out of the video window");
        return false;
    }
    // Root signature and heap as PresentBlit's (pipeline::GraphicsPass: 8 root constants at b0, one SRV table at t0); the
    // pipeline itself is made again below with blending.
    pipeline::GraphicsPass pass;
    if (!pass.create(device, vs, ps, 1, format, 8)) {
        log::error("present-overlay", std::format("pass creation failed format={}", unsigned(format)));
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = pass.rootSig.Get();
    pd.VS.pShaderBytecode = vs.data();
    pd.VS.BytecodeLength = vs.size();
    pd.PS.pShaderBytecode = ps.data();
    pd.PS.BytecodeLength = ps.size();
    auto& blend = pd.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;   // the back buffer's alpha stays as the blit wrote it
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        log::error("present-overlay", std::format("blended pipeline format={} hr=0x{:X}", unsigned(format), unsigned(hr)));
        return false;
    }
    failedDevice_ = nullptr;
    device_ = device;
    format_ = format;
    mode_ = mode;
    rootSig_ = pass.rootSig;
    heap_ = pass.heap;
    pso_ = std::move(pso);
    return true;
}

bool PushPresentOverlay::upload(ID3D12GraphicsCommandList* list, const PushVideoOverlayImage& image) {
    if (!pso_ || !image.valid()) return false;
    // The CPU may write the upload buffer only when the GPU has finished the copy that read it last.
    if (!complete(fence_, uploadValue_)) return false;
    constexpr UINT kAlign = UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT pitch = (image.width * 4u + kAlign - 1u) & ~(kAlign - 1u);
    const uint64_t bytes = uint64_t(pitch) * image.height;
    const bool growTexture = !texture_ || image.width > textureW_ || image.height > textureH_;
    // A texture or a buffer replaced while a list still reads it would be released under it: wait for the last user.
    if ((growTexture || bytes > uploadBytes_) && !complete(fence_, useValue_)) return false;
    if (growTexture) {
        const unsigned w = std::max(textureW_, (image.width + 63) & ~63u), h = std::max(textureH_, (image.height + 63) & ~63u);
        auto texture = pipeline::makeTexture(device_, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
        if (!texture) return false;
        texture->SetName(L"PushFPS in-picture interface");
        texture_ = std::move(texture);
        textureW_ = w;
        textureH_ = h;
        textureState_ = D3D12_RESOURCE_STATE_COMMON;
        pipeline::makeSrv(device_, texture_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, heap_->GetCPUDescriptorHandleForHeapStart());
        log::info("present-overlay", std::format("texture {}x{} for a picture of {}x{}", w, h, image.width, image.height));
    }
    if (bytes > uploadBytes_) {
        auto buffer = pipeline::makeUploadBuffer(device_, bytes);
        if (!buffer) return false;
        upload_ = std::move(buffer);
        uploadBytes_ = bytes;
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(upload_->Map(0, &none, reinterpret_cast<void**>(&mapped))) || !mapped) return false;
    for (unsigned y = 0; y < image.height; ++y)
        std::memcpy(mapped + size_t(y) * pitch, image.rgba.data() + size_t(y) * image.width * 4, size_t(image.width) * 4);
    upload_->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (textureState_ != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = textureState_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload_.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, image.width, image.height, 1, pitch};
    to.pResource = texture_.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    textureState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    pictureX_ = image.x;
    pictureY_ = image.y;
    pictureW_ = image.width;
    pictureH_ = image.height;
    return true;
}

bool PushPresentOverlay::draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned windowW, unsigned windowH, unsigned bufferW,
                              unsigned bufferH) {
    if (!pso_ || !hasPicture() || textureState_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return false;
    const auto p = pushOverlayPlacement(pictureX_, pictureY_, pictureW_, pictureH_, windowW, windowH, bufferW, bufferH);
    if (p.empty()) return false;
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{float(p.left), float(p.top), float(p.right - p.left), float(p.bottom - p.top), 0.0f, 1.0f};
    const D3D12_RECT scissor{LONG(p.left), LONG(p.top), LONG(p.right), LONG(p.bottom)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSig_.Get());
    list->SetPipelineState(pso_.Get());
    const float constants[8] = {p.originX, p.originY, 1.0f / p.scaleX, 1.0f / p.scaleY, float(pictureW_), float(pictureH_), float(unsigned(mode_)), 0.0f};
    list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
    list->SetGraphicsRootDescriptorTable(1, heap_->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return true;
}

}  // namespace veyra::engine
