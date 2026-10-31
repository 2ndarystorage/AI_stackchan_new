#include <ArduinoJson.h>
#include <esp_heap_caps.h>  // 最大連続ブロック計測（第9.6弾）
#include "Whisper.h"

namespace {
constexpr char* API_HOST = "api.openai.com";
constexpr int API_PORT = 443;
constexpr char* API_PATH = "/v1/audio/transcriptions";
}  // namespace

Whisper::Whisper(const char* root_ca, const char* api_key) : client(), key(api_key) {
  client.setCACert(root_ca);
  client.setTimeout(10000); 
  if (!client.connect(API_HOST, API_PORT)) {
    // 第9.6弾: SSLメモリ確保失敗など一時的な接続失敗は、切り分け用の
    // メモリ情報をログに出し、接続を閉じてから1回だけ再試行する
    Serial.printf("Connection failed! heap=%u largest=%u; retry in 1.5s\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    client.stop();
    delay(1500);
    if (!client.connect(API_HOST, API_PORT)) {
      Serial.println("Connection failed! (retry exhausted)");
    }
  }
}

Whisper::~Whisper() {
  client.stop();
}

String Whisper::Transcribe(AudioWhisper* audio) {
  // 第8弾: コンストラクタで接続できていない場合は送信せず即座に失敗を返す
  if (!client.connected()) {
    Serial.println("Whisper: not connected");
    return "";
  }
  char boundary[64] = "------------------------";
  for (auto i = 0; i < 2; ++i) {
    ltoa(random(0x7fffffff), boundary + strlen(boundary), 16);
  }
  const String header = "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
    "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\nja\r\n"
    "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"speak.wav\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\n";
  const String footer = "\r\n--" + String(boundary) + "--\r\n";

  // header
  client.printf("POST %s HTTP/1.1\n", API_PATH);
  client.printf("Host: %s\n", API_HOST);
  client.println("Accept: */*");
  client.printf("Authorization: Bearer %s\n", key.c_str());
  client.printf("Content-Length: %d\n", header.length() + audio->GetSize() + footer.length());
  client.printf("Content-Type: multipart/form-data; boundary=%s\n", boundary);
  client.println();
  client.print(header.c_str());
  client.flush();

  auto ptr = audio->GetBuffer();
  auto remainings = audio->GetSize();
  while (remainings > 0) {
    auto sz = (remainings > 512) ? 512 : remainings;
    client.write(ptr, sz);
    client.flush();
    remainings -= sz;
    ptr += sz;
  }
  client.flush();

  // footer
  client.print(footer.c_str());
  client.flush();

  // wait response
  const auto now = ::millis();
  while (client.available() == 0) {
    if (::millis() - now > 10000) {
      Serial.println(">>> Client Timeout !");
      return "";
    }
  }

  bool isBody = false;
  String body = "";
  // 第8弾: 読み取り全体に締切（20秒）を設け、データ途絶時は0.5秒の猶予で完了とする
  // （従来は available() が一瞬途切れた時点で途中終了し、結果が空になりやすかった）
  {
    const uint32_t startMs = ::millis();
    uint32_t lastDataMs = startMs;
    while (::millis() - startMs < 20000) {
      if (client.available()) {
        const auto line = client.readStringUntil('\r');
        lastDataMs = ::millis();
        if (isBody) {
          body += line;
        } else if (line.equals("\n")) {
          isBody = true;
        }
        if (!client.connected() && !client.available()) break;
      } else {
        if (!client.connected()) break;
        if (::millis() - lastDataMs > 500) break;
        delay(5);
      }
    }
    if (::millis() - startMs >= 20000) {
      Serial.println(">>> Response read timeout");
    }
  }

  StaticJsonDocument<200> doc;
  ::deserializeJson(doc, body);
  return doc["text"].as<String>();
}
