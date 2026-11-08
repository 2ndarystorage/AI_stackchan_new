#pragma once
#include <cassert>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include "TlsAllocationPolicy.h"

namespace tls_test {
struct alignas(std::max_align_t) Allocation { size_t bytes; bool external; };
struct BoundedHeap {
  size_t limit=0, internalBytes=0, externalBytes=0, failures=0, externalCalls=0;
  bool mixed=false;
  void* allocate(size_t n, size_t s, bool ext) {
    size_t bytes=n*s;
    if (!ext && bytes > limit-internalBytes) { ++failures; return nullptr; }
    auto* header=static_cast<Allocation*>(std::calloc(1,sizeof(Allocation)+bytes));
    assert(header); header->bytes=bytes; header->external=ext;
    if(ext) { externalBytes+=bytes; ++externalCalls; } else internalBytes+=bytes;
    return header+1;
  }
  void* external(size_t n,size_t s) { return allocate(n,s,true); }
  void* internal(size_t n,size_t s) { return allocate(n,s,false); }
  void overflow() { ++failures; }
} heap;
void* testCalloc(size_t n,size_t s) {
  return heap.mixed ? allocateTlsMemory(n,s,heap) : heap.internal(n,s);
}
void testFree(void* ptr) {
  if(!ptr) return;
  auto* header=static_cast<Allocation*>(ptr)-1;
  if(header->external) heap.externalBytes-=header->bytes; else heap.internalBytes-=header->bytes;
  std::free(header);
}
}  // namespace tls_test
