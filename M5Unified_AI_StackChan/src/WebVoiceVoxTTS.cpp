#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "WebVoiceVoxRootCA.h"
#include <AudioGeneratorMP3.h>
#include <AudioFileSourceBuffer.h>
#include "AudioFileSourceHTTPSStream.h"
//#include "AudioOutputM5Speaker.h"

//-----------------------------------------------------
extern String VOICEVOX_API_KEY;
extern AudioGeneratorMP3 *mp3;
extern AudioFileSourceBuffer *buff;
extern AudioFileSourceHTTPSStream *file;
extern int preallocateBufferSize;
extern uint8_t *preallocateBuffer;
//extern AudioOutputM5Speaker out;
void StatusCallback(void *cbData, int code, const char *string);
bool playMP3(AudioFileSourceBuffer *buff);
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
  Serial.printf("[TTS] synth start (%u bytes) heap=%u\n",
                (unsigned)strlen(text), (unsigned)ESP.getFreeHeap());

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
