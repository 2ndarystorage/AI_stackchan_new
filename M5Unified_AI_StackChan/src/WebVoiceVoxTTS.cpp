#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>  // 最大連続ブロック計測（第9.6弾）
#include "WebVoiceVoxRootCA.h"
#include <AudioGeneratorMP3.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourcePROGMEM.h>
#include "AudioFileSourceHTTPSStream.h"
//#include "AudioOutputM5Speaker.h"

//-----------------------------------------------------
extern String VOICEVOX_API_KEY;
extern AudioGeneratorMP3 *mp3;
extern AudioFileSourceBuffer *buff;
extern AudioFileSourceHTTPSStream *file;
extern AudioFileSourcePROGMEM *memfile;
extern int preallocateBufferSize;
extern uint8_t *preallocateBuffer;
//extern AudioOutputM5Speaker out;
void StatusCallback(void *cbData, int code, const char *string);
bool playMP3(AudioFileSource *src);
//-----------------------------------------------------

String https_get(const char* url, const char* root_ca) {
  String payload = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
      https.setTimeout(10000);  // 第8弾: 応答待ちの上限（従来は未設定で既定値頼み）
  
      Serial.print("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
//      if (https.begin(*client, "https://api.tts.quest/v1/voicevox/?text=こんにちは世界！)) {  // HTTPS
//      if (https.begin(*client, "https://jigsaw.w3.org/HTTP/connection.html")) {  // HTTPS&speaker=1"
        Serial.print("[HTTPS] GET...\n");
        // start connection and send HTTP header
        int httpCode = https.GET();
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf("[HTTPS] GET... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = https.getString();
          }
        } else {
          Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }  
        https.end();
      } else {
        Serial.printf("[HTTPS] Unable to connect\n");
      }
      // End extra scoping block
    }  
    delete client;
  } else {
    Serial.println("Unable to create client");
  }
  return payload;
}

/* //https_post_json(const char* url, const char* json_string, root_ca_openai);
String https_post_json(const char* url, const char* json_string, const char* root_ca) {
  String payload = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
      https.setTimeout( 25000 ); 
  
      Serial.print("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
        Serial.print("[HTTPS] POST...\n");
        // start connection and send HTTP header
        //https.setTimeout( 10000 ); 
        https.addHeader("Content-Type", "application/json");
        https.addHeader("Authorization", "Bearer YOUR_OPENAI_API_KEY");
        //int httpCode = https.GET();
        int httpCode = https.POST((uint8_t *)json_string, strlen(json_string));
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf("[HTTPS] POST... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = https.getString();
          }
        } else {
          Serial.printf("[HTTPS] POST... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }  
        https.end();
      } else {
        Serial.printf("[HTTPS] Unable to connect\n");
      }
      // End extra scoping block
    }  
    delete client;
  } else {
    Serial.println("Unable to create client");
  }
  return payload;
}
 */
bool voicevox_tts_json_status(const char* url, const char* json_key, const char* root_ca) {
  bool json_data = false;
  DynamicJsonDocument doc(1000);
  String payload = https_get(url, root_ca);
  if(payload != ""){
    Serial.println(payload);
 //    StaticJsonDocument<1000> doc;
//    JsonObject object = doc.as();
    DeserializationError error = deserializeJson(doc, payload.c_str());
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      return json_data;
    }
    json_data = doc[json_key];
//    Serial.println(json_data);
  }
  return json_data;
}

static String URLEncode(const char* msg);  // 実体は後方（従来位置）

// ==== 第8弾: 文単位パイプラインTTS の合成まわり ====
// 合成（tts.quest への GET）は文ごとに何度も発生するため、WiFiClientSecure と
// HTTPClient を使い回して TLS ハンドシェイクの繰り返しを避ける（setReuse(true)）。
// voicevox_synth_url() は合成専用タスクからのみ呼ばれる（単一スレッド前提）。
// 1文あたりの合成上限は TTS_SYNTH_TIMEOUT_MS。tts.quest のキューが混んで
// 応答が返らない場合は上限で諦め、その文をスキップして次へ進む（呼び出し側）。
static const uint32_t TTS_SYNTH_TIMEOUT_MS = 25000;
// 第9.4弾: 失敗時の再試行ポリシー。
// 429（レート制限）は即スキップせずバックオフして再試行し、接続エラー
// （SSLメモリ確保失敗を含む）は旧接続を完全に解放してから作り直して再試行する。
// いずれも再試行は最大 TTS_MAX_RETRY 回まで。
static const int TTS_MAX_RETRY = 2;
static const uint32_t TTS_ERR_RETRY_WAIT_MS = 2000;
static WiFiClientSecure *tts_client = nullptr;
static HTTPClient *tts_https = nullptr;

