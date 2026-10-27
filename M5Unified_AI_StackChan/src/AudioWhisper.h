#ifndef _AUDIOWHISPER_H
#define _AUDIOWHISPER_H

#include "AudioWhisper.h"

class AudioWhisper {
  byte* record_buffer;
  size_t recorded_bytes = 0;  // 実際に録音したデータ長（無音早期終了時に短くなる）
 public:
  AudioWhisper();
  ~AudioWhisper();

  const byte* GetBuffer() const { return record_buffer; }
  size_t GetSize() const;

  void Record();
};

#endif // _AUDIOWHISPER_H
