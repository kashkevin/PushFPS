// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/remoteplay/ClarityOverride.h: the kit's test-only connect overrides.
#include "veyra/remoteplay/ClarityOverride.h"
#include <cstring>
#include <string>

TEST(clarity_bitrate_override_accepts_only_plain_numbers_in_range){
    CHECK(parseClarityBitrateKbps(L"15000")==std::optional<std::uint32_t>(15000u));
    CHECK(parseClarityBitrateKbps(L"1000")==std::optional<std::uint32_t>(1000u));
    CHECK(parseClarityBitrateKbps(L"100000")==std::optional<std::uint32_t>(100000u));
    CHECK(!parseClarityBitrateKbps(L""));
    CHECK(!parseClarityBitrateKbps(L"999"));
    CHECK(!parseClarityBitrateKbps(L"100001"));
    CHECK(!parseClarityBitrateKbps(L"-5000"));
    CHECK(!parseClarityBitrateKbps(L"+5000"));
    CHECK(!parseClarityBitrateKbps(L" 5000"));
    CHECK(!parseClarityBitrateKbps(L"50k"));
    CHECK(!parseClarityBitrateKbps(L"0050000"));   // seven characters: refused, no overflow path
    CHECK(!parseClarityBitrateKbps(L"999999"));
}

TEST(clarity_codec_override_knows_three_words){
    CHECK(parseClarityCodec(L"h264")==std::optional<Codec>(Codec::H264));
    CHECK(parseClarityCodec(L"hevc")==std::optional<Codec>(Codec::H265));
    CHECK(parseClarityCodec(L"hevc-hdr")==std::optional<Codec>(Codec::H265Hdr));
    CHECK(!parseClarityCodec(L""));
    CHECK(!parseClarityCodec(L"HEVC"));
    CHECK(!parseClarityCodec(L"h265"));
}

TEST(clarity_override_unset_changes_nothing_and_logs_nothing){
    VideoProfile video;video.bitrateKbps=15000;video.codec=Codec::H265Hdr;
    const ClarityOverride none{};
    CHECK(!none.any());
    CHECK(!applyClarityOverride(none,video));
    CHECK(video.bitrateKbps==15000&&video.codec==Codec::H265Hdr&&video.width==1920&&video.height==1080&&video.fps==60);
    char line[256];
    CHECK(clarityOverrideLine(none,15000,Codec::H265Hdr,line,sizeof line)==0);
}

TEST(clarity_override_sets_only_what_was_asked_and_says_what_was_saved){
    VideoProfile video;video.bitrateKbps=15000;video.codec=Codec::H265Hdr;
    ClarityOverride o{};o.bitrateKbps=50000u;
    CHECK(applyClarityOverride(o,video));
    CHECK(video.bitrateKbps==50000&&video.codec==Codec::H265Hdr);
    CHECK(video.validate()==std::nullopt);
    char line[256];
    const auto n=clarityOverrideLine(o,15000,Codec::H265Hdr,line,sizeof line);
    CHECK(n>0&&n<sizeof line);
    CHECK(std::strstr(line,"bitrateKbps=50000 savedBitrateKbps=15000 codec=hevc-hdr savedCodec=hevc-hdr")!=nullptr);
    ClarityOverride c{};c.codec=Codec::H265;
    CHECK(applyClarityOverride(c,video));
    CHECK(video.bitrateKbps==50000&&video.codec==Codec::H265);
    // A short buffer is cut, never overrun.
    char tiny[8];
    CHECK(clarityOverrideLine(o,15000,Codec::H265,tiny,sizeof tiny)==sizeof tiny-1&&tiny[sizeof tiny-1]=='\0');
}

TEST(clarity_clip_path_must_be_absolute_and_plain){
    CHECK(clarityClipPathUsable(L"C:\\clips\\gta-foliage.mp4"));
    CHECK(clarityClipPathUsable(L"d:/clips/a.mkv"));
    CHECK(clarityClipPathUsable(L"\\\\nas\\clips\\a.mp4"));
    CHECK(!clarityClipPathUsable(L""));
    CHECK(!clarityClipPathUsable(L"C:"));
    CHECK(!clarityClipPathUsable(L"clip.mp4"));                 // relative
    CHECK(!clarityClipPathUsable(L"..\\clip.mp4"));
    CHECK(!clarityClipPathUsable(L"C:clip.mp4"));               // drive-relative
    CHECK(!clarityClipPathUsable(L"\\\\\\x"));                     // three backslashes
    CHECK(!clarityClipPathUsable(L"C:\\a\nb.mp4"));           // a control character
    CHECK(!clarityClipPathUsable(std::wstring(1024,L'a')));
}
