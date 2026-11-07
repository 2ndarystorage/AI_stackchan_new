#include <Arduino.h>
#include <atomic>
static bool audioRunning();
#include <esp_system.h>  // esp_reset_reason（起動時診断、第8弾）
#include <esp_heap_caps.h>  // 最大連続ブロック計測（メモリ切り分け、第9.6弾）
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include "OperatingMode.h"
#include "OperatingModePage.h"
#include <Avatar.h>
#if !defined(STACKCHAN_AVATAR_GFX_LOCKED) || STACKCHAN_AVATAR_GFX_LOCKED != 0x0913
#error "Use the vendored M5Stack-Avatar with the shared graphics lock"
#endif
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
#include "HttpBody.h"  // chunked デコード付き本文読み取り（第9.22弾）
#include "rootCACertificate.h"
#include "rootCAgoogle.h"
#include "rootCANHK.h"
#include "rootCAopenmeteo.h"
#include "rootCAtrain.h"
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
#include "TlsGate.h"
#include "TlsMemory.h"
#include "VersionBadge.h"
#include "AudioHandover.h"
#include "SpeakerVolume.h"
#include "AudioDiagnosticsPage.h"
#include "Mp3DiagnosticLogger.h"
#include <driver/i2s.h>
#include "GfxLock.h"  // グラフィックス直列化ロック（第9.3弾）

// グラフィックス直列化ロックの実体（第9.3弾、GfxLock.h）。
// setup() の最初で生成し、描画タスク (drawLoop) と loop 側の直接描画が
// 同じ LGFX オブジェクトを同時に触らないよう直列化する。
SemaphoreHandle_t g_gfx_mutex = nullptr;
SemaphoreHandle_t g_tls_mutex = nullptr;
static OperatingMode g_operating_mode = OperatingMode::Normal;
static bool g_mode_switch_pending = false;
static uint32_t g_mode_restart_at = 0;

// Called by the vendored Avatar while the shared graphics mutex is held.
// 第9.23弾: バッジは Face::draw から合成キャンバスを受け取って描く（点滅対策で
// パネル直接描画から移設。バッテリー/音量 HUD と同じキャンバス内描画になった）
void stackchan_draw_version_badge(M5Canvas *spi) {
  if (spi) drawFirmwareVersionBadge(*spi);
}
static bool audioBeginMicrophone() {
  return handoverToMicrophone(M5.Speaker, M5.Mic,
      [](i2s_port_t port) { i2s_driver_uninstall(port); });
}

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

