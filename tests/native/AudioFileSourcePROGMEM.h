#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
// Model the dependency's EOF behavior: close clears its position.
class AudioFileSourcePROGMEM {
 public:
  AudioFileSourcePROGMEM(const void* p, uint32_t n): data(p), size(n) {}
  virtual ~AudioFileSourcePROGMEM() = default;
  virtual uint32_t getPos() { return open ? pos : 0; }
  virtual bool isOpen() { return open; }
  virtual bool close() { open=false; pos=0; return true; }
  uint32_t read(void* dst, uint32_t n) {
    if (!open) return 0;
    n=std::min(n,size-pos); memcpy(dst,static_cast<const char*>(data)+pos,n); pos+=n; return n;
  }
 private:
  const void* data;
  uint32_t size, pos=0;
  bool open=true;
};
