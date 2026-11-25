// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS (veyra/engine/PushDeviceLossResume.h).
// Portable policy only: the conditions, the attempt budget and its sliding window, the backoff wait that a
// user action cuts short (two threads, no sleeps), and the hand-over slot for the connect description
// (moved once, never copied, wiped on every path that does not resume). The Windows glue
// (EngineController::run teardown, RemotePlaySessionSource::keepForResume / takeResume) is not run here.
#include "veyra/engine/PushDeviceLossResume.h"
#include "veyra/remoteplay/ChiakiBackend.h"
#include <cstring>
#include <new>
namespace device_loss_resume_tests {
namespace en=veyra::engine;
using R=en::PushResumeReason;
en::PushDeviceLossFacts lost(){
    en::PushDeviceLossFacts f;f.remoteSession=true;f.deviceLost=true;f.haveDescription=true;return f;
}
// The real secrets: PairingCredentials (move and destructor wipe, inline in ChiakiBackend.h) inside the
// real NativeConnectRequest, the part of RemotePlayConnectDesc that carries them. RemotePlayConnectDesc
// itself needs d3d12.h; its other members (queue limits, decode mode, flags, decode device) hold no secret.
namespace rp=veyra::remoteplay;
using Request=rp::NativeConnectRequest;
static_assert(!std::is_copy_constructible_v<rp::PairingCredentials>);
static_assert(!std::is_copy_constructible_v<Request>);
static_assert(!std::is_copy_constructible_v<en::PushResumeSlot<Request>>);
// Distinctive key bytes (no zero byte): a scan for any 4 of them in a row finds a leftover copy.
// Pseudo-random per seed, never zero: two seeds share no 4-byte run (checked by the tests that need it).
std::uint8_t secretByte(std::uint8_t seed,std::size_t i){
    std::uint32_t x=std::uint32_t(seed)*2654435761u+std::uint32_t(i)*40503u+0x9E37u;
    x^=x>>15;x*=2246822519u;x^=x>>13;
    return std::uint8_t((x&0xFEu)|1u);
}
Request secretRequest(std::uint8_t seed){
    Request r;
    for(std::size_t i=0;i<r.credentials.registrationKey.size();++i)r.credentials.registrationKey[i]=char(secretByte(seed,i));
    for(std::size_t i=0;i<r.credentials.sessionKey.size();++i)r.credentials.sessionKey[i]=secretByte(seed,100+i);
    for(std::size_t i=0;i<r.credentials.accountId.size();++i)r.credentials.accountId[i]=secretByte(seed,200+i);
    return r;
}
bool holdsSecret(const Request& r,std::uint8_t seed){
    const Request want=secretRequest(seed);
    return r.credentials.registrationKey==want.credentials.registrationKey&&r.credentials.sessionKey==want.credentials.sessionKey&&
           r.credentials.accountId==want.credentials.accountId;
}
bool allZero(const rp::PairingCredentials& c){
    for(char b:c.registrationKey)if(b)return false;
    for(auto b:c.sessionKey)if(b)return false;
    for(auto b:c.accountId)if(b)return false;
    return true;
}
// Any 4 consecutive bytes of one of the three keys, anywhere in [bytes, bytes+size).
bool bytesContainSecret(const unsigned char* bytes,std::size_t size,std::uint8_t seed){
    const Request want=secretRequest(seed);
    const auto search=[&](const unsigned char* key,std::size_t keySize){
        for(std::size_t k=0;k+4<=keySize;++k)
            for(std::size_t i=0;i+4<=size;++i)
                if(std::memcmp(bytes+i,key+k,4)==0)return true;
        return false;
    };
    return search(reinterpret_cast<const unsigned char*>(want.credentials.registrationKey.data()),want.credentials.registrationKey.size())||
           search(want.credentials.sessionKey.data(),want.credentials.sessionKey.size())||
           search(want.credentials.accountId.data(),want.credentials.accountId.size());
}
// A slot built in storage this test owns, so its bytes can be scanned after take / replace / destroy.
struct ScannedSlot {
    alignas(en::PushResumeSlot<Request>) unsigned char storage[sizeof(en::PushResumeSlot<Request>)];
    en::PushResumeSlot<Request>* slot;
    ScannedSlot(){std::memset(storage,0,sizeof storage);slot=new(storage) en::PushResumeSlot<Request>();}
    void destroy(){slot->~PushResumeSlot<Request>();slot=nullptr;}
    ~ScannedSlot(){if(slot)destroy();}
    bool contains(std::uint8_t seed)const{return bytesContainSecret(storage,sizeof storage,seed);}
};
}
using namespace device_loss_resume_tests;

