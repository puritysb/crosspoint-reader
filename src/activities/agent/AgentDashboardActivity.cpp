#include "AgentDashboardActivity.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "agent/AgentLog.h"
#include "agentdeck/agent_commands.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/mdns_discovery.h"
#include "agentdeck/ws_client.h"
#include "components/UITheme.h"  // GUI (theme) + ThemeMetrics + Rect
#include "components/icons/agentdeck_mark.h"
#include "components/icons/glyph_claude.h"
#include "components/icons/glyph_codex.h"
#include "components/icons/glyph_openclaw.h"
#include "components/icons/glyph_opencode.h"
#include "fontIds.h"

namespace {
using AgentDeck::AgentState;

// Per-agent creature glyph (src/components/icons/glyph_*.h). MUST be a multiple of
// 8: EInkDisplay::drawImageTransparent reads `width/8` bytes per row, so a width
// like 28 (→ 3 bytes, but convert_icon.py packs 4) desyncs every row into noise.
constexpr int kGlyphPx = 32;

// Canonical 1-bit creature glyph for an agent type → nullptr falls back to a dot.
// Mirrors the AGENT_MONO_GLYPH map (shared/src/svg-renderers/agent-logos.ts). Keep
// every wire agentType covered here — a missing branch silently degrades to a dot.
const uint8_t* glyphForAgent(const char* a) {
  if (!a || !a[0]) return nullptr;
  if (strcmp(a, "claude-code") == 0) return GlyphClaude;
  if (strncmp(a, "codex", 5) == 0) return GlyphCodex;  // codex-cli / codex-app / codex
  if (strcmp(a, "opencode") == 0) return GlyphOpenCode;
  if (strcmp(a, "openclaw") == 0) return GlyphOpenClaw;
  return nullptr;
}

// Format an ISO-8601 resetsAt as a compact "Xd Yh" / "Xh Ym" / "Xm" remaining,
// or "" when the system clock isn't NTP-synced yet or the string won't parse.
// configTime(0,0,…) keeps the zone at UTC so mktime() reads the (UTC "…Z") ISO
// correctly. Best-effort: a missing/unsynced clock just drops the countdown.
std::string formatResetRemaining(const char* iso) {
  if (!iso || !iso[0]) return "";
  const time_t now = time(nullptr);
  if (now < 1700000000) return "";  // ~2023-11 — clock not yet synced
  int Y, Mo, D, H, Mi, S;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &H, &Mi, &S) != 6) return "";
  struct tm tmv = {};
  tmv.tm_year = Y - 1900;
  tmv.tm_mon = Mo - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = H;
  tmv.tm_min = Mi;
  tmv.tm_sec = S;
  const time_t reset = mktime(&tmv);
  if (reset <= now) return "now";
  long secs = (long)(reset - now);
  char buf[16];
  if (secs >= 86400)
    snprintf(buf, sizeof(buf), "%ldd %ldh", secs / 86400, (secs % 86400) / 3600);
  else if (secs >= 3600)
    snprintf(buf, sizeof(buf), "%ldh %ldm", secs / 3600, (secs % 3600) / 60);
  else
    snprintf(buf, sizeof(buf), "%ldm", secs / 60);
  return buf;
}

