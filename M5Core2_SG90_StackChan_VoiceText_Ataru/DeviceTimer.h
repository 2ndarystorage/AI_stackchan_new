#pragma once
#include <stdint.h>

// Relative monotonic deadline; works across millis() rollover for <=24h timers.
class DeviceTimer {
 public:
  bool active = false;
  bool expired = false;
  uint32_t started = 0;
  uint32_t duration = 0;
  bool start(uint32_t seconds, uint32_t now) {
    if (active || seconds < 1 || seconds > 86400) return false;
    active = true; expired = false; started = now; duration = seconds * 1000;
    return true;
  }
  void cancel() { active = false; expired = false; }
  bool update(uint32_t now) {
    if (!active || uint32_t(now - started) < duration) return false;
    active = false; expired = true; return true;
  }
  uint32_t remaining(uint32_t now) const {
    uint32_t elapsed = uint32_t(now - started);
    return active && elapsed < duration ? (duration - elapsed + 999) / 1000 : 0;
  }
};
