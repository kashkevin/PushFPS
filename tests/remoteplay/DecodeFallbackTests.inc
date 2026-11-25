// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/remoteplay/DecodeFallback.h: when a PS5 decode failure moves decoding from hardware to
// software (experiment VEYRA_EXP_RP_DECODE_KEEP_HW). Failure sequences are made up; how often a real
// Wi-Fi link makes the D3D12VA decoder fail is what the [remoteplay-decode] lines are there to record.
#include "veyra/remoteplay/DecodeFallback.h"

TEST(decode_fallback_without_the_switch_is_todays_first_failure_rule){
    using F=veyra::remoteplay::HardwareDecodeFallback;
    F f;
    f.connected();
    for(int i=0;i<5000;++i)f.decoded(true);
    CHECK(f.hardwareFailed()==F::Decision::FallBackToSoftware);   // one failure after 5000 hardware frames
    CHECK(f.fallbacks()==1&&f.kept()==0&&!f.keepHardwareOnStreamError());
    f.connected();
    CHECK(f.hardwareFailed()==F::Decision::FallBackToSoftware&&f.fallbacks()==2);
}

TEST(decode_fallback_keeps_hardware_for_isolated_stream_errors){
    using F=veyra::remoteplay::HardwareDecodeFallback;
    F f(true);
    f.connected();
    // A lossy link: one failure every 200 frames for a minute, each followed by a keyframe that decodes.
    for(int episode=0;episode<18;++episode){
        for(int i=0;i<200;++i)f.decoded(true);
        CHECK(f.hardwareFailed()==F::Decision::KeepHardware);
    }
    CHECK(f.kept()==18&&f.fallbacks()==0&&f.failures()==18&&f.consecutive()==1);
}

TEST(decode_fallback_still_falls_back_when_hardware_never_decoded_or_keeps_failing){
    using F=veyra::remoteplay::HardwareDecodeFallback;
    F f(true);
    f.connected();
    // Hardware that cannot decode this stream at all: today's rule at once.
    CHECK(f.hardwareFailed()==F::Decision::FallBackToSoftware&&f.fallbacks()==1);
    // Hardware that decoded, then broke (device lost, driver fault): three failures with no frame between.
    f.connected();
    for(int i=0;i<100;++i)f.decoded(true);
    CHECK(f.hardwareFailed()==F::Decision::KeepHardware);
    CHECK(f.hardwareFailed()==F::Decision::KeepHardware);
    CHECK(f.hardwareFailed()==F::Decision::FallBackToSoftware);
    CHECK(f.fallbacks()==2&&f.kept()==2&&f.consecutive()==0);
    // A new connection forgets the stream (as hardwareFallback_ is cleared on connect today).
    f.connected();
    CHECK(f.hardwareFrames()==0&&f.hardwareFailed()==F::Decision::FallBackToSoftware);
    // A software frame does not count as hardware evidence; a run ends only after 60 decoded frames.
    F g(true,2);
    g.connected();g.decoded(true);
    CHECK(g.hardwareFailed()==F::Decision::KeepHardware);
    for(int i=0;i<60;++i)g.decoded(false);
    CHECK(g.consecutive()==0&&g.hardwareFrames()==1);
    CHECK(g.hardwareFailed()==F::Decision::KeepHardware);
    CHECK(g.hardwareFailed()==F::Decision::FallBackToSoftware);
    CHECK(F(true,0).maxConsecutive()==1);
}

TEST(decode_fallback_a_decoder_that_fails_after_every_keyframe_still_falls_back){
    using F=veyra::remoteplay::HardwareDecodeFallback;
    // Review 03:19: each keyframe decodes, the next frame fails. Resetting on any decoded frame kept hardware
    // for 1000 failures; now the third such failure falls back.
    F f(true);
    f.connected();
    for(int i=0;i<500;++i)f.decoded(true);
    unsigned fallbacks=0;
    for(int k=0;k<1000&&!fallbacks;++k){
        f.decoded(true);                                   // the keyframe
        if(f.hardwareFailed()==F::Decision::FallBackToSoftware)++fallbacks;
    }
    CHECK(fallbacks==1&&f.kept()==2);
    // 59 frames between failures are not a recovery either; 60 are.
    F g(true);
    g.connected();g.decoded(true);
    CHECK(g.hardwareFailed()==F::Decision::KeepHardware);
    for(int i=0;i<59;++i)g.decoded(true);
    CHECK(g.hardwareFailed()==F::Decision::KeepHardware&&g.consecutive()==2);
    for(int i=0;i<60;++i)g.decoded(true);
    CHECK(g.consecutive()==0&&g.hardwareFailed()==F::Decision::KeepHardware);
}