// Compact, uppercase status badge for the overview rows.
const char* stateBadge(const char* s) {
  if (!s || !s[0]) return "—";
  if (strncmp(s, "awaiting", 8) == 0) return "AWAITING";
  if (strcmp(s, "processing") == 0) return "WORKING";
  if (strcmp(s, "idle") == 0) return "IDLE";
  if (strcmp(s, "disconnected") == 0) return "OFFLINE";
  return s;
}

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
  // Best-effort clock for the LIMITS reset countdowns. Non-blocking — SNTP updates
  // the system time in the background; until it lands, formatResetRemaining() just
  // omits the countdown. UTC offset 0 so mktime() reads the ISO "…Z" resetsAt right.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
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
  // Usage so the LIMITS footer repaints when a quota gauge / reset / subscription moves.
  h = fnvUpdate(h, &s.fiveHourPercent, sizeof(s.fiveHourPercent));
  h = fnvUpdate(h, &s.sevenDayPercent, sizeof(s.sevenDayPercent));
  h = fnvUpdate(h, &s.usageStale, sizeof(s.usageStale));
  h = fnvUpdate(h, s.fiveHourReset, strlen(s.fiveHourReset));
  h = fnvUpdate(h, s.sevenDayReset, strlen(s.sevenDayReset));
  h = fnvUpdate(h, s.codexPlan, strlen(s.codexPlan));
  h = fnvUpdate(h, s.codexActiveUntil, strlen(s.codexActiveUntil));
  h = fnvUpdate(h, s.antigravityPlan, strlen(s.antigravityPlan));
  h = fnvUpdate(h, &s.antigravityCredits, sizeof(s.antigravityCredits));
  AgentDeck::unlockState();
  // Local view/cursor state so navigation repaints.
  uint8_t vm = static_cast<uint8_t>(viewMode);
  h = fnvUpdate(h, &vm, sizeof(vm));
  h = fnvUpdate(h, &overviewCursor, sizeof(overviewCursor));
  h = fnvUpdate(h, &triageIndex, sizeof(triageIndex));
  h = fnvUpdate(h, &optionCursor, sizeof(optionCursor));
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

int AgentDashboardActivity::collectOverview(OverviewRow* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount && n < cap; i++) {
    const auto& se = s.sessions[i];
    if (!se.alive) continue;
    OverviewRow& o = out[n++];
    cp(o.sid, sizeof(o.sid), se.id);
    cp(o.project, sizeof(o.project), se.projectName[0] ? se.projectName : "session");
    cp(o.agentType, sizeof(o.agentType), se.agentType);
    cp(o.state, sizeof(o.state), se.state);
    o.awaiting = (strncmp(se.state, "awaiting", 8) == 0);
  }
  // Observed/single-session fallback: sessions_list empty but a focused
  // state_update is live — surface it as one row so the overview isn't blank.
  if (n == 0 && cap > 0 && s.dataReceived && s.state != AgentState::DISCONNECTED) {
    OverviewRow& o = out[n++];
    cp(o.sid, sizeof(o.sid), s.sessionId[0] ? s.sessionId : s.focusedSessionId);
    cp(o.project, sizeof(o.project), s.projectName[0] ? s.projectName : "session");
    cp(o.agentType, sizeof(o.agentType), s.agentType);
    const bool aw = s.state == AgentState::AWAITING_PERMISSION || s.state == AgentState::AWAITING_OPTION ||
                    s.state == AgentState::AWAITING_DIFF;
    cp(o.state, sizeof(o.state),
       aw ? "awaiting" : (s.state == AgentState::PROCESSING ? "processing" : "idle"));
    o.awaiting = aw;
  }
  AgentDeck::unlockState();
  return n;
}

