#pragma once
#include <cstdint>
class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  virtual bool begin() { return true; }
  virtual bool ConsumeSample(int16_t[2]) { return false; }
  virtual void flush() {}
  virtual bool stop() { return true; }
 protected:
  uint32_t hertz = 24000;
};
