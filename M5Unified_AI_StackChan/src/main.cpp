#include <Arduino.h>
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <nvs.h>
#include <Avatar.h>
#include "KinakoFace.h"  // きなこ写真の顔（カスタムFace）※PenguinFace.h は残置

#include <AudioOutput.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include "AudioFileSourceHTTPSStream.h"
#include "AudioOutputM5Speaker.h"
#include <ServoEasing.hpp> // https://github.com/ArminJo/ServoEasing       
#include "WebVoiceVoxTTS.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "rootCACertificate.h"
#include "rootCAgoogle.h"
#include "rootCANHK.h"
#include <ArduinoJson.h>
#include <ESP32WebServer.h>
#define ENABLE_MDNS
#ifdef ENABLE_MDNS
#define MDNS_HOST "stack-chan"
#include <ESPmDNS.h>
#endif
#include <deque>
#include "AudioWhisper.h"
#include "Whisper.h"
#include "Audio.h"
#include "CloudSpeechClient.h"
#include "WakeWord.h"

// 保存する質問と回答の最大数
const int MAX_HISTORY = 5;

// 過去の質問と回答を保存するデータ構造
std::deque<String> chatHistory;

#define USE_SDCARD
#define WIFI_SSID "SET YOUR WIFI SSID"
#define WIFI_PASS "SET YOUR WIFI PASS"
#define OPENAI_APIKEY "SET YOUR OPENAI APIKEY"
#define VOICEVOX_APIKEY "SET YOUR VOICEVOX APIKEY"
#define STT_APIKEY "SET YOUR STT APIKEY"

#define USE_SERVO
#ifdef USE_SERVO
#if defined(ARDUINO_M5STACK_Core2)
#define PORT_A
//#define PORT_C
#if defined (PORT_A)
  #define SERVO_PIN_X 33  //Core2 PORT A
  #define SERVO_PIN_Y 32
#elif defined (PORT_C)
  #define SERVO_PIN_X 14  //Core2 PORT C (INTERNAL UART2)
  #define SERVO_PIN_Y 13
#endif
#elif defined( ARDUINO_M5STACK_FIRE )
  #define SERVO_PIN_X 21
  #define SERVO_PIN_Y 22
#elif defined( ARDUINO_M5Stack_Core_ESP32 )
  #define SERVO_PIN_X 21
  #define SERVO_PIN_Y 22
#elif defined( ARDUINO_M5STACK_CORES3 )
  #define SERVO_PIN_X 18  //CoreS3 PORT C
  #define SERVO_PIN_Y 17
#endif
#endif

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

using namespace m5avatar;
Avatar avatar;
const Expression expressions_table[] = {
  Expression::Neutral,
  Expression::Happy,
  Expression::Sleepy,
  Expression::Doubt,
  Expression::Sad,
  Expression::Angry
};

ESP32WebServer server(80);

//---------------------------------------------
String OPENAI_API_KEY = "";
String VOICEVOX_API_KEY = "";
String STT_API_KEY = "";
String TTS_SPEAKER_NO = "3";
String TTS_SPEAKER = "&speaker=";
String TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
// 話速: 再生側の速度倍率（AudioOutputM5Speaker が playRaw のサンプルレートに乗算する）。
// 既定は1.2倍。/voice ページで変更でき、NVS "setting"/"speed_x100" に保存される
float TTS_SPEED_RATE = 1.2f;
// 応答高速化: /model ページで選んだモデル（NVS "setting"/"model"）。空ならロール側の指定を使う
String CHAT_MODEL = "";
// 応答高速化: 回答の最大トークン数（ロール側に指定が無い場合のみ適用）
const int CHAT_MAX_TOKENS = 150;
//---------------------------------------------
bool wakeword_is_enable = false;
//---------------------------------------------
// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <title>AIｽﾀｯｸﾁｬﾝ</title>
</head>)KEWL";

static const char APIKEY_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>APIキー設定</title>
  </head>
  <body>
    <h1>APIキー設定</h1>
    <form>
      <label for="role1">OpenAI API Key</label>
      <input type="text" id="openai" name="openai" oninput="adjustSize(this)"><br>
      <label for="role2">VoiceVox API Key</label>
      <input type="text" id="voicevox" name="voicevox" oninput="adjustSize(this)"><br>
      <label for="role3">Speech to Text API Key</label>
      <input type="text" id="sttapikey" name="sttapikey" oninput="adjustSize(this)"><br>
      <button type="button" onclick="sendData()">送信する</button>
    </form>
    <script>
      function adjustSize(input) {
        input.style.width = ((input.value.length + 1) * 8) + 'px';
      }
      function sendData() {
        // FormDataオブジェクトを作成
        const formData = new FormData();

        // 各ロールの値をFormDataオブジェクトに追加
        const openaiValue = document.getElementById("openai").value;
        if (openaiValue !== "") formData.append("openai", openaiValue);

        const voicevoxValue = document.getElementById("voicevox").value;
        if (voicevoxValue !== "") formData.append("voicevox", voicevoxValue);

        const sttapikeyValue = document.getElementById("sttapikey").value;
        if (sttapikeyValue !== "") formData.append("sttapikey", sttapikeyValue);

	    // POSTリクエストを送信
	    const xhr = new XMLHttpRequest();
	    xhr.open("POST", "/apikey_set");
	    xhr.onload = function() {
	      if (xhr.status === 200) {
	        alert("データを送信しました！");
	      } else {
	        alert("送信に失敗しました。");
	      }
	    };
	    xhr.send(formData);
	  }
	</script>
  </body>
</html>)KEWL";

static const char ROLE_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>ロール設定</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		textarea {
			width: 80%;
			height: 200px;
			resize: both;
		}
	</style>
</head>
<body>
	<h1>ロール設定</h1>
	<form onsubmit="postData(event)">
		<label for="textarea">ここにロールを記述してください。:</label><br>
		<textarea id="textarea" name="textarea"></textarea><br><br>
		<input type="submit" value="Submit">
	</form>
	<script>
		function postData(event) {
			event.preventDefault();
			const textAreaContent = document.getElementById("textarea").value.trim();
//			if (textAreaContent.length > 0) {
				const xhr = new XMLHttpRequest();
				xhr.open("POST", "/role_set", true);
				xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
			// xhr.onload = () => {
			// 	location.reload(); // 送信後にページをリロード
			// };
			xhr.onload = () => {
				document.open();
				document.write(xhr.responseText);
				document.close();
			};
				xhr.send(textAreaContent);
//        document.getElementById("textarea").value = "";
				alert("Data sent successfully!");
//			} else {
//				alert("Please enter some text before submitting.");
//			}
		}
	</script>
</body>
</html>)KEWL";

String speech_text = "";
String speech_text_buffer = "";

// ==== ニュース内容のディスプレイ表示（吹き出しへのセグメント表示） ====
// M5Stack-Avatar の吹き出し(Balloon)は1行表示で折り返しがなく、長い文字列は
// 画面(320px)からはみ出して切れる。フォント efontJA_16 を2倍で描くため
// 全角1文字=32px・半角1文字=16px。吹き出しに収まる幅(288px=全角9文字相当)
// ごとに読み上げ文を分割し、発話に合わせて順に切り替えて表示する。
static const int NEWS_SEG_MAX = 64;
static const int NEWS_SEG_MAX_WIDTH_PX = 288;
String g_news_segments[NEWS_SEG_MAX];
int g_news_seg_count = 0;
bool g_news_disp_armed = false;    // exec_news が表示キューを用意した（次の発話開始で有効化）
bool g_news_disp_active = false;   // ニュース表示中
int g_news_disp_idx = 0;
uint32_t g_news_disp_next_ms = 0;  // 次のセグメントへ切り替える時刻
uint32_t g_news_disp_start_ms = 0; // 表示開始時刻
uint32_t g_news_disp_clear_ms = 0; // 発話終了後、吹き出しを消す時刻（0=未定）