void AgentDashboardActivity::handleButtons() {
  using Btn = MappedInputManager::Button;

  // Stamp Back press so release can tell short from long-hold.
  if (mappedInput.wasPressed(Btn::Back)) backPressMs = millis();

  // Not connected yet (WiFi select / discovering / connecting): the only action is
  // Back = leave the dashboard. Keep it responsive in every pre-Connected state so
  // the user is never trapped on a "searching…" screen with no way out.
  if (dashState != DashState::Connected) {
    if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) exitRequested = true;
    return;
  }

  // ── CARD: a session opened for a go/no-go decision (or option pick) ──
  if (viewMode == ViewMode::Card) {
    AwaitingItem items[kAwaitingCap];
    const int n = collectAwaiting(items, kAwaitingCap);
    int idx = -1;
    for (int i = 0; i < n; i++)
      if (selectedSid[0] && strcmp(items[i].sid, selectedSid) == 0) { idx = i; break; }
    if (n == 0 || idx < 0) {  // decision resolved / session gone → back to overview
      viewMode = ViewMode::Overview;
      requestUpdate();
      return;
    }
    triageIndex = idx;
    const AwaitingItem& it = items[idx];
    const bool optMode = it.isFocused && it.isOption && it.optionCount > 0;
    const int optCount = optMode ? it.optionCount : 0;
    if (optMode) {
      if (optionCursor >= optCount) optionCursor = optCount - 1;
      if (optionCursor < 0) optionCursor = 0;
    }

    // Up/Down: move the option cursor (option prompt) or page between awaiting
    // sessions (triage) — re-pinning selectedSid so the card follows the page.
    if (mappedInput.wasReleased(Btn::NavPrevious)) {
      if (optMode) optionCursor = (optionCursor - 1 + optCount) % optCount;
      else if (n > 1) { idx = (idx - 1 + n) % n; strncpy(selectedSid, items[idx].sid, sizeof(selectedSid) - 1); optionCursor = 0; }
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::NavNext)) {
      if (optMode) optionCursor = (optionCursor + 1) % optCount;
      else if (n > 1) { idx = (idx + 1) % n; strncpy(selectedSid, items[idx].sid, sizeof(selectedSid) - 1); optionCursor = 0; }
      requestUpdate();
    }

    // Confirm = approve / select the highlighted option.
    if (mappedInput.wasReleased(Btn::Confirm)) {
      if (optMode) {
        int optIndex = optionCursor;
        AgentDeck::lockState();
        if (optionCursor < AgentDeck::g_state.optionCount) optIndex = AgentDeck::g_state.options[optionCursor].index;
        AgentDeck::unlockState();
        applyDecision(items[idx], true, optIndex);
      } else {
        applyDecision(items[idx], true, -1);
      }
    }

    // Back: short press = deny; long hold = back to overview (don't exit dashboard).
    if (mappedInput.wasReleased(Btn::Back)) {
      const uint32_t held = millis() - backPressMs;
      if (held >= kExitHoldMs) {
        viewMode = ViewMode::Overview;
        requestUpdate();
      } else {
        applyDecision(items[idx], false, -1);
      }
    }
    return;
  }

  // ── DETAIL: read-only session inspector ──
  if (viewMode == ViewMode::Detail) {
    if (mappedInput.wasReleased(Btn::Back)) {
      viewMode = ViewMode::Overview;
      requestUpdate();
    }
    return;
  }

  // ── OVERVIEW: mission-control list (home) ──
  OverviewRow rows[AgentDeckCfg::SESSIONS_CAP];
  const int n = collectOverview(rows, AgentDeckCfg::SESSIONS_CAP);
  if (overviewCursor >= n) overviewCursor = n > 0 ? n - 1 : 0;
  if (overviewCursor < 0) overviewCursor = 0;

  if (mappedInput.wasReleased(Btn::NavPrevious) && n > 1) {
    overviewCursor = (overviewCursor - 1 + n) % n;
    requestUpdate();
  }
  if (mappedInput.wasReleased(Btn::NavNext) && n > 1) {
    overviewCursor = (overviewCursor + 1) % n;
    requestUpdate();
  }

  // Confirm opens the selected row: awaiting → Card, otherwise → Detail.
  if (mappedInput.wasReleased(Btn::Confirm) && n > 0) {
    const OverviewRow& sel = rows[overviewCursor];
    strncpy(selectedSid, sel.sid, sizeof(selectedSid) - 1);
    selectedSid[sizeof(selectedSid) - 1] = '\0';
    optionCursor = 0;
    viewMode = sel.awaiting ? ViewMode::Card : ViewMode::Detail;
    requestUpdate();
  }

  // Back exits the dashboard (guard a stale release from a prior activity).
  if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) exitRequested = true;
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

void AgentDashboardActivity::drawBrandedHeader(const char* title, const char* subtitle) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const Rect r{0, m.topPadding, w, m.headerHeight};
  // Pass nullptr title: Lyra's drawHeader draws the title at contentSidePadding —
  // exactly where our mark goes — and also owns the divider line (it lives inside
  // its `if (title)` block). So we let drawHeader place the battery + subtitle, then
  // lay out the mark + wordmark together on one line and redraw the divider.
  GUI.drawHeader(renderer, r, nullptr, subtitle);

  constexpr int sz = 32;  // mark width must be a multiple of 8 (see kGlyphPx note)
  const int iconY = r.y + (m.headerHeight - sz) / 2 - 2;
  renderer.drawIcon(AgentDeckMark, m.contentSidePadding, iconY, sz, sz);

  if (title) {
    const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleX = m.contentSidePadding + sz + 10;
    const int titleY = r.y + (m.headerHeight - line12) / 2;
    renderer.drawText(UI_12_FONT_ID, titleX, titleY, title, true, EpdFontFamily::BOLD);
  }
  // Divider, matching LyraTheme::drawHeader (3px rule along the header baseline).
  renderer.drawLine(r.x, r.y + r.height - 3, r.x + r.width - 1, r.y + r.height - 3, 3, true);
}

