#include "protocol.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <algorithm>

#include "agent/AgentLog.h"
#include "agent_state.h"
#include "agentdeck_config.h"

namespace AgentDeck {

// ── Global state definitions (declared extern in agent_state.h) ──
// Written on the main/loop task (parseMessage), read on the render task.
DashboardState g_state;
SemaphoreHandle_t g_stateMutex = nullptr;

namespace {

AgentState parseState(const char* s) {
  if (!s) return AgentState::DISCONNECTED;
  if (strcmp(s, "idle") == 0) return AgentState::IDLE;
  if (strcmp(s, "processing") == 0) return AgentState::PROCESSING;
  if (strcmp(s, "awaiting_permission") == 0) return AgentState::AWAITING_PERMISSION;
  if (strcmp(s, "awaiting_option") == 0) return AgentState::AWAITING_OPTION;
  if (strcmp(s, "awaiting_diff") == 0) return AgentState::AWAITING_DIFF;
  return AgentState::DISCONNECTED;
}

// Reusable JSON document — sized elastically by ArduinoJson 7.
JsonDocument doc;

void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void handleStateUpdate(JsonObject& obj) {
  lockState();

  g_state.state = parseState(obj["state"].as<const char*>());

  // state_update is a full snapshot: write every field unconditionally (the
  // `| ""` default clears it when the key is absent) so a previous prompt's
  // sessionId / promptType / etc. can never carry over into the next one.
  copyStr(g_state.projectName, sizeof(g_state.projectName), obj["projectName"] | "");
  copyStr(g_state.modelName, sizeof(g_state.modelName), obj["modelName"] | "");
  copyStr(g_state.agentType, sizeof(g_state.agentType), obj["agentType"] | "");
  copyStr(g_state.effortLevel, sizeof(g_state.effortLevel), obj["effortLevel"] | "");

  // Session routing ids (M3 uses these for approve/deny targeting).
  copyStr(g_state.sessionId, sizeof(g_state.sessionId), obj["sessionId"] | "");
  copyStr(g_state.focusedSessionId, sizeof(g_state.focusedSessionId), obj["focusedSessionId"] | "");
  if (obj["requestId"].is<const char*>())
    copyStr(g_state.requestId, sizeof(g_state.requestId), obj["requestId"].as<const char*>());
  else
    g_state.requestId[0] = '\0';
  g_state.navigable = obj["navigable"] | false;
  g_state.cursorIndex = obj["cursorIndex"] | 0;

  // Current tool
  if (obj["currentTool"].is<const char*>())
    copyStr(g_state.currentTool, sizeof(g_state.currentTool), obj["currentTool"].as<const char*>());
  else
    g_state.currentTool[0] = '\0';
  if (obj["toolInput"].is<const char*>())
    copyStr(g_state.toolInput, sizeof(g_state.toolInput), obj["toolInput"].as<const char*>());
  else
    g_state.toolInput[0] = '\0';

  // Permission / options
  if (obj["question"].is<const char*>())
    copyStr(g_state.question, sizeof(g_state.question), obj["question"].as<const char*>());
  else
    g_state.question[0] = '\0';
  copyStr(g_state.promptType, sizeof(g_state.promptType), obj["promptType"] | "");

  if (obj["options"].is<JsonArray>()) {
    JsonArray opts = obj["options"].as<JsonArray>();
    g_state.optionCount = (uint8_t)std::min((int)opts.size(), 8);
    for (uint8_t i = 0; i < g_state.optionCount; i++) {
      JsonObject o = opts[i].as<JsonObject>();
      copyStr(g_state.options[i].label, sizeof(g_state.options[i].label), o["label"] | "");
      g_state.options[i].index = o["index"] | i;
      g_state.options[i].recommended = o["recommended"] | false;
      g_state.options[i].selected = o["selected"] | false;
      if (o["shortcut"].is<const char*>())
        copyStr(g_state.options[i].action, sizeof(g_state.options[i].action), o["shortcut"].as<const char*>());
      else
        g_state.options[i].action[0] = '\0';
    }
  } else {
    g_state.optionCount = 0;
  }

  g_state.dataReceived = true;
  unlockState();
}

void handleUsageUpdate(JsonObject& obj) {
  lockState();
  g_state.dataReceived = true;

  // -1.0f sentinel for "no data" (0 is a valid value).
  g_state.fiveHourPercent =
      obj["fiveHourPercent"].is<float>() ? obj["fiveHourPercent"].as<float>() : -1.0f;
  g_state.sevenDayPercent =
      obj["sevenDayPercent"].is<float>() ? obj["sevenDayPercent"].as<float>() : -1.0f;

  g_state.inputTokens = obj["inputTokens"] | g_state.inputTokens;
  g_state.outputTokens = obj["outputTokens"] | g_state.outputTokens;
  g_state.toolCalls = obj["toolCalls"] | g_state.toolCalls;
  g_state.sessionDurationSec = obj["sessionDurationSec"] | g_state.sessionDurationSec;
  g_state.estimatedCostUsd =
      obj["estimatedCostUsd"].is<float>() ? obj["estimatedCostUsd"].as<float>() : -1.0f;
  g_state.usageStale = obj["usageStale"] | false;

  // Reset times: only the already-formatted "Xh Ym" form is kept. The original
  // also parsed ISO-8601 + computed relative time via NTP; that path is dropped
  // for M2 (no NTP brought up here). ISO values are stored verbatim so a future
  // milestone can format them.
  auto storeReset = [&](const char* key, char* out, size_t cap) {
    if (obj[key].is<const char*>())
      copyStr(out, cap, obj[key].as<const char*>());
    else
      out[0] = '\0';
  };
  storeReset("fiveHourResetsAt", g_state.fiveHourReset, sizeof(g_state.fiveHourReset));
  storeReset("sevenDayResetsAt", g_state.sevenDayReset, sizeof(g_state.sevenDayReset));

  // Other-agent subscription/limit summary (best-effort — only some hubs send it).
  storeReset("codexPlanType", g_state.codexPlan, sizeof(g_state.codexPlan));
  storeReset("codexSubscriptionActiveUntil", g_state.codexActiveUntil, sizeof(g_state.codexActiveUntil));
  g_state.antigravityPlan[0] = '\0';
  g_state.antigravityCredits = -1.0f;
  if (obj["antigravityStatus"].is<JsonObject>()) {
    JsonObject ag = obj["antigravityStatus"].as<JsonObject>();
    if (ag["planName"].is<const char*>())
      copyStr(g_state.antigravityPlan, sizeof(g_state.antigravityPlan), ag["planName"].as<const char*>());
    // availableCredits arrives as an integer credit count.
    if (ag["availableCredits"].is<int>() || ag["availableCredits"].is<float>())
      g_state.antigravityCredits = ag["availableCredits"].as<float>();
  }

  // Codex rate-limit windows: primary ≈ 5h, secondary ≈ 7d (usedPercent + resetsAt).
  g_state.codexFivePercent = -1.0f;
  g_state.codexSevenPercent = -1.0f;
  g_state.codexFiveReset[0] = '\0';
  g_state.codexSevenReset[0] = '\0';
  if (obj["codexRateLimits"].is<JsonObject>()) {
    JsonObject cx = obj["codexRateLimits"].as<JsonObject>();
    auto window = [&](const char* key, float& pct, char* reset, size_t cap) {
      if (!cx[key].is<JsonObject>()) return;
      JsonObject wnd = cx[key].as<JsonObject>();
      if (wnd["usedPercent"].is<int>() || wnd["usedPercent"].is<float>()) pct = wnd["usedPercent"].as<float>();
      if (wnd["resetsAt"].is<const char*>()) copyStr(reset, cap, wnd["resetsAt"].as<const char*>());
    };
    window("primary", g_state.codexFivePercent, g_state.codexFiveReset, sizeof(g_state.codexFiveReset));
    window("secondary", g_state.codexSevenPercent, g_state.codexSevenReset, sizeof(g_state.codexSevenReset));
    // If codexPlanType wasn't sent at top-level, the windows object may carry it.
    if (g_state.codexPlan[0] == '\0' && cx["planType"].is<const char*>())
      copyStr(g_state.codexPlan, sizeof(g_state.codexPlan), cx["planType"].as<const char*>());
  }

  unlockState();
}

void handleSessionsList(JsonObject& obj) {
  lockState();
  g_state.dataReceived = true;

  JsonArray sessions = obj["sessions"].as<JsonArray>();
  g_state.sessionCount = (uint8_t)std::min((int)sessions.size(), AgentDeckCfg::SESSIONS_CAP);

  for (uint8_t i = 0; i < g_state.sessionCount; i++) {
    JsonObject s = sessions[i].as<JsonObject>();
    SessionInfo& si = g_state.sessions[i];
    copyStr(si.id, sizeof(si.id), s["id"] | "");
    copyStr(si.projectName, sizeof(si.projectName), s["projectName"] | "");
    copyStr(si.modelName, sizeof(si.modelName), s["modelName"] | "");
    copyStr(si.agentType, sizeof(si.agentType), s["agentType"] | "");
    copyStr(si.state, sizeof(si.state), s["state"] | "");
    si.port = s["port"] | 0;
    si.alive = s["alive"] | false;
    copyStr(si.currentTool, sizeof(si.currentTool), s["currentTool"] | "");
    si.elapsedSec = s["elapsedSec"] | 0;
    copyStr(si.question, sizeof(si.question), s["question"] | "");
    copyStr(si.promptType, sizeof(si.promptType), s["promptType"] | "");
    copyStr(si.requestId, sizeof(si.requestId), s["requestId"] | "");
    // Daemon-synthesized activity one-liner; fall back to currentTool / goal.
    const char* act = s["activity"] | (s["currentTask"] | (s["goal"] | ""));
    copyStr(si.activity, sizeof(si.activity), act);
    if (si.activity[0] == '\0') copyStr(si.activity, sizeof(si.activity), si.currentTool);
  }

  unlockState();
}

// Live, forward-only timeline ring (per-session Detail view). Appends entry.raw
// keyed by sessionId; the ring overwrites oldest when full.
void handleTimelineEvent(JsonObject& obj) {
  JsonObject e = obj["entry"].as<JsonObject>();
  if (e.isNull()) return;
  const char* sid = e["sessionId"] | "";
  if (sid[0] == '\0') return;  // unattributed → nothing to show in a session detail
  const char* raw = e["raw"] | "";
  const char* etype = e["type"] | "";
  if (raw[0] == '\0' && etype[0] == '\0') return;

  lockState();
  TimelineItem& it = g_state.timeline[g_state.timelineHead];
  copyStr(it.sid, sizeof(it.sid), sid);
  copyStr(it.text, sizeof(it.text), raw);
  copyStr(it.type, sizeof(it.type), etype);
  g_state.timelineHead = (g_state.timelineHead + 1) % DashboardState::TIMELINE_CAP;
  if (g_state.timelineCount < DashboardState::TIMELINE_CAP) g_state.timelineCount++;
  unlockState();
}

// Reply to query_session_timeline: a batch of recent entries for one session
// (oldest→newest). Loads them into the ring so Detail fills on connect; the ring
// is bounded so the most recent TIMELINE_CAP entries win.
void handleTimelineHistory(JsonObject& obj) {
  JsonArray entries = obj["entries"].as<JsonArray>();
  if (entries.isNull()) return;
  lockState();
  for (JsonObject e : entries) {
    const char* sid = e["sessionId"] | "";
    const char* raw = e["raw"] | "";
    const char* etype = e["type"] | "";
    if (sid[0] == '\0') continue;
    if (raw[0] == '\0' && etype[0] == '\0') continue;
    TimelineItem& it = g_state.timeline[g_state.timelineHead];
    copyStr(it.sid, sizeof(it.sid), sid);
    copyStr(it.text, sizeof(it.text), raw);
    copyStr(it.type, sizeof(it.type), etype);
    g_state.timelineHead = (g_state.timelineHead + 1) % DashboardState::TIMELINE_CAP;
    if (g_state.timelineCount < DashboardState::TIMELINE_CAP) g_state.timelineCount++;
  }
  unlockState();
}

}  // namespace

namespace Protocol {

void parseMessage(const char* json, size_t length) {
  // Reject oversized frames before feeding the elastic JsonDocument — an
  // unbounded sessions_list would otherwise grow the doc until it
  // fragments/exhausts the heap on this no-PSRAM C3.
  if (length > AgentDeckCfg::PROTOCOL_MAX_MSG_BYTES) {
    AgentLog::line("PROTO", "frame too large: %u bytes (max %u) — dropped",
                   (unsigned)length, (unsigned)AgentDeckCfg::PROTOCOL_MAX_MSG_BYTES);
    return;
  }

  doc.clear();
  DeserializationError err = deserializeJson(doc, json, length);
  if (err) {
    AgentLog::line("PROTO", "JSON error: %s", err.c_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  const char* type = obj["type"] | "";

  if (strcmp(type, "state_update") == 0) {
    handleStateUpdate(obj);
  } else if (strcmp(type, "sessions_list") == 0) {
    handleSessionsList(obj);
  } else if (strcmp(type, "usage_update") == 0) {
    handleUsageUpdate(obj);
  } else if (strcmp(type, "timeline_event") == 0) {
    handleTimelineEvent(obj);
  } else if (strcmp(type, "timeline_history") == 0) {
    handleTimelineHistory(obj);
  } else if (strcmp(type, "connection") == 0 || strcmp(type, "connected") == 0) {
    // Connection ack — actual connect/disconnect is tracked by the WS event
    // callbacks. Logged for diagnostics.
    AgentLog::line("PROTO", "connection ack: %s", type);
  } else {
    // Accepted-and-ignored for M2. The original firmware also handled:
    //   device_info_request, display_state, set_orientation, wifi_provision,
    //   timeline_event, timeline_history, touch_diag
    // Those are board/host concerns not needed for a display-only X3 and are
    // deliberately stubbed out here. TODO(M3): wire device_info_request reply
    // if the daemon starts gating on it.
  }
}

}  // namespace Protocol
}  // namespace AgentDeck
