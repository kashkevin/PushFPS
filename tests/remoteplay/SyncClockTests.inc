// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "sync" (Package 9 evidence): the local video clock (src/remoteplay/Timeline.cpp) when the console sends
// fewer frames than its profile's rate, experiment VEYRA_EXP_RP_CLOCK_RATE (followRate), and the wire rate the
// [remoteplay-clock] line reports. Code-level tests on synthetic arrival times, not a PS5 measurement.
namespace sync_clock_tests {
// Arrivals of `seconds` of a console sending `fps` contiguously numbered frames, with 0-3 ms of jitter and a
// 12 ms late frame every 97th, in order (Chiaki delivers in index order).
struct Run {double lagEndMs=0,lagMaxMs=0,lagMinMs=1e9,stepMinMs=1e9,stepMaxMs=0;bool monotonic=true,discontinuity=false;};
inline Run drive(RemotePlayClock& c,double fps,double seconds,HostTime origin=0,double fromSecond=10){
    Run r;std::int64_t previous=-1;HostTime last=origin;
    const auto frames=static_cast<std::uint64_t>(fps*seconds);
    for(std::uint64_t i=0;i<frames;++i){
        HostTime arrival=origin+1000000+static_cast<HostTime>(double(i)*1e7/fps)+static_cast<HostTime>((i*7919)%30000);
        if(i%97==41)arrival+=120000;
        arrival=std::max(arrival,last);last=arrival;
        const auto s=c.video(i+5,arrival);
        if(!s){r.monotonic=false;continue;}
        r.discontinuity|=s->discontinuity;
        if(previous>=0){
            if(s->pts100ns<=previous)r.monotonic=false;
            const double step=double(s->pts100ns-previous)/1e4;r.stepMinMs=std::min(r.stepMinMs,step);r.stepMaxMs=std::max(r.stepMaxMs,step);
        }
        previous=s->pts100ns;
        const double lag=double(arrival-origin-s->pts100ns)/1e4;
        if(double(i)>=fps*fromSecond){r.lagMaxMs=std::max(r.lagMaxMs,lag);r.lagMinMs=std::min(r.lagMinMs,lag);}
        r.lagEndMs=lag;
    }
    return r;
}
}

TEST(sync_clock_default_keeps_the_package9_lag){
    // The default rule (0.5 % per frame) at the Package 9 run rates: the lag grows by 1000 - 16.75 x fps ms a
    // second (the relation every 10 s window of the eight runs followed to 0.05 ms/s). Kept as it is.
    for(double fps:{57.46,58.60,59.37}){
        RemotePlayClock c;c.reset(0,60);CHECK(!c.followRate());
        const auto r=sync_clock_tests::drive(c,fps,70);
        const double expected=(1000.0-16.75*fps)*69.0;
        std::printf("[sync-clock] default %.2f fps stamped as 60: lag after 70 s %.1f ms (relation: %.1f, within 60)\n",fps,r.lagEndMs,expected);
        CHECK(r.monotonic&&!r.discontinuity);
        CHECK(std::abs(r.lagEndMs-expected)<60.0);
    }
}

TEST(sync_clock_follow_rate_bounds_the_lag_for_a_console_rate_within_ten_percent){
    for(unsigned nominal:{60u,30u}){
        const double step=1000.0/nominal;
        for(double share:{0.91,0.9577,0.9767,0.99,0.999,1.0,1.03,1.09}){
            const double fps=nominal*share;
            RemotePlayClock c;c.reset(0,nominal,true);CHECK(c.followRate());
            const auto r=sync_clock_tests::drive(c,fps,120);
            // Bounded: the lower envelope of one second is followed; the deficit of that second plus the jitter is
            // all the lag left (Package 9's worst window, 56.4 fps, is a 6 % deficit: about 64 ms).
            const double deficit=std::max(0.0,1000.0-step*fps);
            std::printf("[sync-clock] follow %2u profile at %.2f fps: lag after warm-up %.1f..%.1f ms, end %.1f, step %.2f..%.2f ms\n",nominal,fps,r.lagMinMs,r.lagMaxMs,r.lagEndMs,r.stepMinMs,r.stepMaxMs);
            CHECK(r.monotonic&&!r.discontinuity);
            CHECK(r.lagMaxMs<deficit+16.0&&r.lagMaxMs<120.0);
            CHECK(r.lagMinMs>-step);                                  // never a frame ahead of its arrival
            CHECK(r.stepMinMs>=step*0.9-0.01&&r.stepMaxMs<=step*1.1+0.01); // the PTS step moves at most 10 %
        }
    }
}

TEST(sync_clock_follow_rate_changes_nothing_at_the_nominal_rate){
    // A console at its nominal rate (59.94 for a 60 profile): both rules give the same small lag.
    RemotePlayClock a,b;a.reset(0,60);b.reset(0,60,true);
    const auto ra=sync_clock_tests::drive(a,59.94,60),rb=sync_clock_tests::drive(b,59.94,60);
    CHECK(ra.monotonic&&rb.monotonic&&ra.lagMaxMs<20&&rb.lagMaxMs<20);
    CHECK(std::abs(ra.lagEndMs-rb.lagEndMs)<5);
    // A decoder reset resets the clock with the switch the source keeps (RemotePlaySource::read).
    b.reset(5,60,b.followRate());CHECK(b.followRate());
}

TEST(sync_clock_wire_fps_is_the_console_rate_per_window){
    RemotePlayClock c;c.reset(0,60);
    CHECK(!c.takeWireFps(0,10));                                      // no frame yet
    HostTime t=0;
    for(std::uint64_t i=1;i<=600;++i){t=static_cast<HostTime>(double(i)*1e7/58.6);CHECK(c.video(i,t));}
    const auto first=c.takeWireFps(600,t);CHECK(first&&std::abs(*first-58.6)<0.01);
    for(std::uint64_t i=601;i<=1200;++i){t=static_cast<HostTime>(600.0*1e7/58.6+double(i-600)*1e7/57.5);CHECK(c.video(i,t));}
    const auto second=c.takeWireFps(1200,t);CHECK(second&&std::abs(*second-57.5)<0.01);
    CHECK(!c.takeWireFps(1200,t));                                    // no time since the mark
    CHECK(!c.takeWireFps(1100,t+10));                                 // an index that went back
    c.reset(0,60);CHECK(!c.takeWireFps(1300,t+20));                   // a reset forgets the mark
}
