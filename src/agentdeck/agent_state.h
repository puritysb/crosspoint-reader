#pragma once
//
// agent_state.h — TRIMMED port of AgentDeck esp32/src/state/agent_state.h.
//
// Keeps only the networking + agent fields the M2 (display-only) layer needs:
//   • AgentState enum
//   • PromptOption + SessionInfo
//   • a minimal DashboardState (connection / agent / usage / prompt / sessions)
//
// DROPPED from the original (LVGL/board/terrarium concerns, not needed here):
//   • CreatureState / CrayfishState / TetraState enums + derivation
//   • Gateway (OpenClaw) topology fields
//   • TimelineEntry ring buffer
//   • host display_state / dim / orientation fields
//   • HUD view-state flags
//
// g_state is read on the render task and written on the main (loop) task, so it
// is still guarded by g_stateMutex exactly like the original.
//
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "agentdeck_config.h"

namespace AgentDeck {

// ===== Agent state enum =====
// Wire strings: disconnected|idle|processing|awaiting_permission|awaiting_option|awaiting_diff
enum class AgentState : uint8_t {
  DISCONNECTED = 0,
  IDLE,
  PROCESSING,
  AWAITING_PERMISSION,
  AWAITING_OPTION,
  AWAITING_DIFF
};

// ===== Prompt option =====
struct PromptOption {
  char label[80];
  char action[40];
  uint8_t index;
  bool recommended;
  bool selected;
};

// ===== Session info (multi-agent) =====
struct SessionInfo {
  // Daemon session ids are UUIDs (36 ch) and can be prefixed ("observed:claude:<uuid>"
  // ≈ 52 ch). The WebSocket path does NOT truncate ids (unlike the serial path), so
  // size to hold the longest form — a short buffer mis-routes approve/deny.
  char id[64];
  char projectName[40];
  char modelName[32];
  char agentType[16];   // "claude-code" / "openclaw" / "codex-cli" / "codex-app" / "opencode"
  char state[20];
  uint16_t port;
  bool alive;
  // Per-session detail (tool/elapsed + awaiting prompt). Populated from the
  // enriched sessions_list; M3 uses requestId to drive approve/deny.
  char currentTool[40];
  uint32_t elapsedSec;
  char question[160];
  char promptType[20];
  char requestId[40];   // gated PreToolUse request id → reply permission_decision (M3)
};

// ===== Main dashboard state (trimmed) =====
struct DashboardState {
  // Connection
  bool wsConnected;
  char bridgeIp[16];
  uint16_t bridgePort;
  char authToken[40];
  uint32_t lastMessageMs;   // millis() of last JSON received; 0 = never

  // Agent (from state_update)
  AgentState state;
  char projectName[40];
  char modelName[32];
  char agentType[16];
  char effortLevel[8];
  char sessionId[64];        // focused session id (M3 routing) — full/prefixed UUID
  char focusedSessionId[64];
  char requestId[40];        // gated request id for the focused awaiting prompt (M3)
  bool navigable;
  int cursorIndex;

  // Current tool (processing indicator)
  char currentTool[40];
  char toolInput[80];

  // Permission / Options
  char question[200];
  char promptType[20];
  PromptOption options[8];
  uint8_t optionCount;

  // Usage (from usage_update)
  float fiveHourPercent;     // 0-100, -1 = no data
  float sevenDayPercent;     // 0-100, -1 = no data
  char fiveHourReset[20];
  char sevenDayReset[20];
  uint32_t inputTokens;
  uint32_t outputTokens;
  uint32_t toolCalls;
  uint32_t sessionDurationSec;
  float estimatedCostUsd;
  bool usageStale;

  // Sessions (multi-agent). Cap matches AgentDeckCfg::SESSIONS_CAP.
  SessionInfo sessions[AgentDeckCfg::SESSIONS_CAP];
  uint8_t sessionCount;

  // Data reception tracking
  bool dataReceived;         // true after first state_update / sessions_list

  void reset() {
    memset(this, 0, sizeof(DashboardState));
    state = AgentState::DISCONNECTED;
    navigable = false;
    cursorIndex = 0;
    // Sentinel -1.0f = "no data" (0 is a valid usage value)
    fiveHourPercent = -1.0f;
    sevenDayPercent = -1.0f;
    estimatedCostUsd = -1.0f;
  }

  // Called while g_stateMutex is held. Clears volatile bridge data so every
  // surface renders a disconnected state instead of stale session data.
  void markBridgeDisconnected() {
    wsConnected = false;
    state = AgentState::DISCONNECTED;
    projectName[0] = '\0';
    modelName[0] = '\0';
    agentType[0] = '\0';
    effortLevel[0] = '\0';
    sessionId[0] = '\0';
    focusedSessionId[0] = '\0';
    requestId[0] = '\0';
    navigable = false;
    cursorIndex = 0;
    currentTool[0] = '\0';
    toolInput[0] = '\0';
    question[0] = '\0';
    promptType[0] = '\0';
    optionCount = 0;
    sessionCount = 0;
    fiveHourPercent = -1.0f;
    sevenDayPercent = -1.0f;
    fiveHourReset[0] = '\0';
    sevenDayReset[0] = '\0';
    usageStale = true;
    dataReceived = false;
  }
};

// Global state — accessed from main (loop) + render task (use mutex)
extern DashboardState g_state;
extern SemaphoreHandle_t g_stateMutex;

// Lazily create the mutex on first use so call order never matters.
inline void ensureStateMutex() {
  if (!g_stateMutex) g_stateMutex = xSemaphoreCreateMutex();
}
inline void lockState() {
  if (g_stateMutex) xSemaphoreTake(g_stateMutex, portMAX_DELAY);
}
inline void unlockState() {
  if (g_stateMutex) xSemaphoreGive(g_stateMutex);
}

}  // namespace AgentDeck