static int newsUtf8CharLen(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

static int newsSegCharCount(const String& seg) {
  int chars = 0;
  for (int i = 0; i < (int)seg.length();) {
    i += newsUtf8CharLen((uint8_t)seg[i]);
    chars++;
  }
  return chars;
}

// セグメントの表示時間：1文字≈180ms、最小3.5秒
static uint32_t newsSegDurationMs(const String& seg) {
  uint32_t d = (uint32_t)newsSegCharCount(seg) * 180;
  return d < 3500 ? 3500 : d;
}

static void newsDispPush(const String& seg) {
  if (seg.length() == 0) return;
  if (g_news_seg_count < NEWS_SEG_MAX) {
    g_news_segments[g_news_seg_count++] = seg;
  }
}

// テキストを吹き出しに収まる幅で分割してキューへ追加（、。の直後での分割を優先）
static void newsDispAddText(const String& text) {
  int n = (int)text.length();
  int start = 0;      // 現在のセグメントの開始バイト位置
  int curW = 0;       // 現在のセグメントの推定幅(px)
  int breakPos = -1;  // 現在のセグメント内にある「、。」直後のバイト位置
  int breakW = 0;     // breakPos 時点の幅
  int i = 0;
  while (i < n) {
    int clen = newsUtf8CharLen((uint8_t)text[i]);
    if (i + clen > n) clen = 1;
    int cw = (clen == 1) ? 16 : 32;  // 半角16px / 全角32px（efontJA_16 の2倍描画）
    if (curW + cw > NEWS_SEG_MAX_WIDTH_PX && i > start) {
      if (breakPos > start && breakW >= NEWS_SEG_MAX_WIDTH_PX / 2) {
        newsDispPush(text.substring(start, breakPos));
        start = breakPos;
        curW -= breakW;
        breakPos = -1;
        breakW = 0;
      } else {
        newsDispPush(text.substring(start, i));
        start = i;
        curW = 0;
        breakPos = -1;
        breakW = 0;
      }
      continue;
    }
    curW += cw;
    if (clen == 3 && (uint8_t)text[i] == 0xE3 && (uint8_t)text[i + 1] == 0x80 &&
        ((uint8_t)text[i + 2] == 0x81 || (uint8_t)text[i + 2] == 0x82)) {
      breakPos = i + clen;  // 、(U+3001) または 。(U+3002) の直後
      breakW = curW;
    }
    i += clen;
  }
  if (start < n) newsDispPush(text.substring(start, n));
}

// ニュース表示キューを破棄する（通常会話の開始時などに呼ぶ）
void newsDispReset() {
  g_news_seg_count = 0;
  g_news_disp_armed = false;
  g_news_disp_active = false;
  g_news_disp_idx = 0;
  g_news_disp_clear_ms = 0;
}

// ==== 音量のタッチ操作（5段階） ====
// レベル1〜5 → M5.Speaker の音量値（0〜255 を等分）
static const uint8_t VOL_VALUES[5] = {51, 102, 153, 204, 255};
uint8_t g_vol_level = 3;             // 現在の音量レベル（1〜5、初期値3）
bool g_vol_gauge_active = false;     // 音量ゲージを吹き出し表示中
uint32_t g_vol_gauge_until = 0;      // ゲージを消す時刻

// 音量レベルをスピーカーへ適用する（save=true で NVS "setting"/"vol" へ保存）
static void applyVolLevel(bool save) {
  uint8_t v = VOL_VALUES[g_vol_level - 1];
  // MP3再生は AudioOutputM5Speaker が仮想チャンネル0へ playRaw するため、
  // マスター音量とチャンネル音量の両方へ設定する（既存の /setting や起動時適用と同じ経路）
  M5.Speaker.setVolume(v);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, v);
  if (save) {
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
      nvs_set_u8(nvs_handle, "vol", g_vol_level);
      nvs_close(nvs_handle);
    }
  }
}

// ==== ニュースカテゴリ選択メニュー ====
struct NewsCatDef {
  const char* label;      // メニュー表示名
  const char* intro;      // 読み上げ導入文
  int nhkCat;             // NHK RSS のカテゴリ番号（-1 = NHKなし）
  const char* googleUrl;  // フォールバック／単独取得用の Google ニュース RSS
};
static const NewsCatDef NEWS_CATS[5] = {
  {"トピックス",   "今日のニュースです。",     0,  "https://news.google.com/rss?hl=ja&gl=JP&ceid=JP:ja"},
  {"AIニュース",   "AIニュースです。",        -1, "https://news.google.com/rss/search?q=AI&hl=ja&gl=JP&ceid=JP:ja"},
  {"芸能ニュース", "芸能ニュースです。",       6,  "https://news.google.com/rss/search?q=%E8%8A%B8%E8%83%BD&hl=ja&gl=JP&ceid=JP:ja"},
  {"テック",       "テックニュースです。",    -1, "https://news.google.com/rss/search?q=%E3%83%86%E3%82%AF%E3%83%8E%E3%83%AD%E3%82%B8%E3%83%BC&hl=ja&gl=JP&ceid=JP:ja"},
  {"その他",       "その他のニュースです。",   1,  "https://news.google.com/rss/search?q=%E7%A4%BE%E4%BC%9A&hl=ja&gl=JP&ceid=JP:ja"},
};
bool g_menu_open = false;        // カテゴリ選択メニュー表示中
int g_menu_sel = 0;              // 選択中のカテゴリ（0〜4）
uint32_t g_menu_deadline = 0;    // 最後のタップから約2.5秒後に確定する時刻

// ==== 定期通知モード（BtnC短押しトグル、5分おきニュース） ====
// 状態は RAM のみ（再起動で OFF に戻る）
bool g_periodic_on = false;          // 定期通知モード
uint32_t g_periodic_next_ms = 0;     // 次回ニュース読み上げの発火時刻
DynamicJsonDocument chat_doc(1024*10);
String json_ChatString = "{\"model\": \"gpt-4o-mini\",\"messages\": [{\"role\": \"user\", \"content\": \"""\"}]}";
String Role_JSON = "";

bool init_chat_doc(const char *data)
{
  DeserializationError error = deserializeJson(chat_doc, data);
  if (error) {
    Serial.println("DeserializationError");
    return false;
  }
  String json_str; //= JSON.stringify(chat_doc);
  serializeJsonPretty(chat_doc, json_str);  // 文字列をシリアルポートに出力する
//  Serial.println(json_str);
    return true;
}

void handleRoot() {
  server.send(200, "text/html", "<html><head><meta charset=\"UTF-8\"/></head><body><pre>hello from m5stack!\n&boxdr;&boxh;&boxh;&boxh;&boxdl;\n&boxv;&#729;_&#729;&boxv;\n&boxur;&boxh;&boxh;&boxh;&boxul;</pre></body></html>");
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
//  server.send(404, "text/plain", message);
  server.send(404, "text/html", String(HEAD) + String("<body>") + message + String("</body>"));
}

void handle_speech() {
  String message = server.arg("say");
  String speaker = server.arg("voice");
  String savedParms = TTS_PARMS;  // voice指定はこの発話だけに使う（設定中の声は変えない）
  if(speaker != "") {
    TTS_PARMS = TTS_SPEAKER + speaker;
  }
  Serial.println(message);
  ////////////////////////////////////////
  // 音声の発声
  ////////////////////////////////////////
  avatar.setExpression(Expression::Happy);
  Voicevox_tts((char*)message.c_str(), (char*)TTS_PARMS.c_str());
  TTS_PARMS = savedParms;
  server.send(200, "text/plain", String("OK"));
}

String https_post_json(const char* url, const char* json_string, const char* root_ca) {
  String payload = "";
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    client -> setCACert(root_ca);
    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is 
      HTTPClient https;
      https.setTimeout( 65000 ); 
  
      Serial.print("[HTTPS] begin...\n");
      if (https.begin(*client, url)) {  // HTTPS
        Serial.print("[HTTPS] POST...\n");
        // start connection and send HTTP header
        https.addHeader("Content-Type", "application/json");
        https.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
        int httpCode = https.POST((uint8_t *)json_string, strlen(json_string));
  
        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf("[HTTPS] POST... code: %d\n", httpCode);
  
          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = https.getString();
            Serial.println("//////////////");
            Serial.println(payload);
            Serial.println("//////////////");
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

String chatGpt(String json_string) {
  String response = "";;
  avatar.setExpression(Expression::Doubt);
  avatar.setSpeechText("考え中…");
  String ret = https_post_json("https://api.openai.com/v1/chat/completions", json_string.c_str(), root_ca_openai);
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("");
  Serial.println(ret);
  if(ret != ""){
    DynamicJsonDocument doc(2000);
    DeserializationError error = deserializeJson(doc, ret.c_str());
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      avatar.setExpression(Expression::Sad);
      avatar.setSpeechText("エラーです");
      response = "エラーです";
      delay(1000);
      avatar.setSpeechText("");
      avatar.setExpression(Expression::Neutral);
    }else{
      const char* data = doc["choices"][0]["message"]["content"];
      Serial.println(data);
      response = String(data);
      std::replace(response.begin(),response.end(),'\n',' ');
    }
  } else {
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("わかりません");
    response = "わかりません";
    delay(1000);
    avatar.setSpeechText("");
    avatar.setExpression(Expression::Neutral);
  }
  return response;
}

String InitBuffer = "";

// 応答高速化の設定を chat_doc に反映する（モデル上書き＋max_tokens補完）
void applyChatSpeedSettings() {
  if (CHAT_MODEL != "") chat_doc["model"] = CHAT_MODEL;
  if (chat_doc["max_tokens"].isNull()) chat_doc["max_tokens"] = CHAT_MAX_TOKENS;
}

void handle_chat() {
  newsDispReset();  // 通常会話が始まるのでニュース表示キューは破棄する
  static String response = "";
  // tts_parms_no = 1;
  String text = server.arg("text");
  String speaker = server.arg("voice");
  if(speaker != "") {
    TTS_PARMS = TTS_SPEAKER + speaker;
  }
  Serial.println(InitBuffer);
  init_chat_doc(InitBuffer.c_str());
  applyChatSpeedSettings();
  // 質問をチャット履歴に追加
  chatHistory.push_back(text);
   // チャット履歴が最大数を超えた場合、古い質問と回答を削除
  if (chatHistory.size() > MAX_HISTORY * 2)
  {
    chatHistory.pop_front();
    chatHistory.pop_front();
  }

  for (int i = 0; i < chatHistory.size(); i++)
  {
    JsonArray messages = chat_doc["messages"];
    JsonObject systemMessage1 = messages.createNestedObject();
    if(i % 2 == 0) {
      systemMessage1["role"] = "user";
    } else {
      systemMessage1["role"] = "assistant";
    }
    systemMessage1["content"] = chatHistory[i];
  }

  String json_string;
  serializeJson(chat_doc, json_string);
  if(speech_text=="" && speech_text_buffer == "") {
    response = chatGpt(json_string);
    speech_text = response;
    //mode = 0;
    // 返答をチャット履歴に追加
    chatHistory.push_back(response);
  } else {
    response = "busy";
  }
  // Serial.printf("chatHistory.max_size %d \n",chatHistory.max_size());
  // Serial.printf("chatHistory.size %d \n",chatHistory.size());
  // for (int i = 0; i < chatHistory.size(); i++)
  // {
  //   Serial.print(i);
  //   Serial.println("= "+chatHistory[i]);
  // }
  serializeJsonPretty(chat_doc, json_string);
  Serial.println("====================");
  Serial.println(json_string);
  Serial.println("====================");
  server.send(200, "text/html", String(HEAD)+String("<body>")+response+String("</body>"));
}

void exec_chatGPT(String text) {
  newsDispReset();  // 通常会話が始まるのでニュース表示キューは破棄する
  static String response = "";
  Serial.println(InitBuffer);
  init_chat_doc(InitBuffer.c_str());
  applyChatSpeedSettings();
  // 質問をチャット履歴に追加
  chatHistory.push_back(text);
   // チャット履歴が最大数を超えた場合、古い質問と回答を削除
  if (chatHistory.size() > MAX_HISTORY * 2)
  {
    chatHistory.pop_front();
    chatHistory.pop_front();
  }

  for (int i = 0; i < chatHistory.size(); i++)
  {
    JsonArray messages = chat_doc["messages"];
    JsonObject systemMessage1 = messages.createNestedObject();
    if(i % 2 == 0) {
      systemMessage1["role"] = "user";
    } else {
      systemMessage1["role"] = "assistant";
    }
    systemMessage1["content"] = chatHistory[i];
  }

  String json_string;
  serializeJson(chat_doc, json_string);
  if(speech_text=="" && speech_text_buffer == "") {
    response = chatGpt(json_string);
    speech_text = response;
    // 返答をチャット履歴に追加
    chatHistory.push_back(response);
  } else {
    response = "busy";
  }
  // Serial.printf("chatHistory.max_size %d \n",chatHistory.max_size());
  // Serial.printf("chatHistory.size %d \n",chatHistory.size());
  // for (int i = 0; i < chatHistory.size(); i++)
  // {
  //   Serial.print(i);
  //   Serial.println("= "+chatHistory[i]);
  // }
  serializeJsonPretty(chat_doc, json_string);
  Serial.println("====================");
  Serial.println(json_string);
  Serial.println("====================");

}
/*
String Role_JSON = "";
void exec_chatGPT1(String text) {
  static String response = "";
  init_chat_doc(Role_JSON.c_str());

  String role = chat_doc["messages"][0]["role"];
  if(role == "user") {chat_doc["messages"][0]["content"] = text;}
  String json_string;
  serializeJson(chat_doc, json_string);

  response = chatGpt(json_string);
  speech_text = response;
//  server.send(200, "text/html", String(HEAD)+String("<body>")+response+String("</body>"));
}
*/
void handle_apikey() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", APIKEY_HTML);
}

