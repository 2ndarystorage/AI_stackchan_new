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
void playMP3(AudioFileSourceBuffer *buff);
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
static WiFiClientSecure *tts_client = nullptr;
static HTTPClient tts_https;

// 接続が壊れた可能性が高いときに破棄する（次回の合成で作り直す）
static void tts_forget_connection() {
  tts_https.end();
  if (tts_client) {
    delete tts_client;
    tts_client = nullptr;
  }
}

// tts.quest へ1文（チャンク）の合成を依頼し、再生用ストリーミングURLを返す。
// 失敗/タイムアウト時は空文字を返す。
String voicevox_synth_url(const char* text, const char* tts_parms) {
  String req_url = String("https://api.tts.quest/v3/voicevox/synthesis?key=") + VOICEVOX_API_KEY +
                   String("&text=") + URLEncode(text) + String(tts_parms);
  uint32_t t0 = millis();
  Serial.printf("[TTS] synth start (%u bytes)\n", (unsigned)strlen(text));

  if (!tts_client) {
    tts_client = new WiFiClientSecure;
    if (!tts_client) {
      Serial.println("[TTS] synth failed: client alloc");
      return "";
    }
    tts_client->setCACert(root_ca);
    tts_client->setTimeout(TTS_SYNTH_TIMEOUT_MS);
  }
  tts_https.setReuse(true);
  tts_https.setTimeout(TTS_SYNTH_TIMEOUT_MS);

  String payload = "";
  if (tts_https.begin(*tts_client, req_url)) {
    int httpCode = tts_https.GET();
    if (httpCode > 0) {
      Serial.printf("[TTS] synth code: %d in %lu ms\n", httpCode, (unsigned long)(millis() - t0));
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        payload = tts_https.getString();  // 本文を読み切る（接続再利用の前提）
        // 接続は再利用のため end() しない
      } else {
        // エラー応答は本文が残っている可能性があるため、再利用せず切断する
        tts_https.end();
      }
    } else {
      Serial.printf("[TTS] synth failed: %s in %lu ms\n",
                    tts_https.errorToString(httpCode).c_str(), (unsigned long)(millis() - t0));
      tts_forget_connection();
      return "";
    }
  } else {
    Serial.println("[TTS] synth failed: begin");
    tts_forget_connection();
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
  buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
  playMP3(buff);
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
