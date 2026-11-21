#pragma once
#include "ChiakiBackend.h"
#include "ConnectionHealth.h"
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
}
