#include "TlsMemory.h"
#include "TlsAllocationPolicy.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

namespace {
portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;
TlsMemoryStats stats = {};

void recordFailure(size_t bytes) {
  const auto task = tlsCurrentTaskTag();
  portENTER_CRITICAL(&statsLock);
  ++stats.failures;
  stats.lastFailedBytes = bytes;
  stats.lastFailedTask = task;
  portEXIT_CRITICAL(&statsLock);
}

struct EspTlsHeap {
  void* external(size_t count, size_t size) {
    void* memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&statsLock);
    if (memory) ++stats.externalAllocations;
    else ++stats.externalFallbacks;
    portEXIT_CRITICAL(&statsLock);
    return memory;
  }
  void* internal(size_t count, size_t size) {
    void* memory = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) recordFailure(count * size);
    else {
      portENTER_CRITICAL(&statsLock);
      ++stats.internalAllocations;
      portEXIT_CRITICAL(&statsLock);
    }
    return memory;
  }
  void overflow() { recordFailure(SIZE_MAX); }
};

void* tlsCalloc(size_t count, size_t size) {
  EspTlsHeap heap;
  return allocateTlsMemory(count, size, heap);
}
void tlsFree(void* memory) { heap_caps_free(memory); }
}  // namespace

uintptr_t tlsCurrentTaskTag() { return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()); }

bool installTlsMemoryPolicy() {
  if (tlsMemorySnapshot().installed) return true;
  // Set once, before Wi-Fi/TLS tasks start. Both old/new allocations use heap_caps_free.
  const int result = mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  portENTER_CRITICAL(&statsLock);
  stats.installed = result == 0;
  portEXIT_CRITICAL(&statsLock);
  return result == 0;
}

TlsMemoryStats tlsMemorySnapshot() {
  portENTER_CRITICAL(&statsLock);
  const auto copy = stats;
  portEXIT_CRITICAL(&statsLock);
  return copy;
}
