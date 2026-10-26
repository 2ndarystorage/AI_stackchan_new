#include <cassert>
#include "../M5Core2_SG90_StackChan_VoiceText_Ataru/DeviceTimer.h"
int main() {
  DeviceTimer t;
  assert(!t.start(0, 100)); assert(!t.start(86401, 100));
  assert(t.start(180, 100)); assert(!t.start(1, 100));
  assert(t.remaining(1099) == 180); assert(t.remaining(1100) == 179);
  assert(!t.update(180099)); assert(t.update(180100)); assert(!t.update(180101));
  assert(t.remaining(180100) == 0); assert(t.expired);
  assert(t.start(2, 0xffffff00u));
  assert(t.remaining(0x2e8u) == 1); assert(t.update(0x6d0u));
  t.cancel(); assert(!t.expired && !t.active);
}
