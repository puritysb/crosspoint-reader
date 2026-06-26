#pragma once
//
// protocol.h — TRIMMED port of AgentDeck esp32/src/net/protocol.{h,cpp}.
//
// Parses inbound bridge JSON and updates g_state. M2 handles the message types
// a display-only client needs: state_update, sessions_list, usage_update, and
// the connection/connected acknowledgements. Other message types are accepted
// and ignored (see protocol.cpp for the M3-stubbed list).
//
#include <cstddef>

namespace AgentDeck {
namespace Protocol {

// Parse one inbound JSON frame and apply it to g_state (thread-safe via mutex).
void parseMessage(const char* json, size_t length);

}  // namespace Protocol
}  // namespace AgentDeck