void handle_apikey_set() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  // openai
  String openai = server.arg("openai");
  // voicetxt
  String voicevox = server.arg("voicevox");
  // voicetxt
  String sttapikey = server.arg("sttapikey");
 
  OPENAI_API_KEY = openai;
  VOICEVOX_API_KEY = voicevox;
  STT_API_KEY = sttapikey;
  Serial.println(openai);
  Serial.println(voicevox);
  Serial.println(sttapikey);

  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("apikey", NVS_READWRITE, &nvs_handle)) {
    nvs_set_str(nvs_handle, "openai", openai.c_str());
    nvs_set_str(nvs_handle, "voicevox", voicevox.c_str());
    nvs_set_str(nvs_handle, "sttapikey", sttapikey.c_str());
    nvs_close(nvs_handle);
  }
  server.send(200, "text/plain", String("OK"));
}

void handle_role() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", ROLE_HTML);
}

bool save_json(){
  // SPIFFSをマウントする
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return false;
  }

  // JSONファイルを作成または開く
  File file = SPIFFS.open("/data.json", "w");
  if(!file){
    Serial.println("Failed to open file for writing");
    return false;
  }

  // JSONデータをシリアル化して書き込む
  serializeJson(chat_doc, file);
  file.close();
  return true;
}

/**
 * アプリからテキスト(文字列)と共にRoll情報が配列でPOSTされてくることを想定してJSONを扱いやすい形に変更
 * 出力形式をJSONに変更
*/
void handle_role_set() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");
  if (role != "") {
//    init_chat_doc(InitBuffer.c_str());
    init_chat_doc(json_ChatString.c_str());
    JsonArray messages = chat_doc["messages"];
    JsonObject systemMessage1 = messages.createNestedObject();
    systemMessage1["role"] = "system";
    systemMessage1["content"] = role;
//    serializeJson(chat_doc, InitBuffer);
  } else {
    init_chat_doc(json_ChatString.c_str());
  }
  //会話履歴をクリア
  chatHistory.clear();

  InitBuffer="";
  serializeJson(chat_doc, InitBuffer);
  Serial.println("InitBuffer = " + InitBuffer);
  Role_JSON = InitBuffer;

  // JSONデータをspiffsへ出力する
  save_json();

  // 整形したJSONデータを出力するHTMLデータを作成する
  String html = "<html><body><pre>";
  serializeJsonPretty(chat_doc, html);
  html += "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  Serial.println(html);
  server.send(200, "text/html", html);
//  server.send(200, "text/plain", String("OK"));
};

// 整形したJSONデータを出力するHTMLデータを作成する
void handle_role_get() {

  String html = "<html><body><pre>";
  serializeJsonPretty(chat_doc, html);
  html += "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  Serial.println(html);
  server.send(200, "text/html", String(HEAD) + html);
};

void handle_face() {
  String expression = server.arg("expression");
  expression = expression + "\n";
  Serial.println(expression);
  switch (expression.toInt())
  {
    case 0: avatar.setExpression(Expression::Neutral); break;
    case 1: avatar.setExpression(Expression::Happy); break;
    case 2: avatar.setExpression(Expression::Sleepy); break;
    case 3: avatar.setExpression(Expression::Doubt); break;
    case 4: avatar.setExpression(Expression::Sad); break;
    case 5: avatar.setExpression(Expression::Angry); break;  
  } 
  server.send(200, "text/plain", String("OK"));
}

void handle_setting() {
  String value = server.arg("volume");
  String led = server.arg("led");
  String speaker = server.arg("speaker");
//  volume = volume + "\n";
  Serial.println(speaker);
  Serial.println(value);
  size_t speaker_no;
  if(speaker != ""){
    speaker_no = speaker.toInt();
    if(speaker_no > 60) {
      speaker_no = 60;
    }
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  if(value == "") value = "180";
  size_t volume = value.toInt();
  uint8_t led_onoff = 0;
  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
    if(volume > 255) volume = 255;
    nvs_set_u32(nvs_handle, "volume", volume);
    if(led != "") {
      if(led == "on") led_onoff = 1;
      else  led_onoff = 0;
      nvs_set_u8(nvs_handle, "led", led_onoff);
    }
    if(speaker != "") nvs_set_u8(nvs_handle, "speaker", speaker_no);

    nvs_close(nvs_handle);
  }
  M5.Speaker.setVolume(volume);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
  server.send(200, "text/plain", String("OK"));
}

// ==== VOICEVOX 話者切り替え（/voice ページ） ====
// tts.quest（Web版VOICEVOX）で使える主な話者。IDは60以下に限定
// （既存の /setting・起動時読み込みが 0〜60 を前提にしているため）。
struct VoiceEntry { uint8_t id; const char* name; };
static const VoiceEntry VOICE_LIST[] = {
  {3,  "ずんだもん（ノーマル）"},
  {1,  "ずんだもん（あまあま）"},
  {7,  "ずんだもん（ツンツン）"},
  {2,  "四国めたん（ノーマル）"},
  {0,  "四国めたん（あまあま）"},
  {8,  "春日部つむぎ"},
  {10, "雨晴はう"},
  {9,  "波音リツ"},
  {14, "冥鳴ひまり"},
  {16, "九州そら"},
  {20, "もち子さん"},
  {11, "玄野武宏"},
  {13, "青山龍星"},
  {21, "剣崎雌雄"},
};
static const size_t VOICE_LIST_SIZE = sizeof(VOICE_LIST) / sizeof(VOICE_LIST[0]);

static const char* voiceName(int id) {
  for (size_t i = 0; i < VOICE_LIST_SIZE; i++) {
    if (VOICE_LIST[i].id == id) return VOICE_LIST[i].name;
  }
  return "カスタム（リスト外）";
}

// ==== ニュース読み上げ（NHK RSS・見出し方式＋Googleニュース フォールバック） ====
// 診断用：直近のニュース取得結果（/news のレスポンスで確認できる）
int g_nhk_http_code = 0;
int g_google_top_http_code = 0;
int g_google_ai_http_code = 0;
// xml 内で startPos 以降の最初の <tag>...</tag> の中身を返す。見つからなければ空文字
static String xmlExtract(const String& xml, const char* tag, int startPos) {
  String openTag = String("<") + tag + ">";
  String closeTag = String("</") + tag + ">";
  int s = xml.indexOf(openTag, startPos);
  if (s < 0) return "";
  s += openTag.length();
  int e = xml.indexOf(closeTag, s);
  if (e < 0) return "";
  String v = xml.substring(s, e);
  v.trim();
  if (v.startsWith("<![CDATA[") && v.endsWith("]]>")) {
    v = v.substring(9, v.length() - 3);
    v.trim();
  }
  return v;
}

