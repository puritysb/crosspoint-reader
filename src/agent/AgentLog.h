#pragma once
//
// AgentLog — AgentDeck integration diagnostic logger.
//
// This unit (XTeink X3) has dead USB-data, so there is NO serial console for
// debugging the AgentDeck networking work. The only durable debug channel is a
// file on the microSD card. AgentLog tees every line to BOTH the normal serial
// LOG_INF (harmless, useful on units that do have USB) and to /agentdeck.log on
// the SD card so post-mortem inspection is possible by pulling the card.
//
// Self-contained on purpose: it does NOT modify lib/Logging so it can never
// interfere with the firmware's crash-report / RTC log buffer.
//
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>

namespace AgentLog {

inline constexpr const char* kLogPath = "/agentdeck.log";

// Append one timestamped line: "[<millis>] <tag>: <message>\n".
inline void line(const char* tag, const char* fmt, ...) {
  char msg[176];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  // Mirror to the serial logger (no-op channel on dead-USB units, but free).
  LOG_INF(tag, "%s", msg);

  if (!Storage.ready()) return;

  // Append via HalStorage so this write shares storageMutex with every other SD
  // user. This runs on the loop task while the render task may be reading SD font
  // glyphs; raw SdMan/FsFile access here would drive SdFat from two tasks at once
  // and can trip the xTaskPriorityDisinherit panic (SdFat #518, see CLAUDE.md).
  HalFile f = Storage.open(kLogPath, O_WRITE | O_CREAT | O_APPEND);
  if (!f) return;

  char outLine[208];
  const int n = snprintf(outLine, sizeof(outLine), "[%lu] %s: %s\n", static_cast<unsigned long>(millis()), tag, msg);
  if (n > 0) {
    f.write(reinterpret_cast<const uint8_t*>(outLine),
            static_cast<size_t>(n < (int)sizeof(outLine) ? n : (int)sizeof(outLine) - 1));
  }
  // No explicit close: HalFile's destructor closes under the mutex (DESTRUCTOR_CLOSES_FILE).
}

}  // namespace AgentLog
