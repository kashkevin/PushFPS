#pragma once
#include "ChiakiBackend.h"
#include "ConnectionHealth.h"
#include "NetDiagnostics.h"
#include <string_view>
#include <vector>
namespace veyra::remoteplay {
struct DiscoveredConsole {std::string host;bool standby=false;std::string consoleId;};
struct DiscoveryReport {
    std::vector<DiscoveredConsole> hosts;
    std::vector<std::string> diagnostics;
    int error=0;
};
DiscoveryReport discoverLocalPs5(std::stop_token);
BackendResult wakeLocalPs5(const NativeConnectRequest&);
// Windows-only half of the route check in ConnectionHealth.h: this PC's active
// IPv4 addresses with their on-link prefix, link type and receive link speed
// (GetAdaptersAddresses). Virtual switches and most VPN adapters report as
// Ethernet. Empty when the lookup fails; never throws.
std::vector<LocalInterface> localInterfaces() noexcept;
// Read-only system facts for [slipstream-net] (NetDiagnostics.h, VEYRA_EXP_RP_STATS=1). routeFacts: the
// system's own route to the console's IPv4 literal (GetBestRoute2): on-link when the next hop is the
// console itself, through a gateway otherwise; Unknown for a host name or when the lookup fails.
// systemNetCounters: the system-wide UDP counters (GetUdpStatisticsEx) and those of the adapter with the
// given index (GetIfEntry2; 0 = none). Nothing is changed, nothing is logged here; never throws.
struct RouteFacts {RouteHop hop=RouteHop::Unknown;std::uint32_t interfaceIndex=0;};
RouteFacts routeFacts(std::string_view host) noexcept;
SystemNetCounters systemNetCounters(std::uint32_t interfaceIndex) noexcept;
}
