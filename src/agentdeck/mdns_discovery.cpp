#include "mdns_discovery.h"

#include <ESPmDNS.h>
#include <esp_idf_version.h>

#include "agent/AgentLog.h"
#include "agentdeck_config.h"

namespace AgentDeck {
namespace Net {

namespace {
BridgeInfo discovered;
bool hasNew = false;
uint32_t lastQueryMs = 0;
}  // namespace

bool mdnsInit() {
  if (!MDNS.begin("agentdeck-x3")) {
    AgentLog::line("MDNS", "responder failed to start");
    return false;
  }
  AgentLog::line("MDNS", "browsing for %s%s", AgentDeckCfg::MDNS_SERVICE, AgentDeckCfg::MDNS_PROTO);
  memset(&discovered, 0, sizeof(discovered));
  return true;
}

bool mdnsPoll(BridgeInfo& out) {
  uint32_t now = millis();
  if (now - lastQueryMs < AgentDeckCfg::MDNS_QUERY_INTERVAL_MS) {
    if (hasNew) {
      out = discovered;
      hasNew = false;
      return true;
    }
    return false;
  }
  lastQueryMs = now;

  int n = MDNS.queryService(AgentDeckCfg::MDNS_SERVICE, AgentDeckCfg::MDNS_PROTO);
  if (n <= 0) return false;

  // Selection priority (robust to a dual-homed host + a duplicate fallback-port
  // daemon, both of which the user's network exhibits):
  //   1. agent=daemon on the CANONICAL port (9120). A daemon on a fallback port
  //      (9121+) is usually a duplicate that failed to demote to client — prefer
  //      the real one and ignore the duplicate so we don't flap between them.
  //   2. any agent=daemon.
  //   3. first service with a port.
  int daemonIdx = -1, daemonCanonicalIdx = -1, firstIdx = -1;
  for (int i = 0; i < n; i++) {
    const uint16_t port = MDNS.port(i);
    if (port == 0) continue;
    if (firstIdx < 0) firstIdx = i;
    bool isDaemon = false;
    const int keys = MDNS.numTxt(i);
    for (int k = 0; k < keys; k++) {
      if (MDNS.txtKey(i, k) == "agent" && MDNS.txt(i, k) == "daemon") {
        isDaemon = true;
        break;
      }
    }
    if (isDaemon) {
      if (daemonIdx < 0) daemonIdx = i;
      if (port == AgentDeckCfg::BRIDGE_DEFAULT_PORT && daemonCanonicalIdx < 0) daemonCanonicalIdx = i;
    }
  }

  int selected = (daemonCanonicalIdx >= 0) ? daemonCanonicalIdx : (daemonIdx >= 0 ? daemonIdx : firstIdx);
  if (selected < 0) return false;

  discovered.port = MDNS.port(selected);
  discovered.found = true;
  discovered.token[0] = '\0';
  discovered.project[0] = '\0';
  discovered.agent[0] = '\0';

  // Parse TXT, including the canonical `ip` the daemon publishes (its single
  // default-route address).
  char txtIp[16] = {0};
  const int numKeys = MDNS.numTxt(selected);
  for (int k = 0; k < numKeys; k++) {
    String key = MDNS.txtKey(selected, k);
    String val = MDNS.txt(selected, k);
    if (key == "token") {
      strncpy(discovered.token, val.c_str(), sizeof(discovered.token) - 1);
      discovered.token[sizeof(discovered.token) - 1] = '\0';
    } else if (key == "project") {
      strncpy(discovered.project, val.c_str(), sizeof(discovered.project) - 1);
      discovered.project[sizeof(discovered.project) - 1] = '\0';
    } else if (key == "agent") {
      strncpy(discovered.agent, val.c_str(), sizeof(discovered.agent) - 1);
      discovered.agent[sizeof(discovered.agent) - 1] = '\0';
    } else if (key == "ip") {
      strncpy(txtIp, val.c_str(), sizeof(txtIp) - 1);
      txtIp[sizeof(txtIp) - 1] = '\0';
    }
  }

  // Prefer the TXT canonical IP over the host A-record: on a dual-homed daemon the
  // hostname resolves to BOTH interface IPs (.60 and .100), which makes the device
  // flip between them every reconnect. The TXT `ip=` is the single default-route addr.
  if (txtIp[0]) {
    strncpy(discovered.ip, txtIp, sizeof(discovered.ip) - 1);
    discovered.ip[sizeof(discovered.ip) - 1] = '\0';
  } else {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    IPAddress ip = MDNS.address(selected);  // ESP-IDF 5.x (pioarduino / Arduino v3)
#else
    IPAddress ip = MDNS.IP(selected);
#endif
    snprintf(discovered.ip, sizeof(discovered.ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  }

  AgentLog::line("MDNS", "found bridge %s:%u agent=%s project=%s", discovered.ip,
                 (unsigned)discovered.port, discovered.agent, discovered.project);
  hasNew = true;
  out = discovered;
  return true;
}

void mdnsRefresh() { lastQueryMs = 0; }

}  // namespace Net
}  // namespace AgentDeck
