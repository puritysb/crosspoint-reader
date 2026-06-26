#include "agent_commands.h"

#include <cstdio>
#include <cstring>

#include "ws_client.h"

namespace AgentDeck {
namespace Commands {

bool buildPermissionDecision(char* out, size_t cap, const char* requestId, const char* decision) {
  if (!out || cap == 0) return false;
  if (!requestId || !requestId[0] || !decision || !decision[0]) {
    if (cap) out[0] = '\0';
    return false;
  }
  int n = snprintf(out, cap,
                   "{\"type\":\"permission_decision\",\"requestId\":\"%s\",\"decision\":\"%s\"}",
                   requestId, decision);
  return n > 0 && (size_t)n < cap;
}

bool buildSelectOption(char* out, size_t cap, const char* sid, int index) {
  if (!out || cap == 0) return false;
  int n;
  if (sid && sid[0])
    n = snprintf(out, cap, "{\"type\":\"select_option\",\"index\":%d,\"sessionId\":\"%s\"}", index, sid);
  else
    n = snprintf(out, cap, "{\"type\":\"select_option\",\"index\":%d}", index);
  return n > 0 && (size_t)n < cap;
}

bool buildSessionEscape(char* out, size_t cap, const char* sid) {
  if (!out || cap == 0) return false;
  if (!sid || !sid[0]) {
    out[0] = '\0';
    return false;
  }
  int n = snprintf(out, cap,
                   "{\"type\":\"session_command\",\"sessionId\":\"%s\",\"command\":{\"type\":\"escape\"}}",
                   sid);
  return n > 0 && (size_t)n < cap;
}

void sendPermissionDecision(const char* requestId, const char* decision) {
  char buf[160];
  if (buildPermissionDecision(buf, sizeof(buf), requestId, decision)) Net::queueOutbound(buf);
}

void sendSelectOption(const char* sid, int index) {
  char buf[96];
  if (buildSelectOption(buf, sizeof(buf), sid, index)) Net::queueOutbound(buf);
}

void sendSessionEscape(const char* sid) {
  char buf[160];
  if (buildSessionEscape(buf, sizeof(buf), sid)) Net::queueOutbound(buf);
}

void sendApprove(const char* requestId, const char* sid, bool approve) {
  if (requestId && requestId[0]) {
    sendPermissionDecision(requestId, approve ? "allow" : "deny");
  } else if (sid && sid[0]) {
    if (approve)
      sendSelectOption(sid, 0);
    else
      sendSessionEscape(sid);
  } else if (approve) {
    // No routable id at all — emit a sessionId-less select_option so the daemon
    // applies it to its focused session. (Deny needs a target session to escape,
    // so it cannot be expressed without an id and is intentionally dropped.)
    sendSelectOption(nullptr, 0);
  }
}

}  // namespace Commands
}  // namespace AgentDeck