// ==== LED輝度の段階設定（第9.19弾） ====
// レベル0〜5 → FastLED の全体輝度。既定レベル4 = 従来の固定上限
// SIDE_LED_MAX_BRIGHTNESS(6) と同じ見え方になるよう対応付けている。
// 設定ダイアログ（右上タップ）で変更し、NVS "setting"/"ledlevel" に保存する。
// 既存の ON/OFF（g_led_enable、/schedule ページ）とは独立で矛盾しない。
// 第9.23弾: 低い側の刻みが 1/255 単位で見分けられなかったため、レベル3を 3→4 に
// 見直し（既定レベル4=6 と上限レベル5=12 は従来どおり）
static const uint8_t LED_LEVEL_VALUES[6] = {0, 1, 2, 4, 6, 12};
uint8_t g_led_level = 4;             // LED輝度レベル（0〜5、既定4。KinakoFace.h から参照）
static void applyLedLevel(bool save) {
  FastLED.setBrightness(LED_LEVEL_VALUES[g_led_level]);
  if (save) {
    nvs_handle_t h;
    if (nvs_open("setting", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_u8(h, "ledlevel", g_led_level);
      nvs_commit(h);
      nvs_close(h);
    }
  }
}
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
  // Apply user gain only once; keep speech and tone channels at unity.
  setSpeechVolume(M5.Speaker, v);
  if (save) {
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
      nvs_set_u8(nvs_handle, "vol", g_vol_level);
      nvs_set_u32(nvs_handle, "volume", v);
      nvs_commit(nvs_handle);
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
static const NewsCatDef NEWS_CATS[6] = {
  {"トピックス",   "今日のニュースです。",     0,  "https://news.google.com/rss?hl=ja&gl=JP&ceid=JP:ja"},
  {"AIニュース",   "AIニュースです。",        -1, "https://news.google.com/rss/search?q=AI&hl=ja&gl=JP&ceid=JP:ja"},
  {"芸能ニュース", "芸能ニュースです。",       6,  "https://news.google.com/rss/search?q=%E8%8A%B8%E8%83%BD&hl=ja&gl=JP&ceid=JP:ja"},
  {"テック",       "テックニュースです。",    -1, "https://news.google.com/rss/search?q=%E3%83%86%E3%82%AF%E3%83%8E%E3%83%AD%E3%82%B8%E3%83%BC&hl=ja&gl=JP&ceid=JP:ja"},
  {"その他",       "その他のニュースです。",   1,  "https://news.google.com/rss/search?q=%E7%A4%BE%E4%BC%9A&hl=ja&gl=JP&ceid=JP:ja"},
  // 第9.23弾: BLE で転送されたテキスト（exec_news_category で特別処理。nhkCat=-2 が目印）
  {"転送ニュース", "転送ニュースです。",      -2, ""},
};
// カテゴリ表示名のアクセサ（KinakoFace.h のメニュー合成描画から使う、第9.26弾）
const char *newsCatLabel(int idx) {
  if (idx < 0 || idx > 5) return "";
  return NEWS_CATS[idx].label;
}
// カテゴリごとの側面LED色（トピックス=白、AI=水色、芸能=ピンク、テック=緑、その他=黄、転送=紫）
static const CRGB NEWS_CAT_COLORS[6] = {
  CRGB::White, CRGB(0, 180, 220), CRGB(255, 80, 150), CRGB(0, 200, 80), CRGB(240, 200, 0), CRGB(170, 90, 255),
};
bool g_menu_open = false;        // カテゴリ選択メニュー表示中
int g_menu_sel = 0;              // 選択中のカテゴリ（0〜4）
uint32_t g_menu_deadline = 0;    // 最後のタップから約2.5秒後に確定する時刻

// ==== 画面消灯・設定ダイアログ（第9.19弾） ====
#define DISPLAY_IDLE_OFF_MS 300000UL     // 無操作で自動消灯するまでの時間（5分）
#define CENTER_TAP_WINDOW_MS 300         // 中央ダブルタップの判定窓（ms）。シングルタップ（会話開始）の発火はこの分だけ遅れる
#define BTNC_TAP_WINDOW_MS 300           // BtnC ダブルタップの判定窓（ms）。短押し（定期通知トグル）の発火はこの分だけ遅れる
#define SETTINGS_DIALOG_TIMEOUT_MS 5000  // 設定ダイアログの無操作自動クローズ（ms）
bool g_settings_open = false;            // 設定ダイアログ表示中（KinakoFace.h から参照）
uint32_t g_settings_deadline = 0;        // ダイアログの自動クローズ時刻
static bool g_display_off = false;       // 画面が消灯中か
static uint32_t g_last_activity_ms = 0;  // 最終操作時刻（タッチ/物理ボタン/発話開始で更新）
static bool g_center_tap_pending = false;  // 中央シングルタップの発火待ち（ダブルタップ判定中）
static uint32_t g_center_tap_ms = 0;       // 中央タップを受け付けた時刻

static void note_activity() { g_last_activity_ms = millis(); }

// 画面を消灯する。描画タスクと同じ GfxLock の中でパネルを操作し、描画と競合させない
static void display_sleep() {
  if (g_display_off) return;
  g_display_off = true;
  g_settings_open = false;
  g_menu_open = false;
  g_center_tap_pending = false;
  {
    GfxLockGuard gfx_guard;
    M5.Display.sleep();  // バックライトOFF＋パネルスリープ（M5GFX）
  }
  Serial.println("[DISPLAY] sleep");
}

// 画面を点灯する。復帰に使った入力そのものは呼び出し側で消費し、他の機能に回さない
static void display_wake() {
  if (!g_display_off) return;
  g_display_off = false;
  note_activity();
  {
    GfxLockGuard gfx_guard;
    M5.Display.wakeup();  // スリープ解除＋バックライトを元の輝度へ復帰
  }
  Serial.println("[DISPLAY] wake");
}

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

// ==== 第9.26弾: パンくずログ（ウォッチドッグ・リセットの箇所特定用） ====
// loop の主要区間の入口で現在の区間名を g_crumb に残し、loop が15秒ごとに
// [HB] としてヒープ残量とともに出力する。TG1WDT リセットが再発した場合、
// リセット直前の [HB] の crumb が「最後にいた区間」を示す。
// 代入は文字列リテラルへのポインタ差し替えのみ（確保なし・軽量）。
static volatile const char *g_crumb = "boot";
static inline void crumb(const char *section) { g_crumb = section; }

String exec_news(bool automatic = false);  // 実体は後方（ニュース読み上げの節）で定義
static void exec_digest_category();    // 第9.23弾: 転送ニュースの読み上げ（実体は後方）
static void append_unread_digest();    // 第9.23弾: 自動読み上げへの未読転送テキスト連結（実体は後方）
String chatGptWithTools(const String& json_string);  // 第9.25弾: Function Calling 対応の会話（実体は後方）
static void memory_inject_facts();                   // 第9.25弾: 長期記憶をシステムプロンプトへ差し込む（実体は後方）
static void memory_note_exchange(const String& user_text, const String& asst_text);  // 第9.25弾: 長期記憶の自動抽出キュー（実体は後方）
static bool ble_digest_active();       // 第9.23弾: BLE 受信モード中か（実体は後方）
void sw_tone();                        // 実体は後方（音まわり）
static bool play_dance_video();      // 実体は後方（SST_ChatGPT の直前）で定義。第9弾
static void dance_boot_check();      // 起動時1回だけの自動再生判定（loop から呼ぶ）。第9.1弾

// スケジューラの発火可否ゲート：発話中・ニュース表示中・メニュー操作中・
// ウェイクワード登録中（mode<0）は発火しない。録音〜応答生成（SST_ChatGPT）は
// loop 内の同期処理のため、その間 run_schedule() 自体が呼ばれず割り込まれない
static bool scheduler_ready() {
  return mode >= 0 && !audioRunning() && speech_text == "" && speech_text_buffer == "" &&
         !g_menu_open && !g_cap_active && !ble_digest_active();  // 第9.23弾: BLE受信中は発火しない
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
  exec_news(true);  // speech_text にニュース本文をセットする（自動読み上げ: 新しい記事がなければニュース部分は無言でスキップし、挨拶のみになる。第9.20弾）
  append_unread_digest();  // 第9.23弾: 未読の転送テキストがあればニュースの後に連結する
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
// 合成して、合成できた文から即座に再生する。合成1文の上限は
// WebVoiceVoxTTS 側で 25 秒とし、超えた文はスキップして次へ進む。
// 状態遷移は loop() 内の tts_drive()（実体は音声再生まわりの後方）が担う。
// 第9.10弾: 先行合成の深さを1文から最大3文へ広げた（ready キュー化）。
// 合成+ダウンロードに6〜12秒かかる一方で短い文の再生は2秒未満もあり、
// 先行1文では文間の無音ギャップが避けられなかったため。さらにセッション
// 先頭では2文目が揃うまで再生開始を最大20秒だけ待つ（プリバッファ）。
struct TtsChunk {
  String text;   // 合成する文（チャンク）
  String parms;  // そのチャンクの話者パラメータ（&speaker=N）
};
struct TtsReadyItem {
  uint8_t *data;      // ダウンロード済みMP3（PSRAM。所有権はキュー保持中は合成タスク側→消費時に再生側へ）
  size_t len;         // data のバイト数（末尾ガードパディング込み）
};
static std::deque<TtsChunk> g_tts_queue;        // 合成待ちチャンク
static std::deque<TtsReadyItem> g_tts_ready_q;  // 合成済みチャンク（先行キュー）
static constexpr size_t TTS_READY_MAX = 3;      // 先行合成の最大深さ（第9.10弾）
static bool g_tts_played_in_session = false;    // このセッションで再生を開始したか（プリバッファ判定用）
static uint32_t g_tts_prebuf_since = 0;         // 先頭チャンクが ready になってプリバッファ待ちに入った時刻（0=待ちでない）
static bool g_tts_synth_busy = false;      // 合成タスクが1文を処理中
static bool g_speech_session = false;      // 発話セッション中（投入〜全チャンク再生完了）
static SemaphoreHandle_t g_tts_mutex = nullptr;
static uint32_t g_tts_gen = 0;             // 発話セッションの世代（中断時に進めて合成結果を無効化）
static std::atomic<bool> g_tts_release_req{false};     // 接続ペア解放の依頼（第9.8弾: 合成タスク自身がアイドル時に実行する）
static uint32_t g_stop_msg_until = 0;      // 「中断しました」吹き出しを消す時刻（0=非表示）
static uint32_t g_info_msg_until = 0;      // 通知吹き出し（ニュース取得失敗など）を消す時刻（0=非表示。第9.6弾）

static unsigned g_tts_failed_chunks = 0;
static bool g_tts_certificate_failed = false;
static bool g_tts_output_failed = false;
static bool tts_processing() {
  if (!g_tts_mutex) return false;
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  bool busy = g_speech_session || g_tts_synth_busy;
  xSemaphoreGive(g_tts_mutex);
  return busy;
}

void ttsSynthTask(void *args);  // 実体は後方（setup でタスク起動）
void tts_drive();               // 実体は後方（loop から毎回呼ぶ）

// テキストを文（。！？と改行で区切る。文末記号は文に含める）に分割し、
// チャンク列にして返す。先頭の文も後続とまとめ、
// 短い導入文も約300バイトまで連結して合成リクエスト数を抑える
// （第9.4弾：約100→約200バイト。tts.quest のレート制限（429）対策として
// v9.12では先頭を含め約300バイトにまとめる）。
static std::deque<TtsChunk> splitSpeechChunks(const String& text, const String& parms) {
  std::deque<String> sentences;
  int n = (int)text.length();
  int start = 0;
  int i = 0;
  while (i < n) {
    uint8_t b = (uint8_t)text[i];
    // 文末記号（。=E3 80 82 / ！=EF BC 81 / ？=EF BC 9F）
    if (b == 0xE3 && i + 2 < n && (uint8_t)text[i + 1] == 0x80 &&
        ((uint8_t)text[i + 2] == 0x82)) {
      sentences.push_back(text.substring(start, i + 3));
      i += 3;
      start = i;
    } else if ((b == 0xEF && i + 2 < n && (uint8_t)text[i + 1] == 0xBC &&
                ((uint8_t)text[i + 2] == 0x81 || (uint8_t)text[i + 2] == 0x9F))) {
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

  for (auto& s0 : sentences) {
    String s = s0;
    s.trim();
    if (s.length() == 0) continue;
    if (cur.length() == 0) {
      cur = s;
      continue;
    }
    if (cur.length() + s.length() > 300) {
      TtsChunk c;
      c.text = cur;
      c.parms = parms;
      chunks.push_back(c);
      cur = s;

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
  if (!g_speech_session) {
    g_speech_session = true;
    g_tts_failed_chunks = 0;
    g_tts_certificate_failed = false;
    g_tts_output_failed = false;
    g_tts_played_in_session = false;  // 第9.10弾: 新セッションではプリバッファ判定をリセット
    g_tts_prebuf_since = 0;
  }
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
  server.send(200, "text/html", "<html><head><meta charset=\"UTF-8\"/></head><body><pre>hello from m5stack!\n&boxdr;&boxh;&boxh;&boxh;&boxdl;\n&boxv;&#729;_&#729;&boxv;\n&boxur;&boxh;&boxh;&boxh;&boxul;</pre><p><a href=\"/mode\">動作モード設定（通常 / BLE転送専用）</a></p><p><a href=\"/memory\">長期記憶メモリ（覚えていること）</a></p></body></html>");
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

String https_post_json(const char* url, const char* json_string, const char* root_ca, int* out_http_code = nullptr) {
  String payload = "";
  if (out_http_code) *out_http_code = 0;
  TlsGateGuard tls_guard;
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
        if (out_http_code) *out_http_code = httpCode;
  
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
  memory_inject_facts();  // 第9.25弾: 長期記憶をシステムプロンプトへ追加
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
    response = chatGptWithTools(json_string);
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
  crumb("chat");
  captionReset();  // 通常会話が始まるのでニュース表示キューは破棄する
  static String response = "";
  Serial.println(InitBuffer);
  init_chat_doc(InitBuffer.c_str());
  applyChatSpeedSettings();
  memory_inject_facts();  // 第9.25弾: 長期記憶をシステムプロンプトへ追加
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
    response = chatGptWithTools(json_string);
    speech_text = response;
    g_pending_pose = POSE_LAUGH;  // 第9弾: 会話応答の再生中は「笑う」ポーズ
    // 会話応答文も2行字幕の対象にする（発話の再生開始時に表示）
    capBuildFromText(response);
    g_led_news_color = CRGB::White;
    if (g_cap_count > 0) g_cap_armed = true;
    // 返答をチャット履歴に追加
    chatHistory.push_back(response);
    memory_note_exchange(text, response);  // 第9.25弾: 長期記憶の自動抽出キューへ（発話後にアイドル時実行）
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

  long requestedVolume = value == "" ? M5.Speaker.getVolume() : value.toInt();
  size_t volume = requestedVolume < 0 ? 0 : (requestedVolume > 255 ? 255 : requestedVolume);
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
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
  g_vol_level = volumeToLevel((uint8_t)volume);
  applyVolLevel(true);
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

// ==== ニュースの鮮度・重複対策（第9.20弾） ====
// 実機で古い台風ニュース（フィード先頭に残り続ける過去記事）を繰り返し
// 読み上げる事象への対策。
// (a) 鮮度: 記事の pubDate が48時間より古いものは読み上げ対象から外す。
//     日付が取れない記事は除外しない（重複防止側で繰り返しを防ぐ）。
//     未来日時の異常値（現在+24時間より先）は「日付なし」として扱う。
// (b) 重複: 読み上げ済み記事のタイトルハッシュを SPIFFS に記録し、
//     次回以降はスキップする。保持は最大60件・5日で自動的に掃除する。
// (c) すべて古い/既読で新しい記事がなかった場合は g_news_nothing_new を立て、
//     手動では「新しいニュースはありません」と読み上げ、自動読み上げ
//     （定期通知・スケジューラ）では無言でスキップする（exec_news 側）。
#define NEWS_OLD_LIMIT_SEC (48L * 3600L)
#define NEWS_SEEN_MAX 60
#define NEWS_SEEN_KEEP_SEC (5L * 24L * 3600L)
#define NEWS_SEEN_PATH "/news_seen.txt"

bool g_news_nothing_new = false;     // 直近の exec_news 系で「新しい記事がなかった」
static bool g_news_feed_ok = false;  // いずれかのフィードの取得自体には成功した
static int g_news_skip_old = 0;      // 直近の取得で古さにより除外した件数
static int g_news_skip_seen = 0;     // 直近の取得で既読により除外した件数

// 時刻が NTP 同期済みらしい値か（2026-01-01 UTC 以降）。未同期時は鮮度判定をしない
static bool newsNowValid(time_t now) { return now > (time_t)1767225600L; }

// civil 日付 → epoch 日数（days_from_civil）
static int64_t newsDaysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = (int64_t)y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// RSS の pubDate（RFC822: "Sun, 01 Nov 2026 06:30:00 +0900" / "... GMT"）を
// epoch 秒に変換する。解釈できなければ 0（= 日付なし）を返す
static time_t newsParseRssDate(const String& s) {
  static const char* MONS[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int monPos = -1, mon = 0;
  for (int i = 0; i < 12; i++) {
    int p = s.indexOf(MONS[i]);
    if (p >= 0) { monPos = p; mon = i + 1; break; }
  }
  if (monPos < 0) return 0;
  // 日: 月名の直前にある数値
  int day = 0;
  {
    int i = monPos - 1;
    while (i >= 0 && s[i] == ' ') i--;
    int end = i + 1;
    while (i >= 0 && isDigit(s[i])) i--;
    if (end - (i + 1) <= 0) return 0;
    day = s.substring(i + 1, end).toInt();
  }
  // 年: 月名の後ろにある4桁数値
  int year = 0, yearEnd = -1;
  for (int i = monPos + 3; i + 4 <= (int)s.length(); i++) {
    if (isDigit(s[i]) && isDigit(s[i + 1]) && isDigit(s[i + 2]) && isDigit(s[i + 3])) {
      year = s.substring(i, i + 4).toInt();
      yearEnd = i + 4;
      break;
    }
  }
  if (day <= 0 || year < 2000) return 0;
  // 時刻: 年の後ろの HH:MM:SS
  int hh = 0, mm = 0, ss = 0;
  int colon = (yearEnd > 0) ? s.indexOf(':', yearEnd) : -1;
  if (colon >= 2) {
    hh = s.substring(colon - 2, colon).toInt();
    mm = s.substring(colon + 1, colon + 3).toInt();
    if (colon + 3 < (int)s.length() && s[colon + 3] == ':') ss = s.substring(colon + 4, colon + 6).toInt();
  }
  // タイムゾーン: "+0900"/"-0500" 形式。無ければ GMT/UT は +0、JST は +9時間
  long tzSec = 0;
  bool tzFound = false;
  for (int i = 0; i + 4 < (int)s.length(); i++) {
    if ((s[i] == '+' || s[i] == '-') && isDigit(s[i + 1]) && isDigit(s[i + 2]) && isDigit(s[i + 3]) && isDigit(s[i + 4])) {
      long th = s.substring(i + 1, i + 3).toInt();
      long tm = s.substring(i + 3, i + 5).toInt();
      tzSec = (th * 3600 + tm * 60) * (s[i] == '-' ? -1 : 1);
      tzFound = true;
      break;
    }
  }
  if (!tzFound && s.indexOf("JST") >= 0) tzSec = 9 * 3600;
  int64_t epoch = newsDaysFromCivil(year, mon, day) * 86400LL + hh * 3600LL + mm * 60LL + ss - tzSec;
  return (time_t)epoch;
}

// 記事が「古い」（48時間超）かどうか。日付なし・時刻未同期・未来の異常値は古いとしない
static bool newsIsOld(time_t pub, time_t now) {
  if (pub == 0 || !newsNowValid(now)) return false;
  if (pub > now + 24L * 3600L) return false;  // 未来日時の異常値は日付なし扱い
  return (now - pub) > NEWS_OLD_LIMIT_SEC;
}

// タイトルの FNV-1a 32bit ハッシュ（重複判定用）
static uint32_t newsTitleHash(const String& title) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < title.length(); i++) {
    h ^= (uint8_t)title[i];
    h *= 16777619u;
  }
  return h;
}

struct NewsSeenEntry { uint32_t hash; uint32_t epoch; };
static NewsSeenEntry g_news_seen[NEWS_SEEN_MAX];
static int g_news_seen_count = 0;
static bool g_news_seen_loaded = false;

static void newsSeenLoad() {
  if (g_news_seen_loaded) return;
  g_news_seen_loaded = true;
  g_news_seen_count = 0;
  File f = SPIFFS.open(NEWS_SEEN_PATH, FILE_READ);
  if (!f) return;
  while (f.available() && g_news_seen_count < NEWS_SEEN_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    int sp = line.indexOf(' ');
    if (sp <= 0) continue;
    g_news_seen[g_news_seen_count].hash = (uint32_t)strtoul(line.substring(0, sp).c_str(), nullptr, 16);
    g_news_seen[g_news_seen_count].epoch = (uint32_t)line.substring(sp + 1).toInt();
    g_news_seen_count++;
  }
  f.close();
  Serial.printf("[News] seen store loaded: %d entries\n", g_news_seen_count);
}

// 第9.26弾: 既読ストアの保存は「ダーティーフラグ＋後段で一括保存」に変更した。
// 従来は記事が採用されるたびに（＝Google RSS のストリーム走査の最中に）
// SPIFFS へ全件書き戻しをしていた。ESP32 のフラッシュ書き込み/消去は他コアを
// キャッシュ無効化で止めるため、走査中の書き込みは割り込みウォッチドッグ
// （TG1WDT）を踏む経路になり得る（v9.25 初回セッションのリセットの最有力候補）。
// 保存自体は取得完了後（TLS を閉じた後）に1回だけ行う。
static bool g_news_seen_dirty = false;

// 保持期間（5日）を過ぎた記録を掃除してから全件を書き戻す
static void newsSeenSave() {
  crumb("spiffs-seen");
  time_t now = time(nullptr);
  bool nowOk = newsNowValid(now);
  int w = 0;
  for (int i = 0; i < g_news_seen_count; i++) {
    uint32_t ep = g_news_seen[i].epoch;
    // epoch 0 = 時刻未同期時の記録。日付では消さず、件数上限側で押し出す
    if (nowOk && ep != 0 && (int64_t)now - (int64_t)ep > NEWS_SEEN_KEEP_SEC) continue;
    g_news_seen[w++] = g_news_seen[i];
  }
  g_news_seen_count = w;
  File f = SPIFFS.open(NEWS_SEEN_PATH, FILE_WRITE);
  if (!f) { Serial.println("[News] seen store save failed"); return; }
  for (int i = 0; i < g_news_seen_count; i++) {
    f.printf("%08lx %lu\n", (unsigned long)g_news_seen[i].hash, (unsigned long)g_news_seen[i].epoch);
  }
  f.close();
}

static bool newsSeenContains(uint32_t h) {
  newsSeenLoad();
  for (int i = 0; i < g_news_seen_count; i++) if (g_news_seen[i].hash == h) return true;
  return false;
}

// 読み上げ対象に決まった記事を記録する（満杯時は最も古い記録を押し出す）
static void newsSeenAdd(uint32_t h) {
  newsSeenLoad();
  if (newsSeenContains(h)) return;
  if (g_news_seen_count >= NEWS_SEEN_MAX) {
    int oldest = 0;
    for (int i = 1; i < g_news_seen_count; i++) if (g_news_seen[i].epoch < g_news_seen[oldest].epoch) oldest = i;
    for (int i = oldest; i + 1 < g_news_seen_count; i++) g_news_seen[i] = g_news_seen[i + 1];
    g_news_seen_count--;
  }
  time_t now = time(nullptr);
  g_news_seen[g_news_seen_count].hash = h;
  g_news_seen[g_news_seen_count].epoch = newsNowValid(now) ? (uint32_t)now : 0;
  g_news_seen_count++;
  // 第9.26弾: ここではメモリ上の記録だけ更新し、SPIFFS への書き戻しは
  // 取得完了後に newsSeenFlush() が一括で行う（走査中のフラッシュ書き込み回避）
  g_news_seen_dirty = true;
}

// ダーティーなら既読ストアを1回だけ書き戻す（取得系の出口と loop の空きで呼ぶ）
static void newsSeenFlush() {
  if (!g_news_seen_dirty) return;
  g_news_seen_dirty = false;
  newsSeenSave();
}

// 1記事分の鮮度・重複フィルタ。true = 読み上げ対象にする（その場合は既読に記録する）
static bool newsAcceptArticle(const String& title, time_t pub) {
  if (newsIsOld(pub, time(nullptr))) {
    g_news_skip_old++;
    Serial.printf("[News] skip old article: %s\n", title.c_str());
    return false;
  }
  uint32_t h = newsTitleHash(title);
  if (newsSeenContains(h)) {
    g_news_skip_seen++;
    Serial.printf("[News] skip already-read article: %s\n", title.c_str());
    return false;
  }
  newsSeenAdd(h);
  return true;
}

// NHKニュースRSS（カテゴリ nhkCat）の本文を取得する（第9.6弾で分離）。
// 成否コードは g_nhk_http_code に残す（接続失敗時は負値）。
static String fetchNhkRssBody(int nhkCat) {
  crumb("news-nhk");
  String payload = "";
  g_nhk_http_code = 0;
  Serial.printf("[NHK RSS] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  TlsGateGuard tls_guard;
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
          int readChunks = 0;
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
              // 第9.26弾: データが流れ続ける場合でも loop を独占しないよう、
              // 約4KB 読むごとに短く譲る（アイドルタスクと他処理の飢餓防止）
              if ((++readChunks & 7) == 0) delay(1);
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
  g_news_feed_ok = true;  // フィード自体は取得できた（第9.20弾）

  String result = intro;
  int pos = 0;
  int count = 0;
  while (count < 3) {
    int itemPos = payload.indexOf("<item>", pos);
    if (itemPos < 0) break;
    pos = itemPos + 6;
    String title = xmlExtract(payload, "title", itemPos);
    if (title == "") break;
    // pubDate は次の <item> の手前までで探す（他記事の日付を拾わない）
    String pubStr = "";
    {
      int nextItem = payload.indexOf("<item>", pos);
      int pubPos = payload.indexOf("<pubDate>", itemPos);
      if (pubPos >= 0 && (nextItem < 0 || pubPos < nextItem)) pubStr = xmlExtract(payload, "pubDate", itemPos);
    }
    // 第9.20弾: 48時間より古い記事と既読記事は読み上げ対象から外す
    if (!newsAcceptArticle(title, newsParseRssDate(pubStr))) continue;
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
  crumb("news-google");
  outCount = 0;
  httpCodeOut = 0;
  Serial.printf("[Google RSS] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  TlsGateGuard tls_guard;
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
          const char* patPub = "<pubDate>";
          const char* patItemEnd = "</item>";
          int state = 0;  // 0:<item>待ち 1:<title>待ち 2:見出し取得中 3:<pubDate>待ち 4:pubDate取得中
          int match = 0;
          int matchEnd = 0;
          String title = "";
          // 第9.20弾: 見出しは <pubDate>（または </item>）まで保留し、鮮度・重複
          // フィルタを通ったものだけを outTitles へ出す（pubDate は title の後ろにある）
          String pendingTitle = "";
          String pubStr = "";
          auto emitPending = [&]() {
            if (pendingTitle == "") return;
            if (outCount < want && newsAcceptArticle(pendingTitle, newsParseRssDate(pubStr))) {
              outTitles[outCount++] = pendingTitle;
            }
            pendingTitle = "";
            pubStr = "";
          };
          const uint32_t MAX_SCAN = 120 * 1024;
          uint32_t scanned = 0;
          uint32_t startMs = millis();
          uint8_t buf[256];
          int readChunks = 0;
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
            // 第9.26弾: 高速に流れ込む場合でも約1KB ごとに譲り、loop の
            // 独占を避ける（従来はデータがある間 delay なしで回り続けていた）
            if ((++readChunks & 3) == 0) delay(1);
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
                    pendingTitle = title;  // 第9.20弾: pubDate を見てから確定する
                    state = 3;
                  } else {
                    state = 0;
                  }
                  title = "";
                  match = 0;
                  matchEnd = 0;
                }
              } else if (state == 4) {
                if (pubStr.length() < 64) pubStr += c;
                if (pubStr.endsWith("</pubDate>")) {
                  pubStr = pubStr.substring(0, pubStr.length() - 10);
                  pubStr.trim();
                  emitPending();
                  state = 0;
                  match = 0;
                  matchEnd = 0;
                }
              } else if (state == 3) {
                // <pubDate> と </item> の先に来た方で確定する（日付なし記事も拾う）
                if (c == patPub[match]) {
                  match++;
                  if (patPub[match] == '\0') {
                    state = 4;
                    match = 0;
                    pubStr = "";
                  }
                } else {
                  match = (c == patPub[0]) ? 1 : 0;
                }
                if (state == 3) {
                  if (c == patItemEnd[matchEnd]) {
                    matchEnd++;
                    if (patItemEnd[matchEnd] == '\0') {
                      emitPending();
                      state = 0;
                      match = 0;
                      matchEnd = 0;
                    }
                  } else {
                    matchEnd = (c == patItemEnd[0]) ? 1 : 0;
                  }
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
          emitPending();  // 第9.20弾: 走査打ち切り時に残った見出しを日付なしとして拾う
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
  if (httpCodeOut == 200) g_news_feed_ok = true;  // フィード自体は取得できた（第9.20弾）
  if (!ok && httpCodeOut < 0) {
    Serial.printf("[Google RSS] retry after connection failure (code %d)\n", httpCodeOut);
    delay(1500);
    ok = fetchGoogleHeadlinesOnce(url, want, outTitles, outCount, httpCodeOut);
    if (httpCodeOut == 200) g_news_feed_ok = true;
  }
  return ok;
}

// ニュース読み上げ文を組み立てて speech_text にセットする。読み上げ文を返す（全て失敗した場合は空文字）
// あわせて字幕キュー（見出し＋本文）を構築し、次の発話開始時に表示が始まるよう arm する
String exec_news(bool automatic) {
  if (tts_processing()) { avatar.setSpeechText("音声処理の終了を待ってください"); return ""; }
  crumb("news");
  // 第9.8弾: ニュース取得（TLS）の前に、TTS接続ペアの解放を合成タスクへ
  // 依頼し、少し待って内蔵ヒープ（最大連続ブロック）を回復させてから
  // 取得する。前セッションの TLS バッファが残っていると、Google RSS の
  // ハンドシェイクがメモリ確保失敗（-32512）になる事象への対策。
  g_tts_release_req = true;
  for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);
  // 第9.20弾: 鮮度・重複対策の状態を初期化する
  g_news_nothing_new = false;
  g_news_feed_ok = false;
  g_news_skip_old = 0;
  g_news_skip_seen = 0;
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
  // 第9.26弾: 2本目の TLS ハンドシェイクはヒープに余裕があるときだけ行う。
  // v9.25 初回セッションでは1本目の取得開始時点で free=51432 / largest=49140
  // まで逼迫していた。largest が 40000 を切る状態では無理に繋がず、一般
  // ニュースだけで読み上げる（安全弁。閾値は実測の動作点 49140 に余裕を持たせた）。
  String aiHeads[2];
  int aiCount = 0;
  if (ESP.getFreeHeap() >= 45000 &&
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 40000) {
    String heads[2];
    int n = 0;
    if (fetchGoogleHeadlines("https://news.google.com/rss/search?q=AI&hl=ja&gl=JP&ceid=JP:ja", 2, heads, n, g_google_ai_http_code)) {
      text += (text == "") ? String("AIニュースです。") : String("続いて、AIニュースです。");
      for (int i = 0; i < n; i++) {
        text += heads[i] + "。";
        aiHeads[aiCount++] = heads[i];
      }
    }
  } else {
    Serial.printf("[News] AI feed skipped: low heap (free=%u largest=%u)\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
  } else if (g_news_feed_ok) {
    // 第9.20弾: 取得はできたが、古い記事と既読ばかりで新しい記事がなかった
    g_news_nothing_new = true;
    Serial.printf("[News] nothing new (skipped old=%d seen=%d, automatic=%d)\n",
                  g_news_skip_old, g_news_skip_seen, automatic ? 1 : 0);
    if (!automatic) {
      speech_text = "新しいニュースはありません";
      avatar.setSpeechText("新しいニュースはありません");
      g_info_msg_until = millis() + 4000;
      g_pending_news_speech = true;
    }
    // 自動読み上げ（定期通知・スケジューラ）の場合は無言でスキップする
  } else {
    speech_text = "ニュースを取得できませんでした";
    // 第9.6弾: 音声が出ない場合でも理由が分かるよう、吹き出しにも表示して残す
    avatar.setSpeechText("ニュースを取得できませんでした");
    g_info_msg_until = millis() + 4000;
    g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
  }
  newsSeenFlush();  // 第9.26弾: 既読ストアの書き戻しは取得完了後（TLS終了後）に1回だけ
  crumb("loop");
  return text;
}

// カテゴリ単独のニュース読み上げ（タッチメニュー用）
// NHKカテゴリがあれば NHK（見出し＋説明×3）を優先し、失敗時は Google 見出し×3 で代替。
// NHKのないカテゴリ（AI／テック）は最初から Google 見出し×3。
// 従来の exec_news()（トピックス＋AI連結）とは独立した読み上げにする
void exec_news_category(int cat) {
  if (tts_processing()) { avatar.setSpeechText("音声処理の終了を待ってください"); return; }
  crumb("news-cat");
  if (cat < 0 || cat > 5) cat = 0;
  if (cat == 5) { exec_digest_category(); return; }  // 第9.23弾: 転送ニュース
  const NewsCatDef& c = NEWS_CATS[cat];
  // 第9.8弾: exec_news() と同様、取得前に TTS 接続ペアの解放を依頼する
  g_tts_release_req = true;
  for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);
  // 第9.20弾: 鮮度・重複対策の状態を初期化する
  g_news_nothing_new = false;
  g_news_feed_ok = false;
  g_news_skip_old = 0;
  g_news_skip_seen = 0;
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
    g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
  } else if (g_news_feed_ok) {
    // 第9.20弾: 取得はできたが、古い記事と既読ばかりで新しい記事がなかった
    // （カテゴリ読み上げは手動トリガーのため、その旨を短く読み上げる）
    g_news_nothing_new = true;
    Serial.printf("[News] nothing new in category (skipped old=%d seen=%d)\n",
                  g_news_skip_old, g_news_skip_seen);
    speech_text = "新しいニュースはありません";
    avatar.setSpeechText("新しいニュースはありません");
    g_info_msg_until = millis() + 4000;
    g_pending_news_speech = true;
  } else {
    speech_text = "ニュースを取得できませんでした";
    // 第9.6弾: 音声が出ない場合でも理由が分かるよう、吹き出しにも表示して残す
    avatar.setSpeechText("ニュースを取得できませんでした");
    g_info_msg_until = millis() + 4000;
    g_pending_news_speech = true;  // この発話はニュース由来（中央タップで中断可能）
  }
  newsSeenFlush();  // 第9.26弾: 既読ストアの書き戻しは取得完了後に1回だけ
  crumb("loop");
}

void handle_news() {
  String news = exec_news();
  String diag = String("\n[diag] NHK httpCode=") + String(g_nhk_http_code)
              + " GoogleTop httpCode=" + String(g_google_top_http_code)
              + " GoogleAI httpCode=" + String(g_google_ai_http_code)
              + " skippedOld=" + String(g_news_skip_old)
              + " skippedSeen=" + String(g_news_skip_seen);
  if (news != "") {
    server.send(200, "text/plain", news + diag);
  } else if (g_news_nothing_new) {
    server.send(200, "text/plain", String("新しいニュースはありません") + diag);
  } else {
    server.send(200, "text/plain", String("ニュースを取得できませんでした") + diag);
  }
}

// ==== 天気予報（Open-Meteo・第9.20弾） ====
// ChatGPT はネットを見られないため、ニュースと同じ方式で本体が直接取得して
// 読み上げる。取得先は Open-Meteo（APIキー不要）、対象は東京（35.68, 139.69）。
// TLS は rootCAopenmeteo.h（ISRG Root X1 + X2。SSL Labs で両エンドポイントの
// 実チェーン確認済み）をピン留めし、TlsGateGuard で他 TLS と直列化する。
int g_weather_http_code = 0;  // 診断用（/weather のレスポンスで確認できる）

static const char* WEATHER_API_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=35.68&longitude=139.69"
  "&daily=weathercode,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
  "&timezone=Asia%2FTokyo";

// Open-Meteo の JSON 本文を取得する（1回分）。HTTPコードは g_weather_http_code に残す
// 第9.22弾: 本文は HttpBody.h の ChunkReader で読む。Open-Meteo は
// Transfer-Encoding: chunked で返すため、生ストリームをそのまま String に
// 入れると先頭にチャンクサイズ行（実測で本文638バイトに対し+12バイト）が
// 混ざり、deserializeJson が失敗していた（v9.21 実機で確認）。
// Content-Length 形式の応答でも同じ経路で読める。
static bool fetchWeatherJsonOnce(String& out) {
  out = "";
  g_weather_http_code = 0;
  Serial.printf("[Weather] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  TlsGateGuard tls_guard;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setCACert(root_ca_openmeteo);
  client->setTimeout(10000);
  bool ok = false;
  {
    HTTPClient https;
    https.setTimeout(10000);
    const char* reqHeaders[] = {"Content-Length", "Transfer-Encoding"};
    https.collectHeaders(reqHeaders, 2);
    if (https.begin(*client, WEATHER_API_URL)) {
      int code = https.GET();
      g_weather_http_code = code;
      Serial.printf("[Weather] GET... code: %d\n", code);
      if (code == HTTP_CODE_OK) {
        int size = https.getSize();  // Content-Length（無ければ -1）
        String te = https.header("Transfer-Encoding");
        te.toLowerCase();
        bool chunked = te.indexOf("chunked") >= 0;
        Serial.printf("[Weather] headers: content-length=%d transfer-encoding=%s (mode=%s)\n",
                      size, te.c_str(), chunked ? "chunked" : (size > 0 ? "identity" : "close"));
        WiFiClient *stream = https.getStreamPtr();
        if (stream) {
          ChunkReader reader;
          reader.begin(stream, chunked, 12000);
          String payload;
          int readStatus = 1;
          bool complete = httpReadBody(reader, payload, 32 * 1024,
                                       size > 0 ? size : -1, &readStatus);
          Serial.printf("[Weather] body %u bytes (complete=%d, status=%d)\n",
                        (unsigned)payload.length(), (int)complete, readStatus);
          if (complete && payload.length() > 0) { out = payload; ok = true; }
        }
      }
      https.end();
    }
  }
  delete client;
  return ok;
}

// 接続失敗時は、全て解放したうえで1回だけ再試行する（ニュース取得と同じ方針）
static bool fetchWeatherJson(String& out) {
  bool ok = fetchWeatherJsonOnce(out);
  if (!ok && g_weather_http_code < 0) {
    Serial.printf("[Weather] retry after connection failure (code %d)\n", g_weather_http_code);
    delay(1500);
    ok = fetchWeatherJsonOnce(out);
  }
  return ok;
}

// WMO weather code → 日本語の天気表現
static String weatherCodeJa(int code) {
  switch (code) {
    case 0: return "晴れ";
    case 1: return "晴れ時々曇り";
    case 2: return "曇り時々晴れ";
    case 3: return "曇り";
    case 45: case 48: return "霧";
    case 51: case 53: case 55: case 56: case 57: return "小雨";
    case 61: case 63: case 66: case 67: return "雨";
    case 65: return "強い雨";
    case 71: case 73: case 77: return "雪";
    case 75: return "強い雪";
    case 80: case 81: return "にわか雨";
    case 82: return "激しいにわか雨";
    case 85: case 86: return "にわか雪";
    case 95: case 96: case 99: return "雷雨";
    default: break;
  }
  if (code >= 95) return "雷雨";
  if (code >= 85) return "にわか雪";
  if (code >= 80) return "にわか雨";
  if (code >= 70) return "雪";
  if (code >= 60) return "雨";
  if (code >= 50) return "小雨";
  return "曇り";
}

// "2026-11-02" → 曜日（0=日 … 6=土）。Sakamoto の方法。解釈できなければ -1
static int weatherWeekday(const String& dateStr) {
  if (dateStr.length() < 10) return -1;
  int y = dateStr.substring(0, 4).toInt();
  int m = dateStr.substring(5, 7).toInt();
  int d = dateStr.substring(8, 10).toInt();
  if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
  static const int T[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

static const char* WEATHER_WDAYS[] = {"日曜日", "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日"};

// 天気の読み上げ文を組み立てる（発話の副作用なし。第9.25弾で Function Calling の
// get_weather ツールからも使うため、取得＋文生成だけを分離した）
// dayOffset >= 0: その日（0=今日 1=明日 2=明後日）／ dayOffset < 0: 週間（7日分）
static String buildWeatherText(int dayOffset) {
  // ニュース取得と同様、TLS の前に TTS 接続ペアの解放を依頼してヒープを回復させる
  g_tts_release_req = true;
  for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);
  String text = "";
  String payload;
  if (fetchWeatherJson(payload)) {
    DynamicJsonDocument doc(8192);
    DeserializationError jerr = deserializeJson(doc, payload);
    if (jerr == DeserializationError::Ok) {
      JsonArray times = doc["daily"]["time"];
      JsonArray codes = doc["daily"]["weathercode"];
      JsonArray tmaxs = doc["daily"]["temperature_2m_max"];
      JsonArray tmins = doc["daily"]["temperature_2m_min"];
      JsonArray pops  = doc["daily"]["precipitation_probability_max"];
      size_t days = times.size();
      if (days > 0 && codes.size() == days && tmaxs.size() == days && tmins.size() == days) {
        if (dayOffset >= 0) {
          if ((size_t)dayOffset < days) {
            const char* label = (dayOffset == 0) ? "今日" : (dayOffset == 1) ? "明日" : "明後日";
            text = String(label) + "の東京の天気は" + weatherCodeJa(codes[dayOffset].as<int>())
                 + "、最高気温は" + String((int)lroundf(tmaxs[dayOffset].as<float>())) + "度"
                 + "、最低気温は" + String((int)lroundf(tmins[dayOffset].as<float>())) + "度";
            if (pops.size() == days && !pops[dayOffset].isNull()) {
              text += "、降水確率は" + String(pops[dayOffset].as<int>()) + "パーセント";
            }
            text += "です。";
          }
        } else {
          text = "東京の週間天気予報です。";
          for (size_t i = 0; i < days && i < 7; i++) {
            String dateStr = times[i].as<String>();
            int wd = weatherWeekday(dateStr);
            text += String(dateStr.substring(5, 7).toInt()) + "月"
                  + String(dateStr.substring(8, 10).toInt()) + "日";
            if (wd >= 0) text += String("（") + WEATHER_WDAYS[wd] + "）";
            text += "は" + weatherCodeJa(codes[i].as<int>())
                  + "、最高" + String((int)lroundf(tmaxs[i].as<float>())) + "度"
                  + "、最低" + String((int)lroundf(tmins[i].as<float>())) + "度。";
          }
        }
      }
    } else {
      // 第9.22弾: 失敗理由（InvalidInput / NoMemory 等）と本文先頭を残す
      Serial.printf("[Weather] JSON parse failed: %s (body %u bytes, head=\"%.24s\")\n",
                    jerr.c_str(), (unsigned)payload.length(), payload.c_str());
    }
  }
  return text;
}

// 天気の読み上げ文を組み立てて speech_text にセットする（ニュースと同じ発話経路）
static String exec_weather_impl(int dayOffset) {
  if (tts_processing()) { avatar.setSpeechText("音声処理の終了を待ってください"); return ""; }
  captionReset();  // 前回の表示キューが残っていたら破棄する
  String text = buildWeatherText(dayOffset);
  if (text != "") {
    // 会話応答と同じく、読み上げ文を2行字幕の対象にする
    capBuildFromText(text);
    g_led_news_color = CRGB::White;
    if (g_cap_count > 0) g_cap_armed = true;
    speech_text = text;
  } else {
    speech_text = "天気予報を取得できませんでした";
    avatar.setSpeechText("天気予報を取得できませんでした");
    g_info_msg_until = millis() + 4000;
  }
  g_pending_news_speech = true;  // ニュースと同じく中央タップで中断可能にする
  return text;
}

String exec_weather(int dayOffset) { return exec_weather_impl(dayOffset); }
String exec_weather_weekly() { return exec_weather_impl(-1); }

void handle_weather() {
  String w = exec_weather(1);  // テスト用: 明日の予報を読み上げ開始する
  String diag = String("\n[diag] Weather httpCode=") + String(g_weather_http_code);
  if (w != "") {
    server.send(200, "text/plain", w + diag);
  } else {
    server.send(200, "text/plain", String("天気予報を取得できませんでした") + diag);
  }
}

// ==== 電車の運行情報（日テレ防災・第9.21弾） ====
// ChatGPT はネットを見られないため、天気と同じ方式で本体が直接取得して
// 読み上げる。取得元は日テレ気象・防災サイトの関東エリア鉄道情報ページ
// （bosai.news.ntv.co.jp）。「現在運行情報のある路線」だけが
// 会社名・路線名・状態（列車遅延／運転見合わせ等）とともに列挙される形式で、
// 対象路線が載っていなければ平常と判断できる。無料の公式APIはなく、
// かつて広く使われた RTI技研の delay.json は提供元解散で停止済みのため、
// このページを採用した（2026-11-01 実データで形式を確認）。
// TLS は rootCAtrain.h（ISRG Root X1 + X2）をピン留めし、TlsGateGuard で
// 他 TLS と直列化する。ページ全体は保持せず、直近バイト列だけを残す
// ストリーミング走査で対象路線名とその直後の状態語を探す（Transfer-Encoding:
// chunked の場合は生ストリームをデコードしながら走査する）。
int g_train_http_code = 0;  // 診断用（/train のレスポンスで確認できる）

static const char* TRAIN_INFO_URL =
  "https://bosai.news.ntv.co.jp/railway/area/%E9%96%A2%E6%9D%B1";  // 関東

// 状態の重大度（大きいほど重い。同路線に複数エントリがあれば最大を採る）
#define TRAIN_ST_NONE   0
#define TRAIN_ST_RESUME 1  // 運転再開
#define TRAIN_ST_DELAY  2  // 列車遅延
#define TRAIN_ST_DISORDER 3  // ダイヤ乱れ
#define TRAIN_ST_PLAN   4  // 運転計画
#define TRAIN_ST_SUSPEND 5  // 運転見合わせ

struct TrainLineState {
  const char* pattern;    // ページ内で探す路線名（部分一致。東急/JR の接頭辞に依存しない）
  const char* speakName;  // 読み上げ用路線名
  int status;             // TRAIN_ST_*
  int phase;              // 0=路線名を検索中, 1=状態語の窓を収集中, 2=確定
  uint32_t nameEndAbs;    // 路線名一致の終了位置（コンテンツ絶対位置）
  uint32_t minPos;        // 次に路線名を探す下限（コンテンツ絶対位置）
};

static int trainEvalStatus(const String& win) {
  if (win.indexOf("運転見合わせ") >= 0 || win.indexOf("運休") >= 0) return TRAIN_ST_SUSPEND;
  if (win.indexOf("運転計画") >= 0) return TRAIN_ST_PLAN;
  if (win.indexOf("ダイヤ乱れ") >= 0 || win.indexOf("ダイヤの乱れ") >= 0 ||
      win.indexOf("運転変更") >= 0) return TRAIN_ST_DISORDER;
  if (win.indexOf("列車遅延") >= 0 || win.indexOf("遅延") >= 0) return TRAIN_ST_DELAY;
  if (win.indexOf("運転再開") >= 0) return TRAIN_ST_RESUME;
  return TRAIN_ST_NONE;
}

// 運行情報ページを1回取得し、対象路線の状態を lines[] に埋める
static bool fetchTrainStatusOnce(TrainLineState* lines, int nLines) {
  g_train_http_code = 0;
  Serial.printf("[Train] start heap=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  TlsGateGuard tls_guard;
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setCACert(root_ca_train);
  client->setTimeout(12000);
  bool ok = false;
  {
    HTTPClient https;
    https.setTimeout(12000);
    const char* reqHeaders[] = {"Transfer-Encoding"};
    https.collectHeaders(reqHeaders, 1);
    if (https.begin(*client, TRAIN_INFO_URL)) {
      int code = https.GET();
      g_train_http_code = code;
      Serial.printf("[Train] GET... code: %d\n", code);
      if (code == HTTP_CODE_OK) {
        WiFiClient *stream = https.getStreamPtr();
        if (stream) {
          bool chunked = String(https.header("Transfer-Encoding")).indexOf("chunked") >= 0;
          // チャンク転送デコーダの状態
          int chunkState = 0;          // 0=サイズ行, 1=データ, 2=データ直後の改行読み捨て
          uint32_t chunkLeft = 0;
          String sizeLine;
          // ストリーミング走査の状態（recent は直近バイト列のみ保持）
          String recent;
          recent.reserve(1400);
          uint32_t total = 0;          // コンテンツ総バイト数
          uint32_t recentBase = 0;     // recent[0] のコンテンツ絶対位置
          const uint32_t WINDOW = 400; // 路線名の直後で状態語を探す範囲
          const uint32_t deadline = millis() + 25000;
          const uint32_t MAX_CONTENT = 1024 * 1024;
          uint8_t rbuf[512];
          bool streamDone = false;
          // コンテンツバイト列を走査へ流し込む（走査状態を前進させる）
          auto feedContent = [&](const uint8_t* p, int n) {
            recent.concat((const char*)p, (unsigned int)n);
            total += (uint32_t)n;
            if (recent.length() > 1200) {
              int drop = (int)recent.length() - 1200;
              recent.remove(0, drop);
              recentBase += (uint32_t)drop;
            }
            for (int li = 0; li < nLines; li++) {
              TrainLineState& L = lines[li];
              if (L.phase == 0) {
                int from = (L.minPos > recentBase) ? (int)(L.minPos - recentBase) : 0;
                int idx = recent.indexOf(L.pattern, from);
                if (idx >= 0) {
                  L.nameEndAbs = recentBase + (uint32_t)idx + strlen(L.pattern);
                  L.phase = 1;
                }
              }
              if (L.phase == 1 && total - L.nameEndAbs >= WINDOW) {
                int start = (int)(L.nameEndAbs - recentBase);
                if (start < 0) start = 0;
                String win = recent.substring(start, start + (int)WINDOW);
                int cut = win.indexOf("</a>");
                int cut2 = win.indexOf("<a");
                if (cut < 0 || (cut2 >= 0 && cut2 < cut)) cut = cut2;
                if (cut >= 0) win = win.substring(0, cut);
                int st = trainEvalStatus(win);
                if (st > L.status) L.status = st;
                if (st > TRAIN_ST_NONE) {
                  L.phase = 2;
                } else {
                  L.phase = 0;              // 名前だけの出現だったので後方で探し直す
                  L.minPos = L.nameEndAbs;
                }
              }
            }
          };
          while ((int32_t)(millis() - deadline) < 0 && total < MAX_CONTENT && !streamDone) {
            bool allDone = true;
            for (int li = 0; li < nLines; li++) if (lines[li].phase != 2) allDone = false;
            if (allDone) break;  // 全路線が確定したら残りは読まない
            size_t avail = stream->available();
            if (avail == 0) {
              if (!stream->connected()) break;
              delay(1);
              continue;
            }
            int n = stream->read(rbuf, avail < sizeof(rbuf) ? avail : sizeof(rbuf));
            if (n <= 0) { delay(1); continue; }
            if (!chunked) {
              feedContent(rbuf, n);
            } else {
              // チャンク転送をデコードしながら feedContent へ渡す
              int i = 0;
              while (i < n && !streamDone) {
                if (chunkState == 0) {
                  char c = (char)rbuf[i++];
                  if (c == '\n') {
                    long sz = strtol(sizeLine.c_str(), nullptr, 16);
                    sizeLine = "";
                    if (sz <= 0) { streamDone = true; break; }
                    chunkLeft = (uint32_t)sz;
                    chunkState = 1;
                  } else if (c != '\r') {
                    sizeLine += c;
                    if (sizeLine.length() > 10) sizeLine = "";  // 異常な行は捨てる
                  }
                } else if (chunkState == 1) {
                  int take = (int)((uint32_t)(n - i) < chunkLeft ? (uint32_t)(n - i) : chunkLeft);
                  feedContent(rbuf + i, take);
                  i += take;
                  chunkLeft -= (uint32_t)take;
                  if (chunkLeft == 0) chunkState = 2;
                } else {
                  // チャンク直後の CRLF を読み捨てる（LF まで）
                  char c = (char)rbuf[i++];
                  if (c == '\n') chunkState = 0;
                }
              }
            }
          }
          // 窓が WINDOW に満たないまま終わった路線は、残りの窓で最終判定する
          for (int li = 0; li < nLines; li++) {
            TrainLineState& L = lines[li];
            if (L.phase == 1) {
              int start = (int)(L.nameEndAbs - recentBase);
              if (start >= 0 && start < (int)recent.length()) {
                String win = recent.substring(start);
                int cut = win.indexOf("</a>");
                int cut2 = win.indexOf("<a");
                if (cut < 0 || (cut2 >= 0 && cut2 < cut)) cut = cut2;
                if (cut >= 0) win = win.substring(0, cut);
                int st = trainEvalStatus(win);
                if (st > L.status) L.status = st;
              }
              L.phase = 2;
            }
          }
          Serial.printf("[Train] scanned %u bytes\n", (unsigned)total);
          ok = true;  // HTTP 200 で本文を読めた（路線が無ければ平常扱い）
        }
      }
      https.end();
    }
  }
  delete client;
  return ok;
}

static bool fetchTrainStatus(TrainLineState* lines, int nLines) {
  bool ok = fetchTrainStatusOnce(lines, nLines);
  if (!ok && g_train_http_code < 0) {
    Serial.printf("[Train] retry after connection failure (code %d)\n", g_train_http_code);
    for (int li = 0; li < nLines; li++) {  // 再試行前に走査状態を初期化する
      lines[li].status = TRAIN_ST_NONE;
      lines[li].phase = 0;
      lines[li].nameEndAbs = 0;
      lines[li].minPos = 0;
    }
    delay(1500);
    ok = fetchTrainStatusOnce(lines, nLines);
  }
  return ok;
}

// 運行情報の読み上げ文を組み立てる（発話の副作用なし。第9.25弾で Function Calling の
// get_train_delays ツールからも使うため、取得＋文生成だけを分離した）
static String buildTrainText() {
  // ニュース取得と同様、TLS の前に TTS 接続ペアの解放を依頼してヒープを回復させる
  g_tts_release_req = true;
  for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);
  TrainLineState lines[] = {
    {"田園都市線", "田園都市線", TRAIN_ST_NONE, 0, 0, 0},
    {"大井町線",   "大井町線",   TRAIN_ST_NONE, 0, 0, 0},
    {"南武線",     "南武線",     TRAIN_ST_NONE, 0, 0, 0},
  };
  const int nLines = 3;
  String text = "";
  if (fetchTrainStatus(lines, nLines)) {
    for (int li = 0; li < nLines; li++) {
      Serial.printf("[Train] %s status=%d\n", lines[li].speakName, lines[li].status);
      if (lines[li].status == TRAIN_ST_NONE) continue;
      text += String(lines[li].speakName) + "は";
      switch (lines[li].status) {
        case TRAIN_ST_SUSPEND:  text += "運転を見合わせています。"; break;
        case TRAIN_ST_PLAN:     text += "運転計画の変更が予定されています。"; break;
        case TRAIN_ST_DISORDER: text += "ダイヤが乱れています。"; break;
        case TRAIN_ST_DELAY:    text += "列車に遅れが出ています。"; break;
        case TRAIN_ST_RESUME:   text += "運転を再開しています。"; break;
        default: break;
      }
    }
    if (text == "") {
      text = "田園都市線、大井町線、南武線に遅れはありません。";
    }
  }
  return text;
}

// 運行情報の読み上げ文を組み立てて speech_text にセットする（ニュースと同じ発話経路）
static String exec_train() {
  if (tts_processing()) { avatar.setSpeechText("音声処理の終了を待ってください"); return ""; }
  captionReset();  // 前回の表示キューが残っていたら破棄する
  String text = buildTrainText();
  if (text != "") {
    // 会話応答と同じく、読み上げ文を2行字幕の対象にする
    capBuildFromText(text);
    g_led_news_color = CRGB::White;
    if (g_cap_count > 0) g_cap_armed = true;
    speech_text = text;
  } else {
    speech_text = "運行情報を取得できませんでした";
    avatar.setSpeechText("運行情報を取得できませんでした");
    g_info_msg_until = millis() + 4000;
  }
  g_pending_news_speech = true;  // ニュースと同じく中央タップで中断可能にする
  return text;
}

void handle_train() {
  String t = exec_train();  // テスト用: 対象3路線の運行情報を読み上げ開始する
  String diag = String("\n[diag] Train httpCode=") + String(g_train_http_code);
  if (t != "") {
    server.send(200, "text/plain", t + diag);
  } else {
    server.send(200, "text/plain", String("運行情報を取得できませんでした") + diag);
  }
}

// ==== 第9.25弾: Function Calling ＋ 長期記憶メモリ ====
// 会話（exec_chatGPT / handle_chat の共通経路）で ChatGPT に tools を渡し、
// モデルからの tool_calls を本体が実行して結果を返す。実行できるのは
// タイマー登録・天気・運行情報・ニュース・現在日時・長期記憶への保存。
// あわせて、会話の内容から「次回以降も役立つ事実」を抽出して長期記憶に
// 保存し（SD 優先）、次の会話のシステムプロンプトへ差し込む。
// 既存のキーワード分岐（天気・電車・ニュース）は速い経路として従来どおり
// チャット前に処理され、ここはその対象外の会話だけが通る。

// ---------- 長期記憶メモリ ----------
#define MEM_FACTS_PATH "/memory_facts.txt"
#define MEM_FACTS_MAX 40
#define MEM_FACTS_BYTES 4096
#define MEM_FACT_LINE_MAX 200

static std::deque<String> g_mem_facts;
static bool g_mem_loaded = false;
static bool g_mem_on_sd = false;

// digest_open と同じ作法（SD 優先・なければ SPIFFS）
static fs::FS *memfs_open(bool &use_sd) {
use_sd = false;
if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
use_sd = true;
return &SD;
}
if (!SPIFFS.begin(false)) return nullptr;
return &SPIFFS;
}
static void memfs_close(bool use_sd) { if (use_sd) SD.end();}

// UTF-8 の文字境界を壊さないように最大バイト数で切り詰める
static String mem_truncate_utf8(const String& s, size_t max_bytes) {
size_t len = (size_t)s.length();
if (len <= max_bytes) return s;
size_t total = 0, i = 0;
while (i < len) {
uint8_t b = (uint8_t)s[i];
size_t cl = (b < 0x80)? 1: ((b & 0xE0) == 0xC0)? 2: ((b & 0xF0) == 0xE0)? 3: ((b & 0xF8) == 0xF0)? 4: 1;
if (total + cl > max_bytes) break;
total += cl;
i += cl;
}
return s.substring(0, (unsigned)i);
}

static void memory_load() {
if (g_mem_loaded) return;
g_mem_loaded = true;
bool use_sd;
fs::FS *fsys = memfs_open(use_sd);
if (!fsys) {
Serial.println("[MEM] load: filesystem open failed");
return;
}
g_mem_on_sd = use_sd;
if (fsys->exists(MEM_FACTS_PATH)) {
File f = fsys->open(MEM_FACTS_PATH, "r");
if (f) {
while (f.available() && g_mem_facts.size() < MEM_FACTS_MAX) {
String line = f.readStringUntil('\n');
line.trim();
if (line!= "") g_mem_facts.push_back(mem_truncate_utf8(line, MEM_FACT_LINE_MAX));
}
f.close();
}
}
memfs_close(use_sd);
Serial.printf("[MEM] loaded %u fact(s) from %s\n",
(unsigned)g_mem_facts.size(), use_sd? "SD": "SPIFFS");
}

static void memory_save() {
memory_load();
// 上限（40件・約4KB）を超えたら古い順に捨てる
while (g_mem_facts.size() > MEM_FACTS_MAX) g_mem_facts.pop_front();
size_t total = 0;
for (auto& s: g_mem_facts) total += (size_t)s.length() + 1;
while (total > MEM_FACTS_BYTES &&!g_mem_facts.empty()) {
total -= (size_t)g_mem_facts.front().length() + 1;
g_mem_facts.pop_front();
}
bool use_sd;
fs::FS *fsys = memfs_open(use_sd);
if (!fsys) {
Serial.println("[MEM] save: filesystem open failed");
return;
}
g_mem_on_sd = use_sd;
File f = fsys->open(MEM_FACTS_PATH, "w");
if (f) {
for (auto& s: g_mem_facts) {
f.print(s);
f.print('\n');
}
f.close();
} else {
Serial.println("[MEM] save: file open failed");
}
memfs_close(use_sd);
Serial.printf("[MEM] saved %u fact(s) to %s\n",
(unsigned)g_mem_facts.size(), use_sd? "SD": "SPIFFS");
}

// 1件追加する。既に同じ内容があれば false（保存済み）を返す
static bool memory_add_fact(const String& raw) {
memory_load();
String fact = raw;
fact.trim();
if (fact == "") return false;
fact = mem_truncate_utf8(fact, MEM_FACT_LINE_MAX);
for (auto& s: g_mem_facts) {
if (s == fact) return false;
}
g_mem_facts.push_back(fact);
memory_save();
return true;
}

static bool memory_delete_fact(int idx) {
memory_load();
if (idx < 0 || idx >= (int)g_mem_facts.size()) return false;
g_mem_facts.erase(g_mem_facts.begin() + idx);
memory_save();
return true;
}

// init_chat_doc 直後に呼び、保存済みの事実をシステムプロンプトへ差し込む
static void memory_inject_facts() {
memory_load();
if (g_mem_facts.empty()) return;
String block = "\n\n\n";
for (auto& s: g_mem_facts) {
block += "・";
block += s;
block += "\n";
}
block += "これらを踏まえて自然に会話してください。聞かれたら内容を答えても構いません。";
JsonArray messages = chat_doc["messages"];
if (messages.size() > 0) {
JsonObject first = messages[0];
if (first["role"] == "system") {
String cur = first["content"].as<String>();
first["content"] = cur + block;
Serial.printf("[MEM] injected %u fact(s) into system prompt\n", (unsigned)g_mem_facts.size());
return;
}
}
JsonObject sys = messages.createNestedObject();
if (sys.isNull()) {
Serial.println("[MEM] inject: chat_doc full, skipped");
return;
}
sys["role"] = "system";
sys["content"] = block;
Serial.printf("[MEM] injected %u fact(s) as system message\n", (unsigned)g_mem_facts.size());
}

// ---------- 長期記憶の自動抽出（会話後・アイドル時に1回だけ） ----------
static bool g_mem_ext_pending = false;
static String g_mem_ext_user = "";
static String g_mem_ext_asst = "";

static void memory_note_exchange(const String& user_text, const String& asst_text) {
if (user_text == "" || asst_text == "") return;
// エラー応答やビジー応答は抽出対象にしない
if (asst_text == "わかりません" || asst_text == "エラーです" ||
asst_text == "うまくできませんでした" || asst_text == "busy") return;
g_mem_ext_user = user_text;
g_mem_ext_asst = asst_text;
g_mem_ext_pending = true;
}

static void memory_extract_run() {
crumb("mem-extract");
memory_load();
// ニュース・天気と同じく、TLS の前に TTS 接続ペアの解放を依頼する
g_tts_release_req = true;
for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);

DynamicJsonDocument doc(8192);
doc["model"] = (CHAT_MODEL!= "")? CHAT_MODEL: String("gpt-4o-mini");
doc["max_tokens"] = 700;
doc["temperature"] = 0;
JsonArray msgs = doc.createNestedArray("messages");
JsonObject sys = msgs.createNestedObject();
sys["role"] = "system";
sys["content"] =
"あなたは音声アシスタントの長期記憶マネージャです。与えられた『現在の記憶』と『新しい会話』から、"
"次回以降の会話でも役立つユーザーの事実（好み、名前、家族、仕事、予定、体調、持ち物、決めたこと等）"
"だけを抽出・統合し、更新後の記憶一覧を返してください。ルール：1件は短い文（40字程度まで、日本語）。"
"一時的な話題、挨拶、質問そのもの、ニュースや天気の内容は含めない。重複は統合し、矛盾する場合は"
"新しい会話を優先して書き換える。最大40件。出力はJSON配列（文字列の配列）のみ。"
"前置き、説明、コードフェンスは禁止。";
JsonObject usr = msgs.createNestedObject();
usr["role"] = "user";
String u = "現在の記憶:\n";
if (g_mem_facts.empty()) {
u += "（なし）\n";
} else {
for (auto& s: g_mem_facts) {
u += "・";
u += s;
u += "\n";
}
}
u += "\n新しい会話:\nユーザー: " + g_mem_ext_user + "\nアシスタント: " + g_mem_ext_asst +
"\n\n更新後の記憶一覧をJSON配列で返してください。";
usr["content"] = u;

String json_string;
serializeJson(doc, json_string);
int http_code = 0;
String ret = https_post_json("https://api.openai.com/v1/chat/completions",
json_string.c_str(), root_ca_openai, &http_code);
if (ret == "") {
Serial.printf("[MEM] extract failed (http=%d)\n", http_code);
return;
}
DynamicJsonDocument rdoc(8192);
if (deserializeJson(rdoc, ret)) {
Serial.println("[MEM] extract: response parse failed");
return;
}
const char* content = rdoc["choices"][0]["message"]["content"];
if (!content) {
Serial.println("[MEM] extract: no content");
return;
}
String c = content;
// コードフェンスや前置きが混ざっても、配列部分だけを取り出す
int b = c.indexOf('[');
int e = c.lastIndexOf(']');
if (b < 0 || e <= b) {
Serial.println("[MEM] extract: no JSON array in response");
return;
}
c = c.substring(b, e + 1);
DynamicJsonDocument fdoc(6144);
if (deserializeJson(fdoc, c)) {
Serial.println("[MEM] extract: array parse failed");
return;
}
JsonArray arr = fdoc.as<JsonArray>();
if (arr.isNull()) {
Serial.println("[MEM] extract: not an array");
return;
}
// 検証（配列・文字列のみ・件数・総バイト数）を通った場合だけ書き戻す
std::deque<String> next;
size_t total = 0;
for (JsonVariant v: arr) {
if (!v.is<const char*>()) continue;
String s = v.as<String>();
s.trim();
if (s == "") continue;
s = mem_truncate_utf8(s, MEM_FACT_LINE_MAX);
total += (size_t)s.length() + 1;
next.push_back(s);
if (next.size() >= MEM_FACTS_MAX) break;
}
if (total > MEM_FACTS_BYTES) {
Serial.printf("[MEM] extract: result too large (%u bytes), rejected\n", (unsigned)total);
return;
}
g_mem_facts = next;
memory_save();
Serial.printf("[MEM] extract ok: %u fact(s)\n", (unsigned)g_mem_facts.size());
}

// loop から呼ぶ。発話・合成・BLE受信が終わっているアイドル時のみ実行する
void memory_extract_tick() {
if (!g_mem_ext_pending) return;
if (mode < 0 || audioRunning() || speech_text!= "" || speech_text_buffer!= "" ||
tts_processing() || ble_digest_active()) {
return; // 空きを待つ（キューしたまま）
}
g_mem_ext_pending = false; // 成否にかかわらず1会話につき1回だけ試す
if (WiFi.status()!= WL_CONNECTED) {
Serial.println("[MEM] extract skipped (Wi-Fi disconnected)");
return;
}
memory_extract_run();
}

// ---------- Function Calling: タイマー ----------
#define FC_TIMER_MAX 4
#define FC_TIMER_MAX_DEFER_MS (10UL * 60UL * 1000UL)

struct FcTimerSlot {
bool active = false;
bool absolute = false; // false: target は発火 millis / true: target は発火 epoch 秒
uint32_t target = 0;
uint32_t due_since_ms = 0;
String message;
};
static FcTimerSlot g_fc_timers[FC_TIMER_MAX];

static void fc_timer_speak(const String& msg) {
captionReset();
capBuildFromText(msg);
g_led_news_color = CRGB::White;
if (g_cap_count > 0) g_cap_armed = true;
speech_text = msg;
g_pending_news_speech = true; // ニュースと同じく中央タップで中断可能にする
}

static void fc_timer_poll(int slot) {
FcTimerSlot& t = g_fc_timers[slot];
if (!t.active) return;
bool due;
if (t.absolute) {
due = (time(nullptr) >= (time_t)t.target);
} else {
due = ((int32_t)(millis() - t.target) >= 0);
}
if (!due) return;
if (scheduler_ready()) {
t.active = false;
Serial.printf("[FC] timer fired slot=%d\n", slot);
fc_timer_speak(t.message);
} else {
// 発話中などは終わるまで待つ（上限を超えたら破棄してログに残す）
if (t.due_since_ms == 0) t.due_since_ms = millis();
if (millis() - t.due_since_ms > FC_TIMER_MAX_DEFER_MS) {
t.active = false;
Serial.printf("[FC] timer dropped (busy too long) slot=%d\n", slot);
}
}
}

// Scheduler 基盤への単発タスク登録（run_schedule からも発火判定される）
class ScheduleTimerOnce: public ScheduleBase {
int slot_;
public:
explicit ScheduleTimerOnce(int slot): slot_(slot) { sched_type = SCHED_REMINDER;}
void run(struct tm now_time) override {
(void)now_time;
fc_timer_poll(slot_);
if (!g_fc_timers[slot_].active) destroy = true;
}
};

static int fc_timer_register(bool absolute, uint32_t target, const String& message) {
for (int i = 0; i < FC_TIMER_MAX; i++) {
if (g_fc_timers[i].active) continue;
g_fc_timers[i].active = true;
g_fc_timers[i].absolute = absolute;
g_fc_timers[i].target = target;
g_fc_timers[i].due_since_ms = 0;
g_fc_timers[i].message = message;
add_schedule(new ScheduleTimerOnce(i));
return i;
}
return -1;
}

// loop から呼ぶ。run_schedule は NTP 未同期だと早期 return するため、
// 相対タイマーはこちらでも判定する（ポーリングは冪等で二重発火しない）
void fc_timers_tick() {
for (int i = 0; i < FC_TIMER_MAX; i++) fc_timer_poll(i);
}

// ---------- Function Calling: ツール実装 ----------
static String fc_tool_set_timer(JsonObject args) {
int minutes = args["minutes"] | -1;
String timestr = args["time"] | "";
String message = args["message"] | "";
message.trim();
if (message == "") message = "タイマーの時間です";
message = mem_truncate_utf8(message, 120);
bool has_min = minutes > 0;
bool has_time = timestr!= "";
if (has_min && has_time) {
return "エラー：minutes と time はどちらか一方だけを指定してください。";
}
if (!has_min &&!has_time) {
return "エラー：minutes（何分後）か time（HH:MM）のどちらかを指定してください。";
}
if (has_min) {
if (minutes > 24 * 60) return "エラー：タイマーは24時間以内で指定してください。";
int slot = fc_timer_register(false, millis() + (uint32_t)minutes * 60000UL, message);
if (slot < 0) return "エラー：タイマーは同時に4件までです。";
Serial.printf("[FC] timer set slot=%d in %d min\n", slot, minutes);
return String(minutes) + "分後にタイマーをセットしました。時間になったら「" + message + "」と読み上げます。";
}
// 絶対指定 HH:MM
if (timestr.length()!= 5 || timestr[2]!= ':' ||
!isDigit(timestr[0]) ||!isDigit(timestr[1]) ||!isDigit(timestr[3]) ||!isDigit(timestr[4])) {
return "エラー：時刻は HH:MM 形式（例 07:30）で指定してください。";
}
int hh = timestr.substring(0, 2).toInt();
int mm = timestr.substring(3, 5).toInt();
if (hh > 23 || mm > 59) return "エラー：時刻が範囲外です。";
struct tm now_tm;
if (!getLocalTime(&now_tm)) {
return "エラー：現在時刻が未同期のため、時刻指定のタイマーは使えません。何分後かの相対指定を使ってください。";
}
struct tm tgt = now_tm;
tgt.tm_hour = hh;
tgt.tm_min = mm;
tgt.tm_sec = 0;
time_t target = mktime(&tgt);
if (target <= time(nullptr)) target += 24 * 60 * 60; // 過ぎていれば翌日
int slot = fc_timer_register(true, (uint32_t)target, message);
if (slot < 0) return "エラー：タイマーは同時に4件までです。";
Serial.printf("[FC] timer set slot=%d at %02d:%02d\n", slot, hh, mm);
char buf[192];
snprintf(buf, sizeof(buf), "%d時%d分にタイマーをセットしました。時間になったら「%s」と読み上げます。",
hh, mm, message.c_str());
return String(buf);
}

static String fc_tool_datetime() {
struct tm t;
if (!getLocalTime(&t)) {
return "現在時刻は未同期です（Wi-Fi接続直後など）。少し経ってから聞いてください。";
}
static const char* wday[] = {"日", "月", "火", "水", "木", "金", "土"};
char buf[128];
snprintf(buf, sizeof(buf), "%d年%d月%d日（%s曜日）%d時%d分%d秒です。",
t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, wday[t.tm_wday % 7],
t.tm_hour, t.tm_min, t.tm_sec);
return String(buf);
}

// ニュースをモデルへ返す形（見出し＋説明。鮮度フィルタ・既読スキップ込み）
static String buildNewsTextForTool() {
g_tts_release_req = true;
for (int i = 0; i < 10 && g_tts_release_req; i++) delay(10);
g_news_nothing_new = false;
g_news_feed_ok = false;
g_news_skip_old = 0;
g_news_skip_seen = 0;
String dispItems[5];
String dispTitles[5];
String dispDescs[5];
int dispCount = 0;
String text = fetchNewsText(0, "", dispItems, dispTitles, dispDescs, 5, dispCount);
if (text == "") {
String heads[3];
int n = 0;
if (fetchGoogleHeadlines("https://news.google.com/rss?hl=ja&gl=JP&ceid=JP:ja",
3, heads, n, g_google_top_http_code)) {
for (int i = 0; i < n; i++) text += heads[i] + "。";
}
}
if (text == "") {
if (g_news_nothing_new) return "新しいニュースはありません（古い記事と読み上げ済みの記事を除いた結果です）。";
return "エラー：ニュースを取得できませんでした。";
}
newsSeenFlush();  // 第9.26弾: 既読ストアの書き戻しは取得完了後に1回だけ
return text;
}

static void fc_execute_tool(const char* name, const String& args_json,
String& result, int& args_error_count, bool& fatal) {
fatal = false;
DynamicJsonDocument adoc(1024);
DeserializationError aerr = deserializeJson(adoc, args_json);
if (aerr) {
args_error_count++;
Serial.printf("[FC] tool=%s args parse failed (%d)\n", name, args_error_count);
if (args_error_count >= 2) {
fatal = true;
result = "引数エラー";
return;
}
result = "エラー：引数のJSONが不正です。形式を直してもう一度呼び出してください。";
return;
}
JsonObject args = adoc.as<JsonObject>();
if (!strcmp(name, "set_timer")) {
result = fc_tool_set_timer(args);
} else if (!strcmp(name, "get_weather")) {
int off = args["day_offset"] | 1;
String text;
if (off == 7) {
text = buildWeatherText(-1);
} else {
if (off < 0) off = 0;
if (off > 2) off = 2;
text = buildWeatherText(off);
}
result = (text!= "")? text: String("エラー：天気予報を取得できませんでした。");
} else if (!strcmp(name, "get_train_delays")) {
String text = buildTrainText();
result = (text!= "")? text: String("エラー：運行情報を取得できませんでした。");
} else if (!strcmp(name, "get_news")) {
result = buildNewsTextForTool();
} else if (!strcmp(name, "get_current_datetime")) {
result = fc_tool_datetime();
} else if (!strcmp(name, "remember_fact")) {
String fact = args["fact"] | "";
fact.trim();
if (fact == "") {
result = "エラー：覚える内容が空です。";
} else if (memory_add_fact(fact)) {
result = "長期記憶に保存しました：「" + mem_truncate_utf8(fact, MEM_FACT_LINE_MAX) + "」";
} else {
result = "すでに覚えています：「" + mem_truncate_utf8(fact, MEM_FACT_LINE_MAX) + "」";
}
} else {
result = "エラー：未知の関数です。";
}
Serial.printf("[FC] tool=%s done result_len=%u\n", name, (unsigned)result.length());
}

// ---------- Function Calling: ツール定義と会話ループ ----------
static const char FC_TOOLS_JSON[] PROGMEM = R"FCJSON(
{"tools":[
{"type":"function","function":{"name":"set_timer","description":"指定した時間後に、メッセージを声で知らせるタイマーをセットします。お知らせ・目覚まし・リマインダーに使います。","parameters":{"type":"object","properties":{"minutes":{"type":"integer","description":"何分後に知らせるか（相対指定）。timeとは同時指定不可"},"time":{"type":"string","description":"知らせる時刻 HH:MM 形式（絶対指定）。minutesとは同時指定不可"},"message":{"type":"string","description":"時間になったら読み上げる文（例：お薬の時間です）"}},"required":["message"]}}},
{"type":"function","function":{"name":"get_weather","description":"東京の天気予報を取得します。","parameters":{"type":"object","properties":{"day_offset":{"type":"integer","description":"0=今日、1=明日、2=明後日、7=週間予報の要約"}}}}},
{"type":"function","function":{"name":"get_train_delays","description":"田園都市線・大井町線・南武線の運行情報（遅延・運転見合わせ等）を取得します。","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"get_news","description":"最新のニュース見出しを取得します（古い記事と読み上げ済みの記事は除きます）。","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"get_current_datetime","description":"現在の日付・曜日・時刻を取得します。","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"remember_fact","description":"ユーザーについて次回以降も覚えておくべき事実（好み・名前・予定・決めたこと等）を長期記憶に保存します。ユーザーに『覚えて』と言われたときに使います。","parameters":{"type":"object","properties":{"fact":{"type":"string","description":"覚える事実を短い文で"}},"required":["fact"]}}}
]}
)FCJSON";

static DynamicJsonDocument g_fc_tools_doc(4096);
static bool g_fc_tools_ready = false;
static void fc_ensure_tools_doc() {
if (g_fc_tools_ready) return;
g_fc_tools_ready = true;
DeserializationError e = deserializeJson(g_fc_tools_doc, FC_TOOLS_JSON);
if (e) Serial.printf("[FC] tools json parse failed: %s\n", e.c_str());
}

// tools 付きで ChatGPT を呼び、tool_calls を実行しながら最終応答文を返す。
// 使い方は chatGpt() と同じ（戻り値は読み上げ文）。
String chatGptWithTools(const String& json_string) {
crumb("chat-tools");
DynamicJsonDocument req(16384);
if (deserializeJson(req, json_string)) {
// リクエストを組み直せない場合は従来経路にフォールバック
Serial.println("[FC] request parse failed; fallback to plain chat");
return chatGpt(json_string);
}
fc_ensure_tools_doc();
req["tools"] = g_fc_tools_doc["tools"];
req["tool_choice"] = "auto";

g_led_thinking = true; // 側面LED：応答待ち（紫パルス）
avatar.setExpression(Expression::Doubt);
avatar.setSpeechText("考え中…");

String response = "";
bool tools_enabled = true;
bool retried_without_tools = false;
int args_error_count = 0;
bool done = false;
for (int round = 0; round < 4 &&!done; round++) {
String out;
serializeJson(req, out);
int http_code = 0;
uint32_t t0 = millis();
String ret = https_post_json("https://api.openai.com/v1/chat/completions",
out.c_str(), root_ca_openai, &http_code);
Serial.printf("[FC] round=%d http=%d in %lu ms\n", round, http_code,
(unsigned long)(millis() - t0));
if (ret == "") {
if (tools_enabled && http_code >= 400 &&!retried_without_tools) {
// モデルが tools 非対応（400等）: tools を外して1回だけ再試行する
req.remove("tools");
req.remove("tool_choice");
tools_enabled = false;
retried_without_tools = true;
Serial.println("[FC] retry without tools");
continue;
}
response = (http_code < 0)? "わかりません": "エラーです";
done = true;
break;
}
DynamicJsonDocument resp(8192);
if (deserializeJson(resp, ret)) {
Serial.println("[FC] response parse failed");
response = "エラーです";
done = true;
break;
}
JsonObject msg = resp["choices"][0]["message"];
if (msg.isNull()) {
response = "エラーです";
done = true;
break;
}
JsonArray calls = msg["tool_calls"];
if (!calls.isNull() && calls.size() > 0) {
if (round >= 3) {
// ツール往復の上限（3回）に達した
Serial.println("[FC] tool loop limit reached");
response = "うまくできませんでした";
done = true;
break;
}
JsonArray messages = req["messages"];
messages.add(msg); // tool_calls 付きの assistant メッセージを履歴へ（v6 は深コピー）
for (JsonObject call: calls) {
const char* cid = call["id"];
const char* fname = call["function"]["name"];
String fargs = call["function"]["arguments"].as<String>();
String result;
bool fatal = false;
fc_execute_tool(fname? fname: "", fargs, result, args_error_count, fatal);
JsonObject toolmsg = messages.createNestedObject();
toolmsg["role"] = "tool";
toolmsg["tool_call_id"] = cid? cid: "";
toolmsg["content"] = result;
if (fatal) {
response = "うまくできませんでした";
done = true;
break;
}
}
continue;
}
const char* content = msg["content"];
response = content? String(content): "";
std::replace(response.begin(), response.end(), '\n', ' ');
if (response == "") response = "わかりません";
done = true;
}

avatar.setExpression(Expression::Neutral);
avatar.setSpeechText("");
g_led_thinking = false;
return response;
}

// ---------- 長期記憶の管理ページ（本体 Web） ----------
static String mem_html_escape(const String& s) {
String o = s;
o.replace("&", "&amp;");
o.replace("<", "&lt;");
o.replace(">", "&gt;");
o.replace("\"", "&quot;");
return o;
}

static void handle_memory_page() {
memory_load();
String html = String(HEAD) + String("<body><h1>長期記憶メモリ</h1>");
html += "<p>保存先: " + String(g_mem_on_sd? "SDカード": "SPIFFS") + " ／ " +
String((unsigned)g_mem_facts.size()) + "件（上限40件・約4KB）</p>";
if (g_mem_facts.empty()) {
html += "<p>（まだ何も覚えていません）</p>";
}
for (size_t i = 0; i < g_mem_facts.size(); i++) {
html += "<div>" + mem_html_escape(g_mem_facts[i]) +
" <form method=\"POST\" action=\"/memory_del\" style=\"display:inline\">"
"<input type=\"hidden\" name=\"idx\" value=\"" + String((unsigned)i) +
"\"><button type=\"submit\">削除</button></form></div>";
}
html += "<h2>追加</h2><form method=\"POST\" action=\"/memory_add\">"
"<input type=\"text\" name=\"fact\" size=\"48\">"
"<button type=\"submit\">追加</button></form>";
html += "<p><a href=\"/\">本体ホーム</a></p></body></html>";
server.send(200, "text/html", html);
}

static void handle_memory_add() {
if (server.method()!= HTTP_POST) {
server.send(405, "text/plain", "POST only");
return;
}
String fact = server.arg("fact");
fact.trim();
if (fact!= "") memory_add_fact(fact);
server.sendHeader("Location", "/memory");
server.send(303, "text/plain", "");
}

static void handle_memory_del() {
if (server.method()!= HTTP_POST) {
server.send(405, "text/plain", "POST only");
return;
}
memory_delete_fact(server.arg("idx").toInt());
server.sendHeader("Location", "/memory");
server.send(303, "text/plain", "");
}

// テスト用: 記憶一覧を JSON で返す
static void handle_memory_json() {
memory_load();
DynamicJsonDocument doc(6144);
doc["storage"] = g_mem_on_sd? "SD": "SPIFFS";
JsonArray arr = doc.createNestedArray("facts");
for (auto& s: g_mem_facts) arr.add(s);
String out;
serializeJson(doc, out);
server.sendHeader("Access-Control-Allow-Origin", "*");
server.send(200, "application/json", out);
}


// ==== BLE テキスト転送「転送ニュース」（第9.23弾） ====
// Muse／ChatGPT／Codex が要約したテキスト（ITニュース、GitHub／
// Hugging Face ランキング等）を、書き込みページから Web Bluetooth で
// 転送して保存し、ニュースの1カテゴリとして読み上げる。
//
// 設計方針:
// - v9.24: POST /ble で次回だけBLE専用起動を予約し、再起動で通常動作から分離する
//   BLE専用起動ではWi-Fi・音声・Avatar・スケジューラ自体を初期化しない。
//   待ち受け最大3分、完了後は通常モードへ再起動する。
// - 音声処理中は切替予約を受け付けない。
// - GATT（128bit カスタムUUID）:
//     Service : e8f5a001-b7d3-4c1e-9a2b-3d4c5e6f7081
//     CTRL    : e8f5a002-… (Write) "BEGIN:<総バイト数>" / "END" / "ABORT"
//     DATA    : e8f5a003-… (Write) 本文チャンク（応答あり、最大180バイト）
//     STATUS  : e8f5a004-… (Read/Notify) "WAIT" / "RECV n/m" /
//               "DONE dg_NNNN.txt" / "ERR …"
//   本文は「1行目=タイトル、以降=本文」のプレーンテキスト(UTF-8)そのもの。
// - 受信データは PSRAM 上で組み立て、END 受信後に loop 文脈で総バイト数を
//   検証してから正式保存する（FS 操作を BLE タスクで行わない）。
//   途中切断時は受信済みを破棄して受付待ちに戻る。
// - 保存先は microSD の /digest/ を優先し、なければ SPIFFS（第9.19弾の
//   キャプチャと同じ優先順位）。保持は最新10件、1件最大60KB。
// - 既読管理は NVS "setting"/"dgread"（最後に読み上げた連番）。書き込み時の
//   NVS 初期化でリセットされる（ニュース既読と同じ扱い）。
#include <NimBLEDevice.h>

#define BLE_DG_SERVICE_UUID "e8f5a001-b7d3-4c1e-9a2b-3d4c5e6f7081"
#define BLE_DG_CTRL_UUID    "e8f5a002-b7d3-4c1e-9a2b-3d4c5e6f7081"
#define BLE_DG_DATA_UUID    "e8f5a003-b7d3-4c1e-9a2b-3d4c5e6f7081"
#define BLE_DG_STATUS_UUID  "e8f5a004-b7d3-4c1e-9a2b-3d4c5e6f7081"
#define BLE_DG_DEVICE_NAME  "StackChan-Digest"
#define BLE_DG_WINDOW_MS    180000UL  // 受信待ち受けの上限（3分）
#define BLE_DG_HOLD_MS      15000UL   // 完了/エラー後に STATUS を読めるよう保持する時間
#define DIGEST_MAX_BYTES    (60 * 1024)  // 1件の上限
#define DIGEST_KEEP_FILES   10           // 保持する最新件数
#define DIGEST_DIR          "/digest"
#define DIGEST_CAT_INDEX    5            // NEWS_CATS の「転送ニュース」

// ---- 転送テキストの保存（microSD 優先、なければ SPIFFS） ----
static fs::FS *digest_open(bool &use_sd) {
  use_sd = false;
  if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    use_sd = true;
    if (!SD.exists(DIGEST_DIR)) SD.mkdir(DIGEST_DIR);
    return &SD;
  }
  if (!SPIFFS.begin(false)) return nullptr;
  return &SPIFFS;
}
static void digest_close(bool use_sd) { if (use_sd) SD.end(); }

// ファイル名が dg_NNNN.txt 形式なら連番を返す（それ以外は -1）
static int digest_parse_index(const String &name) {
  int slash = name.lastIndexOf('/');
  String base = (slash >= 0) ? name.substring(slash + 1) : name;
  if (!base.startsWith("dg_") || !base.endsWith(".txt")) return -1;
  String num = base.substring(3, base.length() - 4);
  if (num.length() == 0) return -1;
  for (size_t i = 0; i < num.length(); i++) if (!isDigit(num[i])) return -1;
  return num.toInt();
}

struct DigestEntry { int index; String path; };

// 保存先の転送テキストを走査する。戻り値は out に格納した件数、
// max_index は最大の連番（ファイルが無ければ 0）
static int digest_scan(fs::FS &fsys, bool use_sd, DigestEntry *out, int max_out, int &max_index) {
  max_index = 0;
  int count = 0;
  File dir = use_sd ? fsys.open(DIGEST_DIR) : fsys.open("/");
  if (!dir) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String fname = f.name();
    int idx = digest_parse_index(fname);
    if (idx >= 0) {
      if (idx > max_index) max_index = idx;
      if (count < max_out) {
        out[count].index = idx;
        out[count].path = use_sd ? (fname.startsWith("/") ? fname : String(DIGEST_DIR) + "/" + fname)
                                 : fname;
        count++;
      }
    }
    f.close();
  }
  dir.close();
  return count;
}

// 受信したテキストを正式保存する。保持上限を超える分は古い順に削除する
static bool digest_store(const uint8_t *data, size_t len, String &out_name) {
  bool use_sd;
  fs::FS *fsys = digest_open(use_sd);
  if (!fsys) return false;
  DigestEntry entries[24];
  int max_index = 0;
  int count = digest_scan(*fsys, use_sd, entries, 24, max_index);
  while (count >= DIGEST_KEEP_FILES) {
    int oldest = 0;
    for (int i = 1; i < count; i++) if (entries[i].index < entries[oldest].index) oldest = i;
    Serial.printf("[Digest] remove old: %s\n", entries[oldest].path.c_str());
    if (!fsys->remove(entries[oldest].path)) break;  // 削除できなければ諦める（無限ループ防止）
    entries[oldest] = entries[count - 1];
    count--;
  }
  char name[20];
  snprintf(name, sizeof(name), "dg_%04d.txt", max_index + 1);
  String path = String(DIGEST_DIR) + "/" + name;
  File out = fsys->open(path, FILE_WRITE);
  if (!out) { digest_close(use_sd); return false; }
  size_t written = out.write(data, len);
  out.close();
  digest_close(use_sd);
  if (written != len) return false;
  out_name = name;
  Serial.printf("[Digest] stored %s (%u bytes, %s)\n", path.c_str(), (unsigned)len,
                use_sd ? "SD" : "SPIFFS");
  return true;
}

// 最新の転送テキストを読み出す。無ければ false
static bool digest_load_latest(String &out_text, int &out_index) {
  bool use_sd;
  fs::FS *fsys = digest_open(use_sd);
  if (!fsys) return false;
  DigestEntry entries[24];
  int max_index = 0;
  int count = digest_scan(*fsys, use_sd, entries, 24, max_index);
  out_index = 0;
  out_text = "";
  if (count == 0 || max_index == 0) { digest_close(use_sd); return false; }
  String path;
  for (int i = 0; i < count; i++) if (entries[i].index == max_index) path = entries[i].path;
  if (path == "") { digest_close(use_sd); return false; }
  File f = fsys->open(path, FILE_READ);
  if (!f) { digest_close(use_sd); return false; }
  size_t remain = f.size();
  if (remain > DIGEST_MAX_BYTES) remain = DIGEST_MAX_BYTES;
  out_text.reserve(remain + 1);
  uint8_t buf[256];
  while (remain > 0) {
    int n = f.read(buf, remain < sizeof(buf) ? remain : sizeof(buf));
    if (n <= 0) break;
    out_text.concat((const char *)buf, (unsigned int)n);
    remain -= (size_t)n;
  }
  f.close();
  digest_close(use_sd);
  out_index = max_index;
  return out_text.length() > 0;
}

// 既読管理（NVS "setting"/"dgread" に最後に読み上げた連番を保持）
static int digest_last_read_index() {
  uint32_t h;
  int v = 0;
  if (ESP_OK == nvs_open("setting", NVS_READONLY, &h)) {
    int32_t t = 0;
    if (ESP_OK == nvs_get_i32(h, "dgread", &t)) v = (int)t;
    nvs_close(h);
  }
  return v;
}
static void digest_mark_read(int idx) {
  uint32_t h;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &h)) {
    nvs_set_i32(h, "dgread", idx);
    nvs_commit(h);
    nvs_close(h);
  }
}

// ファイル内容（1行目=タイトル、以降=本文）から読み上げ文を作る。
// 本文の改行は文の区切りとして扱い、行末に句読点が無ければ「。」を補う
static bool digest_build_speech(const String &content, String &out_title, String &out_speech) {
  int nl = content.indexOf('\n');
  String title = (nl >= 0) ? content.substring(0, nl) : content;
  title.trim();
  String body = (nl >= 0) ? content.substring(nl + 1) : "";
  out_title = title;
  String b;
  b.reserve(body.length() + 16);
  for (size_t i = 0; i < body.length(); i++) {
    char c = body[i];
    if (c == '\r') continue;
    if (c == '\n') {
      if (b.length() > 0 && !b.endsWith("。") && !b.endsWith("！") && !b.endsWith("？") &&
          !b.endsWith("、") && !b.endsWith(".")) {
        b += "。";
      }
      continue;
    }
    b += c;
  }
  out_speech = "";
  if (title != "") out_speech += title + "。";
  out_speech += b;
  if (out_speech.length() > 0 && !out_speech.endsWith("。") && !out_speech.endsWith("！") &&
      !out_speech.endsWith("？")) {
    out_speech += "。";
  }
  return out_speech.length() > 0;
}

// カテゴリ「転送ニュース」の手動読み上げ（最新の保存テキストを読む）
static void exec_digest_category() {
  if (tts_processing()) { avatar.setSpeechText("音声処理の終了を待ってください"); return; }
  captionReset();  // 前回の表示キューが残っていたら破棄する
  String content;
  int idx = 0;
  String title, speech;
  if (!digest_load_latest(content, idx) || !digest_build_speech(content, title, speech)) {
    speech_text = "転送ニュースはまだありません";
    avatar.setSpeechText("転送ニュースはまだありません");
    g_info_msg_until = millis() + 4000;
    g_pending_news_speech = true;
    return;
  }
  capAddLabel("転送ニュース");
  capBuildFromText(speech);
  g_led_news_color = NEWS_CAT_COLORS[DIGEST_CAT_INDEX];  // 転送ニュースのLED色は紫
  if (g_cap_count > 0) g_cap_armed = true;
  speech_text = "転送ニュースです。" + speech;
  g_pending_news_speech = true;  // ニュースと同じく中央タップで中断可能にする
  digest_mark_read(idx);
  Serial.printf("[Digest] speak latest #%d (%u chars)\n", idx, (unsigned)speech.length());
}

// 自動読み上げ（定期通知・モーニング）用：未読の最新転送テキストがあれば
// speech_text の末尾に連結し、既読にする。無ければ何もしない
static void append_unread_digest() {
  String content;
  int idx = 0;
  if (!digest_load_latest(content, idx)) return;
  if (idx <= digest_last_read_index()) return;  // 既読なら繰り返さない
  String title, speech;
  if (!digest_build_speech(content, title, speech)) return;
  capAddLabel("転送ニュース");
  capBuildFromText(speech);
  bool was_empty = (speech_text == "");
  speech_text += was_empty ? String("転送ニュースです。") : String("続いて、転送ニュースです。");
  speech_text += speech;
  if (was_empty) g_led_news_color = NEWS_CAT_COLORS[DIGEST_CAT_INDEX];
  if (g_cap_count > 0) g_cap_armed = true;
  g_pending_news_speech = true;
  digest_mark_read(idx);
  Serial.printf("[Digest] appended unread #%d to automatic speech\n", idx);
}

// ---- BLE 受信モード ----
enum BleDgState : uint8_t { DG_OFF = 0, DG_WAIT, DG_RECV, DG_COMMIT, DG_DONE, DG_ERROR };
static std::atomic<uint8_t> g_ble_dg_state{DG_OFF};
static std::atomic<uint32_t> g_ble_dg_expect{0};    // BEGIN で申告された総バイト数
static std::atomic<uint32_t> g_ble_dg_received{0};  // 受信済みバイト数
static uint8_t *g_ble_dg_buf = nullptr;          // PSRAM 上の組み立てバッファ（初回に確保して使い回す）
static SemaphoreHandle_t g_ble_dg_mutex = nullptr;
static uint32_t g_ble_dg_deadline = 0;           // 受信窓の終了時刻（WAIT/RECV）
static uint32_t g_ble_dg_off_at = 0;             // DONE/ERROR 後に自動OFFする時刻
static NimBLECharacteristic *g_ble_dg_status_chr = nullptr;

static bool ble_digest_active() { return g_ble_dg_state != DG_OFF; }

static void ble_dg_set_status(const String &s) {
  if (g_ble_dg_status_chr) {
    g_ble_dg_status_chr->setValue(std::string(s.c_str()));
    g_ble_dg_status_chr->notify();
  }
}

class BleDgCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr) override {
    std::string v = chr->getValue();
    String cmd(v.c_str());
    if (cmd.startsWith("BEGIN:")) {
      uint32_t total = (uint32_t)cmd.substring(6).toInt();
      bool accepted = false;
      xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
      if (g_ble_dg_state == DG_WAIT && total > 0 && total <= DIGEST_MAX_BYTES) {
        g_ble_dg_expect = total;
        g_ble_dg_received = 0;
        g_ble_dg_state = DG_RECV;
        accepted = true;
      }
      xSemaphoreGive(g_ble_dg_mutex);
      if (accepted) {
        Serial.printf("[BLE] BEGIN total=%u\n", (unsigned)total);
        ble_dg_set_status("RECV 0/" + String(total));
      } else {
        Serial.printf("[BLE] BEGIN rejected (state=%d total=%u)\n", (int)g_ble_dg_state, (unsigned)total);
        ble_dg_set_status("ERR begin");
      }
    } else if (cmd == "END") {
      xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
      if (g_ble_dg_state == DG_RECV) {
        g_ble_dg_state = DG_COMMIT;  // 検証と保存は loop 側（ble_digest_tick）で行う
        Serial.printf("[BLE] END received=%u expect=%u\n",
                      (unsigned)g_ble_dg_received, (unsigned)g_ble_dg_expect);
      }
      xSemaphoreGive(g_ble_dg_mutex);
    } else if (cmd == "ABORT") {
      bool was = false;
      xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
      if (g_ble_dg_state == DG_RECV) {
        g_ble_dg_received = 0;
        g_ble_dg_expect = 0;
        g_ble_dg_state = DG_WAIT;
        was = true;
      }
      xSemaphoreGive(g_ble_dg_mutex);
      if (was) {
        Serial.println("[BLE] ABORT (partial data discarded)");
        ble_dg_set_status("WAIT");
      }
    }
  }
};

class BleDgDataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr) override {
    if (!g_ble_dg_buf) return;
    std::string v = chr->getValue();
    size_t len = v.size();
    if (len == 0) return;
    uint32_t got = 0, exp = 0;
    bool progress = false;
    xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
    if (g_ble_dg_state == DG_RECV) {
      uint32_t room = (g_ble_dg_expect > g_ble_dg_received) ? (g_ble_dg_expect - g_ble_dg_received) : 0;
      size_t take = (len < room) ? len : room;
      if (take) memcpy(g_ble_dg_buf + g_ble_dg_received, v.data(), take);
      g_ble_dg_received += take;
      got = g_ble_dg_received;
      exp = g_ble_dg_expect;
      progress = (got == exp) || ((got % 1024) < len);  // 約1KBごとと完了時に通知
    }
    xSemaphoreGive(g_ble_dg_mutex);
    if (progress) ble_dg_set_status("RECV " + String(got) + "/" + String(exp));
    // BLE-only boot has no display auto-off or normal activity tasks.
  }
};

class BleDgServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    Serial.println("[BLE] disconnected");
    xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
    if (g_ble_dg_state == DG_RECV) {
      // Do not discard a buffer already frozen for a commit by END.
      g_ble_dg_received = 0;
      g_ble_dg_expect = 0;
      g_ble_dg_state = DG_WAIT;
      Serial.println("[BLE] partial data discarded (disconnect)");
    }
    xSemaphoreGive(g_ble_dg_mutex);
    // NimBLE は切断後に広告を自動再開しないため、受信窓の間は再開する
    if (g_ble_dg_state == DG_WAIT) NimBLEDevice::startAdvertising();
  }
};

