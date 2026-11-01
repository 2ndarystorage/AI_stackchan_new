#pragma once
#include <stdint.h>

// M5Unified squares both gains. Applying the user value twice makes low levels nearly silent.
template<class Speaker>
void setSpeechVolume(Speaker& speaker, uint8_t master) {
  speaker.setVolume(master);
  speaker.setChannelVolume(0, 255);
  speaker.setChannelVolume(7, 255);
}

inline uint8_t volumeToLevel(uint8_t volume) {
  if (!volume) return 0;
  unsigned level = (unsigned(volume) * 5 + 127) / 255;
  return level ? level : 1;
}
