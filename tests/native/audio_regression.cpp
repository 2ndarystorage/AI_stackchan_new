#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdint>
static uint32_t tick=0;
uint32_t millis() { return tick; }
void delay(unsigned n) { tick+=n; }
namespace m5 {
class Speaker_Class {
 public:
  bool reject=true;
  unsigned drain=0;
  bool abrupt=false;
  std::vector<int16_t> received;
  bool playRaw(const int16_t* p, size_t n, uint32_t, bool, int, uint8_t) {
    if (reject) { reject=false; return false; }
    received.insert(received.end(),p,p+n); drain=3; return true;
  }
  bool isPlaying(uint8_t) { if (drain) { --drain; return true; } return false; }
  void stop(uint8_t) { assert(abrupt || drain==0); drain=0; }
};
}
float TTS_SPEED_RATE=1.2f;
#include "../../M5Unified_AI_StackChan/src/AudioOutputM5Speaker.h"
#include "../../M5Unified_AI_StackChan/src/TrackedMemorySource.h"
int main() {
  m5::Speaker_Class speaker;
  AudioOutputM5Speaker output(&speaker);
  for(int16_t i=1;i<=1200;++i) {
    int16_t sample[2]={i,i};
    while(!output.ConsumeSample(sample)) {}
  }
  output.stop();
  assert(speaker.received.size()==2400);
  for(int i=0;i<1200;++i) assert(speaker.received[2*i]==i+1 && speaker.received[2*i+1]==i+1);
  speaker.abrupt=true;
  int16_t sample[2]={7,7};
  for(int i=0;i<321;i++) while(!output.ConsumeSample(sample)) {}
  output.setAbort(true);
  uint32_t before=tick;
  output.stop();
  assert(tick==before); // User cancellation must not wait for natural drain.
  char data[16]={}, copy[16];
  TrackedMemorySource source(data,16);
  assert(source.read(copy,9)==9 && source.getPos()==9);
  source.close(); assert(source.getPos()==9);
  source.close(); assert(source.getPos()==9);
  TrackedMemorySource eof(data,16);
  assert(eof.read(copy,16)==16); eof.close(); assert(eof.getPos()==16);
}
