// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Test-only connect overrides for the kit's "clarity" session (stream "clarity",
// docs/drafts/CLARITY_VERDICT.md section 9; docs/PS5_REMOTE_PLAY_EXPERIMENTS.md "Clarity session").
//   VEYRA_TEST_RP_BITRATE_KBPS=<1000..100000>  the bandwidth asked of the PS5 (launch spec bwKbpsSent)
//   VEYRA_TEST_RP_CODEC=h264|hevc|hevc-hdr     the stream codec asked for
//   VEYRA_TEST_RP_CLIP=<absolute path>         a recorded clip opened instead of the PS5 (PushAutorun.cpp)
// Both are read once by the bridge (src/ui/bridge/RemotePlay.cpp, ps5Connect) and applied to the
// connect request AFTER the console profile was saved, so the user's saved choice never changes.
// Anything else (empty, out of range, a sign, spaces, another word) is ignored: no override.
// Portable, header-only, pure.
#include "veyra/remoteplay/Types.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
namespace veyra::remoteplay {
inline constexpr std::uint32_t kClarityMinKbps=1000,kClarityMaxKbps=100000; // as Validation.cpp
// Decimal digits only, at most 6, within the range above.
inline std::optional<std::uint32_t> parseClarityBitrateKbps(std::wstring_view text)noexcept{
    if(text.empty()||text.size()>6)return std::nullopt;
    std::uint32_t value=0;
    for(const wchar_t c:text){
        if(c<L'0'||c>L'9')return std::nullopt;
        value=value*10u+static_cast<std::uint32_t>(c-L'0');
    }
    if(value<kClarityMinKbps||value>kClarityMaxKbps)return std::nullopt;
    return value;
}
inline std::optional<Codec> parseClarityCodec(std::wstring_view text)noexcept{
    if(text==L"h264")return Codec::H264;
    if(text==L"hevc")return Codec::H265;
    if(text==L"hevc-hdr")return Codec::H265Hdr;
    return std::nullopt;
}
inline const char* clarityCodecName(Codec codec)noexcept{
    return codec==Codec::H264?"h264":codec==Codec::H265?"hevc":"hevc-hdr";
}
struct ClarityOverride {
    std::optional<std::uint32_t> bitrateKbps;
    std::optional<Codec> codec;
    bool any()const noexcept{return bitrateKbps.has_value()||codec.has_value();}
};
// Applies the override to a request's video profile; returns true when something was set.
inline bool applyClarityOverride(const ClarityOverride& o,VideoProfile& video)noexcept{
    if(o.bitrateKbps)video.bitrateKbps=*o.bitrateKbps;
    if(o.codec)video.codec=*o.codec;
    return o.any();
}
// VEYRA_TEST_RP_CLIP=<path> (src/ui/bridge/PushAutorun.cpp): the kit's recorded clip is opened instead of connecting,
// so every arm of a comparison processes the same frames. Usable = an absolute Windows path (X:\... or \\server\...),
// 4..1023 characters, no control characters. The path itself is never logged.
inline bool clarityClipPathUsable(std::wstring_view path)noexcept{
    if(path.size()<4||path.size()>1023)return false;
    for(const wchar_t c:path)if(c<L' '||c==0x7f)return false;
    const bool drive=((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/');
    const bool share=path[0]==L'\\'&&path[1]==L'\\'&&path[2]!=L'\\';
    return drive||share;
}
// The text after "[remoteplay-clarity] " (a component the kit's log filter keeps) (numbers and fixed words only); 0 when nothing to log.
inline std::size_t clarityOverrideLine(const ClarityOverride& o,std::uint32_t savedKbps,Codec savedCodec,char* out,std::size_t size)noexcept{
    if(!o.any()||size==0)return 0;
    const int n=std::snprintf(out,size,"test-override bitrateKbps=%lu savedBitrateKbps=%lu codec=%s savedCodec=%s (VEYRA_TEST_RP_BITRATE_KBPS / VEYRA_TEST_RP_CODEC; this connection only, the saved profile is unchanged)",
        static_cast<unsigned long>(o.bitrateKbps?*o.bitrateKbps:savedKbps),static_cast<unsigned long>(savedKbps),
        clarityCodecName(o.codec?*o.codec:savedCodec),clarityCodecName(savedCodec));
    if(n<=0)return 0;
    return static_cast<std::size_t>(n)<size?static_cast<std::size_t>(n):size-1;
}
} // namespace veyra::remoteplay
