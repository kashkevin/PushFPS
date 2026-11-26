// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace" frame probe, GPU side: a 128 x 72 luma thumbnail of a presented frame (shaders/PushFrameThumb.hlsl) into a
// ring of kEntries readback slots, collected when the fence says it is done: never waited for. Review 2 SHOULD 1: the
// presenter records it at the END of the frame's own blit list, before that list is submitted, so the read of the frame is
// covered by the same fence that protects the frame's slot from the graph's next write (presentationValues_), and no extra
// command slot or submission is used. It is GPU work before the flip: one dispatch of 16 x 9 groups (147 456 point loads) and
// a 36 864-byte copy, inside the Blit stage's GPU timer (so [fg-health] gpu_blit shows its cost). A frame that finds no free
// entry (the GPU is behind) is not probed (counted as skipped). Owner thread only.
#include "veyra/engine/PushFrameProbe.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace veyra::engine {

class PushFrameThumb {
public:
    static constexpr unsigned kEntries = 8;
    static constexpr UINT64 kBytes = UINT64(PushFrameProbe::kCells) * sizeof(float);
    bool create(ID3D12Device* device);
    bool ready() const { return created_; }
    // The format can be read (8/10/16-bit UNORM or a float format).
    static bool readableFormat(DXGI_FORMAT format, bool* floatFormat = nullptr);
    // A free entry exists and nothing is recorded unsubmitted: record() will not refuse for lack of room.
    bool hasRoom() const { return created_ && count_ < kEntries && recorded_ == kEntries; }
    // Records the thumbnail of `source` (in D3D12_RESOURCE_STATE_COMMON before and after) into the next free entry. False when
    // none is free, the format is not readable, or nothing was created; nothing is recorded then.
    bool record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* source, DXGI_FORMAT format, std::uint64_t sequence, bool generated,
                unsigned subframe = 0, unsigned groupFrames = 0);
    // The list with the last recorded entry was submitted; it is done when `fence` reaches `value`.
    void submitted(ID3D12Fence* fence, std::uint64_t value);
    // The list was not submitted: the recorded entry is free again.
    void discarded();
    // Every finished entry in recording order: f(sequence, generated, subframe, groupFrames, thumbnail).
    template <class F> void collect(F&& f) {
        while (count_) {
            auto& e = entries_[head_];
            if (e.state != State::InFlight || !pipeline::fenceComplete(e.fence, e.value)) return;
            f(e.sequence, e.generated, e.subframe, e.groupFrames, mapped_ ? reinterpret_cast<const float*>(mapped_ + std::size_t(head_) * kBytes) : nullptr);
            e.state = State::Free;
            head_ = (head_ + 1) % kEntries;
            --count_;
        }
    }
    // Forget everything in flight (waits nothing: the caller has drained the queue or will not read the ring again).
    void reset();
    ~PushFrameThumb();

private:
    enum class State : std::uint8_t { Free, Recorded, InFlight };
    struct Entry { State state = State::Free; std::uint64_t sequence = 0, value = 0; bool generated = false; unsigned subframe = 0, groupFrames = 0; ID3D12Fence* fence = nullptr; };
    pipeline::ComputePass pass_;
    Microsoft::WRL::ComPtr<ID3D12Resource> thumb_, readback_;
    std::array<Entry, kEntries> entries_{};
    unsigned head_ = 0, count_ = 0, recorded_ = kEntries;
    std::uint8_t* mapped_ = nullptr;
    bool created_ = false, failed_ = false;
};

}  // namespace veyra::engine
