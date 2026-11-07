#pragma once
#include <stddef.h>
#include <stdint.h>

static constexpr size_t TLS_EXTERNAL_THRESHOLD = 4096;

template<class Heap>
void* allocateTlsMemory(size_t count, size_t size, Heap& heap) {
  if (!count || !size) return nullptr;
  if (count > SIZE_MAX / size) { heap.overflow(); return nullptr; }
  if (count * size >= TLS_EXTERNAL_THRESHOLD) {
    if (void* memory = heap.external(count, size)) return memory;
  }
  return heap.internal(count, size);
}