// 接続まわりを完全に破棄する（次回の合成で作り直す）。
// 第9.3弾の実機クラッシュ対策（addr2line で特定）：HTTPClient::end() は
// 未接続のとき内部ポインタ _client を消さずに残すため、WiFiClientSecure
// だけを delete すると、残った _client が解放済みメモリを指し、次回の
// setTimeout()→connected() で LoadProhibited になる。HTTPClient も
// ヒープに置き、クライアントと必ず対で破棄することで残留ポインタを無くす。
static void tts_forget_connection() {
  if (tts_https) {
    tts_https->end();
    delete tts_https;
    tts_https = nullptr;
  }
  if (tts_client) {
    delete tts_client;
    tts_client = nullptr;
  }
}

// 再利用ペアがそろっていなければ作る（片方だけの不整合があれば作り直す）。
static bool tts_ensure_connection() {
  if (tts_client && tts_https) return true;
  tts_forget_connection();
  tts_client = new WiFiClientSecure;
  if (!tts_client) return false;
  tts_client->setCACert(root_ca);
  tts_client->setTimeout(TTS_SYNTH_TIMEOUT_MS);
  tts_https = new HTTPClient;
  if (!tts_https) {
    delete tts_client;
    tts_client = nullptr;
    return false;
  }
  tts_https->setReuse(true);
  tts_https->setTimeout(TTS_SYNTH_TIMEOUT_MS);
  return true;
}

