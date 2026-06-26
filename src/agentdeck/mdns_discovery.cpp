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

  // Prefer the daemon bridge (aggregates all sessions) over a session bridge.
  int daemonIdx = -1;
  int firstIdx = -1;
  for (int i = 0; i < n; i++) {
    if (MDNS.port(i) == 0) continue;
    if (firstIdx < 0) firstIdx = i;
    int numKeys = MDNS.numTxt(i);
    for (int k = 0; k < numKeys; k++) {
      if (MDNS.txtKey(i, k) == "agent" && MDNS.txt(i, k) == "daemon") {
        daemonIdx = i;
        break;
      }
    }
    if (daemonIdx >= 0) break;
  }

  int selected = (daemonIdx >= 0) ? daemonIdx : firstIdx;
  if (selected < 0) return false;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  IPAddress ip = MDNS.address(selected);  // ESP-IDF 5.x (pioarduino / Arduino v3)
#else
  IPAddress ip = MDNS.IP(selected);
#endif
  snprintf(discovered.ip, sizeof(discovered.ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  discovered.port = MDNS.port(selected);
  discovered.found = true;
  discovered.token[0] = '\0';
  discovered.project[0] = '\0';
  discovered.agent[0] = '\0';

  int numKeys = MDNS.numTxt(selected);
  for (int k = 0; k < numKeys; k++) {
    String key = MDNS.txtKey(selected, k);
    String val = MDNS.txt(selected, k);
    if (key == "token") {
      strncpy(discovered.token, val.c_str(), sizeof(discovered.token) - 1);
    } else if (key == "project") {
      strncpy(discovered.project, val.c_str(), sizeof(discovered.project) - 1);
    } else if (key == "agent") {
      strncpy(discovered.agent, val.c_str(), sizeof(discovered.agent) - 1);
    }
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
