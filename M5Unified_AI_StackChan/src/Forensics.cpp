#include "Forensics.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {
constexpr uint32_t FX_MAGIC = 0x46584C47;  // "FXLG"
constexpr int FX_RING_MAX = 96;

struct FxEntry {
  uint32_t ms;    // millis() at the event (per-boot clock)
  uint16_t seq;   // monotonic sequence across reboots (wraps at 65536)
  uint8_t kind;   // FxKind
  uint8_t tag;    // FxWriteTag / FxNetTag / FxSdTag / reset reason
};

struct FxRing {
  uint32_t magic;
  uint32_t boot_count;
  uint16_t head;   // next write index
  uint16_t count;  // valid entries (<= FX_RING_MAX)
  uint32_t next_seq;
  uint32_t reserved;
  FxEntry entries[FX_RING_MAX];
};

// RTC noinit: survives panic / WDT / software resets. Contents are garbage
// after a real power-on, which the magic + bounds checks below reject.
RTC_NOINIT_ATTR FxRing g_fx;
portMUX_TYPE g_fx_mux = portMUX_INITIALIZER_UNLOCKED;
bool g_fx_ready = false;

bool ringSane() {
  return g_fx.magic == FX_MAGIC && g_fx.head < FX_RING_MAX &&
         g_fx.count <= FX_RING_MAX;
}

const char *kindName(uint8_t k) {
  switch (k) {
    case FXK_BOOT: return "BOOT";
    case FXK_WRITE_BEGIN: return "WRITE>";
    case FXK_WRITE_END: return "<WRITE";
    case FXK_NET_BEGIN: return "NET>";
    case FXK_NET_END: return "<NET";
    case FXK_TTS_BEGIN: return "TTS>";
    case FXK_TTS_END: return "<TTS";
    case FXK_LED_GATE_CLOSE: return "LED-GATE-CLOSE";
    case FXK_LED_GATE_OPEN: return "LED-GATE-OPEN";
    case FXK_SD: return "SD";
    case FXK_WIFI: return "WIFI";
    default: return "?";
  }
}

const char *writeTagName(uint8_t t) {
  switch (t) {
    case FXW_LED_LEVEL: return "led-level";
    case FXW_VOL_LEVEL: return "vol-level";
    case FXW_APIKEY: return "apikey";
    case FXW_DATA_JSON: return "data-json";
    case FXW_SETTING_WEB: return "setting-web";
    case FXW_SEEN: return "news-seen";
    case FXW_MEMORY: return "memory-facts";
    case FXW_DIGEST_STORE: return "digest-store";
    case FXW_DIGEST_MARK: return "digest-mark";
    case FXW_VOICE_SPEAKER: return "voice-speaker";
    case FXW_VOICE_SPEED: return "voice-speed";
    case FXW_MODEL: return "model";
    case FXW_SCHEDULE: return "schedule";
    case FXW_SHOT_SAVE: return "shot-save";
    case FXW_SHOT_DELETE: return "shot-delete";
    case FXW_WAKEWORD: return "wakeword-save";
    case FXW_MODE: return "mode-save";
    case FXW_WW_ENABLE: return "ww-enable-save";
    case FXW_SCHED_ON: return "sched-save";
    default: return "write";
  }
}

const char *netTagName(uint8_t t) {
  switch (t) {
    case FXN_NHK: return "nhk";
    case FXN_GOOGLE: return "google";
    case FXN_WEATHER: return "weather";
    case FXN_TRAIN: return "train";
    case FXN_CHAT: return "chat";
    case FXN_EXTRACT: return "mem-extract";
    case FXN_STT: return "stt";
    case FXN_TTS: return "tts";
    case FXN_BLE: return "ble";
    default: return "net";
  }
}

const char *sdTagName(uint8_t t) {
  switch (t) {
    case FXSD_MOUNT_OK: return "mount-ok";
    case FXSD_DEAD: return "card-dead";
    case FXSD_SEEN_SPIFFS: return "seen-spiffs-selected";
    case FXSD_SEEN_MEMORY_ONLY: return "seen-memory-only";
    case FXSD_SEEN_SAVE_SKIPPED: return "seen-save-skipped";
    case FXSD_WRITE_FAIL: return "seen-write-fail";
    default: return "sd";
  }
}

const char *wifiTagName(uint8_t t) {
  switch (t) {
    case FXWIFI_MULTI: return "multi-ap-registered";
    case FXWIFI_CONNECT: return "connect";
    case FXWIFI_SWITCH: return "ap-switch";
    default: return "wifi";
  }
}
}  // namespace

void fxLog(uint8_t kind, uint8_t tag) {
  if (!g_fx_ready) return;
  portENTER_CRITICAL(&g_fx_mux);
  FxEntry &e = g_fx.entries[g_fx.head];
  e.ms = millis();
  e.seq = (uint16_t)(g_fx.next_seq & 0xFFFF);
  e.kind = kind;
  e.tag = tag;
  g_fx.next_seq++;
  g_fx.head = (uint16_t)((g_fx.head + 1) % FX_RING_MAX);
  if (g_fx.count < FX_RING_MAX) g_fx.count++;
  portEXIT_CRITICAL(&g_fx_mux);
}

void fxInit(esp_reset_reason_t reason) {
  const bool cold = (reason == ESP_RST_POWERON) || !ringSane();
  if (cold) {
    memset(&g_fx, 0, sizeof(g_fx));
    g_fx.magic = FX_MAGIC;
    g_fx.next_seq = 1;
  }
  g_fx.boot_count++;
  g_fx_ready = true;
  fxLog(FXK_BOOT, (uint8_t)reason);
  if (cold) Serial.println("[FX] forensic ring initialized (cold boot)");
}

void fxDump() {
  if (!ringSane()) {
    Serial.println("[FX] no forensic history");
    return;
  }
  const int n = g_fx.count;
  Serial.printf("[FX] forensic ring: %d event(s), boot #%lu "
                "(t = ms since that boot, # = sequence)\n",
                n, (unsigned long)g_fx.boot_count);
  const int start = (g_fx.head + FX_RING_MAX - n) % FX_RING_MAX;
  for (int i = 0; i < n; i++) {
    const FxEntry &e = g_fx.entries[(start + i) % FX_RING_MAX];
    const char *tn = "";
    if (e.kind == FXK_WRITE_BEGIN || e.kind == FXK_WRITE_END)
      tn = writeTagName(e.tag);
    else if (e.kind == FXK_NET_BEGIN || e.kind == FXK_NET_END)
      tn = netTagName(e.tag);
    else if (e.kind == FXK_SD)
      tn = sdTagName(e.tag);
    else if (e.kind == FXK_WIFI)
      tn = wifiTagName(e.tag);
    Serial.printf("[FX] #%u t=%lu %s %s\n", (unsigned)e.seq,
                  (unsigned long)e.ms, kindName(e.kind), tn);
  }
}
