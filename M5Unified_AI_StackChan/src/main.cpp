#include <Arduino.h>
#include <esp_system.h>  // esp_reset_reason（起動時診断、第8弾）
#include <esp_heap_caps.h>  // 最大連続ブロック計測（メモリ切り分け、第9.6弾）
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <nvs.h>
#include <Avatar.h>
#include "KinakoFace.h"  // きなこ写真の顔（カスタムFace）※PenguinFace.h は残置
#include "pose_img.h"    // ポーズ画像12種（第9弾）
#include "dance_mjpeg.h" // ダンス動画（モーションJPEG、第9弾）

#include <AudioOutput.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourcePROGMEM.h>
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
#include <FastLED.h>
#include "Scheduler.h"
#include "GfxLock.h"  // グラフィックス直列化ロック（第9.3弾）

// グラフィックス直列化ロックの実体（第9.3弾、GfxLock.h）。
// setup() の最初で生成し、描画タスク (drawLoop) と loop 側の直接描画が
// 同じ LGFX オブジェクトを同時に触らないよう直列化する。
SemaphoreHandle_t g_gfx_mutex = nullptr;

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

// ==== 読み上げ文の2行字幕表示（顔キャンバス内パネル） ====
// M5Stack-Avatar の吹き出し(Balloon)は1行表示で折り返しがなく、長い文は
// 画面からはみ出して切れる。そこで読み上げ文は、顔(KinakoFace)の描画
// キャンバス内に画面下部の字幕パネル（2行）として描く。顔と同じフレーム
// 合成で描かれるためちらつきがない（描画自体は KinakoFace.h 側）。
// フォントは efontJA_16 等倍（全角16px・半角8px）。1行は全角19文字相当
// （304px）まで、1ユニット＝2行。送りは従来どおり時間ベースで、ユニット内の
// 総文字数×180ms（最小3.5秒）を目安に読み上げの進行へ合わせる。
// ニュースは各記事の見出しをその記事の表示の先頭に置き、先頭ユニットには
// カテゴリ名（ラベル）を表示する。通常会話の ChatGPT 応答文も字幕対象。
// 短いシステム表示（音量ゲージ・メニュー・定期通知等）は従来どおり吹き出し。
static const int CAP_MAX_UNITS = 48;
static const float CAP_LINE_UNITS = 19.0f;  // 1行の容量（全角=1.0 / 半角=0.5）
String g_cap_units[CAP_MAX_UNITS][2];       // 字幕ユニット [unit][行0/行1]
int g_cap_count = 0;
bool g_cap_armed = false;    // 字幕キューが用意された（次の発話開始で表示開始）
bool g_cap_active = false;   // 字幕表示中
int g_cap_idx = 0;
uint32_t g_cap_next_ms = 0;  // 次のユニットへ切り替える時刻
uint32_t g_cap_start_ms = 0; // 表示開始時刻
uint32_t g_cap_clear_ms = 0; // 発話終了後、字幕を消す時刻（0=未定）

// ニュース由来の発話かどうか（中央タップでの中断対象の判定に使う）
bool g_pending_news_speech = false;  // exec_news 系が発話をセットした（開始待ち）
bool g_news_playing = false;         // ニュース発話セッション中

// 字幕の共有描画バッファ（loop 側が裏バッファへ書いてから front を切り替え、
// KinakoFace の描画タスク側が front を読む。宣言は KinakoFace.h 側にも extern）
volatile bool g_caption_show = false;
volatile uint8_t g_caption_front = 0;
char g_caption_buf[2][2][128];

// バッテリーアイコンの共有状態（第9.6弾。宣言は KinakoFace.h 側にも extern）
// loop 側が数秒おきに M5.Power から取得して更新し、描画側は読むだけ。
// 取得できていない間は -1（アイコンを出さない）。取得失敗時は前回値を維持する。
volatile int g_batt_pct = -1;          // 残量 %（0〜100、-1=未取得）
volatile bool g_batt_charging = false; // 充電中か
static uint32_t g_batt_last_ms = 0;
static void battery_icon_tick() {
  uint32_t now = millis();
  if (g_batt_last_ms != 0 && (int32_t)(now - g_batt_last_ms) < 5000) return;
  g_batt_last_ms = now;
  int32_t lv = M5.Power.getBatteryLevel();
  if (lv >= 0 && lv <= 100) g_batt_pct = (int)lv;
  g_batt_charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
}

// ==== ポーズ画像の切り替え状態（第9弾） ====
// 優先順位と復帰先はここ（main.cpp 側）で一元管理する。
// 「一時ポーズ（期限内）> 状態ポーズ > 表情連動 > 待機（ピース）」の順。
// 第9.2弾: ポーズの解決と JPEG デコードは loop 側 (resolve_target_pose /
// pose_prepare_tick) で行い、描画側 (KinakoFace) はデコード済みスプライトの
// 切替と pushSprite だけを行う（drawLoop のスタック不足対策）。
volatile int g_pose_state = -1;             // 状態ポーズ（会話開始・発話セッション中）
volatile int g_pose_timed = -1;             // 一時ポーズ（ウェイク検出のジャンプ等）
volatile uint32_t g_pose_timed_until = 0;   // 一時ポーズの終了時刻 (millis)
static int g_pending_pose = -1;             // 次の発話セッションで使うポーズ（発話のセット側が指定し、loop の発話開始時に消費）

namespace m5avatar { KinakoImage *g_kinako_image = nullptr; }  // 実体（宣言は KinakoFace.h）

// 現在表示すべきポーズを解決する（KinakoFace 側フォールバック解決と同一規則）
static int resolve_target_pose() {
  uint32_t now = millis();
  int timed = g_pose_timed;
  if (timed >= 0 && timed < POSE_COUNT &&
      (int32_t)(g_pose_timed_until - now) > 0) {
    return timed;
  }
  int st = g_pose_state;
  if (st >= 0 && st < POSE_COUNT) return st;
  switch (avatar.getExpression()) {
    case Expression::Happy:  return POSE_HEART;
    case Expression::Sleepy: return POSE_SLEEP;
    case Expression::Doubt:  return POSE_SURPRISE;
    case Expression::Sad:    return POSE_BOW;
    case Expression::Angry:  return POSE_FISTS;
    default:                 return POSE_PEACE;  // Neutral は待機ポーズ
  }
}

// loop から毎回呼ぶ。目標ポーズが変わったときだけ裏スプライトへ先行デコード
// され、描画側はデコード完了後にスプライトを切り替える。
// 第9.3弾: デコード (LGFX キャンバス操作) を描画タスクと直列化するため、
// グラフィックス直列化ロックの中で行う。ポーズが変わらない回はロックの
// 取得・解放だけで即座に戻る。デコード中は描画が最大でデコード時間
// （数十〜百数十ms）待たされるが、顔は静止画のため実害はない。
static void pose_prepare_tick() {
  GfxLockGuard gfx_guard;
  kinakoPreparePose(resolve_target_pose());
}

static void captionHide() { g_caption_show = false; }

static void captionShowUnit(int idx) {
  if (idx < 0 || idx >= g_cap_count) { captionHide(); return; }
  uint8_t back = (uint8_t)(g_caption_front ^ 1);
  strncpy(g_caption_buf[back][0], g_cap_units[idx][0].c_str(), sizeof(g_caption_buf[back][0]) - 1);
  g_caption_buf[back][0][sizeof(g_caption_buf[back][0]) - 1] = '\0';
  strncpy(g_caption_buf[back][1], g_cap_units[idx][1].c_str(), sizeof(g_caption_buf[back][1]) - 1);
  g_caption_buf[back][1][sizeof(g_caption_buf[back][1]) - 1] = '\0';
  g_caption_front = back;
  g_caption_show = true;
}

