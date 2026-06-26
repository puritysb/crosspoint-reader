#pragma once
//
// mdns_discovery.h — port of AgentDeck esp32/src/net/mdns_discovery.{h,cpp}.
//
// Browses for the AgentDeck daemon over mDNS (_agentdeck._tcp) and returns its
// ip/port/token from the TXT records. Polled cooperatively from the dashboard
// activity loop (no network task).
//
#include <cstdint>

namespace AgentDeck {
namespace Net {

struct BridgeInfo {
  char ip[16];
  uint16_t port;
  char token[40];
  char project[40];
  char agent[16];
  bool found;
};

// Start the mDNS responder + begin browsing. Returns false if responder failed.
bool mdnsInit();

// Non-blocking poll. Returns true (and fills out) when a bridge is discovered.
bool mdnsPoll(BridgeInfo& out);

// Force the next poll to query immediately.
void mdnsRefresh();

}  // namespace Net
}  // namespace AgentDeck
