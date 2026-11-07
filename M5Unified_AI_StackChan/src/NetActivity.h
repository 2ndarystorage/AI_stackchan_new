#pragma once
#include <stdint.h>

// ==== 通信アクティブ管理（第9.28弾・対策A） ====
// 「通信中は側面 LED の RMT 送信を全面停止する」ための参照カウント。
// カウントは TlsGateGuard（全 TLS 経路を自動で覆う）と NetActivityScope
// （TLS を使わない経路：STT・BLE）の両方から増減し、ledTask は
// netActivityActive() の間 FastLED.show() を一切呼ばない。
// これにより、キャッシュ無効化が起き得る通信フェーズの最中に
// RMT 補充割り込み（フラッシュ上の HAL コード）が実行される条件を
// 構造的に消す。

void netActivityBegin();
void netActivityEnd();
bool netActivityActive();
int netActivityCount();

void netActivityBeginTag(uint8_t fxNetTag);
void netActivityEndTag(uint8_t fxNetTag);

// RAII スコープ：通信区間の開始・終了をカウントとフォレンジックへ記録する
struct NetActivityScope {
  uint8_t tag;
  explicit NetActivityScope(uint8_t t) : tag(t) { netActivityBeginTag(tag); }
  ~NetActivityScope() { netActivityEndTag(tag); }
  NetActivityScope(const NetActivityScope &) = delete;
  NetActivityScope &operator=(const NetActivityScope &) = delete;
};