// NHKニュースRSS（カテゴリ nhkCat）を取得し、先頭3件を読み上げ用テキストにして返す。失敗時は空文字
// dispItems には表示用に「見出し＋説明」を1件ずつ格納する（吹き出し表示キュー構築用）
// intro は読み上げ文の導入（例「今日のニュースです。」）
String fetchNewsText(int nhkCat, const char* intro, String* dispItems, int maxDispItems, int& dispCount) {
  String payload = "";
  dispCount = 0;
  g_nhk_http_code = 0;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return "";
  client->setCACert(root_ca_nhk);
  {
    HTTPClient https;
    https.setTimeout(15000);
    if (https.begin(*client, String("https://www3.nhk.or.jp/rss/news/cat") + String(nhkCat) + ".xml")) {
      int httpCode = https.GET();
      g_nhk_http_code = httpCode;
      Serial.printf("[NHK RSS] GET... code: %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK) {
        payload = https.getString();
      }
      https.end();
    }
  }
  delete client;
  if (payload == "") return "";

  String result = intro;
  int pos = 0;
  int count = 0;
  while (count < 3) {
    int itemPos = payload.indexOf("<item>", pos);
    if (itemPos < 0) break;
    String title = xmlExtract(payload, "title", itemPos);
    if (title == "") break;
    String desc = xmlExtract(payload, "description", itemPos);
    result += title + "。";
    if (desc != "") result += desc + "。";
    if (dispItems && dispCount < maxDispItems) {
      String item = title + "。";
      if (desc != "") item += desc + "。";
      dispItems[dispCount++] = item;
    }
    pos = itemPos + 6;
    count++;
  }
  if (count == 0) return "";
  return result;
}

// HTMLエンティティを簡易デコードする（Googleニュースの見出し用）
static String decodeEntities(String s) {
  s.replace("&amp;", "&");
  s.replace("&quot;", "\"");
  s.replace("&#39;", "'");
  s.replace("&apos;", "'");
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  return s;
}

// Googleニュース見出し末尾の「 - 媒体名」を取り除く
static String stripMediaSuffix(String t) {
  int idx = t.lastIndexOf(" - ");
  if (idx > 0) t = t.substring(0, idx);
  return t;
}

// GoogleニュースRSSをストリームで少しずつ読みながら、<item> の <title> を最大 want 件抽出する。
// フィード全文は保持せず、抽出済み見出しだけを outTitles に残す（走査上限 120KB で打ち切り）。
static bool fetchGoogleHeadlines(const char* url, int want, String* outTitles, int& outCount, int& httpCodeOut) {
  outCount = 0;
  httpCodeOut = 0;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setCACert(root_ca_google);
  {
    HTTPClient https;
    https.setTimeout(15000);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (https.begin(*client, url)) {
      int code = https.GET();
      httpCodeOut = code;
      Serial.printf("[Google RSS] GET... code: %d\n", code);
      if (code == HTTP_CODE_OK) {
        WiFiClient *stream = https.getStreamPtr();
        if (stream) {
          const char* patItem = "<item>";
          const char* patTitle = "<title>";
          int state = 0;  // 0:<item>待ち 1:<title>待ち 2:見出し取得中
          int match = 0;
          String title = "";
          const uint32_t MAX_SCAN = 120 * 1024;
          uint32_t scanned = 0;
          uint32_t startMs = millis();
          uint8_t buf[256];
          while (outCount < want && scanned < MAX_SCAN && (millis() - startMs) < 25000) {
            size_t avail = stream->available();
            if (avail == 0) {
              if (!stream->connected()) break;
              delay(1);
              continue;
            }
            int n = stream->read(buf, avail < sizeof(buf) ? avail : sizeof(buf));
            if (n <= 0) { delay(1); continue; }
            for (int i = 0; i < n && outCount < want && scanned < MAX_SCAN; i++) {
              char c = (char)buf[i];
              scanned++;
              if (state == 2) {
                if (title.length() < 400) title += c;
                if (title.endsWith("</title>")) {
                  title = title.substring(0, title.length() - 8);
                  title.trim();
                  if (title.startsWith("<![CDATA[") && title.endsWith("]]>")) {
                    title = title.substring(9, title.length() - 3);
                    title.trim();
                  }
                  title = stripMediaSuffix(decodeEntities(title));
                  if (title != "") {
                    outTitles[outCount++] = title;
                  }
                  title = "";
                  state = 0;
                  match = 0;
                }
              } else {
                const char* pat = (state == 0) ? patItem : patTitle;
                if (c == pat[match]) {
                  match++;
                  if (pat[match] == '\0') {
                    state++;
                    match = 0;
                    if (state == 2) title = "";
                  }
                } else {
                  match = (c == pat[0]) ? 1 : 0;
                }
              }
            }
          }
          Serial.printf("[Google RSS] headlines: %d (scanned %u bytes)\n", outCount, (unsigned)scanned);
        }
      }
      https.end();
    }
  }
  delete client;
  return outCount > 0;
}

// ニュース読み上げ文を組み立てて speech_text にセットする。読み上げ文を返す（全て失敗した場合は空文字）
// あわせて吹き出し表示用のセグメント列を構築し、次の発話開始時に表示が始まるよう arm する
String exec_news() {
  newsDispReset();  // 前回の表示キューが残っていたら破棄する
  // 一般ニュース：NHK（見出し＋説明）→ 失敗時は Google トップニュースの見出しで代替
  String dispItems[5];
  int dispCount = 0;
  String text = fetchNewsText(0, "今日のニュースです。", dispItems, 5, dispCount);
  if (text == "") {
    String heads[3];
    int n = 0;
    if (fetchGoogleHeadlines("https://news.google.com/rss?hl=ja&gl=JP&ceid=JP:ja", 3, heads, n, g_google_top_http_code)) {
      text = "今日のニュースです。";
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        if (dispCount < 5) dispItems[dispCount++] = heads[i] + "。";
      }
    }
  }
  bool hasGeneral = (text != "");
  // AIニュース：Google検索フィードの見出し（失敗しても一般ニュースだけ読み上げる）
  String aiHeads[2];
  int aiCount = 0;
  {
    String heads[2];
    int n = 0;
    if (fetchGoogleHeadlines("https://news.google.com/rss/search?q=AI&hl=ja&gl=JP&ceid=JP:ja", 2, heads, n, g_google_ai_http_code)) {
      text += (text == "") ? String("AIニュースです。") : String("続いて、AIニュースです。");
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        aiHeads[aiCount++] = heads[i];
      }
    }
  }
  if (text != "") {
    // 表示用セグメント列を構築する（loop の発話開始時に吹き出し表示を開始）
    if (hasGeneral) {
      newsDispPush("今日のニュースです");
      for (int i = 0; i < dispCount; i++) newsDispAddText(dispItems[i]);
    }
    if (aiCount > 0) {
      newsDispPush("AIニュース");
      for (int i = 0; i < aiCount; i++) newsDispAddText(aiHeads[i] + "。");
    }
    if (g_news_seg_count > 0) g_news_disp_armed = true;
    speech_text = text;
  } else {
    speech_text = "ニュースを取得できませんでした";
  }
  return text;
}

// カテゴリ単独のニュース読み上げ（タッチメニュー用）
// NHKカテゴリがあれば NHK（見出し＋説明×3）を優先し、失敗時は Google 見出し×3 で代替。
// NHKのないカテゴリ（AI／テック）は最初から Google 見出し×3。
// 従来の exec_news()（トピックス＋AI連結）とは独立した読み上げにする
void exec_news_category(int cat) {
  if (cat < 0 || cat > 4) cat = 0;
  const NewsCatDef& c = NEWS_CATS[cat];
  newsDispReset();  // 前回の表示キューが残っていたら破棄する
  String dispItems[5];
  int dispCount = 0;
  String text = "";
  if (c.nhkCat >= 0) {
    text = fetchNewsText(c.nhkCat, c.intro, dispItems, 5, dispCount);
  }
  if (text == "") {
    String heads[3];
    int n = 0;
    int dummyCode = 0;
    if (fetchGoogleHeadlines(c.googleUrl, 3, heads, n, dummyCode)) {
      text = c.intro;
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        if (dispCount < 5) dispItems[dispCount++] = heads[i] + "。";
      }
    }
  }
  if (text != "") {
    // 表示用セグメント列を構築する（loop の発話開始時に吹き出し表示を開始）
    newsDispPush(c.label);
    for (int i = 0; i < dispCount; i++) newsDispAddText(dispItems[i]);
    if (g_news_seg_count > 0) g_news_disp_armed = true;
    speech_text = text;
  } else {
    speech_text = "ニュースを取得できませんでした";
  }
}

void handle_news() {
  String news = exec_news();
  String diag = String("\n[diag] NHK httpCode=") + String(g_nhk_http_code)
              + " GoogleTop httpCode=" + String(g_google_top_http_code)
              + " GoogleAI httpCode=" + String(g_google_ai_http_code);
  if (news != "") {
    server.send(200, "text/plain", news + diag);
  } else {
    server.send(200, "text/plain", String("ニュースを取得できませんでした") + diag);
  }
}

