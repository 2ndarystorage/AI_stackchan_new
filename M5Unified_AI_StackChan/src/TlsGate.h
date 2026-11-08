#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "NetActivity.h"

extern SemaphoreHandle_t g_tls_mutex;

// One cloud request at a time: TLS record buffers must not overlap on Core2.
// 第9.28弾: ゲート取得中は「通信アクティブ」として数える（対策Aの LED
// 送信ゲート）。全 TLS 経路がこのガードを通るため、通信区間の囲み忘れが
// 構造的に起きない。
class TlsGateGuard {
 public:
  TlsGateGuard() : locked(g_tls_mutex && xSemaphoreTakeRecursive(g_tls_mutex, portMAX_DELAY) == pdTRUE) {
    if (locked) netActivityBegin();
  }
  ~TlsGateGuard() {
    if (locked) {
      netActivityEnd();
      xSemaphoreGiveRecursive(g_tls_mutex);
    }
  }
  TlsGateGuard(const TlsGateGuard&) = delete;
  TlsGateGuard& operator=(const TlsGateGuard&) = delete;
 private:
  bool locked;
};
