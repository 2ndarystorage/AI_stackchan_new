#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_tls_mutex;

// One cloud request at a time: TLS record buffers must not overlap on Core2.
class TlsGateGuard {
 public:
  TlsGateGuard() : locked(g_tls_mutex && xSemaphoreTakeRecursive(g_tls_mutex, portMAX_DELAY) == pdTRUE) {}
  ~TlsGateGuard() { if (locked) xSemaphoreGiveRecursive(g_tls_mutex); }
  TlsGateGuard(const TlsGateGuard&) = delete;
  TlsGateGuard& operator=(const TlsGateGuard&) = delete;
 private:
  bool locked;
};