// tts.quest へ1文（チャンク）の合成を依頼し、再生用ストリーミングURLを返す。
// 失敗/タイムアウト時は空文字を返す（429・接続エラーは内部で再試行する）。
String voicevox_synth_url(const char* text, const char* tts_parms) {
  String req_url = String("https://api.tts.quest/v3/voicevox/synthesis?key=") + VOICEVOX_API_KEY +
                   String("&text=") + URLEncode(text) + String(tts_parms);
  uint32_t t0 = millis();
  Serial.printf("[TTS] synth start (%u bytes) heap=%u largest=%u\n",
                (unsigned)strlen(text), (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  static const char* retryAfterHeader[] = {"Retry-After"};
  String payload = "";
  for (int attempt = 0; attempt <= TTS_MAX_RETRY; attempt++) {
    if (!tts_ensure_connection()) {
      Serial.printf("[TTS] synth failed: client alloc heap=%u\n", (unsigned)ESP.getFreeHeap());
      return "";
    }
    if (!tts_https->begin(*tts_client, req_url)) {
      Serial.println("[TTS] synth failed: begin");
      tts_forget_connection();
      if (attempt < TTS_MAX_RETRY) { delay(TTS_ERR_RETRY_WAIT_MS); continue; }
      return "";
    }
    tts_https->collectHeaders(retryAfterHeader, 1);
    int httpCode = tts_https->GET();
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
      Serial.printf("[TTS] synth code: %d in %lu ms\n", httpCode, (unsigned long)(millis() - t0));
      payload = tts_https->getString();  // 本文を読み切る（接続再利用の前提）
      // 接続は再利用のため end() しない
      if (payload != "") break;
      // 本文が空＝異常応答。接続を作り直して再試行する
      Serial.println("[TTS] synth failed: empty body");
      tts_forget_connection();
      if (attempt < TTS_MAX_RETRY) { delay(TTS_ERR_RETRY_WAIT_MS); continue; }
      return "";
    }
    if (httpCode == HTTP_CODE_TOO_MANY_REQUESTS) {
      // 429: レート制限。Retry-After があれば尊重し（最大30秒）、無ければ
      // 5秒→10秒のバックオフで再試行する。制限中の接続は使い回さない。
      String ra = tts_https->header("Retry-After");
      tts_forget_connection();
      if (attempt >= TTS_MAX_RETRY) {
        Serial.printf("[TTS] synth code: 429 (retry exhausted) in %lu ms\n",
                      (unsigned long)(millis() - t0));
        return "";
      }
      uint32_t wait_ms = 5000UL * (uint32_t)(attempt + 1);
      long ra_sec = ra.toInt();
      if (ra_sec > 0) {
        wait_ms = (uint32_t)ra_sec * 1000UL;
        if (wait_ms > 30000UL) wait_ms = 30000UL;
      }
      Serial.printf("[TTS] synth code: 429; wait %lu ms and retry (%d/%d)\n",
                    (unsigned long)wait_ms, attempt + 1, TTS_MAX_RETRY);
      delay(wait_ms);
      continue;
    }
    if (httpCode > 0) {
      // その他の HTTP エラーは、その文をスキップする（接続は作り直す）
      Serial.printf("[TTS] synth code: %d in %lu ms\n", httpCode, (unsigned long)(millis() - t0));
      tts_forget_connection();
      return "";
    }
    // 接続エラー（SSL のメモリ確保失敗を含む）。旧接続の TLS コンテキストを
    // 完全に解放してから新規接続で再試行し、二重に TLS が存在する時間を無くす。
    Serial.printf("[TTS] synth failed: %s in %lu ms heap=%u\n",
                  tts_https->errorToString(httpCode).c_str(),
                  (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
    tts_forget_connection();
    if (attempt < TTS_MAX_RETRY) { delay(TTS_ERR_RETRY_WAIT_MS); continue; }
    return "";
  }
  if (payload == "") return "";

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload.c_str());
  if (error) {
    Serial.print(F("[TTS] synth JSON parse failed: "));
    Serial.println(error.f_str());
    return "";
  }
  if (!doc["success"]) {
    Serial.println("[TTS] synth result: success=false");
    return "";
  }
  const char* mp3url = doc["mp3StreamingUrl"];
  if (!mp3url) return "";
  Serial.printf("[TTS] synth OK in %lu ms (isApiKeyValid: %s)\n",
                (unsigned long)(millis() - t0), doc["isApiKeyValid"] ? "OK" : "NG");
  return String(mp3url);
}

// ==== 第9.6弾: MP3ダウンロード（TLS一本化）とメモリ再生 ====
// 第9.4/9.5系の実機で、合成用TLSと再生ストリーム用TLSが同時存在すると、
// 内蔵ヒープ不足（合成開始時点で空き約55KB・断片化）により再生側の
// SSLメモリ確保が失敗し、ニュースが無音になる問題が確認された。
// そこで再生経路を変更する：文の音声はまずMP3を丸ごとPSRAMへ
// ダウンロードし（この間だけTLSを使う）、接続を閉じてからメモリ上の
// データで再生する。再生中に次文を合成・ダウンロードしても、TLS接続は
// 常に最大1本だけになる。
// ダウンロードは合成と同じクライアントペアを使い回す（接続先ホストが
// 変わるため接続自体は張り替えるが、TLSコンテキストが同時に2つ存在する
// ことはない）。接続を閉じるタイミングで TLS バッファは解放される。
//
// ==== 第9.8弾: ダウンロードの完全性保証と再生末尾対策 ====
// 第9.7弾の実機で、再生した文が MP3:ERROR_BUFLEN で途中停止し、音が
// 細切れになる問題が確認された。原因は次のとおり（実測・実コードで確認）。
// (1) tts.quest の mp3StreamingUrl（audio*.tts.quest の audio.mp3s）は
//     Content-Length が無く Transfer-Encoding: chunked で送られる。
//     従来は HTTPClient::getStreamPtr() の生ストリームを読んでいたため、
//     チャンク境界ごとにフレーミング文字列（"\r\n<16進サイズ>\r\n"）が
//     MP3データへ混入し、デコーダがその都度 desync して音が途切れた。
//     さらに終端判定が「チャンク形式は2秒無着信で完了」だったため、
//     低速受信（実測 約30KBに5秒）の途中の無通信を完了と誤認し、
//     末尾の欠けたMP3を再生することがあった（→ ERROR_BUFLEN で停止）。
// (2) libmad は最終フレームのデコードに後続バイト（MAD_BUFFER_GUARD=8）を
//     必要とするため、完全なMP3でもデータがフレーム境界でぴったり
//     終わると末尾で BUFLEN になり得る。→ 検証済みデータの末尾に
//     ゼロのパディングを付けてから再生側へ渡す。
// 第9.8弾の方式:
// - Transfer-Encoding と Content-Length をヘッダで判定し、チャンク形式は
//   プロトコルどおりに解読して終端チャンク（サイズ0）で完了を確定する。
//   Content-Length がある場合は受信バイト数と必ず突き合わせる。
//   無着信は「失敗」としてのみ扱い、完了とは絶対にみなさない。
// - 受信後、MP3フレーム列を先頭から走査して末尾まで過不足なく届いて
//   いることを検証する（検証NG・不完全は再試行対象とし、最終的には
//   従来のストリーム再生へフォールバックする）。
// - ダウンロード全体の締切は、低速回線で大きい文を取り切れるよう60秒。
static const size_t TTS_DL_MAX_BYTES = 768 * 1024;  // 1文MP3の上限（PSRAM 4MBに対して余裕を持たせた値）
static const uint32_t TTS_DL_TIMEOUT_MS = 60000;    // ダウンロード全体の締切
static const uint32_t TTS_DL_STALL_MS = 5000;       // データ無着信の許容（超えたら失敗。完了判定には使わない）
static const uint32_t TTS_DL_RETRY_WAIT_MS = 1500;  // 失敗時の再試行間隔（再試行は1回だけ）
static const size_t TTS_DL_GUARD_BYTES = 64;        // 末尾パディング（MAD_BUFFER_GUARD=8 に対する余裕分）

// ダウンロードしたデータが「末尾までそろったMP3」かを、フレームヘッダを
// 先頭から辿って検証する。ID3v2 は読み飛ばし、末尾の ID3v1（128バイトの
// "TAG"）と短いゼロ埋めは許容する。フリー形式など長さを追えない場合は
// true（受入）にして、誤判定で再生を妨げない。
static bool mp3_data_complete(const uint8_t *d, size_t len) {
  if (d == nullptr || len < 4) return false;
  size_t pos = 0;
  if (len >= 10 && memcmp(d, "ID3", 3) == 0) {
    size_t id3size = ((size_t)(d[6] & 0x7F) << 21) | ((size_t)(d[7] & 0x7F) << 14) |
                     ((size_t)(d[8] & 0x7F) << 7) | (size_t)(d[9] & 0x7F);
    pos = 10 + id3size + ((d[5] & 0x10) ? 10 : 0);
    if (pos >= len) return false;
  }
  static const uint16_t kBitrateMpeg1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t kBitrateMpeg2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  static const uint32_t kRateMpeg1[4]  = {44100, 48000, 32000, 0};
  static const uint32_t kRateMpeg2[4]  = {22050, 24000, 16000, 0};
  static const uint32_t kRateMpeg25[4] = {11025, 12000, 8000, 0};
  size_t frames = 0;
  while (pos + 4 <= len) {
    uint32_t h = ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos + 1] << 16) |
                 ((uint32_t)d[pos + 2] << 8) | (uint32_t)d[pos + 3];
    if ((h & 0xFFE00000u) != 0xFFE00000u) return false;  // 同期ビットなし
    int version = (h >> 19) & 3;   // 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=予約
    int layer = (h >> 17) & 3;     // 1=Layer III のみ対象
    if (version == 1 || layer != 1) return false;
    int brIdx = (h >> 12) & 15;
    int srIdx = (h >> 10) & 3;
    int padding = (h >> 9) & 1;
    if (brIdx == 15 || srIdx == 3) return false;
    if (brIdx == 0) return true;   // フリー形式は長さを追えないので検証を打ち切る（受入）
    uint32_t bitrate = (uint32_t)((version == 3 ? kBitrateMpeg1L3 : kBitrateMpeg2L3)[brIdx]) * 1000;
    uint32_t rate = (version == 3 ? kRateMpeg1 : (version == 2 ? kRateMpeg2 : kRateMpeg25))[srIdx];
    if (bitrate == 0 || rate == 0) return false;
    size_t frameLen = (size_t)((version == 3 ? 144u : 72u) * bitrate / rate) + (size_t)padding;
    if (frameLen < 4 || pos + frameLen > len) return false;  // 末尾が欠けている
    pos += frameLen;
    frames++;
  }
  if (frames == 0) return false;
  if (pos == len) return true;
  size_t rest = len - pos;
  if (rest == 128 && memcmp(d + pos, "TAG", 3) == 0) return true;  // ID3v1
  if (rest < 512) {  // 末尾の短いゼロ埋めは許容する
    bool allZero = true;
    for (size_t i = pos; i < len; i++) {
      if (d[i] != 0) { allZero = false; break; }
    }
    if (allZero) return true;
  }
  return false;
}

