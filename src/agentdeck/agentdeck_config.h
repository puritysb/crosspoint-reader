#pragma once
//
// agentdeck_config.h — minimal config shim for the ported AgentDeck networking.
//
// Replaces AgentDeck's esp32/src/config.h for this fork. The original config.h
// carried a large amount of board/terrarium/timeline tuning that the trimmed M2
// networking layer does not need. This header keeps ONLY the constants the
// ported net/ files reference, so the AgentDeck source ports with rewritten
// include paths and nothing else.
//
// Board: esp32-c3-devkitm-1 (no PSRAM) — uses the tight inbound-frame cap.
//
#include <cstddef>
#include <cstdint>

namespace AgentDeckCfg {

// ===== Network / mDNS =====
constexpr uint16_t BRIDGE_DEFAULT_PORT = 9120;
constexpr uint16_t BRIDGE_PORT_MAX = 9139;
constexpr const char* MDNS_SERVICE = "_agentdeck";
constexpr const char* MDNS_PROTO = "_tcp";
constexpr const char* FIRMWARE_VERSION = "0.1.1";
constexpr uint8_t PROTOCOL_REVISION = 2;

// UDP broadcast discovery (mDNS fallback). The daemon's BroadcastModule sends a
// small JSON beacon here every 2 s; the device listens on the same port. mDNS
// multicast (224.0.0.251:5353) is frequently filtered by home routers — AP
// Isolation, IGMP Snooping without multicast enhancement, mesh hops — so this
// unicast-friendly subnet broadcast carries the same discovery payload as a
// safety net. See AgentDeck/bridge/src/broadcast.ts.
constexpr uint16_t UDP_DISCOVERY_PORT = 9121;

// ===== WebSocket =====
constexpr uint32_t WS_RECONNECT_MIN_MS = 1000;
constexpr uint32_t WS_RECONNECT_MAX_MS = 8000;
constexpr uint32_t WS_PING_INTERVAL_MS = 15000;
constexpr uint32_t WS_PONG_TIMEOUT_MS = 30000;

// ===== mDNS discovery cadence =====
constexpr uint32_t MDNS_QUERY_INTERVAL_MS = 5000;

// Upper bound on an inbound bridge frame fed to the elastic ArduinoJson
// JsonDocument. A frame past this is dropped before parsing so a malformed or
// oversized sessions_list can't grow the doc until it fragments/exhausts the
// heap. The ESP32-C3 has no PSRAM, so use the tight no-PSRAM cap.
constexpr size_t PROTOCOL_MAX_MSG_BYTES = 8192;

// Session cap — matches the trimmed DashboardState::sessions[] size and the
// daemon's SERIAL_SESSIONS_CAP. Keep in sync with agent_state.h.
constexpr int SESSIONS_CAP = 10;

}  // namespace AgentDeckCfg