int AgentDashboardActivity::drawLimitsFooter() const {
  // Snapshot usage + the best-effort subscription summary under the lock.
  float five, seven, agCredits;
  bool stale;
  char fiveReset[32], sevenReset[32], codexPlan[16], codexUntil[32], agPlan[24];
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  five = s.fiveHourPercent;
  seven = s.sevenDayPercent;
  stale = s.usageStale;
  agCredits = s.antigravityCredits;
  strncpy(fiveReset, s.fiveHourReset, sizeof(fiveReset) - 1);
  fiveReset[sizeof(fiveReset) - 1] = '\0';
  strncpy(sevenReset, s.sevenDayReset, sizeof(sevenReset) - 1);
  sevenReset[sizeof(sevenReset) - 1] = '\0';
  strncpy(codexPlan, s.codexPlan, sizeof(codexPlan) - 1);
  codexPlan[sizeof(codexPlan) - 1] = '\0';
  strncpy(codexUntil, s.codexActiveUntil, sizeof(codexUntil) - 1);
  codexUntil[sizeof(codexUntil) - 1] = '\0';
  strncpy(agPlan, s.antigravityPlan, sizeof(agPlan) - 1);
  agPlan[sizeof(agPlan) - 1] = '\0';
  AgentDeck::unlockState();

  // Compose the optional other-agent subscription line (ChatGPT plan/expiry,
  // Antigravity credits). Empty when the hub sent none of it.
  char subLine[96] = {0};
  int off = 0;
  if (codexPlan[0] || codexUntil[0]) {
    off += snprintf(subLine + off, sizeof(subLine) - off, "ChatGPT");
    if (codexPlan[0]) off += snprintf(subLine + off, sizeof(subLine) - off, " %s", codexPlan);
    if (codexUntil[0]) {
      char d[11] = {0};
      strncpy(d, codexUntil, 10);  // YYYY-MM-DD portion of the ISO date
      off += snprintf(subLine + off, sizeof(subLine) - off, " \xE2\x86\x92 %s", d);
    }
  }
  if (agCredits >= 0 || agPlan[0]) {
    if (off > 0) off += snprintf(subLine + off, sizeof(subLine) - off, "   \xC2\xB7   ");
    if (agCredits >= 0)
      off += snprintf(subLine + off, sizeof(subLine) - off, "AG %d cr", (int)(agCredits + 0.5f));
    else
      off += snprintf(subLine + off, sizeof(subLine) - off, "AG %s", agPlan);
  }

  const int pageH = renderer.getScreenHeight();
  const bool hasUsage = (five >= 0 || seven >= 0);
  const bool hasSub = subLine[0] != '\0';
  if (!hasUsage && !hasSub) return pageH;  // nothing to show → hide the footer

  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowCount = (hasUsage ? 1 : 0) + (hasSub ? 1 : 0);
  const int bandH = 14 + rowCount * (lineS + 6);
  const int top = pageH - m.buttonHintsHeight - bandH;

  renderer.drawLine(pad, top, w - pad, top);  // separator above the footer
  int y = top + 12;
  const int colW = (w - pad * 2 - 16) / 2;  // two columns (5H | 7D)

  if (hasUsage) {
    auto quota = [&](int x, const char* label, float pct, const char* iso) {
      if (pct < 0) return;
      // Right-aligned "NN% · Xh Ym"; the gauge fills the space between.
      std::string rem = formatResetRemaining(iso);
      char rt[28];
      if (!rem.empty())
        snprintf(rt, sizeof(rt), "%d%%%s  %s", (int)(pct + 0.5f), stale ? "*" : "", rem.c_str());
      else
        snprintf(rt, sizeof(rt), "%d%%%s", (int)(pct + 0.5f), stale ? "*" : "");
      renderer.drawText(SMALL_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
      const int lw = renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::BOLD);
      const int rw = renderer.getTextWidth(SMALL_FONT_ID, rt);
      const int gx = x + lw + 8;
      const int gw = colW - lw - 8 - rw - 8;
      const int gh = lineS - 4;
      if (gw > 8) {
        renderer.drawRect(gx, y + 1, gw, gh);
        const int fw = (int)((gw - 2) * (pct / 100.0f));
        if (fw > 0) renderer.fillRect(gx + 1, y + 2, fw, gh - 2);
      }
      renderer.drawText(SMALL_FONT_ID, x + colW - rw, y, rt, true);
    };
    quota(pad, "5H", five, fiveReset);
    quota(pad + colW + 16, "7D", seven, sevenReset);
    y += lineS + 6;
  }

  if (hasSub) {
    renderer.drawText(SMALL_FONT_ID, pad, y,
                      renderer.truncatedText(SMALL_FONT_ID, subLine, w - pad * 2).c_str(), true);
  }
  return top;
}

