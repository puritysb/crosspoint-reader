#include "udp_discovery.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "agent/AgentLog.h"
#include "agentdeck_config.h"

namespace AgentDeck {
namespace Net {

namespace {
constexpr size_t UDP_BEACON_MAX_BYTES = 512;  // daemon payload is ~120 B; cap is DoS guard
WiFiUDP udp;
bool udpBound = false;

// Reject beacons whose stated daemon IP is on a different subnet than ours.
// Same /24 check as the daemon's broadcaster. Without this a laptop tethered
// to the phone hotspot (192.168.43.x) would happily advertise to a device on
// the home WiFi (192.168.68.x) and the device would try to connect across
// subnets, masking the real daemon on the same segment.
bool sameSubnet(const char* a, const char* b) {
  // Compare first three octets. Cheap and matches the daemon's /24 broadcaster.
  int a0, a1, a2, a3, b0, b1, b2, b3;
  if (sscanf(a, "%d.%d.%d.%d", &a0, &a1, &a2, &a3) != 4) return true;  // lenient on parse fail
  if (sscanf(b, "%d.%d.%d.%d", &b0, &b1, &b2, &b3) != 4) return true;
  return a0 == b0 && a1 == b1 && a2 == b2;
}
}  // namespace

bool udpInit() {
  if (udpBound) return true;
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (!udp.begin(AgentDeckCfg::UDP_DISCOVERY_PORT)) {
    AgentLog::line("UDP", "begin failed on port %u", AgentDeckCfg::UDP_DISCOVERY_PORT);
    return false;
  }
  udpBound = true;
  AgentLog::line("UDP", "listening on port %u", AgentDeckCfg::UDP_DISCOVERY_PORT);
  return true;
}

bool udpPoll(BridgeInfo& out) {
  if (!udpBound) return false;

  // Drain any pending beacons. We only need the latest one — older beacons
  // carry the same (or older) data, so skip to the end of the queue.
  const int firstLen = udp.parsePacket();
  if (firstLen <= 0) return false;

  char buf[UDP_BEACON_MAX_BYTES] = {0};
  int lastRead = 0;
  int parsedLen = firstLen;

  // Loop until no more packets; keep only the most recent contents.
  while (true) {
    if (parsedLen > static_cast<int>(sizeof(buf) - 1)) {
      // Oversized — read & discard. WiFiUDP only reads from a single datagram,
      // so a partial read here drops the remainder and parsePacket() below
      // advances to the next packet cleanly.
      char discard[64];
      while (udp.available() > 0) {
        udp.read(discard, sizeof(discard));
      }
    } else {
      lastRead = udp.read(buf, sizeof(buf) - 1);
      if (lastRead > 0) buf[lastRead] = '\0';
    }
    const int next = udp.parsePacket();
    if (next <= 0) break;
    parsedLen = next;
  }

  if (lastRead <= 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    return false;
  }

  const int v = doc["v"] | 0;
  if (v != 1) return false;

  const char* ip = doc["ip"];
  const int port = doc["port"] | 0;
  if (!ip || port == 0) return false;

  // Subnet guard — a stray beacon from a different subnet (e.g. phone
  // hotspot daemon) shouldn't shadow the on-LAN daemon the user actually
  // wants. mDNS doesn't have this issue because the multicast query is
  // scoped to the local segment already.
  const IPAddress local = WiFi.localIP();
  char localIp[16];
  snprintf(localIp, sizeof(localIp), "%u.%u.%u.%u", local[0], local[1], local[2], local[3]);
  if (!sameSubnet(ip, localIp)) {
    return false;
  }

  // Prefer the remote (source) IP from the UDP header over the JSON body —
  // it's harder to spoof and matches what actually sent the packet. Fall
  // back to JSON `ip` only if the header is missing.
  const IPAddress remote = udp.remoteIP();
  memset(&out, 0, sizeof(out));
  const uint32_t remoteRaw = static_cast<uint32_t>(remote);
  if (remoteRaw != 0 && remoteRaw != 0xFFFFFFFFu) {
    snprintf(out.ip, sizeof(out.ip), "%u.%u.%u.%u", remote[0], remote[1], remote[2], remote[3]);
  } else {
    strncpy(out.ip, ip, sizeof(out.ip) - 1);
  }
  out.port = static_cast<uint16_t>(port);
  out.found = true;

  const char* project = doc["project"];
  if (project) strncpy(out.project, project, sizeof(out.project) - 1);

  const char* agent = doc["agent"];
  if (agent) strncpy(out.agent, agent, sizeof(out.agent) - 1);

  const char* token = doc["token"];
  if (token) strncpy(out.token, token, sizeof(out.token) - 1);

  AgentLog::line("UDP", "discovered daemon %s:%u (agent=%s project=%s)", out.ip, (unsigned)out.port, out.agent,
                 out.project);
  return true;
}

void udpStop() {
  if (udpBound) {
    udp.stop();
    udpBound = false;
  }
}

}  // namespace Net
}  // namespace AgentDeck
