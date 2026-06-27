#include "AgentDashboardActivity.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "agent/AgentLog.h"
#include "agentdeck/agent_commands.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/mdns_discovery.h"
#include "agentdeck/ws_client.h"
#include "components/UITheme.h"  // GUI (theme) + ThemeMetrics + Rect
#include "fontIds.h"

namespace {
using AgentDeck::AgentState;

// FNV-1a over a byte range — cheap change-detection signature.
inline uint32_t fnvUpdate(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

const char* agentStateLabel(AgentState s) {
  switch (s) {
    case AgentState::IDLE: return "Idle";
    case AgentState::PROCESSING: return "Working";
    case AgentState::AWAITING_PERMISSION: return "Awaiting permission";
    case AgentState::AWAITING_OPTION: return "Choosing option";
    case AgentState::AWAITING_DIFF: return "Reviewing diff";
    case AgentState::DISCONNECTED:
    default: return "Disconnected";
  }
}
}  // namespace

void AgentDashboardActivity::onEnter() {
  Activity::onEnter();

  dashState = DashState::WifiSelection;
  localIp.clear();
  exitRequested = false;
  registered = false;
  lastSignature = 0;

  // Bring the networking module up from a clean slate.
  AgentDeck::ensureStateMutex();
  AgentDeck::lockState();
  AgentDeck::g_state.reset();
  AgentDeck::unlockState();
  AgentDeck::Net::wsInit();

  AgentLog::line("AGENT", "AgentDashboardActivity onEnter (M2 network)");
  requestUpdate();

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               localIp = wifi.ip;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    localIp = WiFi.localIP().toString().c_str();
    onWifiSelectionComplete(true);
  }
}

void AgentDashboardActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    AgentLog::line("AGENT", "wifi selection cancelled — exiting");
    finish();
    return;
  }
  if (localIp.empty()) localIp = WiFi.localIP().toString().c_str();
  AgentLog::line("AGENT", "wifi up: %s", localIp.c_str());
  startNetworking();
}

void AgentDashboardActivity::startNetworking() {
  AgentDeck::Net::mdnsInit();
  dashState = DashState::Discovering;
  requestUpdate();
}

void AgentDashboardActivity::sendClientRegister() {
  // {"type":"client_register","clientType":"eink-device","clientLabel":"XTeink X3",
  //  "devices":[{"id":"<mac>","name":"XTeink X3","family":"eink","columns":W,"rows":H}]}
  String mac = WiFi.macAddress();
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"client_register\",\"clientType\":\"eink-device\","
                   "\"clientLabel\":\"XTeink X3\",\"devices\":[{\"id\":\"%s\","
                   "\"name\":\"XTeink X3\",\"family\":\"eink\",\"columns\":%d,\"rows\":%d}]}",
                   mac.c_str(), renderer.getScreenWidth(), renderer.getScreenHeight());
  if (n > 0 && (size_t)n < sizeof(buf)) {
    AgentDeck::Net::wsSend(buf);
    AgentLog::line("AGENT", "client_register sent (mac=%s)", mac.c_str());
  }
  // Ask for initial usage; the daemon pushes state_update/sessions_list on connect.
  AgentDeck::Net::wsSend("{\"type\":\"query_usage\"}");
}

uint32_t AgentDashboardActivity::computeStateSignature() const {
  uint32_t h = 2166136261u;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  uint8_t st = static_cast<uint8_t>(s.state);
  h = fnvUpdate(h, &st, sizeof(st));
  h = fnvUpdate(h, &s.wsConnected, sizeof(s.wsConnected));
  h = fnvUpdate(h, &s.dataReceived, sizeof(s.dataReceived));
  h = fnvUpdate(h, &s.sessionCount, sizeof(s.sessionCount));
  h = fnvUpdate(h, s.projectName, strlen(s.projectName));
  h = fnvUpdate(h, s.question, strlen(s.question));
  h = fnvUpdate(h, s.currentTool, strlen(s.currentTool));
  h = fnvUpdate(h, &s.optionCount, sizeof(s.optionCount));
  h = fnvUpdate(h, s.requestId, strlen(s.requestId));
  // Per-session state so the triage list repaints when any session's awaiting
  // status (or its prompt) changes, even if the focused state_update didn't.
  for (uint8_t i = 0; i < s.sessionCount && i < kAwaitingCap; i++) {
    h = fnvUpdate(h, s.sessions[i].id, strlen(s.sessions[i].id));
    h = fnvUpdate(h, s.sessions[i].state, strlen(s.sessions[i].state));
    h = fnvUpdate(h, s.sessions[i].requestId, strlen(s.sessions[i].requestId));
  }
  // Local cursors so triage/option navigation repaints.
  h = fnvUpdate(h, &triageIndex, sizeof(triageIndex));
  h = fnvUpdate(h, &optionCursor, sizeof(optionCursor));
  AgentDeck::unlockState();
  return h;
}