static int newsUtf8CharLen(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

static int capTextCharCount(const String& s) {
  int chars = 0;
  for (int i = 0; i < (int)s.length();) {
    i += newsUtf8CharLen((uint8_t)s[i]);
    chars++;
  }
  return chars;
}

// ユニットの表示時間：総文字数×180ms、最小3.5秒
static uint32_t capUnitDurationMs(int idx) {
  uint32_t d = (uint32_t)(capTextCharCount(g_cap_units[idx][0]) +
                          capTextCharCount(g_cap_units[idx][1])) * 180;
  return d < 3500 ? 3500 : d;
}

// ---- 字幕ビルド（ユニット組み立て） ----
static String g_cap_build[2];   // 組み立て中のユニットの2行
static int g_cap_build_lines = 0;

static void capFlushUnit() {
  if (g_cap_build_lines == 0) return;
  if (g_cap_count < CAP_MAX_UNITS) {
    g_cap_units[g_cap_count][0] = g_cap_build[0];
    g_cap_units[g_cap_count][1] = g_cap_build[1];
    g_cap_count++;
  }
  g_cap_build[0] = "";
  g_cap_build[1] = "";
  g_cap_build_lines = 0;
}

static void capPushLine(const String& line) {
  if (line.length() == 0) return;
  if (g_cap_build_lines >= 2) capFlushUnit();
  g_cap_build[g_cap_build_lines++] = line;
}

// テキストを1行容量で折り返して行を積む（、。！？の直後での改行と改行文字での改行を優先）
static void capAddText(const String& text) {
  int n = (int)text.length();
  int start = 0;      // 現在の行の開始バイト位置
  float curU = 0;     // 現在の行の使用量（全角=1.0 / 半角=0.5）
  int breakPos = -1;  // 現在の行内にある「、。！？」直後のバイト位置
  float breakU = 0;   // breakPos 時点の使用量
  int i = 0;
  while (i < n) {
    if (text[i] == '\n') {  // 改行文字は強制改行
      if (i > start) capPushLine(text.substring(start, i));
      start = i + 1;
      curU = 0;
      breakPos = -1;
      breakU = 0;
      i++;
      continue;
    }
    int clen = newsUtf8CharLen((uint8_t)text[i]);
    if (i + clen > n) clen = 1;
    float cu = (clen == 1) ? 0.5f : 1.0f;
    if (curU + cu > CAP_LINE_UNITS && i > start) {
      if (breakPos > start && breakU >= CAP_LINE_UNITS * 0.5f) {
        capPushLine(text.substring(start, breakPos));
        start = breakPos;
        curU -= breakU;
        breakPos = -1;
        breakU = 0;
      } else {
        capPushLine(text.substring(start, i));
        start = i;
        curU = 0;
        breakPos = -1;
        breakU = 0;
      }
      continue;
    }
    curU += cu;
    if (clen == 3 && (uint8_t)text[i] == 0xE3 && (uint8_t)text[i + 1] == 0x80 &&
        ((uint8_t)text[i + 2] == 0x81 || (uint8_t)text[i + 2] == 0x82)) {
      breakPos = i + clen;  // 、(U+3001) または 。(U+3002) の直後
      breakU = curU;
    }
    if (clen == 3 && (uint8_t)text[i] == 0xEF && (uint8_t)text[i + 1] == 0xBC &&
        ((uint8_t)text[i + 2] == 0x81 || (uint8_t)text[i + 2] == 0x9F)) {
      breakPos = i + clen;  // ！(U+FF01) または ？(U+FF1F) の直後
      breakU = curU;
    }
    i += clen;
  }
  if (start < n) capPushLine(text.substring(start, n));
}

// 見出し：必ず新しいユニットの先頭行から表示する（記事の先頭に置く）
static void capAddHeadline(const String& title) {
  capFlushUnit();
  capAddText(title);
}

// カテゴリ名などのラベル：単独ユニットとして表示する
static void capAddLabel(const String& label) {
  capFlushUnit();
  capPushLine(label);
  capFlushUnit();
}

// 会話応答文などを丸ごと字幕キュー化する
static void capBuildFromText(const String& text) {
  capAddText(text);
  capFlushUnit();
}

// ==== 側面LED（M5GO Bottom2 ベースの SK6812×10、GPIO25） ====
// Core2 本体の LED は電源インジケーターのみで、側面の RGB LED は拡張ベース
// M5GO Bottom2 装着時に両側面の SK6812×10 が GPIO25 へ接続される
// （M5Unified の Core2 ピン表でも GPIO25 は RGBLED 用として定義されている）。
// 競合確認済み：サーボ 33/32、マイク入力 34、スピーカー I2S 12/0/2、
// SD SPI 18/23/38/CS4 のいずれとも重ならない。ベース未装着でも
// GPIO25 に信号が出るだけで無害。駆動は既存依存の FastLED を使う。
#define SIDE_LED_PIN 25
#define SIDE_LED_COUNT 10
#define SIDE_LED_MAX_BRIGHTNESS 6   // 全体の明るさ上限（第9.5弾：48→16、第9.6弾：16→6、省電力化）
CRGB g_side_leds[SIDE_LED_COUNT];
bool g_led_enable = true;            // 全体の有効/無効（NVS "setting"/"led"、既定ON）
bool g_led_listening = false;        // 録音中（青）
bool g_led_thinking = false;         // ChatGPT 応答待ち（紫パルス）
bool g_led_news_active = false;      // ニュース読み上げ中（カテゴリ色）
CRGB g_led_news_color = CRGB::White; // 現在のニュースのカテゴリ色
uint32_t g_led_vol_until = 0;        // 音量ゲージ表示の終了時刻
uint32_t g_led_ww_until = 0;         // ウェイクワード検出フラッシュの終了時刻
uint32_t g_led_dance_until = 0;      // ダンス動画再生中の色サイクル終了時刻（第9弾）

// 字幕キューを破棄する（通常会話の開始時などに呼ぶ）
void captionReset() {
  g_cap_count = 0;
  g_cap_armed = false;
  g_cap_active = false;
  g_cap_idx = 0;
  g_cap_clear_ms = 0;
  g_pending_news_speech = false;
  g_cap_build[0] = "";
  g_cap_build[1] = "";
  g_cap_build_lines = 0;
  captionHide();
  g_led_news_active = false;
}

// ==== 音量のタッチ操作（6段階） ====
// レベル0〜5 → M5.Speaker の音量値（0＝無音、1〜5 は 0〜255 の等分。
// 旧5段階時代の保存値 1〜5 はそのまま同じ音量に対応する）
static const uint8_t VOL_VALUES[6] = {0, 51, 102, 153, 204, 255};
uint8_t g_vol_level = 3;             // 現在の音量レベル（0〜5、初期値3）
bool g_vol_gauge_active = false;     // 音量ゲージを吹き出し表示中
uint32_t g_vol_gauge_until = 0;      // ゲージを消す時刻

// 音量レベルをスピーカーへ適用する（save=true で NVS "setting"/"vol" へ保存）
static void applyVolLevel(bool save) {
  uint8_t v = VOL_VALUES[g_vol_level];
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
// カテゴリごとの側面LED色（トピックス=白、AI=水色、芸能=ピンク、テック=緑、その他=黄）
static const CRGB NEWS_CAT_COLORS[5] = {
  CRGB::White, CRGB(0, 180, 220), CRGB(255, 80, 150), CRGB(0, 200, 80), CRGB(240, 200, 0),
};
bool g_menu_open = false;        // カテゴリ選択メニュー表示中
int g_menu_sel = 0;              // 選択中のカテゴリ（0〜4）
uint32_t g_menu_deadline = 0;    // 最後のタップから約2.5秒後に確定する時刻

// ==== 定期通知モード（BtnC短押しトグル、5分おきニュース） ====
// 状態は RAM のみ（再起動で OFF に戻る）
bool g_periodic_on = false;          // 定期通知モード
uint32_t g_periodic_next_ms = 0;     // 次回ニュース読み上げの発火時刻

// ==== スケジューラ（AI_StackChan_Ex から移植、第7弾） ====
// 時刻ベースのコールバック登録エンジン（src/Scheduler.h/.cpp）を使う。
// 既定の登録は init_schedule()：毎朝7:00 モーニング読み上げ＋時報（7〜23時）。
// 各項目の ON/OFF は NVS "setting" の s_morning / s_hourly に保存され、
// /schedule ページから切り替えられる（既定は両方 ON）。
bool g_sched_morning = true;         // モーニング読み上げ（NVS "setting"/"s_morning"）
bool g_sched_hourly = true;          // 時報（NVS "setting"/"s_hourly"）
extern AudioGeneratorMP3 *mp3;       // 実体は後方（音声再生まわり）で定義
String exec_news();                  // 実体は後方（ニュース読み上げの節）で定義
static bool play_dance_video();      // 実体は後方（SST_ChatGPT の直前）で定義。第9弾
static void dance_boot_check();      // 起動時1回だけの自動再生判定（loop から呼ぶ）。第9.1弾

// スケジューラの発火可否ゲート：発話中・ニュース表示中・メニュー操作中・
// ウェイクワード登録中（mode<0）は発火しない。録音〜応答生成（SST_ChatGPT）は
// loop 内の同期処理のため、その間 run_schedule() 自体が呼ばれず割り込まれない
static bool scheduler_ready() {
  return mode >= 0 && !mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
         !g_menu_open && !g_cap_active;
}

// 毎朝7:00：日付の挨拶＋通常ニュース（トピックス＋AI）
static void sched_fn_morning() {
  if (!g_sched_morning) return;
  struct tm t;
  if (!getLocalTime(&t)) return;
  static const char* wday[] = {"日", "月", "火", "水", "木", "金", "土"};
  char buf[96];
  snprintf(buf, sizeof(buf), "おはようございます。今日は%d月%d日（%s曜日）です。",
           t.tm_mon + 1, t.tm_mday, wday[t.tm_wday % 7]);
  g_pending_pose = POSE_RUN;  // 第9弾: モーニングの発話中は「走る」ポーズ（ニュースのサムズアップより優先）
  exec_news();  // speech_text にニュース本文（失敗時はその旨の文）をセットする
  speech_text = String(buf) + speech_text;
}

// 時報：○時です
static void sched_fn_hourly() {
  if (!g_sched_hourly) return;
  struct tm t;
  if (!getLocalTime(&t)) return;
  speech_text = String(t.tm_hour) + "時です";
}

static void init_schedule() {
  scheduler_set_ready_func(scheduler_ready);
  add_schedule(new ScheduleEveryDay(7, 0, sched_fn_morning));   // 毎朝7:00 モーニング読み上げ
  add_schedule(new ScheduleEveryHour(sched_fn_hourly, 7, 23));  // 時報 7:00〜23:00
}

// ==== 文単位パイプラインTTS（第8弾） ====
// 従来は発話文全体を1回の合成リクエストで送り、合成が終わるまで再生が
// 始まらなかった（tts.quest のキュー混雑時は長文で約60秒待ちの実測あり）。
// 第8弾では文（。！？）単位に分割し、専用の合成タスクが最初の文から順に
// 合成して、合成できた文から即座に再生する。再生中に次の文を先行合成する
// （先行は1文分まで）。合成1文の上限は WebVoiceVoxTTS 側で 25 秒とし、
// 超えた文はスキップして次へ進む。
// 状態遷移は loop() 内の tts_drive()（実体は音声再生まわりの後方）が担う。
struct TtsChunk {
  String text;   // 合成する文（チャンク）
  String parms;  // そのチャンクの話者パラメータ（&speaker=N）
};
static std::deque<TtsChunk> g_tts_queue;   // 合成待ちチャンク
static String g_tts_ready_url = "";        // 合成済みの次文URL（有効な間だけ意味を持つ）
static uint8_t *g_tts_ready_data = nullptr; // 合成済み次文のMP3データ（PSRAM。第9.6弾: メモリ再生用。所有権はスロット保持中は合成タスク側→消費時に再生側へ）
static size_t g_tts_ready_len = 0;          // g_tts_ready_data のバイト数
static bool g_tts_ready = false;           // ready_url がセットされた（空URL=合成失敗でスキップ）
static bool g_tts_synth_busy = false;      // 合成タスクが1文を処理中
static bool g_speech_session = false;      // 発話セッション中（投入〜全チャンク再生完了）
static SemaphoreHandle_t g_tts_mutex = nullptr;
static uint32_t g_tts_gen = 0;             // 発話セッションの世代（中断時に進めて合成結果を無効化）
static uint32_t g_stop_msg_until = 0;      // 「中断しました」吹き出しを消す時刻（0=非表示）
static uint32_t g_info_msg_until = 0;      // 通知吹き出し（ニュース取得失敗など）を消す時刻（0=非表示。第9.6弾）

void ttsSynthTask(void *args);  // 実体は後方（setup でタスク起動）
void tts_drive();               // 実体は後方（loop から毎回呼ぶ）

// テキストを文（。！？と改行で区切る。文末記号は文に含める）に分割し、
// チャンク列にして返す。先頭の文は単独チャンク（初回発話を最速にする）、
// 以降は約200バイトまで連結して合成リクエスト数を抑える
// （第9.4弾：約100→約200バイト。tts.quest のレート制限（429）対策として
// リクエスト数自体を減らす。先頭チャンクは従来どおり1文だけ）。
static std::deque<TtsChunk> splitSpeechChunks(const String& text, const String& parms) {
  std::deque<String> sentences;
  int n = (int)text.length();
  int start = 0;
  int i = 0;
  while (i < n) {
    uint8_t b = (uint8_t)text[i];
    // 文末記号（。=E3 80 82 / ！=E3 80 81 / ？=E3 80 9F）
    if (b == 0xE3 && i + 2 < n && (uint8_t)text[i + 1] == 0x80 &&
        ((uint8_t)text[i + 2] == 0x82 || (uint8_t)text[i + 2] == 0x81 || (uint8_t)text[i + 2] == 0x9F)) {
      sentences.push_back(text.substring(start, i + 3));
      i += 3;
      start = i;
    } else if (text[i] == '\n') {
      if (i > start) sentences.push_back(text.substring(start, i));
      i++;
      start = i;
    } else {
      int clen = newsUtf8CharLen(b);
      if (i + clen > n) clen = 1;
      i += clen;
    }
  }
  if (start < n) sentences.push_back(text.substring(start, n));

  std::deque<TtsChunk> chunks;
  String cur = "";
  bool firstChunk = true;
  for (auto& s0 : sentences) {
    String s = s0;
    s.trim();
    if (s.length() == 0) continue;
    if (cur.length() == 0) {
      cur = s;
      continue;
    }
    if (firstChunk || cur.length() + s.length() > 200) {
      TtsChunk c;
      c.text = cur;
      c.parms = parms;
      chunks.push_back(c);
      cur = s;
      firstChunk = false;
    } else {
      cur += s;
    }
  }
  if (cur.length() > 0) {
    TtsChunk c;
    c.text = cur;
    c.parms = parms;
    chunks.push_back(c);
  }
  return chunks;
}

// 発話テキストを文チャンクに分割してパイプラインへ投入する。
// 合成は合成タスクが非同期に行い、再生は tts_drive() が進める。
static void tts_enqueue(const String& text, const String& parms) {
  std::deque<TtsChunk> chunks = splitSpeechChunks(text, parms);
  if (chunks.empty()) return;
  if (g_tts_mutex == nullptr) g_tts_mutex = xSemaphoreCreateMutex();
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  for (auto& c : chunks) g_tts_queue.push_back(c);
  if (!g_speech_session) g_speech_session = true;
  xSemaphoreGive(g_tts_mutex);
  Serial.printf("[TTS] enqueue %u chunk(s)\n", (unsigned)chunks.size());
}

// ==== Wi-Fi 切断監視と自動再接続（第8弾） ====
// 起動後に AP が落ちたり電波が途切れても復帰できるよう、loop から定期的に
// 接続状態を確認し、切断が続く場合はバックオフ付きで再接続を試みる。
// WiFi.reconnect() は接続開始だけを行う（完了を待たない）ため、
// 再接続試行中も他の処理は止めない。
static uint32_t g_wifi_last_check = 0;
static uint32_t g_wifi_next_retry = 0;
static uint32_t g_wifi_backoff_ms = 10000;
static void wifi_watchdog() {
  uint32_t now = millis();
  if (now - g_wifi_last_check < 3000) return;
  g_wifi_last_check = now;
  if (WiFi.status() == WL_CONNECTED) {
    if (g_wifi_next_retry != 0) Serial.println("[WiFi] reconnected");
    g_wifi_next_retry = 0;
    g_wifi_backoff_ms = 10000;
    return;
  }
  if (g_wifi_next_retry != 0 && (int32_t)(now - g_wifi_next_retry) < 0) return;
  Serial.println("[WiFi] disconnected; trying reconnect...");
  WiFi.reconnect();
  g_wifi_next_retry = now + g_wifi_backoff_ms;
  if (g_wifi_backoff_ms < 60000) g_wifi_backoff_ms *= 2;
}
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
  // 音声の発声（第8弾: 文単位パイプラインへ投入。合成・再生は非同期に進む）
  ////////////////////////////////////////
  avatar.setExpression(Expression::Happy);
  // 発話中扱いにして、再生完了まで他機能（ニュース・スケジューラ等）が
  // 割り込まないようにする（従来は合成完了までハンドラがブロックしていた）
  if (speech_text_buffer == "") {
    speech_text_buffer = message;
    g_news_playing = false;         // /speech の発話はニュースではない（中断対象外）
    g_pending_news_speech = false;
  }
  tts_enqueue(message, TTS_PARMS);
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
      https.setTimeout( 30000 );  // 第8弾: ChatGPT応答の上限（従来65秒→30秒に短縮）
  
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
  g_led_thinking = true;   // 側面LED：応答待ち（紫パルス）
  avatar.setExpression(Expression::Doubt);
  avatar.setSpeechText("考え中…");
  uint32_t chat_t0 = millis();
  String ret = https_post_json("https://api.openai.com/v1/chat/completions", json_string.c_str(), root_ca_openai);
  Serial.printf("[ChatGPT] response in %lu ms\n", (unsigned long)(millis() - chat_t0));
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
  g_led_thinking = false;
  return response;
}

String InitBuffer = "";

// 応答高速化の設定を chat_doc に反映する（モデル上書き＋max_tokens補完）
void applyChatSpeedSettings() {
  if (CHAT_MODEL != "") chat_doc["model"] = CHAT_MODEL;
  if (chat_doc["max_tokens"].isNull()) chat_doc["max_tokens"] = CHAT_MAX_TOKENS;
}

void handle_chat() {
  captionReset();  // 通常会話が始まるのでニュース表示キューは破棄する
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
    g_pending_pose = POSE_LAUGH;  // 第9弾: 会話応答の再生中は「笑う」ポーズ
    // 会話応答文も2行字幕の対象にする（発話の再生開始時に表示）
    capBuildFromText(response);
    g_led_news_color = CRGB::White;
    if (g_cap_count > 0) g_cap_armed = true;
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
  captionReset();  // 通常会話が始まるのでニュース表示キューは破棄する
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
    g_pending_pose = POSE_LAUGH;  // 第9弾: 会話応答の再生中は「笑う」ポーズ
    // 会話応答文も2行字幕の対象にする（発話の再生開始時に表示）
    capBuildFromText(response);
    g_led_news_color = CRGB::White;
    if (g_cap_count > 0) g_cap_armed = true;
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
  // 第9.2弾: APIキーの平文 Serial 出力を廃止（値そのものは出力しない）
  Serial.println("apikey updated via /apikey_set (values not logged)");

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

// NHKニュースRSS（カテゴリ nhkCat）の本文を取得する（第9.6弾で分離）。
// 成否コードは g_nhk_http_code に残す（接続失敗時は負値）。
static String fetchNhkRssBody(int nhkCat) {
  String payload = "";
  g_nhk_http_code = 0;
  Serial.printf("[NHK RSS] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return "";
  client->setCACert(root_ca_nhk);
  client->setTimeout(10000);
  {
    HTTPClient https;
    https.setTimeout(10000);
    if (https.begin(*client, String("https://www3.nhk.or.jp/rss/news/cat") + String(nhkCat) + ".xml")) {
      int httpCode = https.GET();
      g_nhk_http_code = httpCode;
      Serial.printf("[NHK RSS] GET... code: %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK) {
        // 第8弾: 本文は締切・総量上限付きで読む。getString() はデータが
        // 細切れに来続けると全体の締切なくブロックし得るため、ストリームから
        // 直接読む方式に変更した（過去の「取得処理が正常終了しない」不具合の
        // 再発防止）。締切 12 秒・上限 200KB で必ず戻る。
        WiFiClient *stream = https.getStreamPtr();
        if (stream) {
          payload.reserve(64 * 1024);
          const uint32_t deadline = millis() + 12000;
          const size_t MAX_BYTES = 200 * 1024;
          uint8_t rbuf[512];
          while ((int32_t)(millis() - deadline) < 0 && payload.length() < MAX_BYTES) {
            size_t avail = stream->available();
            if (avail == 0) {
              if (!stream->connected()) break;
              delay(1);
              continue;
            }
            int n = stream->read(rbuf, avail < sizeof(rbuf) ? avail : sizeof(rbuf));
            if (n > 0) {
              payload.concat((const char*)rbuf, (unsigned int)n);
            } else {
              delay(1);
            }
          }
          Serial.printf("[NHK RSS] body %u bytes\n", (unsigned)payload.length());
        }
      }
      https.end();
    }
  }
  delete client;
  return payload;
}

// NHKニュースRSS（カテゴリ nhkCat）を取得し、先頭3件を読み上げ用テキストにして返す。失敗時は空文字
// dispItems には表示用に「見出し＋説明」を1件ずつ格納する（吹き出し表示キュー構築用）
// intro は読み上げ文の導入（例「今日のニュースです。」）
String fetchNewsText(int nhkCat, const char* intro, String* dispItems, String* dispTitles, String* dispDescs, int maxDispItems, int& dispCount) {
  dispCount = 0;
  String payload = fetchNhkRssBody(nhkCat);
  if (payload == "" && g_nhk_http_code < 0) {
    // 接続失敗（SSLメモリ確保失敗を含む）は、全て解放したうえで1回だけ再試行する（第9.6弾）
    Serial.printf("[NHK RSS] retry after connection failure (code %d)\n", g_nhk_http_code);
    delay(1500);
    payload = fetchNhkRssBody(nhkCat);
  }
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
      // 見出しと本文は字幕表示（見出しを記事の先頭に置く）用に分けて保持する
      if (dispTitles) dispTitles[dispCount] = title;
      if (dispDescs) dispDescs[dispCount] = desc;
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
static bool fetchGoogleHeadlinesOnce(const char* url, int want, String* outTitles, int& outCount, int& httpCodeOut) {
  outCount = 0;
  httpCodeOut = 0;
  Serial.printf("[Google RSS] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
          // 第8弾: 走査の締切は 25 秒→15 秒に短縮（見出しはフィード先頭にあるため十分）
          while (outCount < want && scanned < MAX_SCAN && (millis() - startMs) < 15000) {
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

// 接続失敗（httpCodeOut < 0、SSLメモリ確保失敗を含む）時は、全て解放した
// うえで1回だけ再試行するラッパー（第9.6弾）
static bool fetchGoogleHeadlines(const char* url, int want, String* outTitles, int& outCount, int& httpCodeOut) {
  bool ok = fetchGoogleHeadlinesOnce(url, want, outTitles, outCount, httpCodeOut);
  if (!ok && httpCodeOut < 0) {
    Serial.printf("[Google RSS] retry after connection failure (code %d)\n", httpCodeOut);
    delay(1500);
    ok = fetchGoogleHeadlinesOnce(url, want, outTitles, outCount, httpCodeOut);
  }
  return ok;
}

// ニュース読み上げ文を組み立てて speech_text にセットする。読み上げ文を返す（全て失敗した場合は空文字）
// あわせて字幕キュー（見出し＋本文）を構築し、次の発話開始時に表示が始まるよう arm する
String exec_news() {
  captionReset();  // 前回の表示キューが残っていたら破棄する
  // 一般ニュース：NHK（見出し＋説明）→ 失敗時は Google トップニュースの見出しで代替
  String dispItems[5];
  String dispTitles[5];
  String dispDescs[5];
  int dispCount = 0;
  String text = fetchNewsText(0, "今日のニュースです。", dispItems, dispTitles, dispDescs, 5, dispCount);
  if (text == "") {
    String heads[3];
    int n = 0;
    if (fetchGoogleHeadlines("https://news.google.com/rss?hl=ja&gl=JP&ceid=JP:ja", 3, heads, n, g_google_top_http_code)) {
      text = "今日のニュースです。";
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        if (dispCount < 5) {
          dispTitles[dispCount] = heads[i];
          dispDescs[dispCount] = "";
          dispItems[dispCount++] = heads[i] + "。";
        }
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
    // 字幕キューを構築する（発話の再生開始時に字幕表示を開始）
    if (hasGeneral) {
      capAddLabel("今日のニュースです");
      for (int i = 0; i < dispCount; i++) {
        capAddHeadline(dispTitles[i]);
        if (dispDescs[i] != "") capAddText(dispDescs[i] + "。");
      }
    }
    if (aiCount > 0) {
      capAddLabel("AIニュース");
      for (int i = 0; i < aiCount; i++) capAddHeadline(aiHeads[i]);
    }
    capFlushUnit();
    g_led_news_color = CRGB::White;  // 通常ニュースのLED色は白
    if (g_cap_count > 0) g_cap_armed = true;
    speech_text = text;
  } else {
    speech_text = "ニュースを取得できませんでした";
    // 第9.6弾: 音声が出ない場合でも理由が分かるよう、吹き出しにも表示して残す
    avatar.setSpeechText("ニュースを取得できませんでした");
    g_info_msg_until = millis() + 4000;
  }
  g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
  return text;
}

// カテゴリ単独のニュース読み上げ（タッチメニュー用）
// NHKカテゴリがあれば NHK（見出し＋説明×3）を優先し、失敗時は Google 見出し×3 で代替。
// NHKのないカテゴリ（AI／テック）は最初から Google 見出し×3。
// 従来の exec_news()（トピックス＋AI連結）とは独立した読み上げにする
void exec_news_category(int cat) {
  if (cat < 0 || cat > 4) cat = 0;
  const NewsCatDef& c = NEWS_CATS[cat];
  captionReset();  // 前回の表示キューが残っていたら破棄する
  String dispItems[5];
  String dispTitles[5];
  String dispDescs[5];
  int dispCount = 0;
  String text = "";
  if (c.nhkCat >= 0) {
    text = fetchNewsText(c.nhkCat, c.intro, dispItems, dispTitles, dispDescs, 5, dispCount);
  }
  if (text == "") {
    String heads[3];
    int n = 0;
    int dummyCode = 0;
    if (fetchGoogleHeadlines(c.googleUrl, 3, heads, n, dummyCode)) {
      text = c.intro;
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        if (dispCount < 5) {
          dispTitles[dispCount] = heads[i];
          dispDescs[dispCount] = "";
          dispItems[dispCount++] = heads[i] + "。";
        }
      }
    }
  }
  if (text != "") {
    // 字幕キューを構築する（先頭ユニットにカテゴリ名、各記事は見出しから）
    capAddLabel(c.label);
    for (int i = 0; i < dispCount; i++) {
      capAddHeadline(dispTitles[i]);
      if (dispDescs[i] != "") capAddText(dispDescs[i] + "。");
    }
    capFlushUnit();
    g_led_news_color = NEWS_CAT_COLORS[cat];  // カテゴリ色をLEDに使う
    if (g_cap_count > 0) g_cap_armed = true;
    speech_text = text;
  } else {
    speech_text = "ニュースを取得できませんでした";
    // 第9.6弾: 音声が出ない場合でも理由が分かるよう、吹き出しにも表示して残す
    avatar.setSpeechText("ニュースを取得できませんでした");
    g_info_msg_until = millis() + 4000;
  }
  g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
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

// ==== スケジュールとLEDの設定（/schedule ページ、第7弾） ====
// 引数 morning / hourly / led = on|off で切り替え、NVS "setting" に保存する
static void schedule_apply_arg(const char* argName, bool& flag, const char* nvsKey) {
  String v = server.arg(argName);
  if (v == "on" || v == "off") {
    flag = (v == "on");
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
      nvs_set_u8(nvs_handle, nvsKey, flag ? 1 : 0);
      nvs_close(nvs_handle);
    }
  }
}

void handle_schedule() {
  bool prevMorning = g_sched_morning;
  bool prevHourly = g_sched_hourly;
  bool prevLed = g_led_enable;
  schedule_apply_arg("morning", g_sched_morning, "s_morning");
  schedule_apply_arg("hourly", g_sched_hourly, "s_hourly");
  schedule_apply_arg("led", g_led_enable, "led");
  String message = "";
  if (g_sched_morning != prevMorning) message += "<p><b>モーニング読み上げを" + String(g_sched_morning ? "ON" : "OFF") + "にしました（本体に保存済み）。</b></p>";
  if (g_sched_hourly != prevHourly) message += "<p><b>時報を" + String(g_sched_hourly ? "ON" : "OFF") + "にしました（本体に保存済み）。</b></p>";
  if (g_led_enable != prevLed) message += "<p><b>側面LEDを" + String(g_led_enable ? "ON" : "OFF") + "にしました（本体に保存済み）。</b></p>";

  String html = String(HEAD) + "<body>";
  html += "<h1>スケジュールとLEDの設定</h1>";
  html += message;
  html += "<p>モーニング読み上げ（毎朝7:00に日付＋ニュース）: <b>" + String(g_sched_morning ? "ON" : "OFF") + "</b> "
          "<a href=\"/schedule?morning=on\">ONにする</a> / <a href=\"/schedule?morning=off\">OFFにする</a></p>";
  html += "<p>時報（7:00〜23:00の毎正時に「○時です」）: <b>" + String(g_sched_hourly ? "ON" : "OFF") + "</b> "
          "<a href=\"/schedule?hourly=on\">ONにする</a> / <a href=\"/schedule?hourly=off\">OFFにする</a></p>";
  html += "<p>側面LED（M5GO Bottom2ベースの SK6812）: <b>" + String(g_led_enable ? "ON" : "OFF") + "</b> "
          "<a href=\"/schedule?led=on\">ONにする</a> / <a href=\"/schedule?led=off\">OFFにする</a></p>";
  html += "<p>発話中など本機が忙しいときに発火時刻が来た場合、モーニング読み上げは最大約5分まで延期され、時報はその回をスキップします。設定は本体に保存され、再起動後も使われます。</p>";
  html += "<p><a href=\"/voice\">声の切り替えはこちら</a> / <a href=\"/model\">AIモデルの切り替えはこちら</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
AudioGeneratorMP3 *mp3;
AudioFileSourceBuffer *buff = nullptr;
int preallocateBufferSize = 30*1024;
uint8_t *preallocateBuffer;
AudioFileSourceHTTPSStream *file = nullptr;
AudioFileSourcePROGMEM *memfile = nullptr;   // メモリ再生用ソース（第9.6弾）
static uint8_t *g_play_data = nullptr;       // 再生中のMP3データ（PSRAM。再生側が所有し、再生終了時に解放）

bool playMP3(AudioFileSource *src){
  return mp3->begin(src, &out);
}

// 再生ソース一式の後片付け（ストリーム再生・メモリ再生の両対応、第9.6弾）。
// 再生が終わっているタイミングでのみ呼ぶこと（再生中ソースを消さない）。
static void tts_cleanup_playback_sources() {
  if (buff != nullptr) { delete buff; buff = nullptr; }
  if (file != nullptr) { delete file; file = nullptr; }
  if (memfile != nullptr) { delete memfile; memfile = nullptr; }
  if (g_play_data != nullptr) { heap_caps_free(g_play_data); g_play_data = nullptr; }
}

// 文単位パイプラインTTS: 合成専用タスク（第8弾）。
// キューから1文を取り出して合成し、結果URLを ready スロットへ置く。
// ready スロットが埋まっている間は次の合成をしない（先行合成は1文分まで）。
// 合成はブロッキング処理なので、再生デコードを行う loop とは別タスクにして
// 再生中の音切れを防ぐ。TLS を使うためスタックは大きめに確保する。
void ttsSynthTask(void *args)
{
  (void)args;
  for (;;)
  {
    TtsChunk chunk;
    bool have = false;
    uint32_t gen = 0;
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    if (!g_tts_ready && !g_tts_queue.empty()) {
      chunk = g_tts_queue.front();
      g_tts_queue.pop_front();
      g_tts_synth_busy = true;
      gen = g_tts_gen;
      have = true;
    }
    xSemaphoreGive(g_tts_mutex);
    if (!have) {
      delay(30);
      continue;
    }
    String url = "";
    if (WiFi.status() == WL_CONNECTED) {
      url = voicevox_synth_url(chunk.text.c_str(), chunk.parms.c_str());
    } else {
      Serial.println("[TTS] synth skipped: WiFi disconnected");
    }
    // 第9.6弾: 合成できたら、その場でMP3をPSRAMへ丸ごとダウンロードしておく。
    // 再生はメモリから行い、再生用にTLS接続を新たに張らない（TLS一本化）。
    // 上限超過・ダウンロード失敗の場合はURLを ready スロットへ残し、
    // 再生側が従来のストリーム再生へフォールバックする。
    uint8_t *dlData = nullptr;
    size_t dlLen = 0;
    String readyUrl = "";
    if (url != "") {
      int rc = voicevox_download_mp3(url, &dlData, &dlLen);
      if (rc != 0) {
        Serial.printf("[TTS] download fallback to streaming (rc=%d)\n", rc);
        readyUrl = url;
      }
    }
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    // 中断などで世代が進んでいたら、この合成結果は破棄する（再生させない）
    if (gen == g_tts_gen) {
      g_tts_ready_url = readyUrl;
      g_tts_ready_data = dlData;
      g_tts_ready_len = dlLen;
      g_tts_ready = true;
      dlData = nullptr;  // 所有権は ready スロットへ移った
    }
    g_tts_synth_busy = false;
    xSemaphoreGive(g_tts_mutex);
    if (dlData != nullptr) heap_caps_free(dlData);  // 世代が進んでいた場合は破棄
  }
}

// 文単位パイプラインTTS: 再生の進行管理（第8弾）。loop から毎回呼ばれる。
// 再生中でないとき、合成済みの文があれば再生を開始し、キューが空になって
// 合成も終わっていれば発話セッションの終了処理（従来は mp3 終了時に実施）を
// 1回だけ行う。
// 再生開始直後の共通処理（第9.6弾で切り出し）：字幕キューが用意されていれば、
// 最初の文の再生開始に合わせて字幕表示を開始する（ニュース・会話応答の両方が対象）
static void tts_on_play_started() {
  if (g_cap_armed && g_cap_count > 0) {
    g_cap_armed = false;
    g_cap_active = true;
    g_led_news_active = true;   // 側面LED：字幕表示中はニュース色（会話時は白）
    g_cap_idx = 0;
    g_cap_start_ms = millis();
    g_cap_clear_ms = 0;
    captionShowUnit(0);
    g_cap_next_ms = millis() + capUnitDurationMs(0);
  }
}

void tts_drive()
{
  if (mp3 == nullptr || mp3->isRunning()) return;

  if (g_tts_ready) {
    String url;
    uint8_t *data = nullptr;
    size_t len = 0;
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    url = g_tts_ready_url;
    data = g_tts_ready_data;
    len = g_tts_ready_len;
    g_tts_ready = false;
    g_tts_ready_url = "";
    g_tts_ready_data = nullptr;
    g_tts_ready_len = 0;
    xSemaphoreGive(g_tts_mutex);
    if (data != nullptr) {
      // 第9.6弾: メモリ再生（再生用にTLSを張らない）
      tts_cleanup_playback_sources();
      g_play_data = data;  // 所有権は再生側へ（再生終了時に後片付けで解放）
      if (voicevox_play_memory(data, len)) {
        Serial.println("[TTS] play start (memory)");
        tts_on_play_started();
        return;
      }
      // begin 失敗時はソースとデータをここで解放する
      if (memfile != nullptr) { delete memfile; memfile = nullptr; }
      if (g_play_data != nullptr) { heap_caps_free(g_play_data); g_play_data = nullptr; }
      Serial.println("[TTS] play begin failed (memory); skip sentence");
      return;
    }
    if (url != "") {
      // ストリーム再生（ダウンロード上限超過時のフォールバック）
      tts_cleanup_playback_sources();
      if (voicevox_play_url(url)) {
        Serial.println("[TTS] play start");
        tts_on_play_started();
        return;
      }
      Serial.println("[TTS] play begin failed; skip sentence");
    } else {
      Serial.println("[TTS] sentence skipped (synth failed or timeout)");
    }
    return;
  }

  // セッション終了判定：投入済みチャンクが残っておらず合成も終わっている
  bool pending;
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  pending = !g_tts_queue.empty() || g_tts_synth_busy;
  xSemaphoreGive(g_tts_mutex);
  if (g_speech_session && !pending) {
    g_speech_session = false;
    Serial.println("[TTS] session end");
    avatar.setExpression(Expression::Neutral);
    speech_text_buffer = "";
    g_news_playing = false;
    g_pose_state = -1;  // 第9弾: セッション終了でポーズを待機/表情連動へ戻す
    g_led_news_active = false;
    if (g_cap_armed) captionReset();  // 全文の合成失敗などで再生されなかった場合
    delay(200);
    M5.Speaker.end();
    M5.Mic.begin();
    if (wakeword_is_enable) mode = 1;
    else mode = 0;
  }
}

// ニュース読み上げの中断（追加D）。中央タップから呼ばれる。
// 再生を即停止し、発話キュー（未合成分＋合成中分）と字幕キューを破棄して
// 待機状態へ戻す。セッション終了処理（Mic復帰・mode復帰）は、キューを
// 空にしたことで tts_drive() が通常の終了判定として実施する。
// 合成タスクが合成中でも、世代カウンタを進めることで結果を無効化できる。
static void stop_news_speech() {
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  g_tts_gen++;  // 合成中の文があっても、その結果は再生させない
  while (!g_tts_queue.empty()) g_tts_queue.pop_front();
  g_tts_ready = false;
  g_tts_ready_url = "";
  if (g_tts_ready_data != nullptr) {  // ダウンロード済みMP3も破棄する（第9.6弾）
    heap_caps_free(g_tts_ready_data);
    g_tts_ready_data = nullptr;
    g_tts_ready_len = 0;
  }
  g_tts_synth_busy = false;
  xSemaphoreGive(g_tts_mutex);
  if (mp3 != nullptr && mp3->isRunning()) mp3->stop();
  tts_cleanup_playback_sources();
  speech_text = "";
  speech_text_buffer = "";
  g_news_playing = false;
  g_pose_state = -1;  // 第9弾: 中断時もポーズを待機/表情連動へ戻す
  captionReset();  // 字幕キュー破棄・字幕パネル消去・LEDのニュース色解除
  // 中断フィードバック：確認音＋吹き出しに短く表示（loop 側で約1.5秒後に消す）
  M5.Speaker.setChannelVolume(7, VOL_VALUES[g_vol_level]);
  M5.Speaker.tone(1046.5f, 60, 7, false);
  avatar.setSpeechText("中断しました");
  g_stop_msg_until = millis() + 1500;
  Serial.println("[NEWS] interrupted by center tap");
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

// 側面LED（M5GO Bottom2 の SK6812×10）の状態表示タスク。
// 録音・応答待ちなどのブロッキング処理中でも光り方を更新できるよう、
// loop() から独立した専用タスクで駆動する。優先順位は
// ダンス動画再生中（色サイクル） > 音量ゲージ > ウェイクワード検出フラッシュ >
// 録音中 > 応答待ち >
// 発話中（ニュースはカテゴリ色／通常発話は音量連動の暖色） > 待機（呼吸点滅）。
// フレームが変化したときだけ FastLED.show() する（静止画中は RMT を止め、
// 録音などへの割り込み影響を抑える）。
void ledTask(void *args)
{
  (void)args;
  static CRGB prev_frame[SIDE_LED_COUNT];
  static bool prev_valid = false;
  float smooth_level = 0.0f;   // 発話中の明滅用に平滑化した音声レベル
  for (;;)
  {
    CRGB frame[SIDE_LED_COUNT];
    for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = CRGB::Black;
    uint32_t now = millis();

    if (g_led_enable) {
      if (now < g_led_dance_until) {
        // ダンス動画再生中：色相を回しながら低輝度でサイクル（全体上限でさらに抑制）
        for (int i = 0; i < SIDE_LED_COUNT; i++) {
          frame[i] = CHSV((uint8_t)(now / 12 + i * 25), 210, 120);
        }
      } else if (now < g_led_vol_until) {
        // 音量ゲージ：レベル×2 個を緑で点灯（6段階→0/2/4/6/8/10個）
        int lit = (int)g_vol_level * 2;
        for (int i = 0; i < lit && i < SIDE_LED_COUNT; i++) frame[i] = CRGB::Green;
      } else if (now < g_led_ww_until) {
        // ウェイクワード検出：白で2回点滅（250ms周期×約1秒）
        if ((now / 250) % 2 == 0) {
          for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = CRGB::White;
        }
      } else if (g_led_listening) {
        // 録音中：青
        for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = CRGB(0, 70, 180);
      } else if (g_led_thinking) {
        // 応答待ち：紫のパルス
        float ph = 0.5f + 0.5f * sinf(now * 0.008f);
        CRGB c(150, 0, 255);
        c.nscale8((uint8_t)(90 + 165 * ph));
        for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = c;
      } else if (mp3 != nullptr && mp3->isRunning()) {
        // 発話中：音声レベルに合わせて明滅（ニュースはカテゴリ色、通常発話は暖色）
        int level = abs(*out.getBuffer());
        if (level > 15000) level = 15000;
        float target = 0.30f + 0.70f * ((float)level / 15000.0f);
        smooth_level += (target - smooth_level) * 0.4f;
        CRGB base = g_led_news_active ? g_led_news_color : CRGB(255, 170, 60);
        CRGB c = base;
        c.nscale8((uint8_t)(255 * smooth_level));
        for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = c;
      } else {
        // 待機：ゆっくり呼吸点滅（定期通知モードONの間はオレンジ、通常は暖色）
        float ph = 0.5f + 0.5f * sinf(now * 0.0016f);
        CRGB base = g_periodic_on ? CRGB(255, 110, 0) : CRGB(255, 160, 70);
        CRGB c = base;
        c.nscale8((uint8_t)(50 + 90 * ph));
        for (int i = 0; i < SIDE_LED_COUNT; i++) frame[i] = c;
      }
    }

    if (!prev_valid || memcmp(prev_frame, frame, sizeof(frame)) != 0) {
      for (int i = 0; i < SIDE_LED_COUNT; i++) g_side_leds[i] = frame[i];
      FastLED.show();
      memcpy(prev_frame, frame, sizeof(frame));
      prev_valid = true;
    }
    delay(30);
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
  uint32_t stt_t0 = millis();
  Serial.printf("[STT] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.println("\r\nRecord start!\r\n");
  g_led_listening = true;   // 側面LED：録音中（青）

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
  g_led_listening = false;
  Serial.printf("[STT] done in %lu ms (%s)\n", (unsigned long)(millis() - stt_t0),
                isGoogle ? "Google" : "Whisper");
  return ret;
}

void setup()
{
  // 第9.3弾: グラフィックス直列化ロックを最初に生成する
  // （avatar.init() で描画タスクが始まる前に必ず用意する）
  g_gfx_mutex = xSemaphoreCreateRecursiveMutex();

  auto cfg = M5.config();

  cfg.external_spk = true;    /// use external speaker (SPK HAT / ATOMIC SPK)
//cfg.external_spk_detail.omit_atomic_spk = true; // exclude ATOMIC SPK
//cfg.external_spk_detail.omit_spk_hat    = true; // exclude SPK HAT
//  cfg.output_power = true;
  M5.begin(cfg);

  // 起動時診断（第8弾）: リセット要因と空きヒープを Serial と画面に出す。
  // 異常リセット（パニック/ウォッチドッグ/電圧低下）の切り分けを
  // 書き込みページのシリアルログで後から追えるようにするため。
  {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reason_str = "UNKNOWN";
    switch (reason) {
      case ESP_RST_POWERON:   reason_str = "POWERON";   break;
      case ESP_RST_EXT:       reason_str = "EXT";       break;
      case ESP_RST_SW:        reason_str = "SW";        break;
      case ESP_RST_PANIC:     reason_str = "PANIC";     break;
      case ESP_RST_INT_WDT:   reason_str = "INT_WDT";   break;
      case ESP_RST_TASK_WDT:  reason_str = "TASK_WDT";  break;
      case ESP_RST_WDT:       reason_str = "WDT";       break;
      case ESP_RST_DEEPSLEEP: reason_str = "DEEPSLEEP"; break;
      case ESP_RST_BROWNOUT:  reason_str = "BROWNOUT";  break;
      case ESP_RST_SDIO:      reason_str = "SDIO";      break;
      default: break;
    }
    Serial.printf("[BOOT] reset reason: %s (%d)\n", reason_str, (int)reason);
    Serial.printf("[BOOT] free heap: %u bytes, free PSRAM: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    M5.Display.printf("reset: %s\n", reason_str);
    M5.Display.printf("heap: %u\n", (unsigned)ESP.getFreeHeap());
  }

  // 第9.4弾: 再生バッファ（30KB）は PSRAM に置き、内蔵ヒープを空ける。
  // TLS ハンドシェイクは内蔵ヒープから大きな塊を必要とし、バッファが
  // 内蔵側にあると SSL のメモリ確保失敗（-32512）を招きやすかった。
  preallocateBuffer = (uint8_t *)ps_malloc(preallocateBufferSize);
  if (!preallocateBuffer) {
    preallocateBuffer = (uint8_t *)malloc(preallocateBufferSize);
  }
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
        uint8_t led_onoff = 1;   // 側面LED（M5GO Bottom2）の既定は ON（第7弾）
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

  { // 音量レベル（タッチ操作・6段階）を NVS "setting"/"vol" から読み込んで適用する
    uint8_t lv = 0;
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      if (ESP_OK == nvs_get_u8(nvs_handle, "vol", &lv) && lv <= 5) {
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

  { // 第7弾：スケジュール項目と側面LEDの ON/OFF を NVS "setting" から読み込む（未保存なら既定ON）
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      uint8_t v;
      if (ESP_OK == nvs_get_u8(nvs_handle, "s_morning", &v)) g_sched_morning = (v != 0);
      if (ESP_OK == nvs_get_u8(nvs_handle, "s_hourly", &v)) g_sched_hourly = (v != 0);
      if (ESP_OK == nvs_get_u8(nvs_handle, "led", &v)) g_led_enable = (v != 0);
      nvs_close(nvs_handle);
    }
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
        // 第9.2弾: APIキーの平文 Serial 出力を廃止（シリアルログへの露出防止）。
        // 読み込めた事実だけを出し、値そのものは絶対に出力しない。
        Serial.println("apikey.txt loaded from SD (values not logged)");
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
  WiFi.setAutoReconnect(true);  // 第8弾: ドライバ側の自動再接続も有効化（loop の監視と併用）
  M5.Lcd.println("\nConnected");

  // 時刻同期（スケジューラ用、第7弾）。現行ファームには NTP 同期が無かったため追加。
  // JST = UTC+9 固定、サーバは ntp.nict.jp と pool.ntp.org
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      Serial.println("NTP time sync OK");
    } else {
      Serial.println("NTP time sync pending (scheduler waits until synced)");
    }
  }

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
  server.on("/schedule", handle_schedule);
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

  // 側面LED（M5GO Bottom2 の SK6812×10 / GPIO25）初期化と表示タスク起動
  FastLED.addLeds<SK6812, SIDE_LED_PIN, GRB>(g_side_leds, SIDE_LED_COUNT);
  FastLED.setBrightness(SIDE_LED_MAX_BRIGHTNESS);
  FastLED.clear(true);
  xTaskCreatePinnedToCore(ledTask, "ledTask", 4096, NULL, 1, NULL, 0);

  // 文単位パイプラインTTS（第8弾）: 合成専用タスクを起動する。
  // TLS ハンドシェイク＋HTTP＋JSON でスタックを多く使うため大きめに確保
  if (g_tts_mutex == nullptr) g_tts_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(ttsSynthTask, "ttsSynth", 24576, NULL, 1, NULL, 0);

  // スケジューラ登録（毎朝7:00 モーニング読み上げ／時報 7〜23時）
  init_schedule();

  avatar.setFace(new KinakoFace());  // きなこ写真の顔に変更（2026-10-31 本人希望）
  // 第9.2弾: 初回ポーズ（待機＝ピース）は描画タスク開始前にここでデコード
  // しておく。draw 内ではデコードしない（drawLoop のスタック不足対策）。
  kinakoPreparePose(POSE_PEACE, true);
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

  // 第9.1弾: 起動時のダンス動画再生は setup() では行わない。
  // 第9弾では setup() 末尾で再生していたが、実機で再生中に panic して
  // ブートループになったため、loop() の初回（起動後 3 秒以降・タスクが
  // 回り始めた状態）に dance_boot_check() 経由で1回だけ再生する方式へ移した。
  // Wi-Fi 未設定の場合は Wifi_setup() の SmartConfig 待ち→再起動ループに
  // なるため、従来どおり初期設定を妨げることもない。
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
  g_pending_pose = POSE_THUMBSUP;  // 第9弾: バッテリー報告中は「サムズアップ」
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

// ==== ダンス動画再生（第9弾／第9.1弾で安全化） ====
// 埋め込みモーションJPEG (dance_mjpeg.h、10fps/320x240/100フレーム) の
// 連結 JPEG を順に LCD へ直接描画する。再生中は Avatar の描画タスクを
// suspend して干渉を防ぎ、終了後に resume する（setExpression は呼ばない
// こと。それ自体が suspend/resume を含むため）。
// 中断条件：画面タップ（どこでも）または物理ボタン A/B/C の押下。
// 1フレームも描けなかった場合（データ/デコード異常）は「踊る」ポーズ画像の
// 一時表示で代替し false を返す。最後まで再生したら true。
//
// 第9.1弾の安全化（第9弾の起動時再生で panic→ブートループした対策）:
//  - 各フレームを描画前に検証する：長さ上限 64KB、SOF で 320x240 を確認。
//    不正なフレームはスキップして続行し、デコードエラーが連続したら打ち切る。
//  - 再生前に空きヒープを確認し、逼迫時は再生自体を行わない。
//  - RTC メモリの「再生中」フラグでブートループを断つ：再生開始で立て、
//    panic で再起動するとフラグが残るため、起動時の自動再生はその回だけ
//    スキップされる（dance_boot_check 参照）。
//  - 起動時の自動再生は setup() ではなく loop() 初回に移設した。
//
// 第9.2弾のガード拡張：実機で「再生は完走したのに、その直後の顔描画再開で
// panic」する事象が確認されたため、フラグは再生完了で即座に下ろさず、
// 再生終了から 5 秒間（通常描画が安定するまで）保持する。その間に panic
// した場合も、次回起動の自動再生がスキップされる。フラグの解除は loop の
// dance_guard_tick() が行う。
RTC_DATA_ATTR static bool g_dance_playing_rtc = false;  // 再生中〜直後クラッシュ検出用
static uint32_t g_dance_guard_clear_at = 0;             // フラグ解除予定時刻 (millis、0=未設定)
static const uint32_t DANCE_FRAME_MAX = 65536;          // 1フレームの長さ上限
static const uint32_t DANCE_HEAP_MIN = 60000;           // 再生に必要な空きヒープ下限

// JPEG のセグメントを走査し、SOF0/SOF2 から画像寸法を取り出す。
// SOI で始まらない・走査が破綻する場合は false（＝検証NGとしてスキップ）。
static bool jpeg_frame_size(const uint8_t *f, uint32_t flen,
                            uint16_t *out_w, uint16_t *out_h) {
  if (f == nullptr || flen < 10 || f[0] != 0xFF || f[1] != 0xD8) return false;
  uint32_t p = 2;
  while (p + 3 < flen) {
    if (f[p] != 0xFF) return false;
    uint8_t marker = f[p + 1];
    if (marker == 0xC0 || marker == 0xC2) {  // SOF0 / SOF2
      if (p + 8 >= flen) return false;
      *out_h = ((uint16_t)f[p + 5] << 8) | f[p + 6];
      *out_w = ((uint16_t)f[p + 7] << 8) | f[p + 8];
      return true;
    }
    if (marker == 0xDA) return false;       // SOS 到達（SOF が無かった）
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
      p += 2;                               // 長さフィールドを持たないマーカー
      continue;
    }
    uint32_t seglen = ((uint32_t)f[p + 2] << 8) | f[p + 3];
    if (seglen < 2) return false;
    p += 2 + seglen;
  }
  return false;
}

static bool play_dance_video() {
  const uint8_t *data = dance_mjpeg;
  const uint32_t len = dance_mjpeg_len;
  if (data == nullptr || len < 1024) {
    Serial.println("[DANCE] aborted: embedded data invalid");
    g_pose_timed = POSE_DANCE;
    g_pose_timed_until = millis() + 9400;
    return false;
  }
  // ヒープが逼迫していたら再生自体を見送る（デコーダ確保失敗や競合を避ける）
  uint32_t heap_before = ESP.getFreeHeap();
  Serial.printf("[DANCE] start: data=%u bytes, free heap=%u\n",
                (unsigned)len, (unsigned)heap_before);
  if (heap_before < DANCE_HEAP_MIN) {
    Serial.println("[DANCE] skipped: low heap");
    return false;
  }
  g_dance_playing_rtc = true;   // クラッシュガード：再生中フラグを立てる
  // 第9.3弾: avatar.suspend() による描画タスクの凍結は廃止した。suspend は
  // 描画タスクをパネル transaction の途中で凍結し得て、SPI mutex の所有権
  // 競合の一因だった。代わりにグラフィックス直列化ロックを再生の間ずっと
  // 保持する: drawLoop は Avatar::draw() の入口でブロックされ、描画は
  // フレーム境界で確実に止まる。ガードは関数を抜けるときに自動解放され、
  // そこで描画が再開する。
  GfxLockGuard gfx_guard;
  g_led_dance_until = millis() + 11000;  // 側面LED：再生中は色サイクル
  const uint32_t frame_ms = 1000 / 10;   // 10fps（第9.1弾で軽量化）
  const uint32_t t0 = millis();
  uint32_t pos = 0;
  int frame_no = 0;        // 時間軸上の通し番号（検証スキップ・コマ落とし込み）
  int drawn = 0;           // 実際に描画できた数
  int skipped = 0;         // 検証NGで飛ばした数
  int errors = 0;          // drawJpg が失敗した数
  int consec_errors = 0;   // 連続デコードエラー数
  bool completed = true;
  while (pos < len) {
    // フレーム先頭 (SOI: FF D8) を探す
    while (pos + 1 < len && !(data[pos] == 0xFF && data[pos + 1] == 0xD8)) pos++;
    if (pos + 1 >= len) break;
    uint32_t fstart = pos;
    pos += 2;
    // フレーム終端 (EOI: FF D9) を探す
    while (pos + 1 < len && !(data[pos] == 0xFF && data[pos + 1] == 0xD9)) pos++;
    if (pos + 1 >= len) { completed = false; break; }
    uint32_t flen = (pos + 2) - fstart;
    pos += 2;
    frame_no++;

    // フレーム検証：長さ上限と SOF 寸法（320x240）。NG はスキップして続行
    uint16_t fw = 0, fh = 0;
    if (flen > DANCE_FRAME_MAX ||
        !jpeg_frame_size(&data[fstart], flen, &fw, &fh) ||
        fw != 320 || fh != 240) {
      skipped++;
      continue;
    }

    // 中断チェック（タップ／物理ボタン）
    M5.update();
    bool interrupt = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
    if (!interrupt && M5.Touch.getCount() > 0) {
      auto td = M5.Touch.getDetail();
      interrupt = td.wasPressed();
    }
    if (interrupt) { completed = false; break; }

    // 表示タイミング：経過時間から出すべきフレーム番号を求め、
    // デコード遅延で遅れていたら中間のフレームを読み飛ばして追いつく
    uint32_t elapsed = millis() - t0;
    int should = (int)(elapsed / frame_ms) + 1;
    if (frame_no < should) continue;  // コマ落とし
    uint32_t target = (uint32_t)(frame_no - 1) * frame_ms;
    if (target > elapsed && target - elapsed <= frame_ms) delay(target - elapsed);
    if (!M5.Lcd.drawJpg(&data[fstart], flen, 0, 0)) {
      errors++;
      if (++consec_errors >= 3) {
        Serial.println("[DANCE] aborted: consecutive decode errors");
        completed = false;
        break;
      }
      continue;  // 1フレームの失敗では止めず次へ
    }
    consec_errors = 0;
    drawn++;
  }
  g_led_dance_until = 0;
  // 第9.3弾: avatar.resume() は廃止（suspend 自体をやめたため）。
  // 第9.2弾: フラグはここでは下ろさない。再生直後の描画再開で落ちるケースを
  // ガードするため、5 秒間は立てたままにし、dance_guard_tick() が下ろす。
  g_dance_guard_clear_at = millis() + 5000;
  Serial.printf("[DANCE] done: drawn=%d skipped=%d errors=%d, heap %u -> %u (%s)\n",
                drawn, skipped, errors, (unsigned)heap_before,
                (unsigned)ESP.getFreeHeap(), completed ? "complete" : "stopped");
  if (drawn == 0) {
    // 再生できなかった場合はポーズ画像で代替表示する
    g_pose_timed = POSE_DANCE;
    g_pose_timed_until = millis() + 9400;
    return false;
  }
  return completed;
}

// 再生後ガードの解除（第9.2弾）。再生が正常に終わり、描画再開後 5 秒が
// 無事に経過したら RTC フラグを下ろす。途中で panic した場合はフラグが
// 残り、次回起動の自動再生がスキップされる。
static void dance_guard_tick() {
  if (g_dance_playing_rtc && g_dance_guard_clear_at != 0 &&
      (int32_t)(millis() - g_dance_guard_clear_at) >= 0) {
    g_dance_playing_rtc = false;
    g_dance_guard_clear_at = 0;
    Serial.println("[DANCE] guard cleared (stable after playback)");
  }
}

// 起動時の自動再生（第9.1弾で setup() から loop() 初回へ移設）。
// 起動後 3 秒経過して通常タスクが回り始めてから、Wi-Fi 接続済みなら
// 1 回だけ再生する。前回起動で再生中〜再生直後に panic していた場合
// （RTC フラグが立ったまま）は、フラグを下ろして今回の自動再生だけを
// スキップし、無限ブートループを断つ（音声トリガーの再生は通常どおり可能）。
static void dance_boot_check() {
  static bool decided = false;
  if (decided) return;
  if (g_dance_playing_rtc) {
    g_dance_playing_rtc = false;
    decided = true;
    Serial.println("[DANCE] boot autoplay skipped: previous playback crashed (boot-loop guard)");
    return;
  }
  uint32_t now = millis();
  if (now > 30000) { decided = true; return; }  // 条件が揃わなければ見送る
  if (now < 3000) return;                       // 起動直後はまだ待つ
  if (WiFi.status() != WL_CONNECTED) return;
  decided = true;
  Serial.println("[DANCE] boot autoplay");
  play_dance_video();
}

void SST_ChatGPT() {
        captionReset();  // 通常会話が始まるのでニュース表示キューは破棄する（ニュース分岐では exec_news が再構築する）
        g_pose_state = POSE_WAVE;  // 第9弾: 会話開始（挨拶〜聞き取り中）は「手振り」
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
            } else if (ret.indexOf("踊") >= 0 || ret.indexOf("ダンス") >= 0) {
              // ダンス動画も ChatGPT を経由しない特例（会話履歴には入れない）
              Serial.println("[DANCE] voice trigger");
              play_dance_video();
              g_pending_pose = POSE_LAUGH;
              speech_text = "踊ってみたよ。";
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
        // 第9弾: 会話開始ポーズ（手振り）を解除する。発話が始まる場合は
        // loop 側の発話開始処理がセッション用ポーズを改めて設定する
        g_pose_state = -1;
}

// 音量ゲージ文字列（6マス表示。例「音量 ■■■□□□」、レベル0は全□）
static String volGaugeText() {
  String s = "音量 ";
  for (int i = 0; i < 6; i++) s += (i < g_vol_level) ? "■" : "□";
  return s;
}

// 音量を delta 段階変える（右上/右下タップ、レベル0〜5・0は無音）
static void change_volume(int delta) {
  int nl = (int)g_vol_level + delta;
  if (nl < 0) nl = 0;
  if (nl > 5) nl = 5;
  bool changed = (nl != (int)g_vol_level);
  g_vol_level = (uint8_t)nl;
  applyVolLevel(changed);
  g_led_vol_until = millis() + 1200;   // 側面LEDにもゲージを表示（レベル×2個を緑で約1.2秒）
  if (!mp3->isRunning()) {
    sw_tone();
  } else {
    // 再生中は sw_tone（Speaker.end を伴う）で再生を止めないよう、
    // 別チャンネル(7)へ短い確認音だけを重ねる
    M5.Speaker.setChannelVolume(7, VOL_VALUES[g_vol_level]);
    M5.Speaker.tone(1320.0f, 70, 7, false);
  }
  // 吹き出しが空いているときだけゲージを表示し、約2秒で消す（loop 側で消去）
  if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
      !g_cap_active && !g_cap_armed && !g_menu_open) {
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
// 第9.3弾: LCD への直接描画はグラフィックス直列化ロックの中で行う
// （描画タスクのパネル transaction と begin/end が競合しないように）。
static void drawMenuList() {
  GfxLockGuard gfx_guard;
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
  if (mp3->isRunning() || speech_text != "" || speech_text_buffer != "" || g_cap_active) return;
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

  wifi_watchdog();  // 第8弾: Wi-Fi 切断の検出と自動再接続

  pose_prepare_tick();  // 第9.2弾: ポーズ画像の先行デコード（draw タスク外で実施）

  battery_icon_tick();  // 第9.6弾: バッテリー残量の定期取得（描画は顔合成側）

  dance_guard_tick();  // 第9.2弾: 再生後ガードの解除（再生終了から5秒安定したら）

  dance_boot_check();  // 第9.1弾: 起動時ダンス動画の自動再生（初回のみ・ガード付き）

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

  // スケジューラ（毎朝7:00モーニング読み上げ／時報）。発火可否は scheduler_ready()
  // で判定し、混雑時の延期・スキップは Scheduler 側で処理する。
  // 定期通知モードとは独立に動作する
  run_schedule();

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
      // 右上＝音量アップ、右下＝音量ダウン（6段階・レベル0は無音）
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
      // 中央タップの優先順位：ニュース読み上げ中＝中断（追加D）／
      // メニュー表示中＝キャンセル／それ以外＝会話開始
      if (box_stt.contain(t.x, t.y))
      {
        if (g_news_playing || g_pending_news_speech)
        {
          stop_news_speech();
        }
        else if (g_menu_open)
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
      if (mode >= 0 && !mp3->isRunning() && speech_text == "" && speech_text_buffer == "" && !g_cap_active) {
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
        !g_cap_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  // 「中断しました」の吹き出しを約1.5秒で消す（同上の条件で、他の表示が始まっていたら触らない）
  if (g_stop_msg_until != 0 && (int32_t)(millis() - g_stop_msg_until) >= 0) {
    g_stop_msg_until = 0;
    if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_cap_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  // 通知吹き出し（ニュース取得失敗など、第9.6弾）を約4秒で消す（同上の条件）
  if (g_info_msg_until != 0 && (int32_t)(millis() - g_info_msg_until) >= 0) {
    g_info_msg_until = 0;
    if (!mp3->isRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_cap_active && !g_menu_open) {
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
    // 第8弾: 文単位パイプラインへ投入する。合成は専用タスクが文ごとに非同期で
    // 行い、最初の文の合成が終わり次第 tts_drive() が再生を始める。
    // 字幕表示の有効化も、最初の文の再生開始時に tts_drive() 側で行う
    // （ここでは arm のままにしておく）。
    // ニュース由来の発話なら、中断対象としてセッション中フラグを立てる。
    g_news_playing = g_pending_news_speech;
    g_pending_news_speech = false;
    // 第9弾: 発話セッション中のポーズを決める
    // （発話側の明示指定を優先し、無ければニュース=サムズアップ、それ以外は表情連動）
    if (g_pending_pose >= 0) {
      g_pose_state = g_pending_pose;
      g_pending_pose = -1;
    } else if (g_news_playing) {
      g_pose_state = POSE_THUMBSUP;
    } else {
      g_pose_state = -1;
    }
    tts_enqueue(speech_text_buffer, TTS_PARMS);
  }

  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      tts_cleanup_playback_sources();  // ストリーム/メモリ両対応の後片付け（第9.6弾）
      Serial.println("mp3 stop");
      // 発話セッションの終了処理（表情・Mic・mode の復帰）は、続きの文が
      // 無いことを確認してから tts_drive() が行う（文ごとに繰り返さない）
    }
    delay(1);
  }
  tts_drive();
  if (!mp3->isRunning()) {
    server.handleClient();
  }

  // 読み上げ字幕（2行パネル）を進める（発話中はユニットを順に切り替え、
  // 発話が終わったら最後のユニットを3秒残してから消す）
  if (g_cap_active) {
    if (mp3->isRunning()) {
      g_cap_clear_ms = 0;
      if (g_cap_idx < g_cap_count - 1 &&
          (int32_t)(millis() - g_cap_next_ms) >= 0) {
        g_cap_idx++;
        captionShowUnit(g_cap_idx);
        g_cap_next_ms = millis() + capUnitDurationMs(g_cap_idx);
      }
    } else if (speech_text_buffer == "") {
      // 発話セッション終了後に、最後のユニットを3秒残してから消す。
      // 第8弾: 文単位パイプラインでは文間に再生が止まる時間があるため、
      // 従来の「開始から5秒超過」という時間ベースの早期終了条件は使わない
      // （セッション終了は speech_text_buffer のクリアで正確に分かる）
      if (g_cap_clear_ms == 0) {
        if (g_cap_idx != g_cap_count - 1) {
          g_cap_idx = g_cap_count - 1;
          captionShowUnit(g_cap_idx);
        }
        g_cap_clear_ms = millis() + 3000;
      } else if ((int32_t)(millis() - g_cap_clear_ms) >= 0) {
        captionReset();
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
        g_led_ww_until = millis() + 1000;   // 側面LED：検出を白フラッシュで知らせる
        g_pose_timed = POSE_JUMP;           // 第9弾: 検出直後は約1秒「ジャンプ」ポーズ
        g_pose_timed_until = millis() + 1000;
        SST_ChatGPT();
    }
  }
}
 
