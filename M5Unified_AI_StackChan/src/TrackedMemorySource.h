#pragma once
#include <AudioFileSourcePROGMEM.h>

// The decoder closes its source at EOF; retain the read position for diagnostics.
class TrackedMemorySource : public AudioFileSourcePROGMEM {
 public:
  TrackedMemorySource(const void* data, uint32_t len)
      : AudioFileSourcePROGMEM(data, len) {}
  uint32_t getPos() override {
    return isOpen() ? AudioFileSourcePROGMEM::getPos() : finalPosition;
  }
  bool close() override {
    if (isOpen()) finalPosition = AudioFileSourcePROGMEM::getPos();
    return AudioFileSourcePROGMEM::close();
  }
 private:
  uint32_t finalPosition = 0;
};