void AgentDashboardActivity::loop() {
  // Handle input FIRST so Back stays responsive: the discovery/connect steps
  // below can block (mDNS queryService ~1s, WS connect) and would otherwise
  // starve the button poll, making "go back" feel dead while not yet connected.
  handleButtons();
  if (exitRequested) {
    finish();
    return;
  }

  if (dashState == DashState::Discovering) {
    AgentDeck::Net::BridgeInfo bridge;
    if (AgentDeck::Net::mdnsPoll(bridge) && bridge.found) {
      AgentLog::line("AGENT", "daemon @ %s:%u (agent=%s) — connecting", bridge.ip, (unsigned)bridge.port,
                     bridge.agent);
      AgentDeck::Net::wsConnect(bridge.ip, bridge.port, bridge.token);
      dashState = DashState::Connecting;
      connectStartMs = millis();
      requestUpdate();
    }
  } else if (dashState == DashState::Connecting || dashState == DashState::Connected) {
    AgentDeck::Net::wsLoop();
    AgentDeck::Net::pumpOutbound();

    const bool nowConnected = AgentDeck::Net::wsConnected();
    if (nowConnected && dashState == DashState::Connecting) {
      dashState = DashState::Connected;
      lastConnectedMs = millis();
      if (!registered) {
        sendClientRegister();
        registered = true;
      }
      requestUpdate();
    } else if (!nowConnected && dashState == DashState::Connecting) {
      // The cached ip:port isn't accepting — most likely the daemon moved to a
      // different port (dynamic 9120→fallback). Don't sit on a stale endpoint:
      // after a grace window, re-resolve fresh via mDNS.
      if (millis() - connectStartMs > kConnectTimeoutMs) {
        AgentLog::line("AGENT", "connect timeout — re-resolving via mDNS");
        AgentDeck::Net::wsDisconnect();  // clears saved ip:port, stops stale auto-reconnect
        AgentDeck::Net::mdnsRefresh();   // force an immediate fresh query
        dashState = DashState::Discovering;
        registered = false;
        requestUpdate();
      }
    } else if (!nowConnected && dashState == DashState::Connected) {
      const uint32_t uptime = millis() - lastConnectedMs;
      registered = false;
      if (uptime >= kHealthyUptimeMs) {
        // Was a healthy connection that dropped (transient / daemon restart on the
        // same port): retry the SAME endpoint — the ws_client library auto-reconnects
        // to it. Avoids flapping across multiple daemons on the LAN. If it stays
        // unreachable, the connect-timeout above falls back to a fresh mDNS resolve.
        AgentLog::line("AGENT", "ws dropped after %ums — retrying same endpoint", (unsigned)uptime);
        dashState = DashState::Connecting;
        connectStartMs = millis();
      } else {
        // Endpoint accepted then dropped us quickly — a flaky/duplicate daemon.
        // Re-resolve to try a different advertiser instead of hammering this one.
        AgentLog::line("AGENT", "ws dropped after %ums — re-resolving (flaky endpoint)", (unsigned)uptime);
        AgentDeck::Net::wsDisconnect();
        AgentDeck::Net::mdnsRefresh();
        dashState = DashState::Discovering;
      }
      requestUpdate();
    }

    // Repaint only when the rendered state actually changed.
    if (dashState == DashState::Connected) {
      uint32_t sig = computeStateSignature();
      if (sig != lastSignature) {
        lastSignature = sig;
        requestUpdate();
      }
    }
  }
}