// getStreamPtr() の生ストリームをバッファ付きで読むための小さなリーダ。
// チャンク解読と終端判定を自前で行うために使う（合成タスク専用）。
struct DlReader {
  WiFiClient *s;
  uint32_t t0;
  uint32_t lastData;
  uint8_t buf[1024];
  size_t pos;
  size_t len;
  // バッファを補充する。戻り値: 1=データあり, 0=無着信タイムアウト,
  // -1=接続終了（これ以上来ない）, -2=全体締切
  int fill() {
    if (pos < len) return 1;
    pos = 0;
    len = 0;
    for (;;) {
      if ((int32_t)(millis() - t0) >= (int32_t)TTS_DL_TIMEOUT_MS) return -2;
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
      if ((int32_t)(millis() - lastData) > (int32_t)TTS_DL_STALL_MS) return 0;
      delay(1);
    }
  }
  // 最大 n バイト読み、実際に読んだ数を返す。n に満たない場合は *status に理由
  size_t read(uint8_t *dst, size_t n, int *status) {
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
  // 1行（\n 終端、\r は除く）を読む。戻り値: 1=成功, それ以外は fill の理由
  int readLine(char *out, size_t maxlen) {
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
};

// PSRAMバッファを need バイト以上に拡張する（末尾ガード分は常に上乗せ確保）
static bool dl_ensure_cap(uint8_t **buf, size_t *cap, size_t need) {
  if (need <= *cap) return true;
  size_t newCap = *cap ? *cap : 128 * 1024;
  while (newCap < need) newCap *= 2;
  if (newCap > TTS_DL_MAX_BYTES) newCap = TTS_DL_MAX_BYTES;
  if (newCap < need) return false;
  uint8_t *nb = (uint8_t *)heap_caps_realloc(*buf, newCap + TTS_DL_GUARD_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (nb == nullptr) return false;
  *buf = nb;
  *cap = newCap;
  return true;
}

static const int DL_OK = 0;
static const int DL_TOO_LARGE = 1;
static const int DL_FAIL_RETRY = -1;    // 接続エラー・不完全・検証NG（再試行する）
static const int DL_FAIL_NORETRY = -2;  // HTTPエラー（再試行しない）

// ダウンロード1回分。成功時は *outData/*outLen（末尾ガード込み長）に
// PSRAMバッファを返す。接続は成否によらず閉じる（TLS一本化の維持）。
static int voicevox_download_once(const String& url, uint32_t t0, uint8_t** outData, size_t* outLen) {
  if (!tts_ensure_connection()) {
    Serial.println("[TTS] download failed: client alloc");
    return DL_FAIL_RETRY;
  }
  // 合成で使った接続（tts.quest向け）は閉じてからダウンロード先へ張り替える
  tts_https->end();
  if (!tts_https->begin(*tts_client, url)) {
    Serial.println("[TTS] download failed: begin");
    return DL_FAIL_RETRY;
  }
  tts_https->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  static const char *dlHeaderKeys[] = {"Content-Length", "Transfer-Encoding"};
  tts_https->collectHeaders(dlHeaderKeys, 2);
  int code = tts_https->GET();
  if (code != HTTP_CODE_OK) {
    if (code > 0) {
      // HTTPエラーは再試行しない（呼び出し側がストリーム再生へフォールバックする）
      Serial.printf("[TTS] download failed: HTTP %d\n", code);
      tts_https->end();
      return DL_FAIL_NORETRY;
    }
    // 接続エラー（SSLメモリ確保失敗を含む）
    Serial.printf("[TTS] download failed: %s heap=%u largest=%u\n",
                  tts_https->errorToString(code).c_str(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    tts_https->end();
    return DL_FAIL_RETRY;
  }
  const int size = tts_https->getSize();  // Content-Length（無ければ -1）
  String te = tts_https->header("Transfer-Encoding");
  te.toLowerCase();
  const bool chunked = te.indexOf("chunked") >= 0;
  const char *mode = chunked ? "chunked" : (size > 0 ? "identity" : "close");
  Serial.printf("[TTS] download headers: content-length=%d transfer-encoding=%s (mode=%s)\n",
                size, te.c_str(), mode);
  if (size > (int)TTS_DL_MAX_BYTES) {
    Serial.printf("[TTS] download too large: %d bytes (cap %u)\n",
                  size, (unsigned)TTS_DL_MAX_BYTES);
    tts_https->end();
    return DL_TOO_LARGE;
  }
  WiFiClient *stream = tts_https->getStreamPtr();
  if (stream == nullptr) {
    tts_https->end();
    return DL_FAIL_RETRY;
  }
  size_t cap = (size > 0) ? (size_t)size : 128 * 1024;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(cap + TTS_DL_GUARD_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) {
    Serial.println("[TTS] download failed: PSRAM alloc");
    tts_https->end();
    return DL_FAIL_RETRY;
  }
  DlReader r;
  r.s = stream;
  r.t0 = t0;
  r.lastData = millis();
  r.pos = 0;
  r.len = 0;
  size_t got = 0;
  bool complete = false;
  bool tooLarge = false;
  int failStatus = 1;
  if (chunked) {
    // チャンク形式: サイズ行→データ→CRLF を繰り返し、サイズ0で終端を確定する
    for (;;) {
      char line[48];
      int lr = r.readLine(line, sizeof(line));
      if (lr != 1) { failStatus = lr; break; }
      char *endp = nullptr;
      long csize = strtol(line, &endp, 16);
      if (endp == line || csize < 0) { failStatus = -3; break; }
      if (csize == 0) {
        // 終端チャンク: トレーラ行を空行まで読み捨てて完了を確定する
        bool trailerOk = true;
        for (;;) {
          lr = r.readLine(line, sizeof(line));
          if (lr != 1) { trailerOk = false; failStatus = lr; break; }
          if (line[0] == '\0') break;
        }
        complete = trailerOk;
        break;
      }
      if (got + (size_t)csize > TTS_DL_MAX_BYTES) { tooLarge = true; break; }
      if (!dl_ensure_cap(&buf, &cap, got + (size_t)csize)) { tooLarge = true; break; }
      int st = 1;
      size_t n = r.read(buf + got, (size_t)csize, &st);
      got += n;
      if (n < (size_t)csize) { failStatus = st; break; }
      uint8_t crlf[2];
      n = r.read(crlf, 2, &st);
      if (n < 2 || crlf[0] != '\r' || crlf[1] != '\n') {
        failStatus = (n < 2 ? st : -3);
        break;
      }
    }
  } else if (size > 0) {
    // Content-Length あり: 申告サイズぴったりまで読み、必ず突き合わせる
    while (got < (size_t)size) {
      size_t want = (size_t)size - got;
      if (want > 8192) want = 8192;
      int st = 1;
      size_t n = r.read(buf + got, want, &st);
      got += n;
      if (n < want) { failStatus = st; break; }
    }
    complete = (got == (size_t)size);
  } else {
    // 長さ不明（接続クローズで終端）: サーバーが接続を閉じたら完了。
    // 無着信は失敗として扱い、完了とはみなさない。
    for (;;) {
      if (!dl_ensure_cap(&buf, &cap, got + 8192)) { tooLarge = true; break; }
      int st = 1;
      size_t want = cap - got;
      if (want > 8192) want = 8192;
      size_t n = r.read(buf + got, want, &st);
      got += n;
      if (st == -1) { complete = true; break; }  // 接続が閉じた＝終端
      if (st != 1 || n == 0) { failStatus = st; break; }
    }
  }
  tts_https->end();  // ここでTLSを閉じる（再生中にTLS接続は残らない）
  if (tooLarge) {
    heap_caps_free(buf);
    Serial.printf("[TTS] download too large: over %u bytes cap\n", (unsigned)TTS_DL_MAX_BYTES);
    return DL_TOO_LARGE;
  }
  if (!complete) {
    heap_caps_free(buf);
    Serial.printf("[TTS] download incomplete: got %u bytes (content-length=%d, mode=%s, reason=%d)\n",
                  (unsigned)got, size, mode, failStatus);
    return DL_FAIL_RETRY;
  }
  // 受信データの完全性を MP3 フレーム列の走査で検証する
  if (!mp3_data_complete(buf, got)) {
    heap_caps_free(buf);
    Serial.printf("[TTS] download rejected: MP3 frame check failed (got %u bytes, content-length=%d, mode=%s)\n",
                  (unsigned)got, size, mode);
    return DL_FAIL_RETRY;
  }
  // libmad が最終フレームをデコードできるよう、末尾にガード用のゼロを付ける
  memset(buf + got, 0, TTS_DL_GUARD_BYTES);
  *outData = buf;
  *outLen = got + TTS_DL_GUARD_BYTES;
  Serial.printf("[TTS] download OK: %u bytes (content-length=%d, mode=%s, +%u guard) in %lu ms heap=%u largest=%u\n",
                (unsigned)got, size, mode, (unsigned)TTS_DL_GUARD_BYTES,
                (unsigned long)(millis() - t0),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return DL_OK;
}

int voicevox_download_mp3(const String& url, uint8_t** outData, size_t* outLen) {
  *outData = nullptr;
  *outLen = 0;
  uint32_t t0 = millis();
  Serial.printf("[TTS] download start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  for (int attempt = 0; attempt <= 1; attempt++) {
    int rc = voicevox_download_once(url, t0, outData, outLen);
    if (rc == DL_OK) return 0;
    if (rc == DL_TOO_LARGE) return 1;
    // 失敗時は接続まわりを完全に解放してから作り直す（第9.4弾の対管理）
    tts_forget_connection();
    if (rc == DL_FAIL_NORETRY) return -1;
    if (attempt < 1) { delay(TTS_DL_RETRY_WAIT_MS); continue; }
    return -1;
  }
  return -1;
}

// 第9.8弾: 接続ペアの明示的な解放。セッション終了後などに合成タスクから
// 呼ばれ、使い回し接続（TLSレコードバッファ等）を内蔵ヒープへ返却する。
// 合成タスクからのみ呼ぶこと（他タスクから直接呼ぶと、使用中オブジェクトを
// 破棄する競合になり得るため、main.cpp 側はフラグ経由で依頼する）。
void voicevox_release_connection() {
  if (tts_client != nullptr || tts_https != nullptr) {
    Serial.printf("[TTS] release connection heap=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  tts_forget_connection();
}

// PSRAM上のMP3データから再生を開始する（TLSは使わない）。
// AudioFileSourcePROGMEM はポインタを参照するだけなので、データ本体は
// 再生終了まで呼び出し側（main.cpp の再生管理）が保持・解放する。
bool voicevox_play_memory(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return false;
  memfile = new AudioFileSourcePROGMEM(data, (uint32_t)len);
  if (memfile == nullptr) return false;
  if (!playMP3(memfile)) {
    Serial.println("[TTS] play begin failed in generator (memory)");
    delete memfile;
    memfile = nullptr;
    return false;
  }
  return true;
}

// 取得済みのストリーミングURLから MP3 再生を開始する
bool voicevox_play_url(const String& url) {
  if (url == "") return false;
  file = new AudioFileSourceHTTPSStream(url.c_str(), root_ca);
  if (file == nullptr || !file->isOpen()) {
    // ストリームを開けなかった（TLSメモリ不足など）場合は、開いていない
    // ソースを再生側へ渡さない。その文はスキップされる（第9.4弾）。
    Serial.println("[TTS] play open failed: stream not open");
    if (file != nullptr) { delete file; file = nullptr; }
    return false;
  }
  buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
  if (!playMP3(buff)) {
    Serial.println("[TTS] play begin failed in generator");
    return false;  // buff/file の後片付けは従来どおり呼び出し側の掃除経路に任せる
  }
  return true;
}

static String URLEncode(const char* msg) {
  const char *hex = "0123456789ABCDEF";
  String encodedMsg = "";

  while (*msg != '\0') {
    if ( ('a' <= *msg && *msg <= 'z')
         || ('A' <= *msg && *msg <= 'Z')
         || ('0' <= *msg && *msg <= '9')
         || *msg  == '-' || *msg == '_' || *msg == '.' || *msg == '~' ) {
      encodedMsg += *msg;
    } else {
      encodedMsg += '%';
      encodedMsg += hex[*msg >> 4];
      encodedMsg += hex[*msg & 0xf];
    }
    msg++;
  }
  return encodedMsg;
}

// char *text0 = "みなさん、こんにちは！私の名前はスタックチャンです。よろしくね！";
// char *tts_parms0 = "&speaker=1";
// char *tts_parms01 = "&speaker=19";
// char *tts_parms02 = "&speaker=28";
// char *tts_parms03 = "&speaker=42";
// char *tts_parms04 = "&speaker=12";
// char *tts_parms05 = "&speaker=45";
// char *tts_parms06 = "&speaker=3";

// 旧 Voicevox_tts()（全文一括合成→再生）は第8弾で廃止し、
// voicevox_synth_url() + voicevox_play_url() による文単位パイプラインへ移行した。
