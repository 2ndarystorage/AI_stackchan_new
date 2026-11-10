"""Exercise the production LED task with a fake LED strip and tick clock."""
import unittest

import test_tts_transport
import test_v916_diagnostics


class LedBrightnessTests(unittest.TestCase):
    def test_preview_sends_new_brightness_and_respects_network_guard(self):
        main = (test_v916_diagnostics.SRC / 'main.cpp').read_text()
        start = main.index('static const uint8_t LED_LEVEL_VALUES')
        levels = main[start:main.index(';', start) + 1]
        task = test_tts_transport.body(main, 'void ledTask(void *args)')
        test_v916_diagnostics.DiagnosticsTests().compile_run(r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
struct CHSV { CHSV(int, int, int) {} };
struct CRGB {
  uint8_t r, g, b;
  CRGB(int r=0, int g=0, int b=0) : r(r),g(g),b(b) {}
  CRGB(CHSV) : CRGB() {}
  void nscale8(int n) { r=r*n/255; g=g*n/255; b=b*n/255; }
  static const CRGB Black, Green, White;
};
const CRGB CRGB::Black{}, CRGB::Green{0,255,0}, CRGB::White{255,255,255};
constexpr int SIDE_LED_COUNT=2, FXK_LED_GATE_CLOSE=1, FXK_LED_GATE_OPEN=2;
CRGB g_side_leds[SIDE_LED_COUNT], g_led_news_color;
std::atomic<bool> g_settings_open{true};
std::atomic<uint8_t> g_led_level{4};
bool g_led_enable=true, g_led_listening=false, g_led_thinking=false;
bool g_led_news_active=false, g_periodic_on=false, gated=false, locked=false;
int g_vol_level=0, tick=0;
uint32_t g_led_dance_until=0, g_led_vol_until=0, g_led_ww_until=0;
void* mp3=nullptr;
struct { int* getBuffer() { static int n=0; return &n; } } out;
bool audioRunning() { return false; }
bool netActivityActive() { return gated; }
void fxLog(int,int) {}
uint32_t millis() { return 30 * tick; }
struct StorageLedGuard {
  StorageLedGuard() { assert(!locked); locked=true; }
  ~StorageLedGuard() { locked=false; }
};
void ledRmtDrain() { assert(locked); }
std::vector<int> scales;
struct {
  void show(int scale) {
    assert(!gated && locked);
    assert((g_side_leds[0].g == 255) == g_settings_open.load());
    scales.push_back(scale);
  }
} FastLED;
void delay(int) {
  switch (++tick) {
    case 1: g_led_level=5; break;
    case 2: break;
    case 3: gated=true; g_led_level=1; break;
    case 4: gated=false; break;
    case 5: g_led_level=0; break;
    case 6: g_settings_open=false; g_led_level=4; break;
    case 7: throw 1;
  }
}
''' + levels + '\n' + task + r'''
int main() {
  try { ledTask(nullptr); } catch (int) {}
  assert((scales == std::vector<int>{6,12,1,0,6}));
}
''')
