// SPDX-License-Identifier: GPL-3.0-only
// Input dispatch model for PS5 Remote Play: time from a pad state change being
// readable on the PC to chiaki_session_set_controller_state() being called.
// A discrete-event simulation in virtual time around the real ControllerQueue.
// It is a model of the two threads' schedules, not a measurement of Windows:
//
//   original : UI polls the pad every 8 ms; the owner loop wakes every
//              `--owner-sleep-ms` (Sleep(1) is 1-2 ms on Windows), decodes a
//              frame when one is there, and sends one queued state per 4 ms tick.
//   new      : UI polls every 4 ms; a changed state wakes the owner, which
//              sends everything pending unless it is inside a decode submit.
//
// Every edge must reach the console exactly once and in order; the run fails otherwise.
//
//   mouse    : `--mode mouse` models a mouse played as the right stick (VEYRA_EXP_RP_KBM): reports
//              at 125, 500, 1000 and 8000 Hz against pad poll intervals of 1, 2, 4 and 8 ms, through
//              the real MouseAim. Per combination: how long a report waits for the poll that sends
//              it (added delay), how many pad states leave per second, how many polls found no new
//              report and how many reports were merged into one state (wasted work on either
//              side), and how far the sent stick tilt is from the hand's true speed.
//              `--table` prints the rows as the Markdown table of docs/KEYBOARD_MOUSE.md.
//              Options: --speed <counts/s> (1500), --seconds <virtual s> (20).
//   stroke   : `--mode stroke` asks what the smoothing window is for. A short mouse stroke (20
//              counts in 10 ms) is sent through MouseAim at `--poll-ms` (4) and read by a game
//              that takes the newest pad state once per picture (`--game-fps`, 30). Per window:
//              how much of the stroke the game turns, and how much that varies with the moment
//              the stroke happened to start. `--table` as above.
//   calibrate: `--mode calibrate` runs the keyboard and mouse calibration (KbmCalibration.h)
//              against simulated games of known dead zone, response and fastest turn, with a
//              simulated player whose taps are 250 ms late and off by up to +/- `--tap-error-ms`
//              (80), over `--seeds` (20) runs per game; per game and profile: what was measured
//              against the truth and the linearity error of the turn before (built-in profile)
//              and after (calibrated profile). `--table` as above.
//              Both are Linux models in virtual time: no Windows timer, no USB stack, no console,
//              and how a real game reads the pad is an assumption.
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/KeyboardMousePad.h"
#include "veyra/remoteplay/KbmCalibrationModel.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace veyra::remoteplay;
namespace {
using Us=long long; // virtual microseconds
struct Dist{std::vector<double> v;
    void add(double x){v.push_back(x);}
    double mean()const{double s=0;for(double x:v)s+=x;return v.empty()?0:s/double(v.size());}
    double pct(double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[std::min(v.size()-1,std::size_t(p*double(v.size())))];}
};
struct MouseRow {
    int mouseHz=0,pollMs=0;
    double waitMeanMs=0,waitP95Ms=0,waitMaxMs=0,statesPerSecond=0,emptyPollsPercent=0,reportsPerPoll=0;
    double tiltErrorPercent[3]={0,0,0};   // smoothing windows of kWindowsMs
};
constexpr double kWindowsMs[3]={0,8,33};
MouseAimConfig modelAim(double smoothingMs,int stickBits=16){
    MouseAimConfig c;c.fullSpeedCounts=4000;c.deadzone=.12;c.smoothingMs=smoothingMs;c.stickBits=stickBits;return c;
}
// One combination. The hand moves at a constant `speed`; the mouse sends whole counts at its
// report rate (a report without movement is not sent, as on a real mouse); the pad poll fires
// every `pollMs` with up to 0.3 ms of timer lateness and sends the state when it changed. The
// mouse's clock is its own: it runs 0.1 % fast, so over a run every phase between report and poll
// occurs, as between a real mouse and a real timer.
MouseRow mouseRun(int mouseHz,int pollMs,double speed,double seconds){
    MouseRow row;row.mouseHz=mouseHz;row.pollMs=pollMs;
    std::mt19937_64 rng(1000003ull*unsigned(mouseHz)+unsigned(pollMs));
    std::uniform_real_distribution<double> unit(0,1);
    const double reportUs=1e6/mouseHz*1.001;const Us end=Us(seconds*1e6);
    MouseAim aims[3]={MouseAim(modelAim(kWindowsMs[0])),MouseAim(modelAim(kWindowsMs[1])),MouseAim(modelAim(kWindowsMs[2]))};
    MouseAim shipped(modelAim(33,8));                          // what the built-in profiles send: 33 ms window, 8-bit steps
    const double deadzone=modelAim(0).deadzone,trueTilt=deadzone+(1-deadzone)*std::min(1.0,speed/modelAim(0).fullSpeedCounts);
    Dist wait;double squared[3]={0,0,0};
    std::uint64_t polls=0,emptyPolls=0,states=0,reports=0,settled=0;
    std::vector<Us> waiting;                                    // report times not yet sent
    const double reportPhase=unit(rng)*reportUs;
    double owed=0;Us nextReport=Us(reportPhase),nextPoll=Us(unit(rng)*pollMs*1000),lastPoll=-1;
    long long reportIndex=0,pollIndex=0;std::int16_t lastSent=0;
    while(true){
        // The earlier of the next report and the next poll.
        if(nextReport<=nextPoll){
            if(nextReport>=end)break;
            owed+=speed*reportUs/1e6;
            const int counts=int(owed);owed-=counts;
            if(counts>0){for(auto& aim:aims)aim.add(counts,0);shipped.add(counts,0);waiting.push_back(nextReport);++reports;}
            ++reportIndex;
            nextReport=Us(reportPhase+double(reportIndex)*reportUs+(unit(rng)-.5)*.04*reportUs);   // +-2 % of the interval
            continue;
        }
        if(nextPoll>=end)break;
        const Us now=nextPoll;++polls;
        if(waiting.empty())++emptyPolls;
        const std::uint64_t elapsed=lastPoll<0?std::uint64_t(pollMs)*1000:std::uint64_t(now-lastPoll);lastPoll=now;
        for(int w=0;w<3;++w){
            const auto out=aims[w].step(elapsed);
            if(now>500000){const double error=(out.x/32767.0-trueTilt)/(trueTilt-deadzone);squared[w]+=error*error;if(w==0)++settled;}
        }
        const auto sent=shipped.step(elapsed);
        if(sent.x!=lastSent)++states;                           // a changed state is one packet: the sender has no minimum interval
        lastSent=sent.x;
        for(const Us at:waiting)wait.add(double(now-at)/1000);
        waiting.clear();
        ++pollIndex;
        nextPoll=Us(double(pollIndex)*pollMs*1000+unit(rng)*300);
    }
    row.waitMeanMs=wait.mean();row.waitP95Ms=wait.pct(.95);row.waitMaxMs=wait.pct(1.0);
    row.statesPerSecond=double(states)/seconds;
    row.emptyPollsPercent=polls?100.0*double(emptyPolls)/double(polls):0;
    row.reportsPerPoll=polls>emptyPolls?double(reports)/double(polls-emptyPolls):0;
    for(int w=0;w<3;++w)row.tiltErrorPercent[w]=settled?100*std::sqrt(squared[w]/double(settled)):0;
    return row;
}
int mouseModel(double speed,double seconds,bool table){
    if(table){
        std::printf("| Mouse | Poll | Wait mean | Wait P95 | Wait max | States sent | Polls without a new report | Reports per poll | Tilt error, no window | 8 ms window | 33 ms window |\n");
        std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
    }
    for(const int mouseHz:{125,500,1000,8000})for(const int pollMs:{1,2,4,8}){
        const MouseRow r=mouseRun(mouseHz,pollMs,speed,seconds);
        if(table)std::printf("| %d Hz | %d ms | %.2f ms | %.2f ms | %.2f ms | %.0f /s | %.0f %% | %.1f | %.0f %% | %.0f %% | %.1f %% |\n",
            r.mouseHz,r.pollMs,r.waitMeanMs,r.waitP95Ms,r.waitMaxMs,r.statesPerSecond,r.emptyPollsPercent,r.reportsPerPoll,
            r.tiltErrorPercent[0],r.tiltErrorPercent[1],r.tiltErrorPercent[2]);
        else std::printf("{\"bench\":\"mouse\",\"label\":\"Linux model\",\"mouseHz\":%d,\"pollMs\":%d,\"speedCountsPerSecond\":%.0f,\"waitMeanMs\":%.3f,\"waitP95Ms\":%.3f,\"waitMaxMs\":%.3f,"
            "\"statesPerSecond\":%.1f,\"emptyPollsPercent\":%.1f,\"reportsPerPoll\":%.3f,\"tiltErrorPercentNoWindow\":%.1f,\"tiltErrorPercent8ms\":%.1f,\"tiltErrorPercent33ms\":%.1f}\n",
            r.mouseHz,r.pollMs,speed,r.waitMeanMs,r.waitP95Ms,r.waitMaxMs,r.statesPerSecond,r.emptyPollsPercent,r.reportsPerPoll,
            r.tiltErrorPercent[0],r.tiltErrorPercent[1],r.tiltErrorPercent[2]);
    }
    return 0;
}
// A 1000 Hz mouse makes one short stroke; a game takes the newest pad state once per picture and
// turns at that state's speed for the whole picture. How much of the stroke does it turn?
int strokeModel(int pollMs,double gameFps,bool table){
    const double frameUs=1e6/gameFps;const int strokeCounts=20,strokeReports=10,trials=4000;
    if(table){
        std::printf("| Window | Turned, mean | Spread (standard deviation) | Least | Most | Delay it would add if the game read every state |\n");
        std::printf("|---|---|---|---|---|---|\n");
    }
    for(const double window:{0.0,8.0,16.0,33.0,50.0}){
        std::mt19937_64 rng(77);std::uniform_real_distribution<double> unit(0,1);
        Dist turned;
        for(int trial=0;trial<trials;++trial){
            MouseAim aim(modelAim(window));
            const Us strokeAt=200000+Us(unit(rng)*frameUs*4);    // any phase against polls and pictures
            Us nextPoll=Us(unit(rng)*pollMs*1000),lastPoll=-1;long long pollIndex=0;
            double nextFrame=unit(rng)*frameUs;int report=0;double tilt=0,counts=0;
            const double deadzone=aim.config().deadzone;
            while(nextPoll<strokeAt+400000){
                const Us now=nextPoll;
                // Pictures up to this poll read the state sent before it.
                while(nextFrame<double(now)){if(tilt>deadzone)counts+=(tilt-deadzone)/(1-deadzone)*aim.config().fullSpeedCounts*frameUs/1e6;nextFrame+=frameUs;}
                while(report<strokeReports&&strokeAt+Us(report)*1000<=now){aim.add(strokeCounts/strokeReports,0);++report;}
                const std::uint64_t elapsed=lastPoll<0?std::uint64_t(pollMs)*1000:std::uint64_t(now-lastPoll);lastPoll=now;
                tilt=aim.step(elapsed).x/32767.0;
                ++pollIndex;nextPoll=Us(double(pollIndex)*pollMs*1000+unit(rng)*300);
            }
            turned.add(100*counts/strokeCounts);
        }
        double variance=0;const double mean=turned.mean();
        for(const double v:turned.v)variance+=(v-mean)*(v-mean);
        const double spread=std::sqrt(variance/double(turned.v.size()));
        const auto delay=mouseSmoothingDelay(window,pollMs);
        if(table)std::printf("| %.0f ms | %.0f %% | %.0f %% | %.0f %% | %.0f %% | %.1f ms mean, %.0f ms at most |\n",window,mean,spread,turned.pct(0),turned.pct(1.0),delay.meanMs,delay.lastMs);
        else std::printf("{\"bench\":\"stroke\",\"label\":\"Linux model\",\"pollMs\":%d,\"gameFps\":%.2f,\"windowMs\":%.0f,\"turnedMeanPercent\":%.1f,\"turnedSpreadPercent\":%.1f,\"turnedMinPercent\":%.1f,\"turnedMaxPercent\":%.1f,"
            "\"addedDelayMeanMsIfEveryStateIsRead\":%.1f,\"addedDelayLastMs\":%.1f}\n",pollMs,gameFps,window,mean,spread,turned.pct(0),turned.pct(1.0),delay.meanMs,delay.lastMs);
    }
    return 0;
}
}
// The calibration against simulated games (a Linux model).
int calibrateModel(double tapErrorMs,int seeds,bool table){
    using kbmmodel::Game;
    struct Case {const char* name;Game walk,aim;};
    const auto game=[](double dz,double maxDeg,double vertical,double curve,Game::Shape shape){
        Game g;g.deadzone=dz;g.maxDegPerSecond=maxDeg;g.vertical=vertical;g.curve=curve;g.shape=shape;return g;};
    const Case cases[]={
        {"power 1.8, dead zone 0.20",game(.2,240,.6,1.8,Game::Shape::Power),game(.15,120,.75,1.4,Game::Shape::Power)},
        {"power 1.0, dead zone 0.10",game(.1,300,.8,1.0,Game::Shape::Power),game(.1,150,.8,1.0,Game::Shape::Power)},
        {"power 2.5, dead zone 0.25",game(.25,180,.5,2.5,Game::Shape::Power),game(.2,90,.6,2.0,Game::Shape::Power)},
        {"two zones, dead zone 0.15",game(.15,270,.6,1,Game::Shape::TwoZone),game(.12,135,.7,1,Game::Shape::TwoZone)}};
    if(table){
        std::printf("| Game (walk / aim) | Profile | Dead zone true / measured | Full turn true / measured | Vertical ratio true / measured | Linearity error before | after (mean, worst) | after, 15-90 %% only | Scale error after (mean, worst) | Minutes |\n");
        std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    }
    for(const auto& c:cases){
        const std::array<Game,3> games{c.walk,c.aim,c.walk};
        auto set=kbmBuiltinProfiles();set.countsPer360=8000;
        for(const auto id:{KbmProfileId::Walk,KbmProfileId::Aim}){
            const Game& g=games[std::size_t(id)];
            Dist dz,turn,ratio,after,fast,scale,minutes;
            const auto before=kbmmodel::linearity(set[id].aim,g,360.0/8000);
            int finished=0;
            for(int seed=1;seed<=seeds;++seed){
                const auto run=kbmmodel::calibrate(set,games,kbmmodel::Player{.25,tapErrorMs/1000,1.0,true},std::uint32_t(seed*7919));
                if(!run.finished)continue;
                ++finished;
                const auto& m=run.measured[std::size_t(id)];
                if(m.deadzone)dz.add(*m.deadzone);
                if(m.turnSeconds)turn.add(*m.turnSeconds);
                if(m.ratio)ratio.add(*m.ratio);
                const auto l=kbmmodel::linearity(kbmCalibratedProfiles(set,run.measured)[id].aim,g,360.0/8000);
                after.add(l.shape);fast.add(l.fast);scale.add(std::abs(l.scale));minutes.add(run.seconds/60);
            }
            const double worstAfter=after.pct(1.0),worstScale=scale.pct(1.0);
            if(table)std::printf("| %s | %s | %.2f / %.3f | %.2f s / %.3f s | %.2f / %.3f | %.0f %% | %.1f %%, %.1f %% | %.1f %% | %.1f %%, %.1f %% | %.1f |\n",
                c.name,kbmProfileName(id),g.deadzone,dz.mean(),360/g.maxDegPerSecond,turn.mean(),1/g.vertical,ratio.mean(),
                before.shape,after.mean(),worstAfter,fast.mean(),scale.mean(),worstScale,minutes.mean());
            else std::printf("{\"bench\":\"calibrate\",\"label\":\"Linux model\",\"game\":\"%s\",\"profile\":\"%s\",\"runs\":%d,\"finished\":%d,\"tapErrorMs\":%.0f,"
                "\"deadzoneTrue\":%.3f,\"deadzoneMean\":%.4f,\"turnTrue\":%.3f,\"turnMean\":%.4f,\"ratioTrue\":%.3f,\"ratioMean\":%.4f,"
                "\"linearityBeforePercent\":%.2f,\"linearityAfterMeanPercent\":%.2f,\"linearityAfterWorstPercent\":%.2f,\"linearityAfterFastMeanPercent\":%.2f,\"scaleAfterMeanPercent\":%.2f,\"scaleAfterWorstPercent\":%.2f,\"minutesMean\":%.2f}\n",
                c.name,kbmProfileName(id),seeds,finished,tapErrorMs,g.deadzone,dz.mean(),360/g.maxDegPerSecond,turn.mean(),1/g.vertical,ratio.mean(),
                before.shape,after.mean(),worstAfter,fast.mean(),scale.mean(),worstScale,minutes.mean());
        }
    }
    return 0;
}
int main(int argc,char** argv){
    std::string mode="original";int edges=20000;double ownerSleepMs=1.5,decodeMs=1.0,wakeMs=0.1,pollMs=4;bool motion=true;
    double mouseSpeed=1500,mouseSeconds=20,gameFps=30,tapErrorMs=80;bool table=false;int seeds=20;
    for(int i=1;i<argc;++i){
        if(!std::strcmp(argv[i],"--mode")&&i+1<argc)mode=argv[++i];
        else if(!std::strcmp(argv[i],"--edges")&&i+1<argc)edges=std::atoi(argv[++i]);
        else if(!std::strcmp(argv[i],"--owner-sleep-ms")&&i+1<argc)ownerSleepMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--decode-ms")&&i+1<argc)decodeMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--wake-ms")&&i+1<argc)wakeMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--poll-ms")&&i+1<argc)pollMs=std::clamp(std::atof(argv[++i]),1.0,8.0); // new mode: VEYRA_EXP_RP_PAD_POLL_MS
        else if(!std::strcmp(argv[i],"--no-motion"))motion=false;
        else if(!std::strcmp(argv[i],"--speed")&&i+1<argc)mouseSpeed=std::clamp(std::atof(argv[++i]),10.0,4000.0);
        else if(!std::strcmp(argv[i],"--seconds")&&i+1<argc)mouseSeconds=std::clamp(std::atof(argv[++i]),1.0,600.0);
        else if(!std::strcmp(argv[i],"--game-fps")&&i+1<argc)gameFps=std::clamp(std::atof(argv[++i]),10.0,240.0);
        else if(!std::strcmp(argv[i],"--table"))table=true;
        else if(!std::strcmp(argv[i],"--tap-error-ms")&&i+1<argc)tapErrorMs=std::clamp(std::atof(argv[++i]),0.0,500.0);
        else if(!std::strcmp(argv[i],"--seeds")&&i+1<argc)seeds=std::clamp(std::atoi(argv[++i]),1,1000);
    }
    if(mode=="mouse")return mouseModel(mouseSpeed,mouseSeconds,table);
    if(mode=="calibrate")return calibrateModel(tapErrorMs,seeds,table);
    if(mode=="stroke")return strokeModel(int(pollMs),gameFps,table);
    const bool original=mode=="original";
    const Us pollUs=original?8000:Us(pollMs*1000);
    std::mt19937_64 rng(807);
    std::uniform_real_distribution<double> gapMs(40,400),unit(0,1);
    // Button edges: alternate press / release of Cross at human intervals.
    std::vector<Us> edgeAt;edgeAt.reserve(std::size_t(edges));
    Us t=100000;for(int i=0;i<edges;++i){t+=Us(gapMs(rng)*1000);edgeAt.push_back(t);}
    const Us end=t+500000;
    ControllerQueue queue(original?ControllerQueue::Policy::Tick:ControllerQueue::Policy::Immediate);
    // UI poll schedule with up to 0.3 ms of timer jitter.
    Us nextPoll=0,nextPollAt=0;std::size_t edgeSeen=0;std::uint32_t buttons=0;std::int16_t stick=0;
    std::vector<Us> polledAt(std::size_t(edges),-1);
    Us wakeAt=-1; // new: earliest time a push asks the owner to run
    auto poll=[&](Us upTo){
        while(nextPollAt<=upTo){
            const Us at=nextPollAt;
            while(edgeSeen<edgeAt.size()&&edgeAt[edgeSeen]<=at){buttons^=ControllerState::Cross;polledAt[edgeSeen]=at;++edgeSeen;}
            ControllerState s;s.inputActive=true;s.buttons=buttons;
            if(motion){s.leftX=++stick;s.motionValid=true;s.motionTimestampUs=std::uint64_t(at);} // stick/gyro change on every poll
            if(queue.push(at*10,s)&&!original&&(wakeAt<0||at<wakeAt))wakeAt=at;
            nextPoll+=pollUs;nextPollAt=nextPoll+Us(unit(rng)*300);
        }
    };
    // Video: 59.94 fps, +-1.5 ms; each frame costs the owner `decodeMs` of submit time.
    Us nextFrame=16683;auto frameJitter=[&]{return Us((unit(rng)*3.0-1.5)*1000);};Us frameAt=nextFrame+frameJitter();
    Dist sampling,dispatch,total;std::size_t delivered=0;std::uint32_t consoleButtons=0;bool orderOk=true;std::uint64_t submits=0;
    auto submit=[&](Us now,const ControllerState& s){
        ++submits;
        if(s.buttons!=consoleButtons){
            // One edge per submitted change: Chiaki turns it into a history event.
            if(delivered>=edgeAt.size()||polledAt[delivered]<0||polledAt[delivered]>now)orderOk=false;
            else{sampling.add(double(polledAt[delivered]-edgeAt[delivered])/1000);dispatch.add(double(now-polledAt[delivered])/1000);total.add(double(now-edgeAt[delivered])/1000);}
            consoleButtons=s.buttons;++delivered;
        }
    };
    Us now=0,nextController=0;
    std::uniform_real_distribution<double> sleepSpread(0.67,1.33); // e.g. 1.5 ms -> 1.0..2.0 ms
    while(now<end){
        poll(now);
        if(original){
            if(now>=nextController){submit(now,queue.take(now*10));nextController=now+4000;}
        }else{
            bool due=now>=nextController;
            for(unsigned n=0;n<ControllerQueue::kCapacity+1;++n){
                if(!due&&!queue.pending())break;
                submit(now,queue.take(now*10));due=false;nextController=now+4000;
                if(!queue.pending())break;
            }
            wakeAt=-1;
        }
        if(frameAt<=now){now+=Us(decodeMs*1000);nextFrame+=16683;frameAt=nextFrame+frameJitter();continue;}
        if(original){now+=Us(ownerSleepMs*sleepSpread(rng)*1000);continue;}
        // new: sleep until a pad change, a frame, or the 4 ms tick, whichever is first.
        Us wake=std::min(nextController,frameAt+Us(wakeMs*1000));
        // Look ahead for the first poll that queues something new.
        while(nextPollAt<wake){poll(nextPollAt);if(wakeAt>=0){wake=std::min(wake,wakeAt+Us(wakeMs*1000));break;}}
        now=std::max(now+1,wake);
    }
    const bool ok=orderOk&&delivered==edgeAt.size()&&queue.overflows()==0;
    std::printf("{\"bench\":\"input\",\"mode\":\"%s\",\"pollMs\":%lld,\"ownerSleepMs\":%.2f,\"motion\":%s,\"edges\":%zu,\"delivered\":%zu,\"inOrder\":%s,\"overflows\":%llu,\"submitsPerSecond\":%.0f,"
        "\"samplingMeanMs\":%.3f,\"dispatchMeanMs\":%.3f,\"totalMeanMs\":%.3f,\"totalP50Ms\":%.3f,\"totalP95Ms\":%.3f,\"totalP99Ms\":%.3f,\"totalMaxMs\":%.3f}\n",
        mode.c_str(),pollUs/1000,original?ownerSleepMs:0.0,motion?"true":"false",edgeAt.size(),delivered,orderOk?"true":"false",static_cast<unsigned long long>(queue.overflows()),double(submits)/(double(end)/1e6),
        sampling.mean(),dispatch.mean(),total.mean(),total.pct(.50),total.pct(.95),total.pct(.99),total.pct(1.0));
    return ok?0:1;
}
