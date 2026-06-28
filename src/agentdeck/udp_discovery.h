#pragma once
//
// udp_discovery.h — UDP broadcast fallback for daemon discovery.
//
// Complements mdns_discovery.{h,cpp}: the daemon (bridge/src/broadcast.ts)
// emits a small JSON beacon to <subnet>.255:9121 every 2 s in addition to its
// mDNS service. mDNS multicast is frequently dropped by home routers (AP
// Isolation, IGMP Snooping without multicast enhancement, mesh hops), so this
// listener recovers discovery in those environments.
//
// Polling is cooperative — call udpPoll() from the dashboard loop, the same
// way mdnsPoll() is called. The two are independent and both can fire; the
// caller picks whichever returns a bridge first.
//
#include "mdns_discovery.h"  // BridgeInfo

namespace AgentDeck {
namespace Net {

// Bind the listening socket. Idempotent — safe to call repeatedly (e.g. after
// WiFi reconnect). Returns false if WiFi is down or the socket cannot bind.
bool udpInit();

// Non-blocking poll. Returns true (and fills out) when a valid daemon beacon
// has been received since the last call. Returns false otherwise.
bool udpPoll(BridgeInfo& out);

// Release the socket. Safe to call multiple times.
void udpStop();

}  // namespace Net
}  // namespace AgentDeck
