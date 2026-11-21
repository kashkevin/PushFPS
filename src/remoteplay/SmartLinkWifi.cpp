// SPDX-License-Identifier: GPL-3.0-only
// SmartLink's Wi-Fi reading and the optional media streaming mode; see
// include/veyra/remoteplay/SmartLinkWifi.h. Blind Windows code: never run here.
#include "veyra/remoteplay/SmartLinkWifi.h"
#include <windows.h>
#include <wlanapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace veyra::remoteplay::smartlink {
struct WifiMonitor::Impl {
    HANDLE client=nullptr;
    GUID iface{};
    bool haveIface=false;
    std::mutex mutex;
    std::condition_variable cv;
    WifiReading reading{};
    StreamingModeState streaming=StreamingModeState::Off;
    bool stopping=false;
    std::thread thread;

    // The one connected WLAN interface; false with none or several.
    bool findInterface()noexcept{
        PWLAN_INTERFACE_INFO_LIST list=nullptr;
        if(WlanEnumInterfaces(client,nullptr,&list)!=ERROR_SUCCESS||!list)return false;
        unsigned connected=0;GUID found{};
        for(DWORD i=0;i<list->dwNumberOfItems;++i){
            if(list->InterfaceInfo[i].isState==wlan_interface_state_connected){++connected;found=list->InterfaceInfo[i].InterfaceGuid;}
        }
        WlanFreeMemory(list);
        if(connected!=1)return false;
        iface=found;return true;
    }
    WifiReading read()noexcept{
        WifiReading r;
        if(!haveIface&&!findInterface())return r;
        haveIface=true;
        DWORD size=0;PVOID data=nullptr;WLAN_OPCODE_VALUE_TYPE type{};
        if(WlanQueryInterface(client,&iface,wlan_intf_opcode_rssi,nullptr,&size,&data,&type)!=ERROR_SUCCESS||!data){
            haveIface=false; // the interface may have gone; look again next time
            return r;
        }
        if(size>=sizeof(LONG)){
            const LONG rssi=*static_cast<const LONG*>(data);
            if(rssi<0&&rssi>-130){r.rssiDbm=static_cast<double>(rssi);r.signalPercent=signalPercentFromRssi(r.rssiDbm);}
        }
        WlanFreeMemory(data);
        return r;
    }
    void poll()noexcept{
        std::unique_lock lock(mutex);
        while(!stopping){
            lock.unlock();
            const WifiReading r=read();
            lock.lock();
            reading=r;
            cv.wait_for(lock,std::chrono::seconds(1),[&]{return stopping;});
        }
    }
};
WifiMonitor::WifiMonitor()noexcept=default;
WifiMonitor::~WifiMonitor(){(void)stop();}
void WifiMonitor::start(bool pollSignal,bool requestStreamingMode)noexcept{
    if(impl_||(!pollSignal&&!requestStreamingMode))return;
    try{impl_=std::make_unique<Impl>();}catch(...){return;}
    DWORD negotiated=0;
    if(WlanOpenHandle(2,nullptr,&negotiated,&impl_->client)!=ERROR_SUCCESS){impl_->client=nullptr;if(requestStreamingMode)impl_->streaming=StreamingModeState::Refused;return;}
    if(requestStreamingMode){
        BOOL on=TRUE;
        const bool found=impl_->findInterface();
        impl_->haveIface=found;
        impl_->streaming=found&&WlanSetInterface(impl_->client,&impl_->iface,wlan_intf_opcode_media_streaming_mode,sizeof(on),&on,nullptr)==ERROR_SUCCESS
            ?StreamingModeState::Requested:StreamingModeState::Refused;
    }
    if(pollSignal){
        try{impl_->thread=std::thread([impl=impl_.get()]{impl->poll();});}catch(...){}
    }
}
WifiReading WifiMonitor::latest()const noexcept{
    if(!impl_)return {};
    std::lock_guard lock(impl_->mutex);return impl_->reading;
}
StreamingModeState WifiMonitor::streamingMode()const noexcept{
    if(!impl_)return StreamingModeState::Off;
    std::lock_guard lock(impl_->mutex);return impl_->streaming;
}
bool WifiMonitor::stop()noexcept{
    if(!impl_)return false;
    {std::lock_guard lock(impl_->mutex);impl_->stopping=true;}
    impl_->cv.notify_all();
    if(impl_->thread.joinable())impl_->thread.join();
    const bool released=impl_->streaming==StreamingModeState::Requested;
    if(impl_->client)WlanCloseHandle(impl_->client,nullptr); // ends the streaming mode this handle asked for
    impl_.reset();
    return released;
}
} // namespace veyra::remoteplay::smartlink