void handle_voice() {
  String set = server.arg("set");
  String message = "";
  if (set != "") {
    int n = set.toInt();
    bool ok = false;
    for (size_t i = 0; i < VOICE_LIST_SIZE; i++) {
      if (VOICE_LIST[i].id == n) ok = true;
    }
    if (ok) {
      TTS_SPEAKER_NO = String(n);
      TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
      uint32_t nvs_handle;
      if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
        nvs_set_u8(nvs_handle, "speaker", (uint8_t)n);
        nvs_close(nvs_handle);
      }
      message = "<p><b>声を「" + String(voiceName(n)) + "」に切り替えました（本体に保存済み）。</b></p>";
    } else {
      message = "<p><b>その話者IDはリストにありません。</b></p>";
    }
  }
  String speed = server.arg("speed");
  if (speed != "") {
    int sx = speed.toInt();
    if (sx >= 80 && sx <= 150) {
      TTS_SPEED_RATE = sx / 100.0f;
      uint32_t nvs_handle;
      if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
        nvs_set_u8(nvs_handle, "speed_x100", (uint8_t)sx);
        nvs_close(nvs_handle);
      }
      message += "<p><b>話速を " + String(TTS_SPEED_RATE, 2) + " 倍に切り替えました（本体に保存済み）。</b></p>";
    } else {
      message += "<p><b>話速の値は 0.8〜1.5 倍の範囲で指定してください。</b></p>";
    }
  }
  String html = String(HEAD) + "<body>";
  html += "<h1>声の切り替え（VOICEVOX）</h1>";
  html += message;
  html += "<p>現在の声: <b>" + String(voiceName(TTS_SPEAKER_NO.toInt())) +
          "（ID " + TTS_SPEAKER_NO + "）</b></p>";
  html += "<ul>";
  for (size_t i = 0; i < VOICE_LIST_SIZE; i++) {
    String id = String(VOICE_LIST[i].id);
    html += "<li>" + String(VOICE_LIST[i].name) + "（ID " + id + "） "
            "<a href=\"/voice?set=" + id + "\">この声にする</a> / "
            "<a href=\"/speech?voice=" + id +
            "&say=こんにちは、わたしはスタックチャンです\">試聴</a></li>";
  }
  html += "</ul>";
  html += "<p>「この声にする」で選んだ声は本体に保存され、再起動後も使われます。"
          "「試聴」はその場で1回だけその声で話します（設定は変わりません）。</p>";
  html += "<h2>話速（再生速度）</h2>";
  html += "<p>現在の話速: <b>" + String(TTS_SPEED_RATE, 2) + " 倍</b></p>";
  html += "<ul>";
  {
    const int speed_opts[] = {100, 110, 120, 130};
    for (int i = 0; i < 4; i++) {
      html += "<li>" + String(speed_opts[i] / 100.0f, 1) + " 倍 "
              "<a href=\"/voice?speed=" + String(speed_opts[i]) + "\">この速さにする</a></li>";
    }
  }
  html += "</ul>";
  html += "<p>再生側で速くする方式のため、速くすると声が少し高くなります。選んだ話速は本体に保存されます。</p>";
  html += "<p><a href=\"/model\">AIモデルの切り替えはこちら</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ==== AIモデル切り替え（/model ページ） ====
struct ModelEntry { const char* id; const char* name; };
static const ModelEntry MODEL_LIST[] = {
  {"gpt-4o-mini",   "gpt-4o-mini（高速・既定）"},
  {"gpt-4.1-mini",  "gpt-4.1-mini（高速・少し高性能）"},
  {"gpt-4o",        "gpt-4o（高品質・やや遅い）"},
  {"gpt-3.5-turbo", "gpt-3.5-turbo（最速・精度は低め）"},
};
static const size_t MODEL_LIST_SIZE = sizeof(MODEL_LIST) / sizeof(MODEL_LIST[0]);

void handle_model() {
  String set = server.arg("set");
  String message = "";
  if (set != "") {
    bool ok = false;
    for (size_t i = 0; i < MODEL_LIST_SIZE; i++) {
      if (set == MODEL_LIST[i].id) ok = true;
    }
    if (ok) {
      CHAT_MODEL = set;
      uint32_t nvs_handle;
      if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
        nvs_set_str(nvs_handle, "model", set.c_str());
        nvs_close(nvs_handle);
      }
      message = "<p><b>モデルを「" + set + "」に切り替えました（本体に保存済み）。</b></p>";
    } else {
      message = "<p><b>そのモデルはリストにありません。</b></p>";
    }
  }
  String current = (CHAT_MODEL != "") ? CHAT_MODEL : String(chat_doc["model"].as<const char*>());
  String html = String(HEAD) + "<body>";
  html += "<h1>AIモデルの切り替え</h1>";
  html += message;
  html += "<p>現在のモデル: <b>" + current + "</b></p>";
  html += "<ul>";
  for (size_t i = 0; i < MODEL_LIST_SIZE; i++) {
    html += "<li>" + String(MODEL_LIST[i].name) +
            " <a href=\"/model?set=" + String(MODEL_LIST[i].id) + "\">このモデルにする</a></li>";
  }
  html += "</ul>";
  html += "<p>選んだモデルは本体に保存され、再起動後も使われます。"
          "応答の速さはモデルだけでなく、回答の長さ（最大150トークンに制限中）や"
          "音声合成の時間にも左右されます。</p>";
  html += "<p><a href=\"/voice\">声の切り替えはこちら</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
AudioGeneratorMP3 *mp3;
AudioFileSourceBuffer *buff = nullptr;
int preallocateBufferSize = 30*1024;
uint8_t *preallocateBuffer;
AudioFileSourceHTTPSStream *file = nullptr;

void playMP3(AudioFileSourceBuffer *buff){
  mp3->begin(buff, &out);
}

// Called when a metadata event occurs (i.e. an ID3 tag, an ICY block, etc.
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) isUnicode; // Punt this ball for now
  // Note that the type and string may be in PROGMEM, so copy them to RAM for printf
  char s1[32], s2[64];
  strncpy_P(s1, type, sizeof(s1));
  s1[sizeof(s1)-1]=0;
  strncpy_P(s2, string, sizeof(s2));
  s2[sizeof(s2)-1]=0;
  Serial.printf("METADATA(%s) '%s' = '%s'\n", ptr, s1, s2);
  Serial.flush();
}

// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void StatusCallback(void *cbData, int code, const char *string)
{
  const char *ptr = reinterpret_cast<const char *>(cbData);
  // Note that the string may be in PROGMEM, so copy it to RAM for printf
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1)-1]=0;
  Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
  Serial.flush();
}

#ifdef USE_SERVO
#define START_DEGREE_VALUE_X 90
//#define START_DEGREE_VALUE_Y 90
#define START_DEGREE_VALUE_Y 85 //
ServoEasing servo_x;
ServoEasing servo_y;
#endif

void lipSync(void *args)
{
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    level = abs(*out.getBuffer());
    if(level<100) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
    avatar->setRotation(gazeX * 5);
    delay(50);
  }
}

bool servo_home = false;

void servo(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef USE_SERVO
    if(!servo_home)
    {
    avatar->getGaze(&gazeY, &gazeX);
    servo_x.setEaseTo(START_DEGREE_VALUE_X + (int)(15.0 * gazeX));
    if(gazeY < 0) {
      int tmp = (int)(10.0 * gazeY);
      if(tmp > 10) tmp = 10;
      servo_y.setEaseTo(START_DEGREE_VALUE_Y + tmp);
    } else {
      servo_y.setEaseTo(START_DEGREE_VALUE_Y + (int)(10.0 * gazeY));
    }
    } else {
//     avatar->setRotation(gazeX * 5);
//     float b = avatar->getBreath();
       servo_x.setEaseTo(START_DEGREE_VALUE_X); 
//     servo_y.setEaseTo(START_DEGREE_VALUE_Y + b * 5);
       servo_y.setEaseTo(START_DEGREE_VALUE_Y);
    }
    synchronizeAllServosStartAndWaitForAllServosToStop();
#endif
    delay(50);
  }
}

void Servo_setup() {
#ifdef USE_SERVO
  if (servo_x.attach(SERVO_PIN_X, START_DEGREE_VALUE_X, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE) == INVALID_SERVO) {
    Serial.print("Error attaching servo x");
  }
  if (servo_y.attach(SERVO_PIN_Y, START_DEGREE_VALUE_Y, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE) == INVALID_SERVO) {
    Serial.print("Error attaching servo y");
  }
  servo_x.setEasingType(EASE_QUADRATIC_IN_OUT);
  servo_y.setEasingType(EASE_QUADRATIC_IN_OUT);
  setSpeedForAllServos(30);

  servo_x.setEaseTo(START_DEGREE_VALUE_X); 
  servo_y.setEaseTo(START_DEGREE_VALUE_Y);
  synchronizeAllServosStartAndWaitForAllServosToStop();
#endif
}

struct box_t
{
  int x;
  int y;
  int w;
  int h;
  int touch_id = -1;

  void setupBox(int x, int y, int w, int h) {
    this->x = x;
    this->y = y;
    this->w = w;
    this->h = h;
  }
  bool contain(int x, int y)
  {
    return this->x <= x && x < (this->x + this->w)
        && this->y <= y && y < (this->y + this->h);
  }
};
static box_t box_servo;
static box_t box_stt;
static box_t box_BtnA;
static box_t box_BtnC;
static box_t box_vol_up;    // 右上：音量アップ
static box_t box_vol_down;  // 右下：音量ダウン
static box_t box_cat_prev;  // 左上：前のニュースカテゴリ
static box_t box_cat_next;  // 左下：次のニュースカテゴリ

