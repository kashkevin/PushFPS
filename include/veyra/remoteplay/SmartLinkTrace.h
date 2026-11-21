// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink trace file: one CSV line per Sample window (SmartLinkTypes.h), so a
// real recording from a PC can be replayed through the predictor and the
// controller (tools/smartlink_sim --replay). The app-side recorder and the
// replay reader both use THIS header, so the format cannot drift.
// Numbers only: no names, no addresses, no network names. Unknown = -1 (RSSI:
// 0). Locale independent (std::to_chars / std::from_chars). Portable,
// header-only, no allocation. Format version 1 (28 columns; 27 in the first draft); the column list is fixed:
// new columns may only be appended (a reader ignores columns it does not know
// and fills missing trailing ones with "unknown").
#include "SmartLinkTypes.h"
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace veyra::remoteplay::smartlink::trace {

inline constexpr std::string_view kHeader=
    "endMs,windowMs,route,packetsReceived,packetsLost,packetsReordered,packetsLate,packetGapMeanMs,packetGapMaxMs,"
    "framesArrived,framesIncomplete,framesFecRecovered,frameGapMaxMs,frameGapExcessMs,lateRescueMs,reorderWaitMs,"
    "rttMs,consoleTargetKbps,measuredKbps,requestedKbps,videoQueueDepth,audioBufferedMs,audioUnderruns,"
    "wifiSignalPercent,wifiRssiDbm,wifiLinkMbps,wifiBandGhz,frameLateMeanMs";
inline constexpr std::size_t kColumns=28;
// A buffer of this size always holds one line (28 numbers of at most 20 characters, commas, newline).
inline constexpr std::size_t kMaxLine=28*22+2;

namespace detail {
struct Out {
    char* p;char* end;bool ok=true;
    void put(char c)noexcept{if(p<end)*p++=c;else ok=false;}
    // Integers are read back through a double, so they are clamped to 2^53
    // (9007199254740992), the largest value a double holds exactly: a count or a
    // millisecond time never comes near it (2^53 ms is about 285000 years).
    static constexpr std::int64_t kMaxInteger=std::int64_t{1}<<53;
    void integer(std::int64_t v)noexcept{
        if(v<0)v=-1;
        if(v>kMaxInteger)v=kMaxInteger;
        auto r=std::to_chars(p,end,v);if(r.ec!=std::errc{}){ok=false;return;}p=r.ptr;
    }
    void number(double v)noexcept{
        if(!(v==v)||v<-1e12||v>1e12){v=-1;} // NaN or absurd: unknown
        auto r=std::to_chars(p,end,v,std::chars_format::fixed,3);if(r.ec!=std::errc{}){ok=false;return;}
        // Trim trailing zeros: "12.500" -> "12.5", "3.000" -> "3".
        char* q=r.ptr;
        while(q>p&&q[-1]=='0')--q;
        if(q>p&&q[-1]=='.')--q;
        p=q;
    }
};
}

// Writes kHeader and a newline. Returns the bytes written (0: buffer too small).
inline std::size_t writeTraceHeader(char* buffer,std::size_t size)noexcept{
    if(!buffer||size<kHeader.size()+2)return 0;
    for(std::size_t i=0;i<kHeader.size();++i)buffer[i]=kHeader[i];
    buffer[kHeader.size()]='\n';buffer[kHeader.size()+1]='\0';
    return kHeader.size()+1;
}
// One line with a newline (and a terminating NUL not counted). Returns the
// bytes written, 0 when the buffer is too small (use kMaxLine).
inline std::size_t formatTraceLine(const Sample& s,char* buffer,std::size_t size)noexcept{
    if(!buffer||size<2)return 0;
    detail::Out o{buffer,buffer+size-1};
    const auto sep=[&]{o.put(',');};
    o.integer(s.endMs);sep();o.number(s.windowMs);sep();o.integer(static_cast<std::int64_t>(s.route));sep();
    o.integer(s.packetsReceived);sep();o.integer(s.packetsLost);sep();o.integer(s.packetsReordered);sep();o.integer(s.packetsLate);sep();
    o.number(s.packetGapMeanMs);sep();o.number(s.packetGapMaxMs);sep();
    o.integer(s.framesArrived);sep();o.integer(s.framesIncomplete);sep();o.integer(s.framesFecRecovered);sep();
    o.number(s.frameGapMaxMs);sep();o.number(s.frameGapExcessMs);sep();o.number(s.lateRescueMs);sep();o.number(s.reorderWaitMs);sep();
    o.number(s.rttMs);sep();o.number(s.consoleTargetKbps);sep();o.number(s.measuredKbps);sep();o.number(s.requestedKbps);sep();
    o.integer(s.videoQueueDepth);sep();o.number(s.audioBufferedMs);sep();o.integer(s.audioUnderruns);sep();
    o.number(s.wifiSignalPercent);sep();
    {const double rssi=s.wifiRssiDbm;if(rssiKnown(rssi))o.number(rssi);else o.put('0');}
    sep();o.number(s.wifiLinkMbps);sep();o.number(s.wifiBandGhz);sep();o.number(s.frameLateMeanMs);
    o.put('\n');
    if(!o.ok)return 0;
    *o.p='\0';
    return static_cast<std::size_t>(o.p-buffer);
}

// Parses one line (with or without its newline). The header line, an empty
// line or a line starting with '#' returns false. A field that is not a
// number becomes unknown; missing trailing fields stay unknown; extra fields
// are ignored. Returns false when the first field (endMs) is not a number.
inline bool parseTraceLine(std::string_view line,Sample& out)noexcept{
    out=Sample{};
    while(!line.empty()&&(line.back()=='\n'||line.back()=='\r'))line.remove_suffix(1);
    if(line.empty()||line.front()=='#'||line.substr(0,5)=="endMs")return false;
    std::array<double,kColumns> v{};v.fill(-1);v[24]=0;
    std::size_t column=0,start=0;bool first=false;
    for(std::size_t i=0;i<=line.size()&&column<kColumns;++i){
        if(i<line.size()&&line[i]!=',')continue;
        const std::string_view field=line.substr(start,i-start);
        double x=0;
        const auto r=std::from_chars(field.data(),field.data()+field.size(),x);
        if(r.ec==std::errc{}&&r.ptr==field.data()+field.size()&&x==x){v[column]=x;if(column==0)first=true;}
        ++column;start=i+1;
    }
    if(!first)return false;
    const auto i64=[&](std::size_t c)->std::int64_t{const double x=v[c];return x>=0&&x<9.2e18?static_cast<std::int64_t>(x):-1;};
    out.endMs=i64(0);out.windowMs=v[1];
    const auto route=i64(2);out.route=route==1?Route::Wired:route==2?Route::Wifi:Route::Unknown;
    out.packetsReceived=i64(3);out.packetsLost=i64(4);out.packetsReordered=i64(5);out.packetsLate=i64(6);
    out.packetGapMeanMs=v[7];out.packetGapMaxMs=v[8];
    out.framesArrived=i64(9);out.framesIncomplete=i64(10);out.framesFecRecovered=i64(11);
    out.frameGapMaxMs=v[12];out.frameGapExcessMs=v[13];out.lateRescueMs=v[14];out.reorderWaitMs=v[15];
    out.rttMs=v[16];out.consoleTargetKbps=v[17];out.measuredKbps=v[18];out.requestedKbps=v[19];
    out.videoQueueDepth=i64(20);out.audioBufferedMs=v[21];out.audioUnderruns=i64(22);
    out.wifiSignalPercent=v[23];out.wifiRssiDbm=v[24];out.wifiLinkMbps=v[25];out.wifiBandGhz=v[26];out.frameLateMeanMs=v[27];
    return true;
}
} // namespace veyra::remoteplay::smartlink::trace
