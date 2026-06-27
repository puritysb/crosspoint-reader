#pragma once
//
// AgentDashboardActivity — entry point for the AgentDeck "Decision Card" mode.
//
// M1: scaffold + AgentLog SD diagnostic channel.
// M2 (this commit): NETWORK layer. Brings up WiFi (mirrors CalibreConnect),
//   discovers the AgentDeck daemon over mDNS (_agentdeck._tcp), connects to it
//   over WebSocket, registers as an eink-device, and renders live connection +
//   agent state. Display-only — button approve/deny lands in M3 (the outbound
//   builders are already ported in agentdeck/agent_commands.*).
// M3: render awaiting Decision Cards + approve/deny via the physical buttons.
//
// Concurrency: net is serviced cooperatively from loop() on the main task — no
// FreeRTOS network task. render() runs on the separate render task and reads the
// mutex-guarded g_state.
//
#include <string>

#include "activities/Activity.h"

class AgentDashboardActivity final : public Activity {
 public:
  explicit AgentDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AgentDashboard", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keep the main loop tight + the radio awake while we're servicing the daemon.
  bool skipLoopDelay() override { return dashState != DashState::WifiSelection; }
  bool preventAutoSleep() override { return dashState != DashState::WifiSelection; }

 private:
  enum class DashState : uint8_t { WifiSelection, Discovering, Connecting, Connected };

  // Bounded below SESSIONS_CAP(10): two AwaitingItem[] arrays live on task stacks
  // (render + main loop) on a no-PSRAM C3, so cap the realistic triage depth.
  static constexpr int kAwaitingCap = 6;

  // One awaiting decision pending a human go/no-go. Collected from the
  // sessions_list (or synthesized from the focused state_update). M3 triages a
  // stack of these and drives approve/deny per item.
  struct AwaitingItem {
    char sid[64];  // full/prefixed session UUID — see SessionInfo::id sizing
    char project[40];
    char agentType[16];
    char question[200];
    char requestId[40];   // present → observed gate (permission_decision)
    char promptType[20];
    bool isFocused;       // == g_state.sessionId → has the rich options[] array
    bool isOption;        // awaiting_option / multi_select → Up/Down picks an option
    uint8_t optionCount;  // meaningful only when isFocused
  };

  // Compact per-session row for the Overview / Mission Control list. Deliberately
  // lighter than AwaitingItem (no question/tool/options) — an array of these lives
  // on a no-PSRAM C3 task stack, so keep it small.
  struct OverviewRow {
    char sid[64];
    char project[40];
    char agentType[16];
    char state[20];
    char activity[80];  // compact "what it's doing" line
    bool awaiting;
  };

  // Which screen the Connected dashboard is showing. Overview is home; OK opens a
  // session's Decision Card (awaiting) or read-only Detail (everything else).
  // Overview is home; opening a session goes to Detail. Detail shows the session
  // timeline AND, when the session is awaiting, the decision options inline at the
  // bottom (scroll down to reach them) — no separate decision card.
  enum class ViewMode : uint8_t { Overview, Detail };

  void onWifiSelectionComplete(bool connected);
  void startNetworking();
  void sendClientRegister();
  uint32_t computeStateSignature() const;

  // Fill out[] with the currently-awaiting sessions; returns the count. Const +
  // takes the state lock internally.
  int collectAwaiting(AwaitingItem* out, int cap) const;
  // Fill out[] with all alive sessions (overview order); returns the count.
  int collectOverview(OverviewRow* out, int cap) const;
  void handleButtons();
  void applyDecision(const AwaitingItem& it, bool approve, int optionIndex);
  void renderOverview(const OverviewRow* rows, int n, int awaitingCount);
  void renderDetail();
  // Branded header (AgentDeck mark + title) shared by every Connected screen.
  void drawBrandedHeader(const char* title, const char* subtitle) const;
  // LIMITS footer — 5H/7D quota gauges. Renders only when the hub supplies usage
  // (fiveHourPercent/sevenDayPercent >= 0); hidden otherwise. Returns the y it
  // started at (so callers know the content ceiling) or pageHeight if nothing drawn.
  int drawLimitsFooter() const;

  // The daemon port is dynamic (9120, falling back up to 9139), so a cached
  // ip:port goes stale across a daemon restart. If a connect attempt doesn't
  // succeed within this window, drop back to Discovering and re-resolve via mDNS
  // instead of hammering the old port. See feedback_daemon_port_flexibility.
  static constexpr uint32_t kConnectTimeoutMs = 10000;
  static constexpr uint32_t kExitHoldMs = 700;          // hold Back this long = exit while a card is up
  static constexpr uint32_t kDecisionCooldownMs = 400;  // debounce a sent decision
  // A connection that survives longer than this is "healthy": on drop we retry the
  // SAME endpoint (transient drop / daemon restart). A connection that dies sooner
  // is a flaky/duplicate daemon — re-resolve via mDNS to try a different advertiser
  // instead of hammering it (multiple daemons on the LAN round-robin otherwise).
  static constexpr uint32_t kHealthyUptimeMs = 8000;

  DashState dashState = DashState::WifiSelection;
  std::string localIp;
  bool exitRequested = false;
  bool registered = false;
  uint32_t lastSignature = 0;
  uint32_t connectStartMs = 0;
  uint32_t lastConnectedMs = 0;  // when we last reached Connected (for healthy-vs-flaky drop)

  // Screen navigation
  ViewMode viewMode = ViewMode::Overview;
  int overviewCursor = 0;     // selected row in the Overview list
  int overviewTop = 0;        // first visible row (scroll window)
  int detailScroll = 0;       // first visible content line in Detail
  int detailMaxScroll = 0;    // set by renderDetail; lets handleButtons know "at bottom"
  char selectedSid[64] = {0}; // session opened into Card/Detail (re-resolved each frame)
  // Installed SD CJK font id (the reader's font when it's an SD font) so CJK text
  // renders instead of □; 0 when none — Latin-only built-in UI fonts have no CJK.
  int cjkFontId = 0;

  // Returns the SD CJK font id for text containing CJK (loading its glyphs), else
  // the given UI font id. Use the return value for both measuring and drawing.
  int fontForText(int uiFontId, const char* text) const;

  // Load the bundled Noto Sans KR (OFL) font shipped on the SD at /.fonts/ so
  // Korean renders without the user installing a font. Loaded once; returns its
  // font id, or 0 when the file isn't present.
  int loadKoreanFont();

  // Decision-card / triage cursors
  int triageIndex = 0;       // which awaiting session is shown
  int optionCursor = 0;      // which option is highlighted (option prompts)
  uint32_t backPressMs = 0;  // Back press timestamp for short(deny)/long(exit/back)
  uint32_t lastDecisionMs = 0;
};