void Wifi_setup() {
  // 前回接続時情報で接続する
  while (WiFi.status() != WL_CONNECTED) {
    M5.Display.print(".");
    Serial.print(".");
    delay(500);
    // 10秒以上接続できなかったら抜ける
    if ( 10000 < millis() ) {
      break;
    }
  }
  M5.Display.println("");
  Serial.println("");
  // 未接続の場合にはSmartConfig待受
  if ( WiFi.status() != WL_CONNECTED ) {
    WiFi.mode(WIFI_STA);
    WiFi.beginSmartConfig();
    M5.Display.println("Waiting for SmartConfig");
    Serial.println("Waiting for SmartConfig");
    while (!WiFi.smartConfigDone()) {
      delay(500);
      M5.Display.print("#");
      Serial.print("#");
      // 30秒以上接続できなかったら抜ける
      if ( 30000 < millis() ) {
        Serial.println("");
        Serial.println("Reset");
        ESP.restart();
      }
    }
    // Wi-fi接続
    M5.Display.println("");
    Serial.println("");
    M5.Display.println("Waiting for WiFi");
    Serial.println("Waiting for WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      M5.Display.print(".");
      Serial.print(".");
      // 60秒以上接続できなかったら抜ける
      if ( 60000 < millis() ) {
        Serial.println("");
        Serial.println("Reset");
        ESP.restart();
      }
    }
  }
}

String SpeechToText(bool isGoogle){
  Serial.println("\r\nRecord start!\r\n");

  String ret = "";
  if( isGoogle) {
    Audio* audio = new Audio();
    audio->Record();  
    Serial.println("Record end\r\n");
    Serial.println("音声認識開始");
    avatar.setSpeechText("わかりました");  
    CloudSpeechClient* cloudSpeechClient = new CloudSpeechClient(root_ca_google, STT_API_KEY.c_str());
    ret = cloudSpeechClient->Transcribe(audio);
    delete cloudSpeechClient;
    delete audio;
  } else {
    AudioWhisper* audio = new AudioWhisper();
    audio->Record();  
    Serial.println("Record end\r\n");
    Serial.println("音声認識開始");
    avatar.setSpeechText("わかりました");  
    Whisper* cloudSpeechClient = new Whisper(root_ca_openai, OPENAI_API_KEY.c_str());
    ret = cloudSpeechClient->Transcribe(audio);
    delete cloudSpeechClient;
    delete audio;
  }
  return ret;
}

void setup()
{
  auto cfg = M5.config();

  cfg.external_spk = true;    /// use external speaker (SPK HAT / ATOMIC SPK)
//cfg.external_spk_detail.omit_atomic_spk = true; // exclude ATOMIC SPK
//cfg.external_spk_detail.omit_spk_hat    = true; // exclude SPK HAT
//  cfg.output_power = true;
  M5.begin(cfg);

  preallocateBuffer = (uint8_t *)malloc(preallocateBufferSize);
  if (!preallocateBuffer) {
    M5.Display.printf("FATAL ERROR:  Unable to preallocate %d bytes for app\n", preallocateBufferSize);
    for (;;) { delay(1000); }
  }
{
  auto micConfig = M5.Mic.config();
  micConfig.stereo = false;
  micConfig.sample_rate = 16000;
  M5.Mic.config(micConfig);
}
  M5.Mic.begin();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 96000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk_cfg);
  }
//  M5.Speaker.begin();

  Servo_setup();
  delay(1000);

  {
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      size_t volume;
      uint8_t led_onoff;
      uint8_t speaker_no;
      nvs_get_u32(nvs_handle, "volume", &volume);
      if(volume > 255) volume = 255;
      M5.Speaker.setVolume(volume);
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
      nvs_get_u8(nvs_handle, "led", &led_onoff);
      // if(led_onoff == 1) Use_LED = true;
      // else  Use_LED = false;
      nvs_get_u8(nvs_handle, "speaker", &speaker_no);
      if(speaker_no > 60) speaker_no = 3;
      TTS_SPEAKER_NO = String(speaker_no);
      TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
      char model_buf[32] = {0};
      size_t model_len = sizeof(model_buf);
      if (ESP_OK == nvs_get_str(nvs_handle, "model", model_buf, &model_len) && model_len > 1) {
        CHAT_MODEL = String(model_buf);
      }
      uint8_t speed_x100 = 0;
      if (ESP_OK == nvs_get_u8(nvs_handle, "speed_x100", &speed_x100) && speed_x100 >= 80 && speed_x100 <= 150) {
        TTS_SPEED_RATE = speed_x100 / 100.0f;
      }
      nvs_close(nvs_handle);
    } else {
      if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
        size_t volume = 180;
        uint8_t led_onoff = 0;
        uint8_t speaker_no = 3;
        nvs_set_u32(nvs_handle, "volume", volume);
        nvs_set_u8(nvs_handle, "led", led_onoff);
        nvs_set_u8(nvs_handle, "speaker", speaker_no);
        nvs_close(nvs_handle);
        M5.Speaker.setVolume(volume);
        M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
        // Use_LED = false;
        TTS_SPEAKER_NO = String(speaker_no);
        TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
      }
    }
  }

  { // 音量レベル（タッチ操作・5段階）を NVS "setting"/"vol" から読み込んで適用する
    uint8_t lv = 0;
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      if (ESP_OK == nvs_get_u8(nvs_handle, "vol", &lv) && lv >= 1 && lv <= 5) {
        g_vol_level = lv;
      } else {
        // "vol" 未保存なら既存の "volume"（/setting の音量）から近いレベルを初期値にする
        size_t volume = 0;
        if (ESP_OK == nvs_get_u32(nvs_handle, "volume", &volume) && volume > 0) {
          int derived = (int)((volume * 5 + 127) / 255);
          if (derived < 1) derived = 1;
          if (derived > 5) derived = 5;
          g_vol_level = (uint8_t)derived;
        }
      }
      nvs_close(nvs_handle);
    }
    applyVolLevel(false);
  }

  M5.Lcd.setTextSize(2);
  Serial.println("Connecting to WiFi");
  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
#ifndef USE_SDCARD
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  OPENAI_API_KEY = String(OPENAI_APIKEY);
  VOICEVOX_API_KEY = String(VOICEVOX_APIKEY);
  STT_API_KEY = String(STT_APIKEY);
#else
  /// settings
  if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    /// wifi
    auto fs = SD.open("/wifi.txt", FILE_READ);
    if(fs) {
      size_t sz = fs.size();
      char buf[sz + 1];
      fs.read((uint8_t*)buf, sz);
      buf[sz] = 0;
      fs.close();

      int y = 0;
      for(int x = 0; x < sz; x++) {
        if(buf[x] == 0x0a || buf[x] == 0x0d)
          buf[x] = 0;
        else if (!y && x > 0 && !buf[x - 1] && buf[x])
          y = x;
      }
      WiFi.begin(buf, &buf[y]);
    } else {
       WiFi.begin();
    }

    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("apikey", NVS_READWRITE, &nvs_handle)) {
      /// radiko-premium
      fs = SD.open("/apikey.txt", FILE_READ);
      if(fs) {
        size_t sz = fs.size();
        char buf[sz + 1];
        fs.read((uint8_t*)buf, sz);
        buf[sz] = 0;
        fs.close();
  
        int y = 0;
        int z = 0;
        for(int x = 0; x < sz; x++) {
          if(buf[x] == 0x0a || buf[x] == 0x0d)
            buf[x] = 0;
          else if (!y && x > 0 && !buf[x - 1] && buf[x])
            y = x;
          else if (!z && x > 0 && !buf[x - 1] && buf[x])
            z = x;
        }

        nvs_set_str(nvs_handle, "openai", buf);
        nvs_set_str(nvs_handle, "voicevox", &buf[y]);
        nvs_set_str(nvs_handle, "sttapikey", &buf[z]);
        Serial.println("------------------------");
        Serial.println(buf);
        Serial.println(&buf[y]);
        Serial.println(&buf[z]);
        Serial.println("------------------------");
      }
      
      nvs_close(nvs_handle);
    }
    SD.end();
  } else {
    WiFi.begin();
  }

  {
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("apikey", NVS_READONLY, &nvs_handle)) {
      Serial.println("nvs_open");

      size_t length1;
      size_t length2;
      size_t length3;
      if(ESP_OK == nvs_get_str(nvs_handle, "openai", nullptr, &length1) && 
         ESP_OK == nvs_get_str(nvs_handle, "voicevox", nullptr, &length2) && 
         ESP_OK == nvs_get_str(nvs_handle, "sttapikey", nullptr, &length3) && 
        length1 && length2 && length3) {
        Serial.println("nvs_get_str");
        char openai_apikey[length1 + 1];
        char voicevox_apikey[length2 + 1];
        char stt_apikey[length3 + 1];
        if(ESP_OK == nvs_get_str(nvs_handle, "openai", openai_apikey, &length1) && 
           ESP_OK == nvs_get_str(nvs_handle, "voicevox", voicevox_apikey, &length2) &&
           ESP_OK == nvs_get_str(nvs_handle, "sttapikey", stt_apikey, &length3)) {
          OPENAI_API_KEY = String(openai_apikey);
          VOICEVOX_API_KEY = String(voicevox_apikey);
          STT_API_KEY = String(stt_apikey);
          // Serial.println(OPENAI_API_KEY);
          // Serial.println(VOICEVOX_API_KEY);
          // Serial.println(STT_API_KEY);
        }
      }
      nvs_close(nvs_handle);
    }
  }
  
#endif

  M5.Lcd.print("Connecting");
  Wifi_setup();
  M5.Lcd.println("\nConnected");

#ifdef ENABLE_MDNS
  if (MDNS.begin(MDNS_HOST)) {
    Serial.println("MDNS responder started");
    M5.Lcd.println("MDNS responder started");
  }
#endif

  Serial.printf_P(PSTR("Go to http://"));
  M5.Lcd.print("Go to http://");
  Serial.println(WiFi.localIP());
  M5.Lcd.println(WiFi.localIP());

#ifdef ENABLE_MDNS
  Serial.printf_P(PSTR("or http://%s.local"), PSTR(MDNS_HOST));
  Serial.println();
  M5.Lcd.printf("or http://%s.local", MDNS_HOST);
  M5.Lcd.println();
