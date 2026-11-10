#pragma once
#include <stdint.h>
#include <esp_system.h>

// ==== クラッシュ耐性フォレンジック（第9.28弾） ====
// RTC noinit メモリ上のイベントリングバッファ。パニック／ウォッチドッグ／
// ソフトウェアリセットを越えて残り、次回起動時にシリアルへダンプされる。
// 「Cache disabled」パニックの再発時に、どの書き込み／通信と LED 送信が
// 重なっていたかを再起動後のログだけで特定するための仕組み。
// 電源断（POR）では fxInit がリングを初期化する。

enum FxKind : uint8_t {
  FXK_BOOT = 1,          // tag = esp_reset_reason_t
  FXK_WRITE_BEGIN = 2,   // tag = FxWriteTag
  FXK_WRITE_END = 3,     // tag = FxWriteTag
  FXK_NET_BEGIN = 4,     // tag = FxNetTag
  FXK_NET_END = 5,       // tag = FxNetTag
  FXK_TTS_BEGIN = 6,     // 合成セッション（1文分）の開始
  FXK_TTS_END = 7,       // 合成セッションの終了
  FXK_LED_GATE_CLOSE = 8,  // 通信開始で LED 送信ゲートが閉じた
  FXK_LED_GATE_OPEN = 9,   // 通信終了で LED 送信ゲートが開いた
  FXK_SD = 10,           // tag = FxSdTag
  FXK_WIFI = 11,         // tag = FxWifiTag（第9.29弾）
};

// フラッシュ書き込み箇所の識別タグ（FlashWriteGuard の引数）
enum FxWriteTag : uint8_t {
  FXW_NONE = 0,
  FXW_LED_LEVEL = 1,     // saveLedLevel の NVS 保存
  FXW_VOL_LEVEL = 2,     // applyVolLevel の NVS 保存
  FXW_APIKEY = 3,        // /apikey_set の NVS 保存
  FXW_DATA_JSON = 4,     // save_json（/data.json の SPIFFS 書き込み）
  FXW_SETTING_WEB = 5,   // /setting ハンドラの NVS 保存
  FXW_SEEN = 6,          // ニュース既読ストアの書き戻し
  FXW_MEMORY = 7,        // 長期記憶の保存
  FXW_DIGEST_STORE = 8,  // 転送テキストの正式保存（削除含む）
  FXW_DIGEST_MARK = 9,   // 転送テキスト既読マークの NVS 保存
  FXW_VOICE_SPEAKER = 10,  // /voice 話者保存
  FXW_VOICE_SPEED = 11,    // /voice 話速保存
  FXW_MODEL = 12,          // /model 保存
  FXW_SCHEDULE = 13,       // /schedule 保存
  FXW_SHOT_SAVE = 14,      // 画面キャプチャ保存（古い分の削除含む）
  FXW_SHOT_DELETE = 15,    // 画面キャプチャ削除
  FXW_WAKEWORD = 16,       // ウェイクワード登録のテンプレート保存（第9.31弾）
  FXW_MODE = 17,           // /ble POST の動作モード保存（第9.31弾）
  FXW_WW_ENABLE = 18,      // ウェイクワード有効/無効の NVS 保存（第9.34弾）
  FXW_SCHED_ON = 19,       // 定期通知 ON/OFF の NVS 保存（第9.34弾）
};

// 通信経路の識別タグ（NetActivityScope の引数）
enum FxNetTag : uint8_t {
  FXN_NONE = 0,
  FXN_NHK = 2,       // NHK RSS 取得
  FXN_GOOGLE = 3,    // Google RSS 取得
  FXN_WEATHER = 4,   // 天気取得
  FXN_TRAIN = 5,     // 運行情報取得
  FXN_CHAT = 6,      // ChatGPT / Function Calling
  FXN_EXTRACT = 7,   // 長期記憶の自動抽出
  FXN_STT = 8,       // 音声認識（Whisper / Google STT）
  FXN_TTS = 9,       // VOICEVOX 合成＋MP3 ダウンロード
  FXN_BLE = 10,      // BLE 受信モード（受信窓）
};

// SD イベントの識別タグ
enum FxSdTag : uint8_t {
  FXSD_MOUNT_OK = 1,         // この起動で初めて SD マウントに成功
  FXSD_DEAD = 2,             // 一度使えた SD が応答しなくなった（セッション中 SD 無効化）
  FXSD_SEEN_SPIFFS = 3,      // 既読ストアが SPIFFS を選択（選択時点で SD なし）
  FXSD_SEEN_MEMORY_ONLY = 4, // 既読ストアがメモリのみ動作へ移行（SD を失った）
  FXSD_SEEN_SAVE_SKIPPED = 5,  // メモリのみのため既読保存を見送った
  FXSD_WRITE_FAIL = 6,       // 既読ストアの SD 書き込みに失敗した
};

// Wi-Fi イベントの識別タグ（第9.29弾：複数 AP 対応）
enum FxWifiTag : uint8_t {
  FXWIFI_MULTI = 1,    // wifi.txt から1組以上を登録し複数 AP モードになった
  FXWIFI_CONNECT = 2,  // AP へ接続した（初回／再接続）
  FXWIFI_SWITCH = 3,   // 接続中 AP が別の AP へ切り替わった
};

void fxInit(esp_reset_reason_t reason);
void fxLog(uint8_t kind, uint8_t tag);
void fxDump();
