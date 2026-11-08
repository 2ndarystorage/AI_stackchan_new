#pragma once
#include <stddef.h>
#include <stdint.h>

struct TlsMemoryStats {
  bool installed;
  uint32_t externalAllocations;
  uint32_t internalAllocations;
  uint32_t externalFallbacks;
  uint32_t failures;
  size_t lastFailedBytes;
  uintptr_t lastFailedTask;
};

bool installTlsMemoryPolicy();
TlsMemoryStats tlsMemorySnapshot();
uintptr_t tlsCurrentTaskTag();
