// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink's Wi-Fi reading and the optional Wi-Fi media streaming mode
// (Windows WLAN API; src/remoteplay/SmartLinkWifi.cpp). Numbers only: the
// network name, BSSID, profile name and addresses are never queried, read or
// stored. Only wlan_intf_opcode_rssi is queried (no location consent is
// involved; wlan_intf_opcode_current_connection is never used, it carries the
// network name and needs location consent since Windows 11 24H2).
//
// - Polling runs on its own thread, at most once a second, never on the packet
//   or owner path; latest() copies the newest reading under a lock.
// - The interface: the one WLAN interface in the connected state. With none or
//   several connected the reading is unknown (the route's adapter GUID is not
//   matched; a PC with two connected Wi-Fi adapters reads unknown).
// - Everything fails soft: no WLAN service, a wired PC, access denied -> unknown.
// - Media streaming mode (VEYRA_EXP_RP_WIFI_STREAMING_MODE=1 only, never
//   SmartLink): WlanSetInterface(wlan_intf_opcode_media_streaming_mode, TRUE)
//   on the connected interface; Windows ends it when the handle closes, which
//   the destructor does on every way out.
// Unverified on Windows (blind code: compiled only by MSVC in CI).
#include "SmartLinkFeed.h"
#include <memory>
namespace veyra::remoteplay::smartlink {
enum class StreamingModeState : std::uint8_t {Off,Requested,Refused};
inline const char* streamingModeName(StreamingModeState s)noexcept{return s==StreamingModeState::Requested?"requested":s==StreamingModeState::Refused?"refused":"off";}
// Windows' link quality mapping (WLAN_REALTIME_CONNECTION_QUALITY: 0..100 is
// -100..-50 dBm, linear). Unknown RSSI (>= 0 or absurd): -1.
inline double signalPercentFromRssi(double rssiDbm)noexcept{
    if(!(rssiDbm<0)||rssiDbm<=-130)return -1;
    const double p=2.0*(rssiDbm+100.0);
    return p<0?0:p>100?100:p;
}
class WifiMonitor {
public:
    WifiMonitor()noexcept;
    ~WifiMonitor();
    WifiMonitor(const WifiMonitor&)=delete;WifiMonitor& operator=(const WifiMonitor&)=delete;
    // poll: read the RSSI once a second; streamingMode: request the media
    // streaming mode. Either may be false. Never throws.
    void start(bool pollSignal,bool requestStreamingMode)noexcept;
    WifiReading latest()const noexcept;
    StreamingModeState streamingMode()const noexcept;
    // Closes the handle (ends a requested streaming mode) and stops polling.
    // Returns true when a requested streaming mode was released.
    bool stop()noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace veyra::remoteplay::smartlink