TEST(device_loss_resume_switch_unset_is_never){
    // Only "1" turns it on; with the switch off the engine never asks (keepForResume) and the session
    // never keeps anything: pushKeepForResume(armed=false, ...) is false whatever happened.
    CHECK(en::pushDeviceLossResumeSwitchedOn(L"1"));
    for(auto v:{L"",L"0",L"on",L"true",L"11",L" 1"})CHECK(!en::pushDeviceLossResumeSwitchedOn(v));
    CHECK(!en::pushKeepForResume(false,true,false));
    en::PushResumeSlot<Request> slot;  // never armed: what the session does without the switch
    CHECK(!slot.armed());
    CHECK(!slot.take().has_value());
}
TEST(device_loss_resume_conditions){
    CHECK(en::decidePushDeviceLossResume(lost())==R::Resume);
    auto f=lost();f.userStopped=true;CHECK(en::decidePushDeviceLossResume(f)==R::UserStop);
    f=lost();f.otherSource=true;CHECK(en::decidePushDeviceLossResume(f)==R::OtherSource);
    f=lost();f.appExit=true;CHECK(en::decidePushDeviceLossResume(f)==R::AppExit);
    f=lost();f.remoteSession=false;CHECK(en::decidePushDeviceLossResume(f)==R::NotRemote);
    f=lost();f.engineError=true;CHECK(en::decidePushDeviceLossResume(f)==R::EngineError);
    f=lost();f.sessionOwnerAlive=true;CHECK(en::decidePushDeviceLossResume(f)==R::RecoveryRunning);
    f=lost();f.haveDescription=false;CHECK(en::decidePushDeviceLossResume(f)==R::NoDescription);
    // A user action wins over everything else, a device loss included.
    f=lost();f.userStopped=true;f.sessionFailed=true;CHECK(en::decidePushDeviceLossResume(f)==R::UserStop);
    for(int r=0;r<=int(R::DeviceUnavailable);++r){const std::string w=en::pushResumeReasonWord(R(r));CHECK(!w.empty()&&w!="unknown"&&w.find(' ')==std::string::npos);}
}
TEST(device_loss_resume_unrelated_session_end_is_never){
    // The session ended on its own (StreamRecovery gave up, refused, threw) or the device is fine.
    auto f=lost();f.sessionFailed=true;CHECK(en::decidePushDeviceLossResume(f)==R::SessionEnded);
    f=lost();f.deviceLost=false;CHECK(en::decidePushDeviceLossResume(f)==R::NoDeviceLoss);
    // The session keeps its description only when close() stopped it and it had not failed.
    CHECK(en::pushKeepForResume(true,true,false));
    CHECK(!en::pushKeepForResume(true,false,false)); // ended by itself
    CHECK(!en::pushKeepForResume(true,true,true));   // failed
}
TEST(device_loss_resume_budget_exhaustion_and_backoff){
    en::PushDeviceLossResumeBudget b;
    const int64_t t0=1'000'000;
    const std::array<int64_t,3> want{1000,3000,10000};
    for(unsigned i=0;i<3;++i){
        auto p=b.plan(t0+i*20000);CHECK(p);CHECK(p->attempt==i+1);CHECK(p->delayMs==want[i]);
        b.commit(t0+i*20000);
    }
    CHECK(!b.plan(t0+60000));                       // a fourth in the window: refused
    CHECK(!b.plan(t0+en::PushDeviceLossResumeBudget::kWindowMs-1));
    // plan() alone spends nothing (a resume cancelled during its backoff costs no attempt).
    en::PushDeviceLossResumeBudget c;CHECK(c.plan(t0)->attempt==1);CHECK(c.plan(t0)->attempt==1);CHECK(c.used(t0)==0);
}
TEST(device_loss_resume_window_slides){
    en::PushDeviceLossResumeBudget b;
    const int64_t w=en::PushDeviceLossResumeBudget::kWindowMs;
    b.commit(0);b.commit(5*60000);b.commit(9*60000);
    CHECK(!b.plan(w-1));
    auto p=b.plan(w);CHECK(p);CHECK(p->attempt==3);CHECK(p->delayMs==10000); // the first left the window
    b.commit(w);
    CHECK(!b.plan(w+1));
    p=b.plan(5*60000+w);CHECK(p&&p->attempt==3);                            // the second left too
    p=b.plan(w+w);CHECK(p&&p->attempt==1&&p->delayMs==1000);                // all gone: back to 1 s
}
TEST(device_loss_resume_second_loss_during_resume_counts){
    // The resumed session loses its device again: the next resume waits longer and the fourth is refused.
    en::PushDeviceLossResumeBudget b;int64_t now=0;std::vector<int64_t> delays;
    for(int loss=0;loss<4;++loss){
        auto f=lost();CHECK(en::decidePushDeviceLossResume(f)==R::Resume);
        if(auto p=b.plan(now)){delays.push_back(p->delayMs);b.commit(now);}
        now+=30000; // 30 s of picture, then the next loss
    }
    CHECK((delays==std::vector<int64_t>{1000,3000,10000}));
}
TEST(device_loss_resume_user_stop_during_backoff){
    // The engine waits on its own condition variable; stop() changes the state and notifies.
    std::mutex m;std::condition_variable cv;bool stopped=false,waiting=false;
    std::thread engineThread;bool completed=true;
    const auto started=std::chrono::steady_clock::now();
    engineThread=std::thread([&]{
        std::unique_lock lock(m);waiting=true;cv.notify_all();
        completed=en::pushResumeBackoff(cv,lock,std::chrono::milliseconds(10000),[&]{return stopped;});
    });
    {std::unique_lock lock(m);cv.wait(lock,[&]{return waiting;});stopped=true;}
    cv.notify_all();
    engineThread.join();
    CHECK(!completed);
    CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds(5));
    // A stop before the wait is not lost either.
    std::unique_lock lock(m);
    CHECK(!en::pushResumeBackoff(cv,lock,std::chrono::milliseconds(10000),[&]{return stopped;}));
    // No cancellation: the whole (short) delay passes.
    stopped=false;
    CHECK(en::pushResumeBackoff(cv,lock,std::chrono::milliseconds(5),[&]{return stopped;}));
}
TEST(device_loss_resume_user_stop_race_many_iterations){
    // stop() at any moment: the wait never completes once stopped is set before it ends, never hangs.
    for(int i=0;i<300;++i){
        std::mutex m;std::condition_variable cv;bool stopped=false;bool completed=false;
        std::thread t([&]{std::unique_lock lock(m);completed=en::pushResumeBackoff(cv,lock,std::chrono::milliseconds(2000),[&]{return stopped;});});
        {std::lock_guard lock(m);stopped=true;}cv.notify_all();
        t.join();CHECK(!completed);
    }
}
TEST(device_loss_resume_pending_flag_life_cycle){
    // PlayerSnapshot::deviceLossResumePending as the engine sets it and the bridge reads it.
    CHECK(!en::pushResumePendingAtLoss(false,true,true));       // switch off: never set
    CHECK(!en::pushResumePendingAtLoss(true,false,true));       // not a PS5 session
    CHECK(!en::pushResumePendingAtLoss(true,true,false));       // budget spent: the failure shows at once
    CHECK(en::pushResumePendingAtLoss(true,true,true));
    // Switch off: exactly the old edge (!wasFailed && failed), whatever came before.
    for(int wasFailed=0;wasFailed<2;++wasFailed)for(int failed=0;failed<2;++failed)
        CHECK(en::pushLiveFailureShownNow(wasFailed,false,failed,false)==(!wasFailed&&failed));
    // Loss with a resume pending: the failure is published together with the flag, nothing shown.
    CHECK(!en::pushLiveFailureShownNow(false,false,true,true));
    CHECK(!en::pushLiveFailureShownNow(true,true,true,true));   // during the backoff and the device wait
    // Resumed: the posted run's snapshot reset clears failed and the flag together, nothing shown.
    CHECK(!en::pushLiveFailureShownNow(true,true,false,false));
    // Abandoned (budget, no device, stop, other condition): the flag clears with the run still failed.
    CHECK(en::pushLiveFailureShownNow(true,true,true,false));
    // A new source opened meanwhile (snapshot reset, not failed): nothing shown.
    CHECK(!en::pushLiveFailureShownNow(true,true,false,false));
    // Once shown, the next poll does not show it again.
    CHECK(!en::pushLiveFailureShownNow(true,false,true,false));
}
TEST(device_loss_resume_credentials_handed_over_once_and_wiped){
    // The real destructor wipes, also with optimisation (volatile writes): scan the storage afterwards.
    {   alignas(rp::PairingCredentials) unsigned char storage[sizeof(rp::PairingCredentials)];std::memset(storage,0,sizeof storage);
        auto* c=new(storage) rp::PairingCredentials();
        Request filled=secretRequest(3);*c=std::move(filled.credentials);
        CHECK(allZero(filled.credentials));CHECK(bytesContainSecret(storage,sizeof storage,3));
        c->~PairingCredentials();
        CHECK(!bytesContainSecret(storage,sizeof storage,3));
    }
    {   // Resumed: moved into the slot (the session's desc is left wiped), taken exactly once.
        ScannedSlot s;s.slot->arm();
        Request desc=secretRequest(42);
        CHECK(en::pushKeepForResume(s.slot->armed(),true,false));
        s.slot->keep(std::move(desc));
        CHECK(allZero(desc.credentials));CHECK(s.slot->holding());CHECK(s.contains(42));
        auto taken=s.slot->take();
        CHECK(taken&&holdsSecret(*taken,42));CHECK(!s.slot->holding());CHECK(!s.slot->armed());
        CHECK(!s.contains(42));                                    // nothing left behind in the slot
        CHECK(!s.slot->take().has_value());                        // a second take gets nothing
        Request moved=std::move(*taken);                           // the engine's hand-over to the new task
        CHECK(allZero(taken->credentials));CHECK(holdsSecret(moved,42));
    }
    {   // Replaced: a second keep wipes the first.
        ScannedSlot s;s.slot->arm();
        s.slot->keep(secretRequest(7));CHECK(s.contains(7));CHECK(!s.contains(9));
        s.slot->keep(secretRequest(9));
        CHECK(!s.contains(7));CHECK(s.contains(9));
        auto taken=s.slot->take();CHECK(taken&&holdsSecret(*taken,9));CHECK(!s.contains(9));
    }
    {   // Not resumed (budget, user stop, app exit): kept but never taken; destroying the slot wipes it.
        ScannedSlot s;s.slot->arm();s.slot->keep(secretRequest(11));CHECK(s.contains(11));
        s.destroy();
        CHECK(!s.contains(11));
    }
    {   // Taken, then dropped by the engine (cancelled during the backoff, device unavailable).
        ScannedSlot s;s.slot->arm();s.slot->keep(secretRequest(13));
        alignas(Request) unsigned char storage[sizeof(Request)];std::memset(storage,0,sizeof storage);
        auto taken=s.slot->take();CHECK(taken);
        auto* dropped=new(storage) Request(std::move(*taken));taken.reset();
        CHECK(bytesContainSecret(storage,sizeof storage,13));
        dropped->~Request();
        CHECK(!bytesContainSecret(storage,sizeof storage,13));CHECK(!s.contains(13));
    }
    {   // Not armed: the session's desc is never kept, and the slot never holds anything.
        ScannedSlot s;CHECK(!en::pushKeepForResume(s.slot->armed(),true,false));CHECK(!s.slot->take());CHECK(!s.contains(5));
    }
}
