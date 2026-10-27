#include <M5Unified.h>
#include "AudioWhisper.h"

//constexpr size_t record_number = 300/2;
constexpr size_t record_number = 400;
//constexpr size_t record_number = 200;
constexpr size_t record_length = 150;
constexpr size_t record_size = record_number * record_length;
constexpr size_t record_samplerate = 16000;
constexpr int headerSize = 44;

AudioWhisper::AudioWhisper() {
  const auto size = record_size * sizeof(int16_t) + headerSize;
  record_buffer = static_cast<byte*>(::heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ::memset(record_buffer, 0, size);
}

AudioWhisper::~AudioWhisper() {
  delete record_buffer;
}

size_t AudioWhisper::GetSize() const {
  if (recorded_bytes > 0) return recorded_bytes + headerSize;
  return record_size * sizeof(int16_t) + headerSize;
}

int16_t* MakeHeader(byte* header) {
  const auto wavDataSize = record_number * record_length * 2;
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSizeMinus8 = wavDataSize + headerSize - 8;
  header[4] = (byte)(fileSizeMinus8 & 0xFF);
  header[5] = (byte)((fileSizeMinus8 >> 8) & 0xFF);
  header[6] = (byte)((fileSizeMinus8 >> 16) & 0xFF);
  header[7] = (byte)((fileSizeMinus8 >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 0x10;  // linear PCM
  header[17] = 0x00;
  header[18] = 0x00;
  header[19] = 0x00;
  header[20] = 0x01;  // linear PCM
  header[21] = 0x00;
  header[22] = 0x01;  // monoral
  header[23] = 0x00;
  header[24] = 0x80;  // sampling rate 16000
  header[25] = 0x3E;
  header[26] = 0x00;
  header[27] = 0x00;
  header[28] = 0x00;  // Byte/sec = 16000x2x1 = 32000
  header[29] = 0x7D;
  header[30] = 0x00;
  header[31] = 0x00;
  header[32] = 0x02;  // 16bit monoral
  header[33] = 0x00;
  header[34] = 0x10;  // 16bit
  header[35] = 0x00;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(wavDataSize & 0xFF);
  header[41] = (byte)((wavDataSize >> 8) & 0xFF);
  header[42] = (byte)((wavDataSize >> 16) & 0xFF);
  header[43] = (byte)((wavDataSize >> 24) & 0xFF);
  return (int16_t*)&header[headerSize];
}

void AudioWhisper::Record() {
  M5.Mic.begin();
  auto *wavData = MakeHeader(record_buffer);
  // 高速化: 話し始めたあと約0.8秒の無音が続いたら録音を早期終了する。
  // 声を検出できない場合は従来どおり最大長まで録音する（安全側）。
  const int kSilenceChunks = 85;          // 1チャンク=150サンプル≒9.4ms → 85≒0.8秒
  const int32_t kVoiceThreshold = 400;    // チャンク平均絶対値のしきい値
  bool hasSpoken = false;
  int silentRun = 0;
  int recordedChunks = 0;
  for (int rec_record_idx = 0; rec_record_idx < record_number; ++rec_record_idx) {
    auto data = &wavData[rec_record_idx * record_length];
    M5.Mic.record(data, record_length, record_samplerate);
    recordedChunks = rec_record_idx + 1;
    int64_t sum = 0;
    for (int i = 0; i < record_length; ++i) {
      int v = data[i];
      sum += (v < 0) ? -v : v;
    }
    int32_t meanAbs = (int32_t)(sum / record_length);
    if (meanAbs >= kVoiceThreshold) {
      hasSpoken = true;
      silentRun = 0;
    } else if (hasSpoken) {
      if (++silentRun >= kSilenceChunks) break;
    }
  }
  M5.Mic.end();
  recorded_bytes = (size_t)recordedChunks * record_length * sizeof(int16_t);
  // WAVヘッダ内のサイズを実際の録音長に合わせて書き直す
  unsigned int dataBytes = (unsigned int)recorded_bytes;
  unsigned int fileSizeMinus8 = dataBytes + headerSize - 8;
  record_buffer[4] = (byte)(fileSizeMinus8 & 0xFF);
  record_buffer[5] = (byte)((fileSizeMinus8 >> 8) & 0xFF);
  record_buffer[6] = (byte)((fileSizeMinus8 >> 16) & 0xFF);
  record_buffer[7] = (byte)((fileSizeMinus8 >> 24) & 0xFF);
  record_buffer[40] = (byte)(dataBytes & 0xFF);
  record_buffer[41] = (byte)((dataBytes >> 8) & 0xFF);
  record_buffer[42] = (byte)((dataBytes >> 16) & 0xFF);
  record_buffer[43] = (byte)((dataBytes >> 24) & 0xFF);
}