void AgentDashboardActivity::onExit() {
  Activity::onExit();

  AgentDeck::Net::wsDisconnect();
  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();  // defrag → Home (mirrors CalibreConnectActivity::onExit)
  }
}

int AgentDashboardActivity::collectAwaiting(AwaitingItem* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount && n < cap; i++) {
    const auto& se = s.sessions[i];
    if (strncmp(se.state, "awaiting", 8) != 0) continue;
    AwaitingItem& o = out[n++];
    cp(o.sid, sizeof(o.sid), se.id);
    cp(o.project, sizeof(o.project), se.projectName);
    cp(o.agentType, sizeof(o.agentType), se.agentType);
    cp(o.question, sizeof(o.question), se.question);
    cp(o.requestId, sizeof(o.requestId), se.requestId);
    cp(o.promptType, sizeof(o.promptType), se.promptType);
    // The rich options[] array belongs to the focused session in state_update,
    // so match against either id the daemon may carry.
    o.isFocused = (s.sessionId[0] != '\0' && strcmp(se.id, s.sessionId) == 0) ||
                  (s.focusedSessionId[0] != '\0' && strcmp(se.id, s.focusedSessionId) == 0);
    o.isOption = (strcmp(se.state, "awaiting_option") == 0) || (strcmp(se.promptType, "multi_select") == 0);
    o.optionCount = o.isFocused ? s.optionCount : 0;
  }
  // Observed/single-session fallback: sessions_list empty but the focused
  // state_update reports an awaiting prompt.
  if (n == 0 && cap > 0 &&
      (s.state == AgentState::AWAITING_PERMISSION || s.state == AgentState::AWAITING_OPTION ||
       s.state == AgentState::AWAITING_DIFF)) {
    AwaitingItem& o = out[n++];
    cp(o.sid, sizeof(o.sid), s.sessionId[0] ? s.sessionId : s.focusedSessionId);
    cp(o.project, sizeof(o.project), s.projectName);
    cp(o.agentType, sizeof(o.agentType), s.agentType);
    cp(o.question, sizeof(o.question), s.question);
    cp(o.requestId, sizeof(o.requestId), s.requestId);
    cp(o.promptType, sizeof(o.promptType), s.promptType);
    o.isFocused = true;
    o.isOption = (s.state == AgentState::AWAITING_OPTION);
    o.optionCount = s.optionCount;
  }
  AgentDeck::unlockState();
  return n;
}

void AgentDashboardActivity::handleButtons() {
  using Btn = MappedInputManager::Button;

  AwaitingItem items[kAwaitingCap];
  const int n = (dashState == DashState::Connected) ? collectAwaiting(items, kAwaitingCap) : 0;

  // Stamp Back press so release can tell short(=deny) from long(=exit).
  if (mappedInput.wasPressed(Btn::Back)) backPressMs = millis();

  if (n == 0) {
    // Nothing to decide → Back exits the dashboard. Guard against a stale release
    // whose press began in a prior activity (no recorded press → backPressMs==0).
    if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) exitRequested = true;
    return;
  }

  // ── Decision-card / triage mode ──
  if (triageIndex >= n) triageIndex = n - 1;
  if (triageIndex < 0) triageIndex = 0;
  const AwaitingItem& it = items[triageIndex];
  const bool optMode = it.isFocused && it.isOption && it.optionCount > 0;
  const int optCount = optMode ? it.optionCount : 0;
  if (optMode) {
    if (optionCursor >= optCount) optionCursor = optCount - 1;
    if (optionCursor < 0) optionCursor = 0;
  }

  // Up/Down (the two hinted nav buttons) are contextual: move the option cursor
  // when the card has options, otherwise page between awaiting sessions (triage).
  if (mappedInput.wasReleased(Btn::Up)) {
    if (optMode) optionCursor = (optionCursor - 1 + optCount) % optCount;
    else if (n > 1) triageIndex = (triageIndex - 1 + n) % n;
    requestUpdate();
  }
  if (mappedInput.wasReleased(Btn::Down)) {
    if (optMode) optionCursor = (optionCursor + 1) % optCount;
    else if (n > 1) {
      triageIndex = (triageIndex + 1) % n;
    }
    requestUpdate();
  }

  // Confirm = approve (yes/no) or select the highlighted option.
  if (mappedInput.wasReleased(Btn::Confirm)) {
    if (optMode) {
      int optIndex = optionCursor;
      AgentDeck::lockState();
      if (optionCursor < AgentDeck::g_state.optionCount) optIndex = AgentDeck::g_state.options[optionCursor].index;
      AgentDeck::unlockState();
      applyDecision(it, true, optIndex);
    } else {
      applyDecision(it, true, -1);
    }
  }

  // Back: short press = deny, long hold = exit.
  if (mappedInput.wasReleased(Btn::Back)) {
    const uint32_t held = millis() - backPressMs;
    if (held >= kExitHoldMs) {
      exitRequested = true;
    } else {
      applyDecision(it, false, -1);
    }
  }
}

