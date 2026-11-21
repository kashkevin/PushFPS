// SPDX-License-Identifier: GPL-3.0-only
// Frame hand-off benchmark for the PS5 Remote Play owner thread: how long a
// complete access unit sits in the session inbox before the decoder owner
// takes it. Portable (no Windows, FFmpeg or Chiaki): it drives the real
// SessionInbox / VideoIngress with a 59.94 fps producer.
//
//   poll : the original owner loop (tryVideo, else sleep 1 ms)
//   wake : the event-driven loop (tryVideo, else WakeSignal::waitFor)
//
// Built without REMOTEPLAY_BENCH_HAVE_WAKE it compiles against the original
// sources and only offers "poll", so the baseline number comes from unmodified code.
// The sleep quantum is a parameter: Linux sleeps 1 ms as ~1.06 ms, Windows as
// 1-2 ms at 1 ms timer resolution and up to 15.6 ms at the default resolution.
#include "veyra/remoteplay/SessionInbox.h"
#ifdef REMOTEPLAY_BENCH_HAVE_WAKE
#include "veyra/remoteplay/WakeSignal.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace veyra::remoteplay;
namespace {
VideoSample configSample(Generation g){
    VideoSample s;s.generation=g;s.kind=SampleKind::CodecConfig;s.width=1920;s.height=1080;
    const std::array<std::uint8_t,14> bytes{0,0,0,1,0x67,0x42,0,0x1f,0,0,1,0x68,0xee,0x3c};
    s.payload=PaddedBytes::copy(bytes);s.arrival100ns=monotonic100ns();return s;
}
VideoSample frameSample(Generation g,std::uint16_t index,const std::vector<std::uint8_t>& body){
    VideoSample s;s.generation=g;s.wireFrameIndex=index;s.width=1920;s.height=1080;
    s.payload=PaddedBytes::copy(body);s.arrival100ns=monotonic100ns();return s;
}
double percentile(std::vector<double>& v,double p){
    if(v.empty())return 0;std::sort(v.begin(),v.end());
    return v[std::min(v.size()-1,std::size_t(p*double(v.size())))];
}
}
int main(int argc,char** argv){
    std::string mode="poll";int frames=3000;double sleepMs=1.0;
    for(int i=1;i<argc;++i){
        if(!std::strcmp(argv[i],"--mode")&&i+1<argc)mode=argv[++i];
        else if(!std::strcmp(argv[i],"--frames")&&i+1<argc)frames=std::atoi(argv[++i]);
        else if(!std::strcmp(argv[i],"--sleep-ms")&&i+1<argc)sleepMs=std::atof(argv[++i]);
    }
#ifndef REMOTEPLAY_BENCH_HAVE_WAKE
    if(mode!="poll"){std::fprintf(stderr,"this build has no WakeSignal: only --mode poll\n");return 2;}
#endif
    SessionInbox inbox;
#ifdef REMOTEPLAY_BENCH_HAVE_WAKE
    auto wake=std::make_shared<WakeSignal>();
    if(mode=="wake")inbox.wakeOn(wake);
#endif
    auto token=inbox.begin(monotonic100ns());const auto g=token.generation();
    token.connected();
    std::atomic<bool> done{false};
    std::vector<double> waitsMs;waitsMs.reserve(std::size_t(frames));
    std::thread owner([&]{
        while(!done.load(std::memory_order_relaxed)){
            auto queued=inbox.tryVideo(monotonic100ns());
            if(queued){
                waitsMs.push_back(double(monotonic100ns()-queued->sample.arrival100ns)/10000.0);
                continue;
            }
#ifdef REMOTEPLAY_BENCH_HAVE_WAKE
            if(mode=="wake"){wake->waitFor(std::chrono::microseconds(4000));continue;}
#endif
            std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(sleepMs));
        }
    });
    // 59.94 fps with +-1.5 ms arrival jitter; a 1080p P frame at 15 Mbit/s is ~31 kB.
    std::vector<std::uint8_t> idr(120000,0x99),inter(31000,0x77);
    idr[0]=0;idr[1]=0;idr[2]=1;idr[3]=0x65;inter[0]=0;inter[1]=0;inter[2]=1;inter[3]=0x41;
    std::mt19937 rng(20261118);std::uniform_real_distribution<double> jitter(-1.5,1.5);
    token.video(configSample(g));
    auto next=std::chrono::steady_clock::now();
    for(int i=0;i<frames;++i){
        next+=std::chrono::microseconds(16683);
        std::this_thread::sleep_until(next+std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double,std::milli>(jitter(rng))));
        token.video(frameSample(g,std::uint16_t(i),i==0?idr:inter));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    done=true;
#ifdef REMOTEPLAY_BENCH_HAVE_WAKE
    wake->notify();
#endif
    owner.join();
    const auto stats=inbox.snapshot().video;
    double sum=0;for(double v:waitsMs)sum+=v;
    const double mean=waitsMs.empty()?0:sum/double(waitsMs.size());
    const double p50=percentile(waitsMs,.50),p95=percentile(waitsMs,.95),p99=percentile(waitsMs,.99),max=waitsMs.empty()?0:waitsMs.back();
    std::printf("{\"bench\":\"handoff\",\"mode\":\"%s\",\"sleepMs\":%.2f,\"frames\":%zu,\"dropped\":%llu,\"meanMs\":%.4f,\"p50Ms\":%.4f,\"p95Ms\":%.4f,\"p99Ms\":%.4f,\"maxMs\":%.4f}\n",
        mode.c_str(),mode=="wake"?0.0:sleepMs,waitsMs.size(),static_cast<unsigned long long>(stats.dropped),mean,p50,p95,p99,max);
    return waitsMs.size()==std::size_t(frames)?0:1;
}
