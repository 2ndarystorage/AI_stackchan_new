#pragma once
// HTTP レスポンス本文の読み取り（第9.22弾で天気取得のために切り出し）。
// TTS ダウンロード（WebVoiceVoxTTS.cpp の DlReader）や電車の走査と同じ作法で、
// 生ストリームをバッファ経由で少しずつ読み、Transfer-Encoding: chunked の
// 場合はフレーミングを剥がして本文だけを返す。Content-Length 形式
// （identity）でもそのまま使える。
// 理由づけ: chunked のまま String に入れると先頭にチャンクサイズ行が混ざり、
// deserializeJson が失敗する（v9.21 実機で天気だけがこの症状になった）。
// 依存は Arduino の Client と String、millis()/delay() のみ。
// テスト容易性のため、このヘッダは単独でコンパイルできる形を保つこと。

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

// 生ストリームをデコードしながら読むリーダ。
// chunked=false なら素通し、true ならチャンク枠を剥がす。
// read() の戻り status: 1=成功, 0=無着信タイムアウト, -1=接続終了,
// -2=全体締切, -3=フレーミング異常
struct ChunkReader {
  Client *s = nullptr;
  bool chunked = false;
  uint32_t t0 = 0;         // 全体締切の起点
  uint32_t timeoutMs = 12000;
  uint32_t stallMs = 5000; // 無着信を許す時間（TTS DlReader の TTS_DL_STALL_MS と同旨）
  uint32_t lastData = 0;
  uint8_t buf[1024];
  size_t pos = 0;
  size_t len = 0;
  // chunked デコーダの状態: 0=サイズ行, 1=データ, 2=データ直後の改行読み捨て
  int chunkState = 0;
  uint32_t chunkLeft = 0;
  bool chunkDone = false;  // 終端チャンク（サイズ0＋トレーラ）まで読み切った

  void begin(Client *stream, bool isChunked, uint32_t timeout) {
    s = stream;
    chunked = isChunked;
    t0 = millis();
    timeoutMs = timeout;
    lastData = t0;
    pos = len = 0;
    chunkState = 0;
    chunkLeft = 0;
    chunkDone = false;
  }

  // 生バッファを補充する。戻り値は read() の status と同じ
  int fill() {
    if (pos < len) return 1;
    pos = 0;
    len = 0;
    for (;;) {
      if ((int32_t)(millis() - t0) >= (int32_t)timeoutMs) return -2;
      size_t avail = s->available();
      if (avail > 0) {
        int n = s->read(buf, avail < sizeof(buf) ? avail : sizeof(buf));
        if (n > 0) {
          len = (size_t)n;
          lastData = millis();
          return 1;
        }
        delay(1);
        continue;
      }
      if (!s->connected()) return -1;
      if ((int32_t)(millis() - lastData) > (int32_t)stallMs) return 0;
      delay(1);
    }
  }

  // 生バイトを最大 n バイト読む（実際に読んだ数を返す）
  size_t readRaw(uint8_t *dst, size_t n, int *status) {
    size_t done = 0;
    *status = 1;
    while (done < n) {
      int f = fill();
      if (f != 1) { *status = f; break; }
      size_t take = len - pos;
      if (take > n - done) take = n - done;
      memcpy(dst + done, buf + pos, take);
      pos += take;
      done += take;
    }
    return done;
  }

  // 1行（\n 終端、\r は除く）を読む。戻り値は fill() の status と同じ
  int readRawLine(char *out, size_t maxlen) {
    size_t i = 0;
    for (;;) {
      int f = fill();
      if (f != 1) return f;
      uint8_t c = buf[pos++];
      if (c == '\n') break;
      if (c != '\r' && i + 1 < maxlen) out[i++] = (char)c;
    }
    out[i] = '\0';
    return 1;
  }

  // 本文バイトを最大 n バイト読む。chunked の場合はフレーミングを透過する。
  // n に満たない場合は *status に理由が入る（chunked の正常終端は status=1
  // のまま chunkDone=true になり、0 バイトを返す）
  size_t read(uint8_t *dst, size_t n, int *status) {
    if (!chunked) return readRaw(dst, n, status);
    *status = 1;
    size_t done = 0;
    while (done < n && !chunkDone) {
      if (chunkState == 0) {
        char line[48];
        int lr = readRawLine(line, sizeof(line));
        if (lr != 1) { *status = lr; break; }
        char *endp = nullptr;
        long csize = strtol(line, &endp, 16);
        if (endp == line || csize < 0) { *status = -3; break; }
        if (csize == 0) {
          // 終端チャンク: トレーラ行を空行まで読み捨てて完了を確定する
          for (;;) {
            lr = readRawLine(line, sizeof(line));
            if (lr != 1) {
              if (lr == -1) chunkDone = true;  // 終端直後に切断された場合も本文は完結
              else *status = lr;
              break;
            }
            if (line[0] == '\0') { chunkDone = true; break; }
          }
          break;
        }
        chunkLeft = (uint32_t)csize;
        chunkState = 1;
      } else if (chunkState == 1) {
        size_t want = n - done;
        if (want > chunkLeft) want = chunkLeft;
        size_t gotRaw = readRaw(dst + done, want, status);
        done += gotRaw;
        chunkLeft -= (uint32_t)gotRaw;
        if (gotRaw < want) break;
        if (chunkLeft == 0) chunkState = 2;
      } else {
        uint8_t crlf[2];
        int st = 1;
        size_t gotRaw = readRaw(crlf, 2, &st);
        if (gotRaw < 2 || crlf[0] != '\r' || crlf[1] != '\n') {
          *status = (gotRaw < 2 ? st : -3);
          break;
        }
        chunkState = 0;
      }
    }
    return done;
  }
};

// 本文を out へ読み切る。本文の最大は maxBytes（超過したら打ち切って失敗）。
// 戻り値: 完結したら true。contentLength が判明している場合はバイト数の
// 一致も確認する。*outStatus に read() の status が残る（診断用）
inline bool httpReadBody(ChunkReader& r, String& out, size_t maxBytes,
                         long contentLength, int* outStatus) {
  out = "";
  out.reserve(4096);
  int lastStatus = 1;
  bool complete = false;
  uint8_t tmp[512];
  for (;;) {
    int st = 1;
    size_t n = r.read(tmp, sizeof(tmp), &st);
    lastStatus = st;
    if (n > 0) {
      if (out.length() + n > maxBytes) {
        // 上限超過: 本文として使えないので失敗扱い（lastStatus は実読取り結果）
        out = "";
        if (outStatus) *outStatus = lastStatus;
        return false;
      }
      out.concat((const char*)tmp, (unsigned int)n);
    }
    if (r.chunked) {
      if (r.chunkDone) { complete = true; break; }
      if (st != 1) break;
      if (n == 0) break;  // 防御的終了（通常は到達しない）
    } else {
      if (contentLength >= 0) {
        if ((long)out.length() >= contentLength) { complete = true; break; }
        if (st != 1) break;
        if (n == 0) break;
      } else {
        if (st == -1) { complete = true; break; }  // 接続クローズ＝終端
        if (st != 1) break;
        if (n == 0) break;
      }
    }
  }
  if (complete && contentLength >= 0 && (long)out.length() != contentLength) {
    complete = false;  // 申告サイズと一致しない本文は信用しない
  }
  if (outStatus) *outStatus = lastStatus;
  return complete;
}