void AgentDashboardActivity::renderCard(const AwaitingItem& it, int idx, int total) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  renderer.clearScreen();

  // Branded header with a triage counter subtitle ("2 / 3") when several await.
  char sub[24] = {0};
  if (total > 1) snprintf(sub, sizeof(sub), "%d / %d", idx + 1, total);
  drawBrandedHeader("Decision", total > 1 ? sub : nullptr);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // agent glyph + agent · project
  const uint8_t* g = glyphForAgent(it.agentType);
  int textX = pad;
  if (g) {
    renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
    textX = pad + kGlyphPx + 12;
  }
  char who[80];
  snprintf(who, sizeof(who), "%s%s%s", it.agentType[0] ? it.agentType : "agent", it.project[0] ? " \xC2\xB7 " : "",
           it.project);
  renderer.drawText(UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
                    renderer.truncatedText(UI_10_FONT_ID, who, w - textX - pad, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  y += (g ? kGlyphPx : line10) + 12;

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

  // Discoverability: tell the user how to leave the card without deciding.
  renderer.drawText(SMALL_FONT_ID, pad, pageH - metrics.buttonHintsHeight - lineS - 6, "Hold Back: back to overview",
                    true);

  // Physically-aligned hints at the actual button positions. Up/Down navigate
  // options (option prompt) or awaiting sessions (triage); shown only when useful.
  const bool hasNav = optMode || total > 1;
  const auto labels =
      mappedInput.mapLabels("Deny", optMode ? "Select" : "Approve", hasNav ? "Prev" : "", hasNav ? "Next" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void AgentDashboardActivity::renderOverview(const OverviewRow* rows, int n, int awaitingCount) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  bool dataReceived;
  AgentDeck::lockState();
  dataReceived = AgentDeck::g_state.dataReceived;
  AgentDeck::unlockState();

  renderer.clearScreen();
  drawBrandedHeader("AgentDeck", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  // Connection line
  char cl[80];
  snprintf(cl, sizeof(cl), "Connected \xC2\xB7 %s", AgentDeck::Net::wsBridgeIp());
  renderer.drawText(SMALL_FONT_ID, pad, y, cl, true);
  y += lineS + 8;

  // Awaiting banner (inverted) — the highest-priority glance signal.
  if (awaitingCount > 0) {
    const int bh = line10 + 12;
    renderer.fillRect(pad, y, w - pad * 2, bh, true);
    char b[48];
    snprintf(b, sizeof(b), "%d agent%s need you", awaitingCount, awaitingCount > 1 ? "s" : "");
    renderer.drawText(UI_10_FONT_ID, pad + 12, y + 6, b, false, EpdFontFamily::BOLD);
    y += bh + 10;
  }

  // Footer first so we know the content ceiling.
  const int footTop = drawLimitsFooter();
  const int rowsBottom = (footTop < pageH ? footTop : pageH - m.buttonHintsHeight) - 8;

  if (n == 0) {
    renderer.drawText(UI_10_FONT_ID, pad, y, dataReceived ? "No active sessions" : "Waiting for agent state\xE2\x80\xA6",
                      true);
  } else {
    const int rowH = kGlyphPx + 12;
    int i = 0;
    for (; i < n; i++) {
      if (y + rowH > rowsBottom) break;  // overflow → "+N more" below
      const bool sel = (i == overviewCursor);
      if (sel) renderer.drawRect(pad - 6, y - 3, w - pad * 2 + 12, rowH);

      const uint8_t* g = glyphForAgent(rows[i].agentType);
      if (g) renderer.drawIcon(g, pad, y + (rowH - kGlyphPx) / 2, kGlyphPx, kGlyphPx);
      const int tx = pad + kGlyphPx + 12;
      const int ty = y + (rowH - line10) / 2;

      // Status badge (right-aligned). Awaiting is inverted so it pops.
      const char* badge = stateBadge(rows[i].state);
      const bool aw = rows[i].awaiting;
      const int bTextW = renderer.getTextWidth(SMALL_FONT_ID, badge, EpdFontFamily::BOLD);
      const int bw = bTextW + (aw ? 14 : 0);
      const int bx = w - pad - bw;
      if (aw) {
        const int bh = lineS + 6;
        renderer.fillRect(bx, ty - 2, bw, bh, true);
        renderer.drawText(SMALL_FONT_ID, bx + 7, ty + 1, badge, false, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(SMALL_FONT_ID, bx, ty + 1, badge, true, EpdFontFamily::REGULAR);
      }

      // Project name (bold when selected), truncated to leave room for the badge.
      const auto style = sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      std::string p = renderer.truncatedText(UI_10_FONT_ID, rows[i].project, bx - tx - 12, style);
      renderer.drawText(UI_10_FONT_ID, tx, ty, p.c_str(), true, style);
      y += rowH + 6;
    }
    if (i < n) {
      char more[24];
      snprintf(more, sizeof(more), "+%d more\xE2\x80\xA6", n - i);
      renderer.drawText(SMALL_FONT_ID, pad, y, more, true);
    }
  }

  // Hint bar. Up/Down only meaningful with more than one row.
  const auto labels =
      mappedInput.mapLabels("Exit", n > 0 ? "Open" : "", n > 1 ? "Up" : "", n > 1 ? "Down" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AgentDashboardActivity::renderDetail() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  // Snapshot the selected session under the lock.
  char project[40] = {0}, agentType[16] = {0}, model[32] = {0}, state[20] = {0}, tool[40] = {0}, question[200] = {0};
  uint32_t elapsed = 0;
  bool found = false;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount; i++) {
    if (selectedSid[0] && strcmp(s.sessions[i].id, selectedSid) == 0) {
      const auto& se = s.sessions[i];
      strncpy(project, se.projectName, sizeof(project) - 1);
      strncpy(agentType, se.agentType, sizeof(agentType) - 1);
      strncpy(model, se.modelName, sizeof(model) - 1);
      strncpy(state, se.state, sizeof(state) - 1);
      strncpy(tool, se.currentTool, sizeof(tool) - 1);
      strncpy(question, se.question, sizeof(question) - 1);
      elapsed = se.elapsedSec;
      found = true;
      break;
    }
  }
  if (!found) {  // observed/single-session fallback → render the focused state
    strncpy(project, s.projectName, sizeof(project) - 1);
    strncpy(agentType, s.agentType, sizeof(agentType) - 1);
    strncpy(model, s.modelName, sizeof(model) - 1);
    strncpy(tool, s.currentTool, sizeof(tool) - 1);
    strncpy(question, s.question, sizeof(question) - 1);
    strncpy(state, agentStateLabel(s.state), sizeof(state) - 1);
    found = s.dataReceived;
  }
  AgentDeck::unlockState();

  renderer.clearScreen();
  drawBrandedHeader("Session", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  if (!found) {
    renderer.drawText(UI_10_FONT_ID, pad, y, "Session ended", true, EpdFontFamily::BOLD);
  } else {
    const uint8_t* g = glyphForAgent(agentType);
    int textX = pad;
    if (g) {
      renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
      textX = pad + kGlyphPx + 12;
    }
    renderer.drawText(UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
                      renderer.truncatedText(UI_10_FONT_ID, project[0] ? project : "session", w - textX - pad,
                                             EpdFontFamily::BOLD)
                          .c_str(),
                      true, EpdFontFamily::BOLD);
    y += (g ? kGlyphPx : line10) + 14;

    auto field = [&](const char* label, const char* value) {
      if (!value || !value[0]) return;
      char ln[128];
      snprintf(ln, sizeof(ln), "%s: %s", label, value);
      renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, ln, w - pad * 2).c_str(), true);
      y += lineS + 6;
    };
    field("Agent", agentType);
    field("Model", model);
    field("State", state);
    field("Tool", tool);
    if (elapsed > 0) {
      char e[32];
      if (elapsed >= 60)
        snprintf(e, sizeof(e), "%um %us", (unsigned)(elapsed / 60), (unsigned)(elapsed % 60));
      else
        snprintf(e, sizeof(e), "%us", (unsigned)elapsed);
      field("Elapsed", e);
    }
    if (question[0]) {
      y += 6;
      auto lines = renderer.wrappedText(SMALL_FONT_ID, question, w - pad * 2, 4);
      for (const auto& l : lines) {
        renderer.drawText(SMALL_FONT_ID, pad, y, l.c_str(), true);
        y += lineS;
      }
    }
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AgentDashboardActivity::render(RenderLock&&) {
  // ── Connected: Overview (home) / Card / Detail ──
  if (dashState == DashState::Connected) {
    if (viewMode == ViewMode::Card) {
      AwaitingItem items[kAwaitingCap];
      const int n = collectAwaiting(items, kAwaitingCap);
      for (int i = 0; i < n; i++) {
        if (selectedSid[0] && strcmp(items[i].sid, selectedSid) == 0) {
          renderCard(items[i], i, n);
          return;
        }
      }
      // Selected awaiting item gone — fall through to Overview (handleButtons
      // resets viewMode on the next loop; don't mutate it from the render task).
    } else if (viewMode == ViewMode::Detail) {
      renderDetail();
      return;
    }

    OverviewRow rows[AgentDeckCfg::SESSIONS_CAP];
    const int n = collectOverview(rows, AgentDeckCfg::SESSIONS_CAP);
    int awaiting = 0;
    for (int i = 0; i < n; i++)
      if (rows[i].awaiting) awaiting++;
    renderOverview(rows, n, awaiting);
    return;
  }

  // ── Pre-Connected: WiFi select / discovering / connecting. Always show Back. ──
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int pad = metrics.contentSidePadding;

  renderer.clearScreen();
  drawBrandedHeader("AgentDeck", nullptr);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  switch (dashState) {
    case DashState::WifiSelection:
      renderer.drawText(UI_10_FONT_ID, pad, y, "Selecting Wi-Fi\xE2\x80\xA6", true, EpdFontFamily::BOLD);
      break;

    case DashState::Discovering:
      renderer.drawText(UI_10_FONT_ID, pad, y, "Discovering daemon\xE2\x80\xA6", true, EpdFontFamily::BOLD);
      y += line10 + 6;
      renderer.drawText(SMALL_FONT_ID, pad, y, ("Wi-Fi: " + localIp).c_str(), true);
      break;

    case DashState::Connecting: {
      char l[64];
      snprintf(l, sizeof(l), "Connecting\xE2\x80\xA6 %s:%u", AgentDeck::Net::wsBridgeIp(),
               (unsigned)AgentDeck::Net::wsBridgePort());
      renderer.drawText(UI_10_FONT_ID, pad, y, l, true, EpdFontFamily::BOLD);
      break;
    }

    case DashState::Connected:
      break;  // handled above
  }

  // Persistent, physically-aligned hint bar — Back always leaves the dashboard.
  const auto labels = mappedInput.mapLabels("Exit", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