void AgentDashboardActivity::applyDecision(const AwaitingItem& it, bool approve, int optionIndex) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs) return;
  lastDecisionMs = millis();

  const char* req = it.requestId[0] ? it.requestId : nullptr;
  const char* sid = it.sid[0] ? it.sid : nullptr;

  if (approve && optionIndex >= 0) {
    AgentDeck::Commands::sendSelectOption(sid, optionIndex);
    AgentLog::line("AGENT", "select_option idx=%d sid=%s", optionIndex, it.sid);
  } else {
    // Two-path approve/deny: requestId → permission_decision; else select_option(0)/escape.
    AgentDeck::Commands::sendApprove(req, sid, approve);
    AgentLog::line("AGENT", "%s sid=%s req=%s", approve ? "approve" : "deny", it.sid, it.requestId);
  }
  // Optimistic: the daemon will push an updated sessions_list/state_update that
  // drops this item from the awaiting set; just repaint.
  optionCursor = 0;
  requestUpdate();
}

void AgentDashboardActivity::renderCard(const AwaitingItem& it, int idx, int total) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = metrics.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();

  // Native header with a triage counter subtitle ("2 / 3") when several await.
  char sub[24] = {0};
  if (total > 1) snprintf(sub, sizeof(sub), "%d / %d", idx + 1, total);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "Decision", total > 1 ? sub : nullptr);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // agent · project
  char who[80];
  snprintf(who, sizeof(who), "%s%s%s", it.agentType[0] ? it.agentType : "agent", it.project[0] ? " \xC2\xB7 " : "",
           it.project);
  renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, who, w - pad * 2).c_str(), true);
  y += line10 + 8;

  // Question (wrapped)
  if (it.question[0]) {
    auto lines = renderer.wrappedText(UI_10_FONT_ID, it.question, w - pad * 2, 5);
    for (const auto& ln : lines) {
      renderer.drawText(UI_10_FONT_ID, pad, y, ln.c_str(), true);
      y += line10;
    }
    y += 8;
  }

  const bool optMode = it.isFocused && it.isOption && it.optionCount > 0;
  if (optMode) {
    AgentDeck::PromptOption opts[8];
    int oc = 0;
    AgentDeck::lockState();
    oc = AgentDeck::g_state.optionCount;
    if (oc > 8) oc = 8;
    for (int i = 0; i < oc; i++) opts[i] = AgentDeck::g_state.options[i];
    AgentDeck::unlockState();
    int cur = optionCursor;
    if (cur >= oc) cur = oc - 1;
    if (cur < 0) cur = 0;
    for (int i = 0; i < oc; i++) {
      char row[100];
      snprintf(row, sizeof(row), "%s %s", (i == cur) ? ">" : "  ", opts[i].label);
      renderer.drawText(UI_10_FONT_ID, pad, y, renderer.truncatedText(UI_10_FONT_ID, row, w - pad * 2).c_str(), true,
                        (i == cur) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += line10 + 2;
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, pad, y, "Approve this action?", true, EpdFontFamily::BOLD);
  }

  // Physically-aligned hints: labels are drawn at the actual button positions.
  // Up/Down navigate options (in an option prompt) or sessions (triage); they are
  // only shown when they do something.
  const bool hasNav = optMode || total > 1;
  const auto labels =
      mappedInput.mapLabels("Deny", optMode ? "Select" : "Approve", hasNav ? "Prev" : "", hasNav ? "Next" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void AgentDashboardActivity::render(RenderLock&&) {
  // A pending decision takes over the whole screen (M3 Decision Card / triage).
  if (dashState == DashState::Connected) {
    AwaitingItem items[kAwaitingCap];
    int n = collectAwaiting(items, kAwaitingCap);
    if (n > 0) {
      if (triageIndex >= n) triageIndex = n - 1;
      if (triageIndex < 0) triageIndex = 0;
      renderCard(items[triageIndex], triageIndex, n);
      return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int pad = metrics.contentSidePadding;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, "Agent Dashboard");
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Snapshot the bits we render under the lock so we don't tear mid-paint.
  AgentState agentState = AgentState::DISCONNECTED;
  bool dataReceived = false;
  char project[40] = {0};
  char question[200] = {0};
  char currentTool[40] = {0};
  uint8_t sessionCount = 0;
  AgentDeck::lockState();
  agentState = AgentDeck::g_state.state;
  dataReceived = AgentDeck::g_state.dataReceived;
  strncpy(project, AgentDeck::g_state.projectName, sizeof(project) - 1);
  strncpy(question, AgentDeck::g_state.question, sizeof(question) - 1);
  strncpy(currentTool, AgentDeck::g_state.currentTool, sizeof(currentTool) - 1);
  sessionCount = AgentDeck::g_state.sessionCount;
  AgentDeck::unlockState();

  switch (dashState) {
    case DashState::WifiSelection:
      renderer.drawText(UI_10_FONT_ID, pad, y, "Selecting Wi-Fi\xE2\x80\xA6", true);
      break;

    case DashState::Discovering:
      renderer.drawText(UI_10_FONT_ID, pad, y, "Discovering daemon\xE2\x80\xA6", true);
      y += line10 + 6;
      renderer.drawText(SMALL_FONT_ID, pad, y, ("Wi-Fi: " + localIp).c_str(), true);
      break;

    case DashState::Connecting: {
      char l[64];
      snprintf(l, sizeof(l), "Connecting\xE2\x80\xA6 %s:%u",
               AgentDeck::Net::wsBridgeIp(), (unsigned)AgentDeck::Net::wsBridgePort());
      renderer.drawText(UI_10_FONT_ID, pad, y, l, true);
      break;
    }

    case DashState::Connected: {
      char l[80];
      snprintf(l, sizeof(l), "Connected: %s", AgentDeck::Net::wsBridgeIp());
      renderer.drawText(UI_10_FONT_ID, pad, y, l, true);
      y += line10 + 10;

      if (!dataReceived) {
        renderer.drawText(SMALL_FONT_ID, pad, y, "Waiting for state\xE2\x80\xA6", true);
      } else {
        // Agent state line
        char st[96];
        snprintf(st, sizeof(st), "State: %s", agentStateLabel(agentState));
        renderer.drawText(UI_10_FONT_ID, pad, y, st, true, EpdFontFamily::BOLD);
        y += line10 + 6;

        if (project[0]) {
          std::string p = renderer.truncatedText(SMALL_FONT_ID, (std::string("Project: ") + project).c_str(),
                                                  w - pad * 2);
          renderer.drawText(SMALL_FONT_ID, pad, y, p.c_str(), true);
          y += line10 + 4;
        }
        if (sessionCount > 0) {
          char sc[48];
          snprintf(sc, sizeof(sc), "Sessions: %u", (unsigned)sessionCount);
          renderer.drawText(SMALL_FONT_ID, pad, y, sc, true);
          y += line10 + 4;
        }
        if (currentTool[0]) {
          std::string t = renderer.truncatedText(SMALL_FONT_ID, (std::string("Tool: ") + currentTool).c_str(),
                                                  w - pad * 2);
          renderer.drawText(SMALL_FONT_ID, pad, y, t.c_str(), true);
          y += line10 + 4;
        }
        if (question[0]) {
          y += 4;
          auto lines = renderer.wrappedText(SMALL_FONT_ID, question, w - pad * 2, 3);
          for (const auto& ln : lines) {
            renderer.drawText(SMALL_FONT_ID, pad, y, ln.c_str(), true);
            y += line10;
          }
        }
      }
      break;
    }
  }

  // Native, physically-aligned hint bar. Only Back (=Exit) is actionable here.
  const auto labels = mappedInput.mapLabels("Exit", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
