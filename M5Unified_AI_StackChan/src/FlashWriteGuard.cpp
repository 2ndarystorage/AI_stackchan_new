#include "FlashWriteGuard.h"

#include <driver/rmt.h>  // rmt_wait_tx_done（RMT アイドル保証、第9.28弾）
#include <esp_log.h>     // esp_log_level_set/get（RMT ドレイン走査中のログ抑止、第9.30弾）

// ミューテックスの単一実体（第9.31弾で main.cpp から移動）。
// setup（main.cpp）がタスク起動前に xSemaphoreCreateMutex() で生成する。
SemaphoreHandle_t g_storage_led_mutex = nullptr;

void storageLedLock() {
  if (g_storage_led_mutex) xSemaphoreTake(g_storage_led_mutex, portMAX_DELAY);
}
void storageLedUnlock() {
  if (g_storage_led_mutex) xSemaphoreGive(g_storage_led_mutex);
}

// 第9.28弾: RMT 送信の「完全なアイドル」まで待つ。FastLED.show() は送信を
// キューするだけで、実際の送信と補充割り込み（フラッシュ上の HAL コード）は
// show() の復帰後も非同期に続く。そのため第9.27弾の「書き込みと show() の
// 相互排他」だけでは、書き込み開始時点で送信中だったフレームと重なり得た。
// IDF の rmt_wait_tx_done は FastLED の RMT4 ドライバ自身が完了判定に使う
// 正規の経路で、ドライバ未導入のチャネルでは即座にエラーが返るため、
// 全チャネル（ESP32 は 8）を走査しても安全で速い。
//
// 第9.30弾: 上記の「即座にエラー」は無害だが、IDF が未導入チャネルへの呼び
// 出しのたびに E ログ「rmt: rmt_wait_tx_done: RMT DRIVER ERR」を出すため、
// アイドル時もドレインのたびに数行のエラーが流れ、本物のエラーが埋もれて
// いた（v9.29 実機で確認）。対処として、ドライバ導入済みチャネルを学習して
// 以後はそのチャネルだけを待つ。rmt_wait_tx_done の戻り値は、未導入なら
// ESP_FAIL、導入済みなら ESP_OK（送信完了）か ESP_ERR_TIMEOUT（送信中）で、
// ESP_FAIL 以外は「そのチャネルは送信し得る」ことの証明になる。未導入の
// チャネルは原理的に送信できないため、待たなくても第9.28弾の不変条件
// 「ロックが空いている＝RMT は完全にアイドル」は一切弱まらない。
// 学習が済むまでは毎回全走査し、その間だけ "rmt" タグのログを止めて元に
// 戻す（起動直後＝FastLED がまだドライバを導入していない間も E ログを
// 出さないため）。マスク等の状態は、両方の呼び出し箇所（FlashWriteGuard と
// ledTask）が g_storage_led_mutex 保持中にのみ触るため競合しない（ミュー
// テックス生成前の起動初期は単独実行区間）。
static uint8_t s_rmt_drain_mask = 0;     // ドライバ導入済みチャネルのマスク
static bool s_rmt_drain_mask_known = false;  // マスク確定済みか
void ledRmtDrain() {
  if (s_rmt_drain_mask_known) {
    // 確定後は導入済みチャネルだけを待つ（E ログは出ない）。
    for (int ch = 0; ch < 8; ch++) {
      if (s_rmt_drain_mask & (uint8_t)(1u << ch)) {
        rmt_wait_tx_done((rmt_channel_t)ch, pdMS_TO_TICKS(30));
      }
    }
    return;
  }
  // 未確定: 全チャネルを走査して導入済みチャネルを学習する。この走査の
  // 間だけ "rmt" タグのログを抑止し、直後に元のレベルへ必ず戻す。
  const esp_log_level_t prev_rmt_log_level = esp_log_level_get("rmt");
  esp_log_level_set("rmt", ESP_LOG_NONE);
  uint8_t found = 0;
  for (int ch = 0; ch < 8; ch++) {
    if (rmt_wait_tx_done((rmt_channel_t)ch, pdMS_TO_TICKS(30)) != ESP_FAIL) {
      found |= (uint8_t)(1u << ch);
    }
  }
  esp_log_level_set("rmt", prev_rmt_log_level);
  if (found != 0) {
    s_rmt_drain_mask = found;
    s_rmt_drain_mask_known = true;
  }
}
