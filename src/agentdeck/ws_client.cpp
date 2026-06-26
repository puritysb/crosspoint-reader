#include "ws_client.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "agent/AgentLog.h"
#include "agent_state.h"
#include "agentdeck_config.h"
#include "protocol.h"

namespace AgentDeck {
namespace Net {

namespace {

WebSocketsClient ws;
bool connected = false;
bool connecting = false;
uint32_t reconnectMs = AgentDeckCfg::WS_RECONNECT_MIN_MS;
uint32_t lastReconnectAttempt = 0;
char savedIp[16] = {0};
uint16_t savedPort = 0;
char savedToken[40] = {0};

// ── outbound queue (any task → loop task) ──
// links2004/WebSockets is not thread-safe; M3 button handlers enqueue here and
// the loop task drains via pumpOutbound().
constexpr int OUTBOX_MAX = 6;
constexpr int OUTBOX_LEN = 200;
char outbox[OUTBOX_MAX][OUTBOX_LEN];
int outboxHead = 0;
int outboxCount = 0;
SemaphoreHandle_t outboxMutex = nullptr;

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      AgentLog::line("WS", "disconnected");
      connected = false;
      connecting = false;
      lockState();
      g_state.markBridgeDisconnected();
      unlockState();
      break;

    case WStype_CONNECTED:
      AgentLog::line("WS", "connected to %s:%u", savedIp, (unsigned)savedPort);
      connected = true;
      connecting = false;
      reconnectMs = AgentDeckCfg::WS_RECONNECT_MIN_MS;
      ws.setReconnectInterval(reconnectMs);
      lockState();
      g_state.wsConnected = true;
      g_state.lastMessageMs = millis();
      unlockState();
      // Post-connect registration + initial queries are issued by the dashboard
      // activity (it owns the device identity: mac + screen dims).
      break;

    case WStype_TEXT:
      Protocol::parseMessage((const char*)payload, length);
      lockState();
      g_state.lastMessageMs = millis();
      unlockState();
      break;

    case WStype_ERROR:
      AgentLog::line("WS", "error");
      break;

    case WStype_PING:
    case WStype_PONG:
    default:
      break;
  }
}

}  // namespace

void wsInit() {
  ensureStateMutex();
  if (!outboxMutex) outboxMutex = xSemaphoreCreateMutex();
}

void queueOutbound(const char* json) {
  if (!json || !json[0] || !outboxMutex) return;
  xSemaphoreTake(outboxMutex, portMAX_DELAY);
  if (outboxCount < OUTBOX_MAX) {
    int idx = (outboxHead + outboxCount) % OUTBOX_MAX;
    strncpy(outbox[idx], json, OUTBOX_LEN - 1);
    outbox[idx][OUTBOX_LEN - 1] = '\0';
    outboxCount++;
  }
  xSemaphoreGive(outboxMutex);
}

void pumpOutbound() {
  if (!outboxMutex) return;
  while (true) {
    char line[OUTBOX_LEN];
    xSemaphoreTake(outboxMutex, portMAX_DELAY);
    if (outboxCount == 0) {
      xSemaphoreGive(outboxMutex);
      break;
    }
    strncpy(line, outbox[outboxHead], sizeof(line));
    line[sizeof(line) - 1] = '\0';
    outboxHead = (outboxHead + 1) % OUTBOX_MAX;
    outboxCount--;
    xSemaphoreGive(outboxMutex);
    if (connected) ws.sendTXT(line);
    // Not connected → drop (no serial bridge on this dead-USB unit).
  }
}

void wsConnect(const char* ip, uint16_t port, const char* token) {
  if (connected || connecting) return;
  connecting = true;

  ws.disconnect();
  delay(10);

  strncpy(savedIp, ip, sizeof(savedIp) - 1);
  savedIp[sizeof(savedIp) - 1] = '\0';
  savedPort = port;
  strncpy(savedToken, token ? token : "", sizeof(savedToken) - 1);
  savedToken[sizeof(savedToken) - 1] = '\0';

  char path[80];
  if (savedToken[0] != '\0')
    snprintf(path, sizeof(path), "/?token=%s", savedToken);
  else
    strcpy(path, "/");

  ws.begin(ip, port, path);
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(reconnectMs);
  ws.enableHeartbeat(AgentDeckCfg::WS_PING_INTERVAL_MS, AgentDeckCfg::WS_PONG_TIMEOUT_MS, 2);

  AgentLog::line("WS", "connecting to %s:%u", ip, (unsigned)port);
}

void wsDisconnect() {
  ws.disconnect();
  connected = false;
  connecting = false;
  savedIp[0] = '\0';  // stop the backoff reconnect loop
}

void wsLoop() {
  ws.loop();

  // Exponential backoff. The library's internal reconnect timer is driven by
  // setReconnectInterval(); we push updated values as our backoff grows.
  if (!connected && savedIp[0] != '\0') {
    uint32_t now = millis();
    if (now - lastReconnectAttempt > reconnectMs) {
      lastReconnectAttempt = now;
      uint32_t next = reconnectMs * 2;
      if (next > AgentDeckCfg::WS_RECONNECT_MAX_MS) next = AgentDeckCfg::WS_RECONNECT_MAX_MS;
      reconnectMs = next;
      ws.setReconnectInterval(reconnectMs);
    }
  }
}

bool wsConnected() { return connected; }
bool wsConnecting() { return connecting; }

void wsSend(const char* json) {
  if (connected && json && json[0]) ws.sendTXT(json);
}

const char* wsBridgeIp() { return savedIp; }
uint16_t wsBridgePort() { return savedPort; }

}  // namespace Net
}  // namespace AgentDeck
