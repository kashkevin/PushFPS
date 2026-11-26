// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// When a PS5 decode failure moves the stream from hardware to software decoding (sprint 5, stream "present",
// experiment VEYRA_EXP_RP_DECODE_KEEP_HW; source RemotePlaySource::read / openDecoder).
//
// Today (exp/rc5), with decode mode Automatic, the FIRST failed submission while the D3D12VA decoder is in use
// frees it and decodes in software for the rest of the connection, even after thousands of hardware frames.
// On a lossy link (Wi-Fi) a failure is usually the stream (lost references, a corrupt unit), not the hardware:
// one such failure then costs every later frame the software decoder's time and a CPU core.
//
// Rule (re-implemented from moonlight-qt, app/streaming/video/ffmpeg.cpp, decoder thread: every decode error
// asks for an IDR; FAILED_DECODES_RESET_THRESHOLD = 20 consecutive failures rebuild the decoder; it never
// gives up hardware for one error): with the switch, a failure is a STREAM error (flush, wait for a keyframe:
// what the software path already does) while hardware decoding has produced a frame on this connection and
// fewer than `maxConsecutive` failures came without kRecoveredFrames (60, a second at 60 FPS) decoded
// between them. Otherwise the decision is today's: fall back to software. (Review 03:19: resetting the count
// on ANY decoded frame let a decoder that decodes each keyframe and fails the next frame stay on hardware
// for ever.) `maxConsecutive` is 3, not 20: each failure here costs a keyframe round trip,
// and the outage rule (StreamRecovery: reconnect after 6 s without a decoded frame) resets the decoder choice;
// three keyframe requests fit well inside it, so a hardware decoder that really broke still falls back before
// a reconnect would hide it and loop.
//
// Portable, no allocation, owned by the decoder thread.
#include <cstdint>

namespace veyra::remoteplay {

class HardwareDecodeFallback {
public:
    static constexpr unsigned kMaxConsecutive=3;
    static constexpr unsigned kRecoveredFrames=60; // decoded frames after a failure that end a run of failures
    enum class Decision {FallBackToSoftware,KeepHardware};

    explicit HardwareDecodeFallback(bool keepHardwareOnStreamError=false,unsigned maxConsecutive=kMaxConsecutive)
        :keep_(keepHardwareOnStreamError),maxConsecutive_(maxConsecutive<1?1:maxConsecutive){}

    // A new connection: nothing is known about this stream yet (today's hardwareFallback_=false).
    void connected(){hardwareFrames_=0;consecutive_=0;sinceFailure_=0;}
    // A frame came out of the decoder; `hardware` = it is a D3D12VA frame.
    void decoded(bool hardware){if(hardware)++hardwareFrames_;if(consecutive_&&++sinceFailure_>=kRecoveredFrames){consecutive_=0;sinceFailure_=0;}}
    // A submission failed while the hardware decoder was in use (decode mode Automatic).
    Decision hardwareFailed(){
        ++failures_;
        sinceFailure_=0;
        if(!keep_||!hardwareFrames_){++fallbacks_;return Decision::FallBackToSoftware;}
        if(++consecutive_>=maxConsecutive_){++fallbacks_;consecutive_=0;return Decision::FallBackToSoftware;}
        ++kept_;return Decision::KeepHardware;
    }

    bool keepHardwareOnStreamError()const{return keep_;}
    unsigned maxConsecutive()const{return maxConsecutive_;}
    std::uint64_t hardwareFrames()const{return hardwareFrames_;}
    unsigned consecutive()const{return consecutive_;}
    std::uint64_t failures()const{return failures_;}   // lifetime, across connections
    std::uint64_t kept()const{return kept_;}           // failures treated as stream errors
    std::uint64_t fallbacks()const{return fallbacks_;} // failures that moved decoding to software

private:
    bool keep_=false;
    unsigned maxConsecutive_=kMaxConsecutive;
    std::uint64_t hardwareFrames_=0;
    unsigned consecutive_=0,sinceFailure_=0;
    std::uint64_t failures_=0,kept_=0,fallbacks_=0;
};

} // namespace veyra::remoteplay
