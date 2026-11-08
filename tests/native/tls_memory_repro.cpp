// Offline Mbed TLS 2.28.1 verification with bounded internal memory. No HTTP/API calls.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <mbedtls/platform.h>
#include <mbedtls/x509_crt.h>
#include "TlsAllocationPolicy.h"

#include "TlsTestHeap.h"
using namespace tls_test;

struct Result { int code; uint32_t flags; size_t failures, externalCalls; bool reachedVerification; };
Result verify(const char* chainPath,const char* caPath,size_t budget,bool mixed,const char* host) {
  assert(heap.internalBytes==0 && heap.externalBytes==0);
  heap.limit=budget; heap.mixed=mixed; heap.failures=heap.externalCalls=0;
  mbedtls_x509_crt chain,ca;
  mbedtls_x509_crt_init(&chain); mbedtls_x509_crt_init(&ca);
  int result=mbedtls_x509_crt_parse_file(&chain,chainPath);
  if(result==0) result=mbedtls_x509_crt_parse_file(&ca,caPath);
  void *input=nullptr,*output=nullptr;
  if(result==0) { input=mbedtls_calloc(1,16384); output=mbedtls_calloc(1,16384); }
  bool reached=result==0 && input && output;
  uint32_t flags=0;
  if(reached) result=mbedtls_x509_crt_verify(&chain,&ca,nullptr,host,&flags,nullptr,nullptr);
  Result answer{result,flags,heap.failures,heap.externalCalls,reached};
  testFree(input); testFree(output);
  mbedtls_x509_crt_free(&chain); mbedtls_x509_crt_free(&ca);
  assert(heap.internalBytes==0 && heap.externalBytes==0);
  return answer;
}
int main(int argc,char**argv) {
  if(argc!=4) { std::fprintf(stderr,"Usage: tls_memory_repro CHAIN_PEM GOOD_CA_PEM WRONG_CA_PEM\n"); return 2; }
  mbedtls_platform_set_calloc_free(testCalloc,testFree);
  Result baseline=verify(argv[1],argv[2],256*1024,false,"api.tts.quest");
  assert(baseline.reachedVerification && baseline.code==0 && baseline.flags==0);
  size_t reproducedBudget=0;
  for(size_t budget=32*1024;budget<96*1024;budget+=64) {
    Result old=verify(argv[1],argv[2],budget,false,"api.tts.quest");
    if(old.reachedVerification && old.code==-9984 && old.flags==8 && old.failures) {
      Result fixed=verify(argv[1],argv[2],budget,true,"api.tts.quest");
      if(fixed.reachedVerification && fixed.code==0 && fixed.flags==0 && fixed.failures==0) {
        reproducedBudget=budget;
        std::printf("REPRO internal budget=%zu: old code=%d flags=0x%08x allocation_failures=%zu\n",budget,old.code,old.flags,old.failures);
        std::printf("FIX same budget: code=%d flags=0x%08x allocation_failures=%zu external_calls=%zu\n",fixed.code,fixed.flags,fixed.failures,fixed.externalCalls);
        break;
      }
    }
  }
  assert(reproducedBudget);
  for(int i=0;i<20;++i) {
    Result result=verify(argv[1],argv[2],reproducedBudget,true,"api.tts.quest");
    assert(result.code==0 && result.flags==0 && result.failures==0);
  }
  Result wrongCA=verify(argv[1],argv[3],256*1024,true,"api.tts.quest");
  assert(wrongCA.code==-9984 && (wrongCA.flags&8) && wrongCA.failures==0);
  Result wrongHost=verify(argv[1],argv[2],256*1024,true,"wrong.example");
  assert(wrongHost.code==-9984 && (wrongHost.flags&4) && wrongHost.failures==0);
  std::puts("PASS: 20 fixed verifications; wrong CA/hostname rejected; zero memory leaks");
}