#endif

  delay(1000);

  server.on("/", handleRoot);

  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  // And as regular external functions:
  server.on("/speech", handle_speech);
  server.on("/face", handle_face);
  server.on("/chat", handle_chat);
  server.on("/apikey", handle_apikey);
  server.on("/setting", handle_setting);
  server.on("/voice", handle_voice);
  server.on("/model", handle_model);
  server.on("/news", handle_news);
  server.on("/apikey_set", HTTP_POST, handle_apikey_set);
  server.on("/role", handle_role);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.onNotFound(handleNotFound);

  init_chat_doc(json_ChatString.c_str());
  // SPIFFSをマウントする
  if(SPIFFS.begin(true)){
    // JSONファイルを開く
    File file = SPIFFS.open("/data.json", "r");
    if(file){
      DeserializationError error = deserializeJson(chat_doc, file);
      if(error){
        Serial.println("Failed to deserialize JSON");
        init_chat_doc(json_ChatString.c_str());
      }
      serializeJson(chat_doc, InitBuffer);
      Role_JSON = InitBuffer;
      String json_str; 
      serializeJsonPretty(chat_doc, json_str);  // 文字列をシリアルポートに出力する
      Serial.println(json_str);
    } else {
      Serial.println("Failed to open file for reading");
      init_chat_doc(json_ChatString.c_str());
    }
  } else {
    Serial.println("An Error has occurred while mounting SPIFFS");
  }

  server.begin();
  Serial.println("HTTP server started");
  M5.Lcd.println("HTTP server started");  
  
  Serial.printf_P(PSTR("/ to control the chatGpt Server.\n"));
  M5.Lcd.print("/ to control the chatGpt Server.\n");
  delay(3000);

  audioLogger = &Serial;
  mp3 = new AudioGeneratorMP3();
//  mp3->RegisterStatusCB(StatusCallback, (void*)"mp3");
  wakeword_init();

  avatar.setFace(new KinakoFace());  // きなこ写真の顔に変更（2026-10-31 本人希望）
  avatar.init(16);  // 写真をカラー表示するため 16bit カラーで初期化（従来は 1bit）
  avatar.addTask(lipSync, "lipSync");
  avatar.addTask(servo, "servo");
  avatar.setSpeechFont(&fonts::efontJA_16);

//  M5.Speaker.setVolume(200);
  // タッチレイアウト：四隅＝音量（右上/右下）とニュースカテゴリ選択（左上/左下）、
  // 中央＝会話開始(STT)、左右の帯＝独り言切替(box_BtnA)/定期通知トグル(box_BtnC)、
  // 下部中央＝サーボ切替。四隅・中央・帯が重ならないよう配置している
  box_servo.setupBox(130, 185, 60, 50);
  box_stt.setupBox(110, 80, 100, 80);
  box_BtnA.setupBox(0, 100, 40, 60);
  box_BtnC.setupBox(280, 100, 40, 60);
  box_vol_up.setupBox(230, 0, 90, 70);
  box_vol_down.setupBox(230, 170, 90, 70);
  box_cat_prev.setupBox(0, 0, 90, 70);
  box_cat_next.setupBox(0, 170, 90, 70);
}

String random_words[18] = {"あなたは誰","楽しい","怒った","可愛い","悲しい","眠い","ジョークを言って","泣きたい","怒ったぞ","こんにちは","お疲れ様","詩を書いて","疲れた","お腹空いた","嫌いだ","苦しい","俳句を作って","歌をうたって"};
int random_time = -1;
bool random_speak = true;
static int lastms1 = 0;

void report_batt_level(){
  mode = 0;
  char buff[100];
   int level = M5.Power.getBatteryLevel();
  if(M5.Power.isCharging())
    sprintf(buff,"充電中、バッテリーのレベルは%d％です。",level);
  else
    sprintf(buff,"バッテリーのレベルは%d％です。",level);
  avatar.setExpression(Expression::Happy);
  mode = 0; 
  speech_text = String(buff);
  delay(1000);
  avatar.setExpression(Expression::Neutral);
}

void switch_monologue_mode(){
    String tmp;
    mode = 0;
    if(random_speak) {
      tmp = "独り言始めます。";
      lastms1 = millis();
      random_time = 40000 + 1000 * random(30);
    } else {
      tmp = "独り言やめます。";
      random_time = -1;
    }
    random_speak = !random_speak;
    avatar.setExpression(Expression::Happy);
    mode = 0;
    speech_text = tmp;
    delay(1000);
    avatar.setExpression(Expression::Neutral);
}

void sw_tone(){
      M5.Mic.end();
        M5.Speaker.tone(1000, 100);
      delay(500);
        M5.Speaker.end();
      M5.Mic.begin();
}

void SST_ChatGPT() {
        newsDispReset();  // 通常会話が始まるのでニュース表示キューは破棄する（ニュース分岐では exec_news が再構築する）
        bool prev_servo_home = servo_home;
        random_speak = true;
        random_time = -1;
#ifdef USE_SERVO
        servo_home = true;
#endif
        avatar.setExpression(Expression::Happy);
        avatar.setSpeechText("御用でしょうか？");
        String ret;
        if(OPENAI_API_KEY != STT_API_KEY){
          Serial.println("Google STT");
          ret = SpeechToText(true);
        } else {
          Serial.println("Whisper STT");
          ret = SpeechToText(false);
        }
#ifdef USE_SERVO
        servo_home = prev_servo_home;
#endif
        Serial.println("音声認識終了");
        Serial.println("音声認識結果");
        if(ret != "") {
          Serial.println(ret);
          if (!mp3->isRunning() && speech_text=="" && speech_text_buffer == "") {
            if (ret.indexOf("ニュース") >= 0) {
              // ニュースは ChatGPT を経由せず RSS を読み上げる（会話履歴には入れない）
              avatar.setExpression(Expression::Happy);
              avatar.setSpeechText("ニュースです");
              exec_news();
            } else {
              exec_chatGPT(ret);
            }
            mode = 0;
          }
        } else {
          Serial.println("音声認識失敗");
          avatar.setExpression(Expression::Sad);
          avatar.setSpeechText("聞き取れませんでした");
          delay(2000);
          avatar.setSpeechText("");
          avatar.setExpression(Expression::Neutral);
        } 
}

// 音量ゲージ文字列（例「音量 ■■■□□」）
static String volGaugeText() {
  String s = "音量 ";
  for (int i = 0; i < 5; i++) s += (i < g_vol_level) ? "■" : "□";
  return s;
}

// 音量を delta 段階変える（右上/右下タップ）
static void change_volume(int delta) {
  int nl = (int)g_vol_level + delta;
  if (nl < 1) nl = 1;
  if (nl > 5) nl = 5;
  bool changed = (nl != (int)g_vol_level);
  g_vol_level = (uint8_t)nl;
  applyVolLevel(changed);
  if (!mp3->isRunning()) {
    sw_tone();
  } else {
    // 再生中は sw_tone（Speaker.end を伴う）で再生を止めないよう、
    // 別チャンネル(7)へ短い確認音だけを重ねる
    M5.Speaker.setChannelVolume(7, VOL_VALUES[g_vol_level - 1]);
    M5.Speaker.tone(1320.0f, 70, 7, false);
  }
  // 吹き出しが空いているときだけゲージを表示し、約2秒で消す（loop 側で消去）
  if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
      !g_news_disp_active && !g_news_disp_armed && !g_menu_open) {
    avatar.setSpeechText(volGaugeText().c_str());
    g_vol_gauge_active = true;
    g_vol_gauge_until = millis() + 2000;
  }
}

// メニュー選択中を吹き出しに表示する（例「2/5 AIニュース」）
static void menuShowBalloon() {
  String s = String(g_menu_sel + 1) + "/5 " + NEWS_CATS[g_menu_sel].label;
  avatar.setSpeechText(s.c_str());
}

// メニューのカテゴリ一覧を画面左側へ描く。
// Avatar の再描画で上書きされるため、メニュー表示中は loop のたびに描き直す
static void drawMenuList() {
  M5.Lcd.fillRect(0, 76, 176, 106, TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontJA_16);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i = 0; i < 5; i++) {
    M5.Lcd.setCursor(4, 80 + i * 20);
    M5.Lcd.print((i == g_menu_sel) ? "▶ " : "  ");
    M5.Lcd.print(NEWS_CATS[i].label);
  }
}

// 左上/左下タップ：メニューを開く／前後のカテゴリへ移動する
static void menu_step(int delta) {
  // 発話中・ニュース表示中はメニューを開かない（吹き出しを奪わない）
  if (mp3->isRunning() || speech_text != "" || speech_text_buffer != "" || g_news_disp_active) return;
  if (g_menu_open) {
    g_menu_sel = (g_menu_sel + delta + 5) % 5;
  } else {
    g_menu_open = true;
  }
  g_menu_deadline = millis() + 2500;
  sw_tone();
  menuShowBalloon();
}

// 定期通知モードの ON/OFF（物理 BtnC 短押し・タッチ box_BtnC 共用）
static void toggle_periodic() {
  sw_tone();
  bool speechFree = !mp3->isRunning() && speech_text == "" && speech_text_buffer == "";
  if (!g_periodic_on) {
    g_periodic_on = true;
    g_periodic_next_ms = millis();  // すぐ1回読み上げる（発話が空き次第 loop が発火）
    avatar.setSpeechText("定期通知オン");
    if (speechFree) speech_text = "定期通知モードをオンにしました";
  } else {
    g_periodic_on = false;
    avatar.setSpeechText("定期通知オフ");
    if (speechFree) speech_text = "定期通知モードをオフにしました";
  }
}