static BleDgCtrlCallbacks g_ble_dg_ctrl_cb;
static BleDgDataCallbacks g_ble_dg_data_cb;
static BleDgServerCallbacks g_ble_dg_server_cb;

// BLE initialization may wait indefinitely inside NimBLE 1.4.3. Keep the UI and
// a bounded recovery timer on the main task; recover by reboot, not task deletion.
static std::atomic<int> g_ble_boot_result{0}; // 0 starting, 1 ready, -1 failed
static uint32_t g_ble_boot_deadline = 0;
static constexpr uint32_t BLE_BOOT_TIMEOUT_MS = 15000;

static void ble_mode_screen(const char *status) {
  M5.Display.fillScreen(TFT_BLACK);
  drawFirmwareVersionBadge(M5.Display);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 36);
  M5.Display.println("BLE transfer only");
  M5.Display.println("Wi-Fi / voice: OFF");
  M5.Display.println(status);
  M5.Display.println("C: reboot to normal mode");
}

static void ble_digest_stop(const char *reason) {
  Serial.printf("[MODE] BLE -> normal reboot (%s)\n", reason);
  // The one-shot boot token was cleared before BLE initialization. Reboot also
  // handles an unresponsive host safely, without freeing live callback buffers.
  ESP.restart();
}

static void ble_boot_task(void *) {
  // Fail closed before NimBLE's NVS recovery path; never request an NVS erase.
  if (nvs_flash_init() != ESP_OK) {
    g_ble_boot_result.store(-1);
    vTaskDelete(nullptr);
    return;
  }
  NimBLEDevice::init(BLE_DG_DEVICE_NAME);
  NimBLEDevice::setMTU(185);
  NimBLEServer *bleServer = NimBLEDevice::createServer();
  if (!bleServer) { g_ble_boot_result.store(-1); vTaskDelete(nullptr); return; }
  bleServer->setCallbacks(&g_ble_dg_server_cb);
  NimBLEService *svc = bleServer->createService(BLE_DG_SERVICE_UUID);
  if (!svc) { g_ble_boot_result.store(-1); vTaskDelete(nullptr); return; }
  NimBLECharacteristic *ctrl = svc->createCharacteristic(BLE_DG_CTRL_UUID, NIMBLE_PROPERTY::WRITE);
  NimBLECharacteristic *data = svc->createCharacteristic(BLE_DG_DATA_UUID, NIMBLE_PROPERTY::WRITE);
  g_ble_dg_status_chr = svc->createCharacteristic(
      BLE_DG_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  if (!ctrl || !data || !g_ble_dg_status_chr) {
    g_ble_boot_result.store(-1); vTaskDelete(nullptr); return;
  }
  ctrl->setCallbacks(&g_ble_dg_ctrl_cb);
  data->setCallbacks(&g_ble_dg_data_cb);
  g_ble_dg_status_chr->setValue(std::string("WAIT"));
  if (!svc->start()) { g_ble_boot_result.store(-1); vTaskDelete(nullptr); return; }
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv) { g_ble_boot_result.store(-1); vTaskDelete(nullptr); return; }
  adv->addServiceUUID(BLE_DG_SERVICE_UUID);
  adv->setScanResponse(true);
  g_ble_dg_deadline = millis() + BLE_DG_WINDOW_MS;
  g_ble_dg_state = DG_WAIT;
  if (!adv->start()) { g_ble_boot_result.store(-1); vTaskDelete(nullptr); return; }
  Serial.printf("[BLE] advertising as %s (BLE-only boot) heap=%u largest=%u\n",
                BLE_DG_DEVICE_NAME, (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  g_ble_boot_result.store(1);
  vTaskDelete(nullptr);
}

static void ble_mode_setup() {
  ble_mode_screen("Starting (max 15s)...");
  g_ble_boot_deadline = millis() + BLE_BOOT_TIMEOUT_MS;
  g_ble_dg_mutex = xSemaphoreCreateMutex();
  g_ble_dg_buf = (uint8_t *)ps_malloc(DIGEST_MAX_BYTES);
  if (!g_ble_dg_mutex || !g_ble_dg_buf ||
      xTaskCreatePinnedToCore(ble_boot_task, "bleBoot", 6144, nullptr, 1, nullptr, 0) != pdPASS)
    g_ble_boot_result.store(-1);
}

static void ble_digest_tick() {
  uint8_t st = g_ble_dg_state;
  uint32_t now = millis();
  if (st == DG_COMMIT) {
    // END freezes the buffer against writes/ABORT/disconnect while FS saves it.
    xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
    uint32_t got = g_ble_dg_received, exp = g_ble_dg_expect;
    xSemaphoreGive(g_ble_dg_mutex);
    String name;
    bool ok = (exp > 0 && got == exp && g_ble_dg_buf != nullptr);
    if (ok) ok = digest_store(g_ble_dg_buf, got, name);
    g_ble_dg_state = ok ? DG_DONE : DG_ERROR;
    g_ble_dg_off_at = millis() + BLE_DG_HOLD_MS;
    ble_dg_set_status(ok ? "DONE " + name : String("ERR store"));
    ble_mode_screen(ok ? "Saved. Reboot in 15s." : "Save failed. Reboot in 15s.");
    Serial.printf("[BLE] commit ok=%d got=%u expect=%u\n", ok, (unsigned)got, (unsigned)exp);
    return;
  }
  if ((st == DG_WAIT || st == DG_RECV) && modeDeadlineReached(now, g_ble_dg_deadline)) {
    // Callbacks must see the timeout state under the same lock as END/DATA.
    xSemaphoreTake(g_ble_dg_mutex, portMAX_DELAY);
    if (g_ble_dg_state == DG_WAIT || g_ble_dg_state == DG_RECV) {
      g_ble_dg_state = DG_ERROR;
      g_ble_dg_off_at = now + 3000;
    }
    xSemaphoreGive(g_ble_dg_mutex);
    if (g_ble_dg_state == DG_ERROR) {
      ble_dg_set_status("ERR timeout");
      ble_mode_screen("Timeout. Reboot in 3s.");
    }
    return;
  }
  if ((st == DG_DONE || st == DG_ERROR) && modeDeadlineReached(now, g_ble_dg_off_at))
    ble_digest_stop(st == DG_DONE ? "done" : "error/timeout");
}

static void ble_mode_loop() {
  M5.update();
  // Never interrupt an FS commit. loop runs button handling before a save starts.
  if (M5.BtnC.wasPressed()) { ble_digest_stop("button C"); return; }
  int result = g_ble_boot_result.load();
  if (result < 0 || (result == 0 && modeDeadlineReached(millis(), g_ble_boot_deadline))) {
    ble_mode_screen("BLE startup failed. Returning...");
    delay(500);
    ble_digest_stop("startup failed/timeout");
    return;
  }
  if (result == 1) {
    static bool shown = false;
    if (!shown) { ble_mode_screen("WAIT (3 minutes)"); shown = true; }
    ble_digest_tick();
  }
  delay(10);
}

// Read-only GET: old links must not initialize Bluetooth in a normal boot.
static void handle_mode() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", OPERATING_MODE_HTML);
}

// Explicit same-origin POST only. Reply first, then restart at the loop boundary.
static void handle_ble() {
  if (server.header("X-Stackchan-Mode") != "ble") {
    server.send(403, "application/json", "{\"error\":\"mode header required\"}");
    return;
  }
  if (g_mode_switch_pending || !canEnterBleMode(audioRunning(), tts_processing(),
                                               speech_text != "" || speech_text_buffer != "")) {
    server.send(409, "application/json", "{\"error\":\"busy; wait for audio and speech to finish\"}");
    return;
  }
  esp_err_t err = saveNextOperatingMode(OperatingMode::BleTransfer);
  if (err != ESP_OK) {
    Serial.printf("[MODE] cannot save next mode: %d\n", (int)err);
    server.send(500, "application/json", "{\"error\":\"mode setting could not be saved\"}");
    return;
  }
  g_mode_switch_pending = true;
  g_mode_restart_at = millis() + 750;
  server.send(202, "application/json", "{\"mode\":\"ble\",\"rebooting\":true}");
}

// GET /digest: テスト用（最新の転送ニュースを読み上げる）
static void handle_digest() {
  String content;
  int idx = 0;
  bool has = digest_load_latest(content, idx);
  exec_digest_category();
  server.send(200, "text/plain",
              has ? (String("転送ニュース #") + String(idx) + " を読み上げます")
                  : String("転送ニュースはまだありません"));
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

// ==== 再生計測ログ（第9.10弾） ====
// 「再生が実時間より遅いのか」「途中で止まっているのか」を実機ログから
// 切り分けるため、再生開始時に想定音声長を出し、終了時に実所要時間と
// 消費バイト数を出す。想定音声長は先頭フレームヘッダから求める
// （tts.quest のMP3は CBR 固定: MPEG2 L3 160kbps/24kHz・480B/フレーム・
// 24ms/フレームを実測で確認済み。一般の L3 ヘッダでも計算できる）。
// Only the decoder task pumps PCM. UI/JPEG/HTTP work must not pace playback.
static SemaphoreHandle_t g_decoder_mutex = nullptr;
static std::atomic<bool> g_decoder_running{false};
static std::atomic<bool> g_decoder_finished{false};
static uint32_t g_decoder_end_ms = 0;
static uint32_t g_decoder_last_ms = 0;
static uint32_t g_decoder_max_gap_ms = 0;
static bool audioRunning() { return g_decoder_running.load(); }
static void audioDecodeTask(void*) {
  for (;;) {
    xSemaphoreTake(g_decoder_mutex, portMAX_DELAY);
    if (g_decoder_running) {
      uint32_t now = millis();
      if (g_decoder_last_ms) {
        uint32_t gap = now - g_decoder_last_ms;
        if (gap > g_decoder_max_gap_ms) g_decoder_max_gap_ms = gap;
      }
      g_decoder_last_ms = now;
      const bool decoding = mp3->loop();
      if (!decoding || out.hasFault()) {
        mp3->stop();
        g_decoder_end_ms = millis();
        g_decoder_running = false;
        g_decoder_finished = true;
      }
    }
    xSemaphoreGive(g_decoder_mutex);
    vTaskDelay(1);
  }
}

static uint32_t g_play_start_ms = 0;    // 再生開始 millis（0=計測中でない）
static std::atomic<uint32_t> g_play_data_len{0};    // 再生中データの総バイト数（メモリ再生時）
static int32_t  g_play_expect_ms = -1;  // 想定音声長ms（-1=不明／ストリーム再生）

static bool mp3SourceAtEnd() {
  return memfile && g_play_data_len && memfile->getPos() >= g_play_data_len;
}
static Mp3DiagnosticLogger g_mp3_logger(Serial, mp3SourceAtEnd);

static int32_t mp3_expected_duration_ms(const uint8_t *d, size_t len) {
  if (d == nullptr || len < 4) return -1;
  size_t off = 0;
  if (len >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
    off = 10 + (((size_t)(d[6] & 0x7F) << 21) | ((size_t)(d[7] & 0x7F) << 14) |
                ((size_t)(d[8] & 0x7F) << 7) | (size_t)(d[9] & 0x7F));
    if (off + 4 > len) return -1;
  }
  uint32_t h = ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
               ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
  if ((h >> 21) != 0x7FF) return -1;              // 同期ワード
  int ver = (h >> 19) & 3;                        // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
  int layer = (h >> 17) & 3;                      // 1=Layer III
  int bri = (h >> 12) & 0xF;
  int sri = (h >> 10) & 3;
  int pad = (h >> 9) & 1;
  if (layer != 1 || bri == 0 || bri == 0xF || sri == 3 || ver == 1) return -1;
  static const int br_mpeg1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static const int br_mpeg2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  static const int sr_base[3] = {44100, 48000, 32000};
  int br = ((ver == 3) ? br_mpeg1[bri] : br_mpeg2[bri]) * 1000;
  int sr = sr_base[sri] >> ((ver == 3) ? 0 : (ver == 2) ? 1 : 2);
  if (br <= 0 || sr <= 0) return -1;
  int frame_len = ((ver == 3) ? (144 * br / sr) : (72 * br / sr)) + pad;
  int samples = (ver == 3) ? 1152 : 576;
  if (frame_len <= 0) return -1;
  size_t frames = (len - off) / (size_t)frame_len;  // 末尾ガード(64B)は1フレーム未満なので切り捨てで吸収
  return (int32_t)(frames * (size_t)samples * 1000 / (size_t)sr);
}

// 再生終了時の計測ログ。ソース解放（tts_cleanup_playback_sources）の前に呼ぶこと。
// reason: consumed が総量にほぼ達していれば natural（末尾まで再生）。
// 大きく残っていれば early-stop（途中でデコードが止まった）。
// elapsed と expect（話速を加味した expect_wall）を比べれば、再生が
// 実時間より遅かったかどうかも分かる。
static void tts_log_play_end() {
  if (g_play_start_ms == 0) return;
  uint32_t elapsed = (g_decoder_end_ms ? g_decoder_end_ms : millis()) - g_play_start_ms;
  uint32_t consumed = (memfile != nullptr) ? memfile->getPos() : 0;
  int32_t wall = (g_play_expect_ms >= 0 && TTS_SPEED_RATE > 0.1f)
                     ? (int32_t)(g_play_expect_ms / TTS_SPEED_RATE) : -1;
  const char *reason = "stream";
  if (g_play_data_len > 0) {
    reason = out.hasFault() ? "output-error"
        : (consumed >= g_play_data_len || g_play_data_len - consumed <= 8192) ? "natural" : "early-stop";
  }
  Serial.printf("[TTS] mp3 stop: elapsed=%ums expect=%dms expect_wall=%dms consumed=%u/%uB reason=%s max_decode_gap=%ums\n",
                (unsigned)elapsed, (int)g_play_expect_ms, (int)wall,
                (unsigned)consumed, (unsigned)g_play_data_len, reason, (unsigned)g_decoder_max_gap_ms);
  if (out.hasFault()) {
    g_tts_output_failed = true;
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    ++g_tts_failed_chunks;
    xSemaphoreGive(g_tts_mutex);
  }
  Serial.printf("[AUDIO] PCM submitted=%u peak=%u output_fault=%d master=%u channel0=%u\n",
      (unsigned)out.submittedSamples(), (unsigned)out.peak(), out.hasFault(),
      (unsigned)M5.Speaker.getVolume(), (unsigned)M5.Speaker.getChannelVolume(0));
  g_play_start_ms = 0;
  g_play_data_len = 0;
  g_play_expect_ms = -1;
}

bool playMP3(AudioFileSource *src){
  const auto spk = M5.Speaker.config();
  Serial.printf("[AUDIO] begin firmware=%s board=%d bck=%d ws=%d data=%d port=%d master=%u channel0=%u mute=%d\n",
      STACKCHAN_FIRMWARE_LABEL, (int)M5.getBoard(), (int)spk.pin_bck, (int)spk.pin_ws, (int)spk.pin_data_out,
      (int)spk.i2s_port, (unsigned)M5.Speaker.getVolume(), (unsigned)M5.Speaker.getChannelVolume(0), M5.Speaker.getVolume()==0);
  xSemaphoreTake(g_decoder_mutex, portMAX_DELAY);
  g_decoder_finished = false;
  g_decoder_end_ms = g_decoder_last_ms = g_decoder_max_gap_ms = 0;
  out.setAbort(false);
  bool ok = mp3->begin(src, &out);
  if (!ok) Serial.println("[AUDIO] output begin failed; playback was not started");
  g_decoder_running = ok;
  xSemaphoreGive(g_decoder_mutex);
  return ok;
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
// キューから1文を取り出して合成し、結果を ready キューへ積む。
// ready キューが上限（第9.10弾: 3文）に達している間は次の合成をしない。
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
    if (g_tts_ready_q.size() < TTS_READY_MAX && !g_tts_queue.empty()) {
      chunk = g_tts_queue.front();
      g_tts_queue.pop_front();
      voicevox_reset_cancel();
      g_tts_synth_busy = true;
      gen = g_tts_gen;
      have = true;
    }
    xSemaphoreGive(g_tts_mutex);
    if (!have) {
      // 第9.8弾: 解放依頼があれば、接続ペアの破棄は合成タスク自身が行う
      // （他タスクから破棄すると、使用中オブジェクトを触る競合になり得る）
      if (g_tts_release_req) {
        g_tts_release_req = false;
        voicevox_release_connection();
      }
      delay(30);
      continue;
    }
    uint8_t *dlData = nullptr;
    size_t dlLen = 0;
    {
      TlsGateGuard tls_guard;
      String url;
      if (WiFi.status() == WL_CONNECTED) url = voicevox_synth_url(chunk.text.c_str(), chunk.parms.c_str());
      else Serial.println("[TTS] synth skipped: WiFi disconnected");
      if (url != "") {
        int rc = voicevox_download_mp3(url, &dlData, &dlLen);
        if (rc != 0) Serial.printf("[TTS] download failed (rc=%d); no streaming fallback\n", rc);
      }
      voicevox_release_connection();
    }
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    // 中断などで世代が進んでいたら、この合成結果は破棄する（再生させない）
    if (gen == g_tts_gen && dlData != nullptr && dlLen > 0) {
      TtsReadyItem item;
      item.data = dlData;
      item.len = dlLen;
      g_tts_ready_q.push_back(item);
      dlData = nullptr;  // 所有権は ready キューへ移った
      Serial.printf("[TTS] ready queue: %u chunk(s) stocked\n", (unsigned)g_tts_ready_q.size());
    }
    if (gen == g_tts_gen && dlData == nullptr && dlLen == 0) {
      ++g_tts_failed_chunks;
      Serial.println("[TTS] chunk failed; no empty ready item queued");
    }
    if (gen == g_tts_gen && voicevox_certificate_error()) {
      g_tts_certificate_failed = true;
      g_tts_failed_chunks += g_tts_queue.size();
      g_tts_queue.clear();
      Serial.println("[TTS] session halted: certificate verification failed; inspect [TLS] flags");
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
  if (mp3 == nullptr || audioRunning()) return;

  // ready キューの状態をスナップショットする
  bool has_ready;
  bool more_coming;
  size_t ready_n;
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  has_ready = !g_tts_ready_q.empty();
  more_coming = !g_tts_queue.empty() || g_tts_synth_busy;
  ready_n = g_tts_ready_q.size();
  xSemaphoreGive(g_tts_mutex);

  if (has_ready) {
    // 第9.10弾: プリバッファ。セッション先頭では、後続チャンクがまだ
    // 来る見込みがあり ready が1文だけなら、2文目が揃うまで再生開始を
    // 最大20秒だけ待つ（短い文が続いたときの文間ギャップ対策）。
    if (!g_tts_played_in_session && more_coming && ready_n < 2) {
      uint32_t now = millis();
      if (g_tts_prebuf_since == 0) {
        g_tts_prebuf_since = now;
        Serial.println("[TTS] prebuffer: hold first play until 2nd chunk is ready (max 20s)");
      }
      if (now - g_tts_prebuf_since < 20000) return;
      Serial.println("[TTS] prebuffer: timeout, start with 1 chunk");
    }
    g_tts_prebuf_since = 0;

    uint8_t *data = nullptr;
    size_t len = 0;
    xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
    TtsReadyItem item = g_tts_ready_q.front();
    g_tts_ready_q.pop_front();
    data = item.data;
    len = item.len;
    xSemaphoreGive(g_tts_mutex);
    if (data != nullptr) {
      // 第9.6弾: メモリ再生（再生用にTLSを張らない）
      tts_cleanup_playback_sources();
      g_play_data = data;  // 所有権は再生側へ（再生終了時に後片付けで解放）
      if (voicevox_play_memory(data, len)) {
        // 第9.10弾: 再生計測の開始（想定音声長は先頭フレームから算出）
        g_play_start_ms = millis();
        g_play_data_len = (uint32_t)len;
        g_play_expect_ms = mp3_expected_duration_ms(data, len);
        int32_t wall = (g_play_expect_ms >= 0 && TTS_SPEED_RATE > 0.1f)
                           ? (int32_t)(g_play_expect_ms / TTS_SPEED_RATE) : -1;
        Serial.printf("[TTS] play start (memory): %uB expect=%dms expect_wall=%dms\n",
                      (unsigned)len, (int)g_play_expect_ms, (int)wall);
        g_tts_played_in_session = true;
        tts_on_play_started();
        return;
      }
      // begin 失敗時はソースとデータをここで解放する
      if (memfile != nullptr) { delete memfile; memfile = nullptr; }
      if (g_play_data != nullptr) { heap_caps_free(g_play_data); g_play_data = nullptr; }
      g_tts_output_failed = true;
      xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
      ++g_tts_failed_chunks;
      xSemaphoreGive(g_tts_mutex);
      Serial.println("[TTS] play begin failed (memory); skip sentence");
      return;
    }
    Serial.println("[TTS] invalid ready item skipped");
    return;
  }

  // セッション終了判定：投入済みチャンクが残っておらず合成も終わっていて、
  // ready キューにも残っていない
  bool pending;
  xSemaphoreTake(g_tts_mutex, portMAX_DELAY);
  pending = !g_tts_queue.empty() || g_tts_synth_busy || !g_tts_ready_q.empty();
  xSemaphoreGive(g_tts_mutex);
  if (g_speech_session && !pending) {
    g_speech_session = false;
    Serial.printf("[TTS] session end: failed_chunks=%u\n", g_tts_failed_chunks);
    if (g_tts_output_failed || g_tts_certificate_failed || (g_tts_failed_chunks && !g_tts_played_in_session)) {
      avatar.setSpeechText(g_tts_output_failed ? "スピーカー出力エラー。/audioで確認"
          : g_tts_certificate_failed ? "TLS証明書エラー。診断ログを確認"
          : "音声取得に失敗しました。ログを確認してください");
      g_info_msg_until = millis() + 5000;
    }
    // 第9.8弾: セッション終了後は TTS 接続ペアを解放し、TLSバッファ等を
    // 内蔵ヒープへ返却する（最大連続ブロックの回復。実際の破棄は合成
    // タスクがアイドル時に行う）。次の発話では接続を作り直す。
    g_tts_release_req = true;
    avatar.setExpression(Expression::Neutral);
    speech_text_buffer = "";
    g_news_playing = false;
    g_pose_state = -1;  // 第9弾: セッション終了でポーズを待機/表情連動へ戻す
    g_led_news_active = false;
    if (g_cap_armed) captionReset();  // 全文の合成失敗などで再生されなかった場合
    delay(200);
    audioBeginMicrophone();
    // v9.16: Speaker.end() leaves I2S installed; audioBeginMicrophone releases it first.
    Serial.println("[AUDIO] handover spk->mic (session end)");
    if (wakeword_is_enable) mode = 1;
    else mode = 0;
  }
}

static void handle_audio_status() {
  if (xSemaphoreTake(g_decoder_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    server.send(503, "application/json", "{\"busy\":true}");
    return;
  }
  StaticJsonDocument<768> doc;
  const auto cfg = M5.Speaker.config();
  doc["firmware"] = STACKCHAN_FIRMWARE_LABEL;
  doc["board"] = (int)M5.getBoard();
  doc["pmic"] = (int)M5.Power.getType();
  doc["volume_level"] = g_vol_level;
  doc["master"] = M5.Speaker.getVolume();
  doc["channel0"] = M5.Speaker.getChannelVolume(0);
  doc["muted"] = M5.Speaker.getVolume() == 0;
  doc["speaker_running"] = M5.Speaker.isRunning();
  doc["speaker_task_ready"] = speakerBackendReady(M5.Speaker);
  doc["mic_running"] = M5.Mic.isRunning();
  doc["bck"] = cfg.pin_bck;
  doc["ws"] = cfg.pin_ws;
  doc["data"] = cfg.pin_data_out;
  doc["i2s_port"] = (int)cfg.i2s_port;
  doc["pcm_submitted_samples"] = out.submittedSamples();
  doc["pcm_peak"] = out.peak();
  doc["output_fault"] = out.hasFault();
  doc["decoding"] = audioRunning();
  const auto tls = tlsMemorySnapshot();
  doc["tls_memory_policy"] = tls.installed;
  doc["tls_external_allocations"] = tls.externalAllocations;
  doc["tls_allocation_failures"] = tls.failures;
  doc["tls_last_failed_bytes"] = (unsigned)tls.lastFailedBytes;
  xSemaphoreGive(g_decoder_mutex);
  String json;
  serializeJson(doc, json);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void handle_tls_test() {
  if (server.method() != HTTP_POST || server.header("X-Stackchan-TLS-Test") != "1") {
    server.send(403, "application/json", "{\"error\":\"Use /audio\"}"); return;
  }
  if (tts_processing() || audioRunning() || speech_text != "" || speech_text_buffer != "") {
    server.send(409, "application/json", "{\"busy\":true}"); return;
  }
  const auto result = voicevox_probe_tls();
  StaticJsonDocument<384> doc;
  doc["firmware"] = STACKCHAN_FIRMWARE_LABEL;
  doc["connected"] = result.connected;
  doc["error"] = result.error;
  doc["verify_flags"] = result.flags;
  doc["allocation_failure"] = result.allocationFailure;
  doc["external_allocations"] = result.externalAllocations;
  doc["failed_allocations"] = result.failedAllocations;
  doc["http_requests_sent"] = 0;
  String json; serializeJson(doc, json);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void handle_audio_test() {
  // A custom header prevents cross-origin HTML forms from sounding the test.
  if (server.method() != HTTP_POST || server.header("X-Stackchan-Audio-Test") != "1") {
    server.send(403, "text/plain", "Use the test button on /audio"); return;
  }
  if (tts_processing() || audioRunning() || speech_text != "" || speech_text_buffer != "") {
    server.send(409, "text/plain", "音声処理の終了後に実行してください"); return;
  }
  const uint8_t master = M5.Speaker.getVolume();
  const uint8_t channel = M5.Speaker.getChannelVolume(7);
  M5.Mic.end();
  setSpeechVolume(M5.Speaker, 102);
  bool queued = M5.Speaker.begin() && speakerBackendReady(M5.Speaker);
  if (queued) {
    queued = M5.Speaker.tone(660, 150, 7, true);
    delay(200);
    queued = M5.Speaker.tone(880, 150, 7, true) && queued;
    delay(200);
  }
  audioBeginMicrophone();
  setSpeechVolume(M5.Speaker, master);
  M5.Speaker.setChannelVolume(7, channel);
  Serial.printf("[AUDIO] offline test queued=%d restored_master=%u\n", queued, (unsigned)master);
  server.send(queued ? 200 : 500, "text/plain", queued
      ? "2音の出力を要求しました。聞こえたか確認してください。元の音量に戻しました。"
      : "スピーカーの開始に失敗しました。診断状態とログを確認してください。");
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
  // 先行合成済みチャンクも全て破棄する（第9.10弾: ready キュー化に追随。
  // 所有権の一元管理は従来どおりここで解放する）
  while (!g_tts_ready_q.empty()) {
    TtsReadyItem item = g_tts_ready_q.front();
    g_tts_ready_q.pop_front();
    if (item.data != nullptr) heap_caps_free(item.data);
  }
  voicevox_cancel();
  g_tts_prebuf_since = 0;
  xSemaphoreGive(g_tts_mutex);
  xSemaphoreTake(g_decoder_mutex, portMAX_DELAY);
  out.setAbort(true);
  if (mp3 != nullptr && audioRunning()) mp3->stop();
  g_decoder_running = false;
  g_decoder_finished = false;
  tts_log_play_end();
  xSemaphoreGive(g_decoder_mutex);
  g_tts_release_req = true;  // 第9.8弾: 中断時も TTS 接続ペアの解放を依頼する
  tts_cleanup_playback_sources();
  speech_text = "";
  speech_text_buffer = "";
  g_news_playing = false;
  g_pose_state = -1;  // 第9弾: 中断時もポーズを待機/表情連動へ戻す
  captionReset();  // 字幕キュー破棄・字幕パネル消去・LEDのニュース色解除
  // 中断フィードバック：確認音＋吹き出しに短く表示（loop 側で約1.5秒後に消す）
  M5.Speaker.setChannelVolume(7, 255);
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
  static uint8_t shown_brightness = 0xFF;  // 最後に show() で出力した全体輝度（第9.23弾）
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
      } else if (mp3 != nullptr && audioRunning()) {
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

    // 第9.23弾: 輝度だけが変わった場合も出力する。従来はフレーム（ピクセル値）の
    // 変化でしか show() しなかったため、静止フレーム（聞き取り中の青一色・音量
    // ゲージ等）では FastLED.setBrightness() の変更が LED に届かなかった。
    uint8_t cur_brightness = FastLED.getBrightness();
    if (!prev_valid || cur_brightness != shown_brightness ||
        memcmp(prev_frame, frame, sizeof(frame)) != 0) {
      for (int i = 0; i < SIDE_LED_COUNT; i++) g_side_leds[i] = frame[i];
      FastLED.show();
      memcpy(prev_frame, frame, sizeof(frame));
      prev_valid = true;
      shown_brightness = cur_brightness;
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
  TlsGateGuard tls_guard;
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

// 画面キャプチャ HTTP ハンドラの前方宣言（実体はキャプチャ処理の後方、第9.19弾）
static void handle_shots_list();
static void handle_shot_download();
static void handle_shot_delete();

void setup()
{
  esp_err_t mode_error = ESP_OK;
  g_operating_mode = consumeNextOperatingMode(mode_error);
  if (g_operating_mode == OperatingMode::BleTransfer) {
    auto ble_cfg = M5.config();
    ble_cfg.internal_spk = false;
    ble_cfg.external_spk = false;
    ble_cfg.external_speaker_value = 0;
    ble_cfg.internal_mic = false;
    M5.begin(ble_cfg);
    Serial.println("[BOOT] Firmware " STACKCHAN_FIRMWARE_LABEL);
    Serial.println("[MODE] BLE-only: Wi-Fi/TLS/audio/Avatar/scheduler not started");
    ble_mode_setup();
    return;
  }
  // Memory release is irreversible in this boot. BLE is only entered by reboot.
  size_t heap_before_bt_release = ESP.getFreeHeap();
  esp_err_t bt_release = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE
      ? esp_bt_mem_release(ESP_BT_MODE_BTDM) : ESP_ERR_INVALID_STATE;
  size_t heap_after_bt_release = ESP.getFreeHeap();
  // 第9.3弾: グラフィックス直列化ロックを最初に生成する
  // （avatar.init() で描画タスクが始まる前に必ず用意する）
  g_gfx_mutex = xSemaphoreCreateRecursiveMutex();
  g_tls_mutex = xSemaphoreCreateRecursiveMutex();

  auto cfg = M5.config();

  cfg.internal_spk = true;
  cfg.external_spk = false;
  cfg.external_speaker_value = 0;  // This build targets the built-in Core2 speaker.
//  cfg.output_power = true;
  M5.begin(cfg);
  Serial.printf("[MODE] normal: BT memory release=%d heap=%u -> %u mode_read=%d\n",
                (int)bt_release, (unsigned)heap_before_bt_release,
                (unsigned)heap_after_bt_release, (int)mode_error);
  if (bt_release != ESP_OK) {
    Serial.println("[BOOT] FATAL: normal-mode BT memory release failed");
    for (;;) delay(1000);
  }
  if (!g_gfx_mutex || !g_tls_mutex || stackchan_avatar_gfx_lock_revision() != STACKCHAN_AVATAR_GFX_LOCKED) {
    Serial.println("[BOOT] FATAL: graphics/TLS lock unavailable");
    for (;;) delay(1000);
  }
  if (!installTlsMemoryPolicy()) {
    Serial.println("[BOOT] FATAL: TLS memory policy unavailable");
    for (;;) delay(1000);
  }
  Serial.println("[BOOT] TLS memory: PSRAM >=4096B, internal small allocations");
  Serial.println("[BOOT] Firmware " STACKCHAN_FIRMWARE_LABEL);
  Serial.println("[BOOT] Avatar gfx lock: vendored-v9.13 (shared mutex, draw stack=8192)");
  voicevox_log_ca();
  drawFirmwareVersionBadge(M5.Display);
  M5.Display.setCursor(0, 22);

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
  audioBeginMicrophone();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 48000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk_cfg);
  }
//  M5.Speaker.begin();

  Servo_setup();
  delay(1000);

  {
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      size_t volume = 153;
      uint8_t led_onoff = 1;
      uint8_t speaker_no = 3;
      nvs_get_u32(nvs_handle, "volume", &volume);
      if(volume > 255) volume = 255;
      M5.Speaker.setVolume(volume);
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, 255);
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
        M5.Speaker.setChannelVolume(m5spk_virtual_channel, 255);
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
        if (ESP_OK == nvs_get_u32(nvs_handle, "volume", &volume)) {
          g_vol_level = volumeToLevel((uint8_t)(volume > 255 ? 255 : volume));
        }
      }
      nvs_close(nvs_handle);
    }
    applyVolLevel(false);
    Serial.printf("[AUDIO] volume level=%u master=%u channel0=%u muted=%d\n",
        (unsigned)g_vol_level, (unsigned)M5.Speaker.getVolume(),
        (unsigned)M5.Speaker.getChannelVolume(0), M5.Speaker.getVolume()==0);
  }

  { // 第7弾：スケジュール項目と側面LEDの ON/OFF を NVS "setting" から読み込む（未保存なら既定ON）
    uint32_t nvs_handle;
    if (ESP_OK == nvs_open("setting", NVS_READONLY, &nvs_handle)) {
      uint8_t v;
      if (ESP_OK == nvs_get_u8(nvs_handle, "s_morning", &v)) g_sched_morning = (v != 0);
      if (ESP_OK == nvs_get_u8(nvs_handle, "s_hourly", &v)) g_sched_hourly = (v != 0);
      if (ESP_OK == nvs_get_u8(nvs_handle, "led", &v)) g_led_enable = (v != 0);
      if (ESP_OK == nvs_get_u8(nvs_handle, "ledlevel", &v) && v <= 5) g_led_level = v;  // 第9.19弾: LED輝度レベル
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

  const char* diagnosticHeaders[] = {"X-Stackchan-Audio-Test", "X-Stackchan-TLS-Test", "X-Stackchan-Mode"};
  server.collectHeaders(diagnosticHeaders, 3);
  server.on("/audio", []() { server.send_P(200, "text/html; charset=utf-8", AUDIO_DIAGNOSTICS_HTML); });
  server.on("/audio_status", HTTP_GET, handle_audio_status);
  server.on("/audio_test", HTTP_POST, handle_audio_test);
  server.on("/tls_test", HTTP_POST, handle_tls_test);
  server.on("/shots", HTTP_GET, handle_shots_list);    // 第9.19弾: キャプチャ一覧
  server.on("/shot", HTTP_GET, handle_shot_download);  // 第9.19弾: キャプチャ取得
  server.on("/shotdel", handle_shot_delete);           // 第9.19弾: キャプチャ削除（GET/POST）
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
  server.on("/weather", HTTP_GET, handle_weather);  // 第9.20弾: 天気テスト用（明日の予報）
  server.on("/train", HTTP_GET, handle_train);      // 第9.21弾: 運行情報テスト用（対象3路線）
  server.on("/mode", HTTP_GET, handle_mode);
  server.on("/ble", HTTP_GET, handle_mode);  // Read-only settings; never initialize BLE here.
  server.on("/ble", HTTP_POST, handle_ble);
  server.on("/digest", HTTP_GET, handle_digest);    // 第9.23弾: 転送ニュース読み上げ（テスト用）
  server.on("/memory", HTTP_GET, handle_memory_page);      // 第9.25弾: 長期記憶の一覧・追加・削除
  server.on("/memory_add", HTTP_POST, handle_memory_add);  // 第9.25弾
  server.on("/memory_del", HTTP_POST, handle_memory_del);  // 第9.25弾
  server.on("/memory_json", HTTP_GET, handle_memory_json); // 第9.25弾: 記憶一覧（テスト用）
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
    // 第9.26弾: ファイル不在・空ファイルは正常系（設定未保存）として扱い、
    // 警告も内容ダンプも出さない。従来は未保存でも毎回
    // 「Failed to deserialize JSON」と設定 JSON のダンプが出ていた。
    // 中身があるのに解釈できない場合だけ、理由を1行で知らせる。
    File file = SPIFFS.open("/data.json", "r");
    if(file && file.size() > 0){
      DeserializationError error = deserializeJson(chat_doc, file);
      file.close();
      if(error){
        Serial.printf("[BOOT] data.json: parse failed (%s); using defaults\n", error.c_str());
        init_chat_doc(json_ChatString.c_str());
      }
      serializeJson(chat_doc, InitBuffer);
      Role_JSON = InitBuffer;
    } else {
      if(file) file.close();
      init_chat_doc(json_ChatString.c_str());
    }
  } else {
    Serial.println("An Error has occurred while mounting SPIFFS");
  }

  server.begin();
  Serial.println("HTTP server started");
  Serial.println("[AUDIO] LAN status and offline speaker test: /audio");
  M5.Lcd.println("HTTP server started");  
  
  Serial.printf_P(PSTR("/ to control the chatGpt Server.\n"));
  M5.Lcd.print("/ to control the chatGpt Server.\n");
  delay(3000);

  audioLogger = &g_mp3_logger;
  // MP3デコーダの作業領域（約38KB。mad_frame 約21KB＋mad_synth
  // 約13KB＋入力バッファ等）は、従来 begin() のたびに内蔵ヒープへ
  // malloc され stop() で free されており、大きな断片化要因になっていた。
  // 第9.8弾ではライブラリの preallocation 経路で PSRAM に置いた。
  // 第9.10弾では「PSRAM 上ではデコードが実時間に追いつかない」仮説の
  // 検証として、起動時に内蔵ヒープから常駐確保する方式へ変更したが、
  // 実機では内蔵ヒープの常時圧迫により TLS ハンドシェイクがメモリ不足
  // （X509/SSL/PK の Allocation failed）で全滅し、音声が流れなくなった。
  // 第9.11弾: 配置を第9.8弾の PSRAM へ戻す（回復優先）。確保は起動時に
  // 行い常駐させる点と、配置先を起動ログに残す点は維持する。
  // なお確保した領域はデバイスの寿命中ずっと保持する（解放しない）。
  {
    size_t mp3WsSize = (size_t)AudioGeneratorMP3::preAllocSize();
    size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    void *mp3Ws = ps_malloc(mp3WsSize);
    const char *mp3WsPlace = "PSRAM";
    if (mp3Ws != nullptr) {
      mp3 = new AudioGeneratorMP3(mp3Ws, (int)mp3WsSize);
      Serial.printf("[BOOT] mp3 workspace: %u bytes in %s (boot-time, resident; largest internal block before=%u after=%u)\n",
                    (unsigned)mp3WsSize, mp3WsPlace, (unsigned)largestBefore,
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (mp3 == nullptr) {
      mp3 = new AudioGeneratorMP3();
      Serial.printf("[BOOT] mp3 workspace: prealloc failed (%u bytes); default per-begin allocation\n", (unsigned)mp3WsSize);
    }
  }
//  mp3->RegisterStatusCB(StatusCallback, (void*)"mp3");
  g_decoder_mutex = xSemaphoreCreateMutex();
  if (!g_decoder_mutex || xTaskCreatePinnedToCore(audioDecodeTask, "mp3Decode", 8192,
                                               nullptr, 2, nullptr, 1) != pdPASS) {
    Serial.println("[BOOT] FATAL: MP3 decoder task unavailable");
    for (;;) delay(1000);
  }
  Serial.println("[BOOT] MP3 decoder task ready (core=1 priority=2 " STACKCHAN_FIRMWARE_LABEL ")");
  wakeword_init();

  // 側面LED（M5GO Bottom2 の SK6812×10 / GPIO25）初期化と表示タスク起動
  FastLED.addLeds<SK6812, SIDE_LED_PIN, GRB>(g_side_leds, SIDE_LED_COUNT);
  FastLED.setBrightness(LED_LEVEL_VALUES[g_led_level]);  // 第9.19弾: 段階設定（既定レベル4＝従来上限6と同じ）
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
  // タッチレイアウト：右上＝設定ダイアログ（第9.19弾。表示中は四隅が音量/LED調整）、
  // 左上/左下＝ニュースカテゴリ選択、右下＝単独では無効（旧・音量ダウンは廃止）、
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

  g_last_activity_ms = millis();  // 画面自動消灯タイマーの起点（第9.19弾）
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
      if (tts_processing() || audioRunning()) {
        // A confirmation sound must not switch I2S back to the microphone.
        M5.Speaker.setChannelVolume(7, 255);
        M5.Speaker.tone(1000, 60, 7, false);
        return;
      }
      Serial.println("[AUDIO] handover via sw_tone");  // 第9.10弾: I2S切り分け用マーカー
      M5.Mic.end();
        M5.Speaker.tone(1000, 100);
      delay(500);
        audioBeginMicrophone();
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
  crumb("dance");
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
    drawFirmwareVersionBadge(M5.Lcd);
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

// TLS ゲートが空いているかを副作用なく調べる（取れたらすぐ返す）。
// 起動時ダンスの「穏やかな瞬間」判定に使う（第9.26弾）。
static bool tls_gate_free() {
  if (!g_tls_mutex) return true;
  if (xSemaphoreTakeRecursive(g_tls_mutex, 0) == pdTRUE) {
    xSemaphoreGiveRecursive(g_tls_mutex);
    return true;
  }
  return false;
}

// 起動時の自動再生（第9.1弾で setup() から loop() 初回へ移設）。
// 前回起動で再生中〜再生直後に panic していた場合（RTC フラグが立ったまま）は、
// フラグを下ろして今回の自動再生だけをスキップし、無限ブートループを断つ
// （音声トリガーの再生は通常どおり可能）。
// 第9.26弾: 発火条件を「起動3秒後・Wi-Fi接続」だけから「穏やかな瞬間」へ変更。
// v9.25 初回セッションでは起動直後のヒープが約52KBまで逼迫しており、他処理
// （ニュース取得・スケジューラ挨拶）と重なる自動再生はリスクが大きい。そこで
// 起動10秒以降・ヒープに余裕（自動再生専用の下限80KB、再生ガード自体は60KB）・
// 発話なし・TLS 空き・メニュー/ダイアログなし、が揃うまで最大120秒待つ。
// 揃わなければ自動再生だけ見送る（音声トリガーは従来どおり）。
static const uint32_t DANCE_BOOT_HEAP_MIN = 80000;   // 自動再生専用のヒープ下限
static const uint32_t DANCE_BOOT_WINDOW_MS = 120000; // 自動再生の判断窓（起動後）
static void dance_boot_check() {
  static bool decided = false;
  static bool defer_logged = false;
  if (decided) return;
  if (g_dance_playing_rtc) {
    g_dance_playing_rtc = false;
    decided = true;
    Serial.println("[DANCE] boot autoplay skipped: previous playback crashed (boot-loop guard)");
    return;
  }
  uint32_t now = millis();
  if (now < 10000) return;  // 起動直後はまだ待つ（第9.26弾: 3秒→10秒）
  const char *blocker = nullptr;
  if (WiFi.status() != WL_CONNECTED) blocker = "wifi";
  else if (ESP.getFreeHeap() < DANCE_BOOT_HEAP_MIN) blocker = "heap";
  else if (audioRunning() || tts_processing() ||
           speech_text != "" || speech_text_buffer != "") blocker = "speech";
  else if (!tls_gate_free()) blocker = "tls";
  else if (g_menu_open || g_settings_open) blocker = "ui";
  if (blocker != nullptr) {
    if (!defer_logged) {
      defer_logged = true;
      Serial.printf("[DANCE] boot autoplay deferred: %s (heap=%u)\n",
                    blocker, (unsigned)ESP.getFreeHeap());
    }
    if (now > DANCE_BOOT_WINDOW_MS) {
      decided = true;
      Serial.printf("[DANCE] boot autoplay skipped: no calm moment (%s)\n", blocker);
    }
    return;
  }
  decided = true;
  Serial.println("[DANCE] boot autoplay");
  play_dance_video();
  crumb("loop");
}

void SST_ChatGPT() {
        crumb("stt");
        if (tts_processing()) return;
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
          if (!audioRunning() && speech_text=="" && speech_text_buffer == "") {
            if (ret.indexOf("天気") >= 0 || ret.indexOf("予報") >= 0) {
              // 天気は ChatGPT を経由せず Open-Meteo から取得して読み上げる（第9.20弾）
              avatar.setExpression(Expression::Happy);
              avatar.setSpeechText("天気予報です");
              if (ret.indexOf("週間") >= 0) {
                exec_weather_weekly();
              } else if (ret.indexOf("明後日") >= 0) {
                exec_weather(2);
              } else if (ret.indexOf("今日") >= 0) {
                exec_weather(0);
              } else {
                exec_weather(1);  // 「明日」を含む場合と、日付指定がない場合の既定
              }
            } else if (ret.indexOf("遅延") >= 0 || ret.indexOf("遅れ") >= 0 ||
                       ret.indexOf("運行") >= 0 || ret.indexOf("電車") >= 0) {
              // 運行情報は ChatGPT を経由せず日テレ防災から取得して読み上げる（第9.21弾）
              avatar.setExpression(Expression::Happy);
              avatar.setSpeechText("運行情報です");
              exec_train();
            } else if (ret.indexOf("ニュース") >= 0) {
              // ニュースは ChatGPT を経由せず RSS を読み上げる（会話履歴には入れない）
              avatar.setExpression(Expression::Happy);
              avatar.setSpeechText("ニュースです");
              exec_news();
            } else if (ret.indexOf("踊") >= 0 || ret.indexOf("ダンス") >= 0) {
              // ダンス動画も ChatGPT を経由しない特例（会話履歴には入れない）
              Serial.println("[DANCE] voice trigger");
              play_dance_video();
              crumb("loop");
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

// LED輝度レベルを delta 段階変える（第9.19弾、設定ダイアログの右下/左下）。
// 変更は即時に FastLED へ反映され（ledTask が次フレームで表示）、NVS に保存される
static void change_led_level(int delta) {
  int nl = (int)g_led_level + delta;
  if (nl < 0) nl = 0;
  if (nl > 5) nl = 5;
  if (nl == (int)g_led_level) return;
  g_led_level = (uint8_t)nl;
  applyLedLevel(true);
  Serial.printf("[LED] brightness level=%u (FastLED=%u)\n",
                (unsigned)g_led_level, (unsigned)LED_LEVEL_VALUES[g_led_level]);
  if (!audioRunning()) {
    sw_tone();
  } else {
    M5.Speaker.setChannelVolume(7, 255);
    M5.Speaker.tone(1320.0f, 70, 7, false);
  }
}

// ==== 画面キャプチャ（第9.19弾） ====
// BtnC ダブルタップで LCD の内容を読み出し、24bit BMP(320x240) で保存する。
// 保存先は microSD の /shots/ を優先し、カードが使えない場合は SPIFFS の
// /shots/（名前の接頭辞による仮想フォルダ）へフォールバックする。
// 暴走防止: 保存先ごとにファイル数の上限を持ち、超えたら古い順に削除する。
#define SHOTS_DIR "/shots"
#define SHOTS_MAX_SD 100     // microSD 側の最大保持数
#define SHOTS_MAX_SPIFFS 3   // SPIFFS 側の最大保持数（1枚約230KBのため少なめ）

// キャプチャ保存先のファイルシステムを選ぶ。SD が使える場合は SD を開いた
// 状態で返し、呼び出し側は使い終わったら shots_close() で閉じること
static fs::FS *shots_open(bool &use_sd) {
  use_sd = false;
  if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    use_sd = true;
    if (!SD.exists(SHOTS_DIR)) SD.mkdir(SHOTS_DIR);
    return &SD;
  }
  return &SPIFFS;
}
static void shots_close(bool use_sd) {
  if (use_sd) SD.end();
}

// ファイル名が shot_NNNN.bmp 形式なら連番を返す（それ以外は -1）
static int shots_parse_index(const String &name) {
  int slash = name.lastIndexOf('/');
  String base = (slash >= 0) ? name.substring(slash + 1) : name;
  if (!base.startsWith("shot_") || !base.endsWith(".bmp")) return -1;
  String num = base.substring(5, base.length() - 4);
  if (num.length() == 0) return -1;
  for (size_t i = 0; i < num.length(); i++) if (!isDigit(num[i])) return -1;
  return num.toInt();
}

// 保存先にあるキャプチャを走査する。count=件数、max_index=最大の連番、
// oldest_path=最も古い（連番が最小の）ファイルのフルパス
static void shots_scan(fs::FS &fsys, bool use_sd, int &count, int &max_index, String &oldest_path) {
  count = 0;
  max_index = 0;
  int min_index = -1;
  oldest_path = "";
  File dir = use_sd ? fsys.open(SHOTS_DIR) : fsys.open("/");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String fname = f.name();
    int idx = shots_parse_index(fname);
    if (idx >= 0) {
      count++;
      if (idx > max_index) max_index = idx;
      if (min_index < 0 || idx < min_index) {
        min_index = idx;
        oldest_path = use_sd ? (fname.startsWith("/") ? fname : String(SHOTS_DIR) + "/" + fname)
                             : fname;
      }
    }
    f.close();
  }
  dir.close();
}

// ファイル名の検証（HTTP 経由の取得/削除用）。shot_NNNN.bmp の形だけを許可し、
// パス区切りや ".." を含むものは弾く（パストラバーサル対策）
static bool shots_valid_name(const String &name) {
  if (name.length() == 0 || name.length() > 32) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) return false;
  return shots_parse_index(name) >= 0;
}

// 画面キャプチャを実行する。成功したら保存先とファイル名を返す
static bool capture_screen(String &out_where, String &out_name) {
  bool use_sd;
  fs::FS *fsys = shots_open(use_sd);
  int count, max_index;
  String oldest;
  shots_scan(*fsys, use_sd, count, max_index, oldest);
  const int max_files = use_sd ? SHOTS_MAX_SD : SHOTS_MAX_SPIFFS;
  // 上限を超えないよう、古いものから削除する（保存後に max_files 以下に収める）
  while (count >= max_files && oldest.length() > 0) {
    Serial.printf("[SHOT] remove old: %s\n", oldest.c_str());
    String prev = oldest;
    fsys->remove(oldest);
    shots_scan(*fsys, use_sd, count, max_index, oldest);
    if (oldest == prev) break;  // 削除できなかったら諦める（無限ループ防止）
  }
  char name[24];
  snprintf(name, sizeof(name), "shot_%04d.bmp", max_index + 1);
  String path = String(SHOTS_DIR) + "/" + name;

  // フレーム全体を PSRAM へ読み出してから書き込む（描画ロックの保持を読み出し中だけにする）
  const int W = 320, H = 240;
  // 空き容量の確認（不足する場合はファイルを作らずに失敗として返す）
  {
    const uint32_t need = 54 + (uint32_t)W * 3 * (uint32_t)H;
    uint64_t total = use_sd ? SD.totalBytes() : SPIFFS.totalBytes();
    uint64_t used = use_sd ? SD.usedBytes() : SPIFFS.usedBytes();
    if (total < used || total - used < need) {
      Serial.println("[SHOT] not enough free space");
      shots_close(use_sd);
      return false;
    }
  }
  uint16_t *frame = (uint16_t *)ps_malloc((size_t)W * H * sizeof(uint16_t));
  if (!frame) { shots_close(use_sd); return false; }
  {
    GfxLockGuard gfx_guard;
    M5.Display.readRect(0, 0, W, H, frame);
  }

  File out = fsys->open(path, FILE_WRITE);
  if (!out) {
    free(frame);
    shots_close(use_sd);
    return false;
  }
  // BMP ヘッダ（BITMAPFILEHEADER 14 + BITMAPINFOHEADER 40、24bit 非圧縮）
  const uint32_t row_bytes = (uint32_t)W * 3;  // 960（4の倍数なので行パディング不要）
  const uint32_t img_bytes = row_bytes * (uint32_t)H;
  const uint32_t file_bytes = 54 + img_bytes;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  memcpy(hdr + 2, &file_bytes, 4);
  uint32_t data_off = 54;
  memcpy(hdr + 10, &data_off, 4);
  uint32_t dib = 40;
  memcpy(hdr + 14, &dib, 4);
  int32_t w32 = W, h32 = H;
  memcpy(hdr + 18, &w32, 4);
  memcpy(hdr + 22, &h32, 4);  // 正の値＝ボトムアップ
  uint16_t planes = 1, bpp = 24;
  memcpy(hdr + 26, &planes, 2);
  memcpy(hdr + 28, &bpp, 2);
  memcpy(hdr + 34, &img_bytes, 4);
  out.write(hdr, sizeof(hdr));
  // 画素: ボトムアップ（最下行から）、RGB565 → BGR888 へ変換しながら書く
  uint8_t *row = (uint8_t *)malloc(row_bytes);
  bool ok = (row != nullptr);
  if (ok) {
    for (int y = H - 1; y >= 0 && ok; y--) {
      const uint16_t *src = frame + (size_t)y * W;
      for (int x = 0; x < W; x++) {
        uint16_t c = src[x];
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3) | (uint8_t)(((c >> 11) & 0x1F) >> 2);
        uint8_t g = (uint8_t)(((c >> 5) & 0x3F) << 2) | (uint8_t)(((c >> 5) & 0x3F) >> 4);
        uint8_t b = (uint8_t)((c & 0x1F) << 3) | (uint8_t)((c & 0x1F) >> 2);
        row[x * 3 + 0] = b;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = r;
      }
      ok = (out.write(row, row_bytes) == row_bytes);
    }
    free(row);
  }
  out.close();
  free(frame);
  shots_close(use_sd);
  if (!ok) return false;
  out_where = use_sd ? "SD" : "SPIFFS";
  out_name = name;
  Serial.printf("[SHOT] saved %s to %s (%u bytes)\n", path.c_str(), out_where.c_str(), (unsigned)file_bytes);
  return true;
}

// BtnC ダブルタップ時の処理：キャプチャして結果をトーンと吹き出しで知らせる
static void do_screen_capture() {
  if (g_display_off) display_wake();
  note_activity();
  String where, name;
  if (capture_screen(where, name)) {
    sw_tone();
    delay(120);
    sw_tone();
    if (!audioRunning()) {
      avatar.setSpeechText(("キャプチャ保存 " + where).c_str());
      g_info_msg_until = millis() + 3000;
    }
  } else {
    // 失敗時は画面表示で知らせる（保存先なし・容量不足など）
    avatar.setSpeechText("キャプチャ失敗");
    g_info_msg_until = millis() + 4000;
    Serial.println("[SHOT] capture failed");
  }
}

// ==== 画面キャプチャの HTTP 連携（第9.19弾） ====
// 一覧: GET /shots → {"storage":"SD"|"SPIFFS","files":[{"name","size"},...]} の JSON
// 取得: GET /shot?name=shot_0001.bmp → BMP を attachment で返す
// 削除: GET/POST /shotdel?name=shot_0001.bmp → 削除して JSON で結果を返す
// いずれも CORS ヘッダ（Access-Control-Allow-Origin: *）を付ける。
// 保存先の優先順位はキャプチャ時と同じ（microSD があれば SD、無ければ SPIFFS）。
static void shots_send_error(int code, const char *msg) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", String("{\"error\":\"") + msg + "\"}");
}

static void handle_shots_list() {
  bool use_sd;
  fs::FS *fsys = shots_open(use_sd);
  String json = String("{\"storage\":\"") + (use_sd ? "SD" : "SPIFFS") + "\",\"files\":[";
  File dir = use_sd ? fsys->open(SHOTS_DIR) : fsys->open("/");
  if (dir) {
    bool first = true;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String fname = f.name();
      if (shots_parse_index(fname) >= 0) {
        int slash = fname.lastIndexOf('/');
        String base = (slash >= 0) ? fname.substring(slash + 1) : fname;
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + base + "\",\"size\":" + String((unsigned)f.size()) + "}";
      }
      f.close();
    }
    dir.close();
  }
  json += "]}";
  shots_close(use_sd);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handle_shot_download() {
  if (!server.hasArg("name")) { shots_send_error(400, "name required"); return; }
  String name = server.arg("name");
  if (!shots_valid_name(name)) { shots_send_error(400, "invalid name"); return; }
  bool use_sd;
  fs::FS *fsys = shots_open(use_sd);
  String path = String(SHOTS_DIR) + "/" + name;
  File f = fsys->open(path, FILE_READ);
  if (!f) {
    shots_close(use_sd);
    shots_send_error(404, "not found");
    return;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(f, "image/bmp");
  f.close();
  shots_close(use_sd);
}

static void handle_shot_delete() {
  if (!server.hasArg("name")) { shots_send_error(400, "name required"); return; }
  String name = server.arg("name");
  if (!shots_valid_name(name)) { shots_send_error(400, "invalid name"); return; }
  bool use_sd;
  fs::FS *fsys = shots_open(use_sd);
  String path = String(SHOTS_DIR) + "/" + name;
  bool ok = fsys->exists(path) && fsys->remove(path);
  shots_close(use_sd);
  if (ok) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"deleted\":\"" + name + "\"}");
  } else {
    shots_send_error(404, "not found");
  }
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
  if (!audioRunning()) {
    sw_tone();
  } else {
    // 再生中は sw_tone（Speaker.end を伴う）で再生を止めないよう、
    // 別チャンネル(7)へ短い確認音だけを重ねる
    M5.Speaker.setChannelVolume(7, 255);
    M5.Speaker.tone(1320.0f, 70, 7, false);
  }
  // 吹き出しが空いているときだけゲージを表示し、約2秒で消す（loop 側で消去）
  if (!audioRunning() && speech_text == "" && speech_text_buffer == "" &&
      !g_cap_active && !g_cap_armed && !g_menu_open) {
    avatar.setSpeechText(volGaugeText().c_str());
    g_vol_gauge_active = true;
    g_vol_gauge_until = millis() + 2000;
  }
}

// メニュー選択中を吹き出しに表示する（例「2/5 AIニュース」）
static void menuShowBalloon() {
  String s = String(g_menu_sel + 1) + "/6 " + NEWS_CATS[g_menu_sel].label;
  avatar.setSpeechText(s.c_str());
}

// メニューのカテゴリ一覧描画は第9.26弾で KinakoFace.h の顔合成キャンバス
// へ移設した（旧 drawMenuList は M5.Lcd への直接描画で、顔スプライトの
// 毎フレーム転写に上書きされて点滅していた）。ここに残るのは選択状態と
// 確定判定（menu_step / loop の締切処理）のみ。

// 左上/左下タップ：メニューを開く／前後のカテゴリへ移動する
static void menu_step(int delta) {
  // 発話中・ニュース表示中はメニューを開かない（吹き出しを奪わない）
  if (audioRunning() || speech_text != "" || speech_text_buffer != "" || g_cap_active) return;
  if (g_menu_open) {
    g_menu_sel = (g_menu_sel + delta + 6) % 6;
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
  bool speechFree = !audioRunning() && speech_text == "" && speech_text_buffer == "";
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
  if (g_operating_mode == OperatingMode::BleTransfer) { ble_mode_loop(); return; }
  if (g_mode_switch_pending) {
    if (modeDeadlineReached(millis(), g_mode_restart_at)) ESP.restart();
    delay(10);
    return;
  }
  static int lastms = 0;

  // 第9.26弾: パンくずハートビート。15秒ごとに現在区間（crumb）とヒープを出す。
  // TG1WDT リセットが再発したら、直前の [HB] が最後にいた区間と残量を示す。
  {
    static uint32_t hb_last = 0;
    uint32_t hb_now = millis();
    if (hb_now - hb_last >= 15000) {
      hb_last = hb_now;
      const char *crumb_snapshot = (const char *)g_crumb;
      Serial.printf("[HB] t=%lu heap=%u largest=%u crumb=%s mode=%d seen_dirty=%d\n",
                    (unsigned long)hb_now, (unsigned)ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    crumb_snapshot, mode, g_news_seen_dirty ? 1 : 0);
    }
  }
  // 第9.26弾: 既読ストア書き戻しの保険（本筋は取得系の出口での一括保存。
  // ダーティーが残っていたらアイドル時に保存する）
  if (g_news_seen_dirty && !tts_processing() &&
      speech_text == "" && speech_text_buffer == "") {
    newsSeenFlush();
  }

  wifi_watchdog();  // 第8弾: Wi-Fi 切断の検出と自動再接続

  pose_prepare_tick();  // 第9.2弾: ポーズ画像の先行デコード（draw タスク外で実施）

  battery_icon_tick();  // 第9.6弾: バッテリー残量の定期取得（描画は顔合成側）

  dance_guard_tick();  // 第9.2弾: 再生後ガードの解除（再生終了から5秒安定したら）

  dance_boot_check();  // 第9.1弾: 起動時ダンス動画の自動再生（初回のみ・ガード付き）

  if (random_time >= 0 && millis() - lastms1 > random_time)
  {
    lastms1 = millis();
    random_time = 40000 + 1000 * random(30);
    if (!audioRunning() && speech_text=="" && speech_text_buffer == "") {
      exec_chatGPT(random_words[random(18)]);
      mode = 0;
    }
  }

  // 定期通知モード：ON で発火時刻を過ぎたらニュース（トピックス＋AI）を読み上げる。
  // 再生中・発話待ちがある場合は割り込まず、約30秒後に再試行する
  if (g_periodic_on && (int32_t)(millis() - g_periodic_next_ms) >= 0) {
    if (mode >= 0 && !audioRunning() && speech_text == "" && speech_text_buffer == "" && !ble_digest_active()) {
      g_menu_open = false;
      avatar.setSpeechText("ニュースです");
      exec_news(true);  // 自動読み上げ: 新しい記事がなければ無言でスキップ（第9.20弾）
      append_unread_digest();  // 第9.23弾: 未読の転送テキストがあれば続けて読み上げる
      if (g_news_nothing_new && speech_text == "") avatar.setSpeechText("");
      g_periodic_next_ms = millis() + 300000UL;
    } else {
      g_periodic_next_ms = millis() + 30000UL;
    }
  }

  // スケジューラ（毎朝7:00モーニング読み上げ／時報）。発火可否は scheduler_ready()
  // で判定し、混雑時の延期・スキップは Scheduler 側で処理する。
  // 定期通知モードとは独立に動作する
  run_schedule();
  fc_timers_tick();       // 第9.25弾: Function Calling のタイマー判定（相対指定は NTP 未同期でも発火させる）
  memory_extract_tick();  // 第9.25弾: 長期記憶の自動抽出（アイドル時のみ・1会話1回）

  M5.update();

  // 画面消灯中は、最初のタッチ/物理ボタンを「復帰」専用として消費する（第9.19弾）。
  // 復帰に使った入力で会話開始・カテゴリ変更・ボタン機能が誤作動しないようにする
  bool input_swallow = false;
  if (g_display_off) {
    bool wake_input = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();
    if (!wake_input && M5.Touch.getCount() > 0) {
      auto td = M5.Touch.getDetail();
      wake_input = td.wasPressed();
    }
    if (wake_input) display_wake();
    input_swallow = true;
  }

  if (!input_swallow && M5.BtnA.wasPressed())
  {
    note_activity();
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
  if (!input_swallow && M5.BtnB.wasPressed()) {
    note_activity();
    btnB_press_start = millis();
    btnB_long_fired = false;
  }

  if (!tts_processing() && M5.BtnB.pressedFor(2000)) {
     btnB_long_fired = true;  // 長押し（ウェイクワード登録）が発動したら短押し扱いしない
     M5.Mic.end();
     M5.Speaker.tone(1000, 100);
     delay(500);
     M5.Speaker.tone(600, 100);
     delay(1000);
    audioBeginMicrophone();
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
      if (!audioRunning() && speech_text == "" && speech_text_buffer == "") {
        sw_tone();
        avatar.setExpression(Expression::Happy);
        avatar.setSpeechText("ニュースです");
        exec_news();
      }
    }
    btnB_press_start = 0;
  }

  // BtnC短押し＝定期通知モードのトグル、ダブルタップ＝画面キャプチャ、
  // 長押し(2秒)＝バッテリー残量読み上げ（第9.19弾でダブルタップを追加）。
  // ダブル判定のため、短押し（定期通知トグル）の発火は最大 BTNC_TAP_WINDOW_MS(300ms) 遅れる
  static uint32_t btnC_press_start = 0;
  static bool btnC_long_fired = false;
  static uint32_t btnC_tap_deadline = 0;  // 保留中の短押し（定期通知トグル）の確定時刻（0=なし）
  if (!input_swallow && M5.BtnC.wasPressed()) {
    note_activity();
    btnC_press_start = millis();
    btnC_long_fired = false;
  }
  if (M5.BtnC.pressedFor(2000) && !btnC_long_fired) {
    btnC_long_fired = true;  // 長押しが発動したら短押し扱いしない
    btnC_tap_deadline = 0;   // 保留中の単押しも取り消す
    sw_tone();
    report_batt_level();
  }
  if (M5.BtnC.wasReleased()) {
    if (!btnC_long_fired && btnC_press_start != 0 && (millis() - btnC_press_start) < 1500 && mode >= 0) {
      if (btnC_tap_deadline != 0) {
        // 判定窓内の2回目のタップ＝ダブルタップ → 画面キャプチャ
        btnC_tap_deadline = 0;
        do_screen_capture();
      } else {
        // 1回目のタップは保留し、窓内に2回目がなければ定期通知トグルとして確定する
        btnC_tap_deadline = millis() + BTNC_TAP_WINDOW_MS;
      }
    }
    btnC_press_start = 0;
  }
  // 保留中の BtnC 短押しの確定（ダブルタップにならなかった場合）
  if (btnC_tap_deadline != 0 && (int32_t)(millis() - btnC_tap_deadline) >= 0) {
    btnC_tap_deadline = 0;
    if (mode >= 0) toggle_periodic();
  }

#if defined(ARDUINO_M5STACK_Core2) || defined( ARDUINO_M5STACK_CORES3 )
  auto count = M5.Touch.getCount();
  if (count && !input_swallow)
  {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed())
    {          
      note_activity();
      if (g_settings_open)
      {
        // 設定ダイアログ表示中はモーダル（第9.19弾）：四隅＝音量/LED輝度調整、
        // 中央＝閉じる。それ以外のタップと四隅の本来の機能（カテゴリ切替等）は無効
        g_settings_deadline = millis() + SETTINGS_DIALOG_TIMEOUT_MS;
        if (box_vol_up.contain(t.x, t.y)) change_volume(+1);            // 右上＝音量＋
        else if (box_cat_prev.contain(t.x, t.y)) change_volume(-1);     // 左上＝音量－
        else if (box_vol_down.contain(t.x, t.y)) change_led_level(+1);  // 右下＝LED輝度＋
        else if (box_cat_next.contain(t.x, t.y)) change_led_level(-1);  // 左下＝LED輝度－
        else if (box_stt.contain(t.x, t.y))
        {
          g_settings_open = false;
          sw_tone();
        }
      }
      else
      {
      // 右上＝設定ダイアログを開く（第9.19弾）。従来の右上＝音量アップ／
      // 右下＝音量ダウンのタッチ割り当ては廃止し、右下単独のタップは無効。
      // 音量・LED輝度の調整はダイアログ表示中の四隅タップで行う
      if (box_vol_up.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        g_settings_open = true;
        g_settings_deadline = millis() + SETTINGS_DIALOG_TIMEOUT_MS;
        sw_tone();
      }
      // 左上＝前のカテゴリ、左下＝次のカテゴリ（ニュース選択メニュー）
      if (box_cat_prev.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        menu_step(-1);
      }
      if (box_cat_next.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        menu_step(+1);
      }
      // 中央タップの優先順位：ニュース読み上げ中＝中断（追加D）／
      // メニュー表示中＝キャンセル／それ以外＝会話開始
      if (box_stt.contain(t.x, t.y))
      {
        if (g_news_playing || g_pending_news_speech)
        {
          g_center_tap_pending = false;
          stop_news_speech();
        }
        else if (g_menu_open)
        {
          g_center_tap_pending = false;
          g_menu_open = false;
          sw_tone();
          if (!audioRunning()) avatar.setSpeechText("");
        }
        else if (!audioRunning())
        {
          // 中央ダブルタップで即消灯（第9.19弾）。1回目のタップは保留し、
          // 判定窓（300ms）内の2回目で消灯する。窓を過ぎたらシングルタップ
          // として loop 側で会話開始する（発火は最大300ms遅れる）
          if (g_center_tap_pending &&
              (int32_t)(millis() - g_center_tap_ms) <= (int32_t)CENTER_TAP_WINDOW_MS)
          {
            g_center_tap_pending = false;
            display_sleep();
          }
          else
          {
            g_center_tap_pending = true;
            g_center_tap_ms = millis();
          }
        }
      }
#ifdef USE_SERVO
      if (box_servo.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        servo_home = !servo_home;
        sw_tone();
      }
#endif
      if (box_BtnA.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        sw_tone();
        switch_monologue_mode();
      }
      // 右の帯＝定期通知モードのトグル（物理 BtnC 短押しと同じ挙動）
      if (box_BtnC.contain(t.x, t.y))
      {
        g_center_tap_pending = false;
        toggle_periodic();
      }
      }  // 設定ダイアログでない場合の通常タッチ処理ここまで（第9.19弾）
    }
  }
#endif

  // ニュースカテゴリ選択メニュー：一覧の描画は顔合成側（KinakoFace.h）。
  // ここでは、最後のタップから約2.5秒操作がなければ選択中カテゴリを
  // 読み上げて閉じる確定判定だけを行う
  if (g_menu_open) {
    if ((int32_t)(millis() - g_menu_deadline) >= 0) {
      if (mode >= 0 && !audioRunning() && speech_text == "" && speech_text_buffer == "" && !g_cap_active) {
        g_menu_open = false;
        avatar.setSpeechText("ニュースです");
        exec_news_category(g_menu_sel);
      } else {
        g_menu_deadline = millis() + 2500;  // 混んでいる間は確定を少し延ばす
      }
    }
  }

  // ==== 画面まわり（第9.19弾）: ダイアログ自動クローズ／中央タップ確定／自動消灯 ====
  if (g_settings_open && (int32_t)(millis() - g_settings_deadline) >= 0) {
    g_settings_open = false;
  }
  // 中央シングルタップの確定（ダブルタップにならなかった場合に会話開始）
  if (g_center_tap_pending &&
      (int32_t)(millis() - g_center_tap_ms) > (int32_t)CENTER_TAP_WINDOW_MS) {
    g_center_tap_pending = false;
    if (!g_display_off && !g_settings_open && !g_menu_open && !ble_digest_active() &&
        !g_news_playing && !g_pending_news_speech && !audioRunning() &&
        speech_text == "" && speech_text_buffer == "") {
      sw_tone();
      SST_ChatGPT();
    }
  }
  // 発話・合成中は無操作タイマーを進めない（終わってから5分で消灯する）
  if (audioRunning() || tts_processing()) note_activity();
  // 自動消灯: 発話系がすべて止まっている状態で5分間無操作なら消灯する。
  // ダンス再生中は loop が play_dance_video() の中で回るため、ここへは来ない
  if (!g_display_off && mode >= 0 && !audioRunning() && !tts_processing() &&
      speech_text == "" && speech_text_buffer == "" && !g_cap_active &&
      (int32_t)(millis() - g_last_activity_ms) >= (int32_t)DISPLAY_IDLE_OFF_MS) {
    display_sleep();
  }

  // 音量ゲージの吹き出しを約2秒で消す（発話やニュース表示・メニューが始まっていたら触らない）
  if (g_vol_gauge_active && (int32_t)(millis() - g_vol_gauge_until) >= 0) {
    g_vol_gauge_active = false;
    if (!audioRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_cap_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  // 「中断しました」の吹き出しを約1.5秒で消す（同上の条件で、他の表示が始まっていたら触らない）
  if (g_stop_msg_until != 0 && (int32_t)(millis() - g_stop_msg_until) >= 0) {
    g_stop_msg_until = 0;
    if (!audioRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_cap_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  // 通知吹き出し（ニュース取得失敗など、第9.6弾）を約4秒で消す（同上の条件）
  if (g_info_msg_until != 0 && (int32_t)(millis() - g_info_msg_until) >= 0) {
    g_info_msg_until = 0;
    if (!audioRunning() && speech_text == "" && speech_text_buffer == "" &&
        !g_cap_active && !g_menu_open) {
      avatar.setSpeechText("");
    }
  }

  if(speech_text != ""){
    // 第9.19弾: 発話セッション開始は「操作」として扱い、消灯中なら点灯する
    note_activity();
    g_center_tap_pending = false;
    if (g_display_off) display_wake();
    avatar.setExpression(Expression::Happy);
    speech_text_buffer = speech_text;
    speech_text = "";
    M5.Mic.end();
    if (!M5.Speaker.begin() || !speakerBackendReady(M5.Speaker)) {
      Serial.println("[AUDIO] speaker start failed; speech not queued");
      speech_text_buffer = "";
      g_pending_news_speech = false;
      captionReset();
      avatar.setSpeechText("スピーカー開始失敗。/audioで確認");
      g_info_msg_until = millis() + 5000;
      audioBeginMicrophone();
      return;
    }
    Serial.println("[AUDIO] handover mic->spk (speech start)");  // 第9.10弾: I2S切り分け用マーカー
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

  if (g_decoder_finished.exchange(false)) {
    tts_log_play_end();
    tts_cleanup_playback_sources();
  }
  tts_drive();
  if (!audioRunning()) {
    server.handleClient();
  }
  if (g_mode_switch_pending) return;

  // 読み上げ字幕（2行パネル）を進める（発話中はユニットを順に切り替え、
  // 発話が終わったら最後のユニットを3秒残してから消す）
  if (g_cap_active) {
    if (audioRunning()) {
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
 
