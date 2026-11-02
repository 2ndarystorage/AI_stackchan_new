#pragma once
#include <stdint.h>
#include <nvs.h>

enum class OperatingMode : uint8_t { Normal = 0, BleTransfer = 1 };

// Only this namespace is written. Wi-Fi, keys, audio and schedule settings stay intact.
inline esp_err_t saveNextOperatingMode(OperatingMode mode) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("runmode", NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_u8(handle, "next", static_cast<uint8_t>(mode));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

inline OperatingMode consumeNextOperatingMode(esp_err_t &err) {
  nvs_handle_t handle;
  err = nvs_open("runmode", NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) { err = ESP_OK; return OperatingMode::Normal; }
  if (err != ESP_OK) return OperatingMode::Normal;
  uint8_t next = 0;
  err = nvs_get_u8(handle, "next", &next);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err != ESP_OK || next == 0) return OperatingMode::Normal;
  // Consume before starting BLE: timeout, cancel or a crash cannot select BLE again.
  err = saveNextOperatingMode(OperatingMode::Normal);
  if (err != ESP_OK || next != static_cast<uint8_t>(OperatingMode::BleTransfer))
    return OperatingMode::Normal;
  return OperatingMode::BleTransfer;
}

inline bool modeDeadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

inline bool canEnterBleMode(bool audio, bool tts, bool pendingSpeech) {
  return !audio && !tts && !pendingSpeech;
}