void loop()
{
  static int lastms = 0;

  if (random_time >= 0 && millis() - lastms1 > random_time)
  {
    lastms1 = millis();
    random_time = 40000 + 1000 * random(30);
    if (!mp3->isRunning() && speech_text=="" && speech_text_buffer == "") {
      exec_chatGPT(random_words[random(18)]);
      mode = 0;
    }
  }

  // 定期通知モード：ON で発火時刻を過ぎたらニュース（トピックス＋AI）を読み上げる。
  // 再生中・発話待ちがある場合は割り込まず、約30秒後に再試行する
  if (g_periodic_on && (int32_t)(millis() - g_periodic_next_ms) >= 0) {
    if (mode >= 0 && !mp3->isRunning() && speech_text == "" && speech_text_buffer == "") {
      g_menu_open = false;
      avatar.setSpeechText("ニュースです");
      exec_news();
      g_periodic_next_ms = millis() + 300000UL;
    } else {
      g_periodic_next_ms = millis() + 30000UL;
    }
  }

  M5.update();
  if (M5.BtnA.wasPressed())
  {
    if(mode >= 0){
      sw_tone();
      if(mode == 0){
        avatar.setSpeechText("ウェイクワード有効");
        mode = 1;
        wakeword_is_enable = true;
      } else {
        avatar.setSpeechText("ウェイクワード無効");
        mode = 0;
        wakeword_is_enable = false;
      }
      delay(1000);
      avatar.setSpeechText("");
    }
  }

  // BtnB短押し（ニュース読み上げ）検出用：押下時刻を記録する
  static uint32_t btnB_press_start = 0;
  static bool btnB_long_fired = false;
  if (M5.BtnB.wasPressed()) {
    btnB_press_start = millis();
    btnB_long_fired = false;
  }

  if (M5.BtnB.pressedFor(2000)) {
     btnB_long_fired = true;  // 長押し（ウェイクワード登録）が発動したら短押し扱いしない
     M5.Mic.end();
     M5.Speaker.tone(1000, 100);
     delay(500);
     M5.Speaker.tone(600, 100);
     delay(1000);
    M5.Speaker.end();
    M5.Mic.begin();
    random_time = -1;           //独り言停止
    random_speak = true;
    wakeword_is_enable = false; //wakeword 無効
    mode = -1;
#ifdef USE_SERVO
      servo_home = true;
      delay(500);
#endif
    avatar.setSpeechText("ウェイクワード登録開始");
  }

  // BtnBを短く押して離したらニュース読み上げ（長押し登録とは競合させない）
  if (M5.BtnB.wasReleased()) {
    if (!btnB_long_fired && btnB_press_start != 0 && (millis() - btnB_press_start) < 1500 && mode >= 0) {
      if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "") {
        sw_tone();
        avatar.setExpression(Expression::Happy);
        avatar.setSpeechText("ニュースです");
        exec_news();
      }
    }
    btnB_press_start = 0;
  }

  // BtnC短押し＝定期通知モードのトグル、長押し(2秒)＝バッテリー残量読み上げ
  static uint32_t btnC_press_start = 0;
  static bool btnC_long_fired = false;
  if (M5.BtnC.wasPressed()) {
    btnC_press_start = millis();
    btnC_long_fired = false;
  }
  if (M5.BtnC.pressedFor(2000) && !btnC_long_fired) {
    btnC_long_fired = true;  // 長押しが発動したら短押し扱いしない
    sw_tone();
    report_batt_level();
  }
  if (M5.BtnC.wasReleased()) {
    if (!btnC_long_fired && btnC_press_start != 0 && (millis() - btnC_press_start) < 1500 && mode >= 0) {
      toggle_periodic();
    }
    btnC_press_start = 0;
  }

#if defined(ARDUINO_M5STACK_Core2) || defined( ARDUINO_M5STACK_CORES3 )
  auto count = M5.Touch.getCount();
  if (count)
  {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed())
    {          
      // 右上＝音量アップ、右下＝音量ダウン（5段階）
      if (box_vol_up.contain(t.x, t.y))
      {
        change_volume(+1);
      }
      if (box_vol_down.contain(t.x, t.y))
      {
        change_volume(-1);
      }
      // 左上＝前のカテゴリ、左下＝次のカテゴリ（ニュース選択メニュー）
      if (box_cat_prev.contain(t.x, t.y))
      {
        menu_step(-1);
      }
      if (box_cat_next.contain(t.x, t.y))
      {
        menu_step(+1);
      }
      // 中央＝会話開始。メニュー表示中はキャンセルとして働く
      if (box_stt.contain(t.x, t.y))
      {
        if (g_menu_open)
        {
          g_menu_open = false;
          sw_tone();
          if (!mp3->isRunning()) avatar.setSpeechText("");
        }
        else if (!mp3->isRunning())
        {
          sw_tone();
          SST_ChatGPT();
        }
      }
#ifdef USE_SERVO
      if (box_servo.contain(t.x, t.y))
      {
        servo_home = !servo_home;
        M5.Speaker.tone(1000, 100);
      }
#endif
      if (box_BtnA.contain(t.x, t.y))
      {
        sw_tone();
        switch_monologue_mode();
      }
      // 右の帯＝定期通知モードのトグル（物理 BtnC 短押しと同じ挙動）
      if (box_BtnC.contain(t.x, t.y))
      {
        toggle_periodic();
      }
    }
  }
#endif

  // ニュースカテゴリ選択メニュー：表示中は一覧を描き続け、
  // 最後のタップから約2.5秒操作がなければ選択中カテゴリを読み上げて閉じる
  if (g_menu_open) {
    drawMenuList();
    if ((int32_t)(millis() - g_menu_deadline) >= 0) {
      if (mode >= 0 && !mp3->isRunning() && speech_text == "" && speech_text_buffer == "" && !g_news_disp_active) {
        g_menu_open = false;
        avatar.setSpeechText("ニュースです");
        exec_news_category(g_menu_sel);
      } else {
        g_menu_deadline = millis() + 2500;  // 混んでいる間は確定を少し延ばす
      }
    }
  }

  // 音量ゲージの吹き出しを約2秒で消す（発話やニュース表示・メニューが始まっていたら触らない）
  if (g_vol_gauge_active && (int32_t)(millis() - g_vol_gauge_until) >= 0) {
    g_vol_gauge_active = false;
    if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_news_disp_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  if(speech_text != ""){
    avatar.setExpression(Expression::Happy);
    speech_text_buffer = speech_text;
    speech_text = "";
    M5.Mic.end();
    M5.Speaker.begin();
    mode = 0;
    Voicevox_tts((char*)speech_text_buffer.c_str(), (char*)TTS_PARMS.c_str());
    // ニュース発話なら、内容の吹き出し表示を開始する
    if (g_news_disp_armed && g_news_seg_count > 0) {
      g_news_disp_armed = false;
      g_news_disp_active = true;
      g_news_disp_idx = 0;
      g_news_disp_start_ms = millis();
      g_news_disp_clear_ms = 0;
      avatar.setSpeechText(g_news_segments[0].c_str());
      g_news_disp_next_ms = millis() + newsSegDurationMs(g_news_segments[0]);
    }
  }

  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      if(file != nullptr){delete file; file = nullptr;}
      Serial.println("mp3 stop");
      avatar.setExpression(Expression::Neutral);
      speech_text_buffer = "";
      delay(200);
      M5.Speaker.end();
      M5.Mic.begin();
      if(wakeword_is_enable) mode = 1;
      else  mode = 0;
    }
    delay(1);
  } else {
  server.handleClient();
  }

  // ニュース内容の吹き出し表示を進める（発話中はセグメントを順に切り替え、
  // 発話が終わったら最後のセグメントを3秒残してから消す）
  if (g_news_disp_active) {
    if (mp3->isRunning()) {
      g_news_disp_clear_ms = 0;
      if (g_news_disp_idx < g_news_seg_count - 1 &&
          (int32_t)(millis() - g_news_disp_next_ms) >= 0) {
        g_news_disp_idx++;
        avatar.setSpeechText(g_news_segments[g_news_disp_idx].c_str());
        g_news_disp_next_ms = millis() + newsSegDurationMs(g_news_segments[g_news_disp_idx]);
      }
    } else if (speech_text_buffer == "" || (millis() - g_news_disp_start_ms) > 5000) {
      // 発話終了（または発話開始に失敗）：最後のセグメントを3秒残してから消す
      if (g_news_disp_clear_ms == 0) {
        if (g_news_disp_idx != g_news_seg_count - 1) {
          g_news_disp_idx = g_news_seg_count - 1;
          avatar.setSpeechText(g_news_segments[g_news_disp_idx].c_str());
        }
        g_news_disp_clear_ms = millis() + 3000;
      } else if ((int32_t)(millis() - g_news_disp_clear_ms) >= 0) {
        avatar.setSpeechText("");
        newsDispReset();
      }
    }
  }

  if (mode == 0) { return; }
  else if (mode < 0) {
    if(wakeword_regist()){
      avatar.setSpeechText("ウェイクワード登録終了");
      delay(1000);
      avatar.setSpeechText("");
      mode = 0;
      wakeword_is_enable = false;
#ifdef USE_SERVO
      servo_home = false;
#endif
    }
  }
  else if (mode > 0 && wakeword_is_enable) {
    if( wakeword_compare()){
        Serial.println("wakeword_compare OK!");
        sw_tone();
        SST_ChatGPT();
    }
  }
}
 
