"""Compile the real ESP allocator with deterministic heap/RTOS/mbedTLS mocks."""
import pathlib
import subprocess
import tempfile
import unittest
from test_tts_transport import body

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT/'M5Unified_AI_StackChan/src'


class TlsMemoryTests(unittest.TestCase):
    def test_allocator_policy_fallback_failure_accounting_and_thread_safety(self):
        stubs = {
            'esp_heap_caps.h': r'''
#pragma once
#include <stddef.h>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
void* heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void*);
''',
            'freertos/FreeRTOS.h': r'''
#pragma once
#include <mutex>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
''',
            'freertos/task.h': '#pragma once\ninline void* xTaskGetCurrentTaskHandle() { return (void*)77; }\n',
            'mbedtls/platform.h': r'''
#pragma once
#include <stddef.h>
int mbedtls_platform_set_calloc_free(void*(*)(size_t,size_t),void(*)(void*));
''',
        }
        code = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include "TlsMemory.h"
#include "esp_heap_caps.h"
void*(*allocate)(size_t,size_t)=nullptr;
void(*release)(void*)=nullptr;
bool failExternal=false,failInternal=false;
unsigned lastCaps=0, installations=0;
std::atomic<unsigned> live{0};
void* heap_caps_calloc(size_t n,size_t size,unsigned caps) {
  // lastCaps is checked only in the single-threaded phase.
  if(caps==5 && failExternal) return nullptr;
  if(caps==6 && failInternal) return nullptr;
  void* memory=std::calloc(n,size); if(memory)++live; return memory;
}
void heap_caps_free(void* p) { if(p)--live;std::free(p); }
int mbedtls_platform_set_calloc_free(void*(*a)(size_t,size_t),void(*f)(void*)) {
  ++installations;allocate=a;release=f;return 0;
}
int main() {
 void* oldAllocation=heap_caps_calloc(1,256,6);
 assert(installTlsMemoryPolicy()); assert(installTlsMemoryPolicy()); assert(installations==1);
 release(oldAllocation); assert(live==0); // Existing internal allocations remain free-compatible.
 auto initial=tlsMemorySnapshot(); assert(initial.installed && initial.failures==0);
 void* small=allocate(1,512); void* large=allocate(2,2048);
 assert(small && large);
 for(unsigned i=0;i<4096;++i)assert(((unsigned char*)large)[i]==0);
 auto stats=tlsMemorySnapshot(); assert(stats.internalAllocations==1 && stats.externalAllocations==1);
 release(small); release(large); assert(live==0);
 failExternal=true; void* fallback=allocate(1,4096); assert(fallback); release(fallback);
 stats=tlsMemorySnapshot(); assert(stats.externalFallbacks==1 && stats.failures==0 && stats.internalAllocations==2);
 failInternal=true;
 assert(!allocate(1,512)); stats=tlsMemorySnapshot();
 assert(stats.failures==1 && stats.lastFailedBytes==512 && stats.lastFailedTask==77);
 assert(!allocate(1,4096)); stats=tlsMemorySnapshot();assert(stats.failures==2 && stats.lastFailedBytes==4096);
 assert(!allocate(SIZE_MAX,2)); stats=tlsMemorySnapshot();assert(stats.failures==3 && stats.lastFailedBytes==SIZE_MAX);
 assert(!allocate(0,512)); assert(tlsMemorySnapshot().failures==3);
 failExternal=failInternal=false;
 const unsigned before=tlsMemorySnapshot().externalAllocations;
 auto request=[] { for(int i=0;i<1000;++i) { void* p=allocate(1,4096);assert(p);release(p); } };
 std::thread a(request),b(request);a.join();b.join();
 assert(tlsMemorySnapshot().externalAllocations-before==2000 && live==0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            folder = pathlib.Path(tmp)
            for name, text in stubs.items():
                p=folder/name
                p.parent.mkdir(parents=True,exist_ok=True)
                p.write_text(text)
            (folder/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-pthread','-I',tmp,'-I',str(SRC),
                            str(folder/'test.cpp'),str(SRC/'TlsMemory.cpp'),'-o',str(folder/'test')],check=True)
            subprocess.run([str(folder/'test')],check=True,timeout=10)

    def test_install_precedes_wifi_and_security_is_not_disabled(self):
        setup=(SRC/'main.cpp').read_text().split('void setup()',1)[1]
        self.assertLess(setup.index('installTlsMemoryPolicy()'),setup.index('WiFi.begin('))
        self.assertIn('FATAL: TLS memory policy unavailable',setup)
        client=(SRC/'CheckedTlsClient.h').read_text()
        self.assertNotIn('setInsecure(',client)
        self.assertIn('_CA_cert, false, _cert, _private_key, nullptr, nullptr, false, _alpn_protos',client)
        self.assertIn('after.lastFailedTask == tlsCurrentTaskTag()',client)

    def test_device_probe_is_handshake_only_and_reports_real_failures(self):
        source=(SRC/'WebVoiceVoxTTS.cpp').read_text()
        probe=body(source,'VoicevoxTlsProbe voicevox_probe_tls() {')
        declaration=body((SRC/'WebVoiceVoxTTS.h').read_text(),'struct VoicevoxTlsProbe')+';\n'
        self.assertNotIn('VOICEVOX_API_KEY',probe)
        self.assertNotIn('HTTPClient',probe)
        self.assertNotIn('GET(',probe)
        self.assertNotIn('POST(',probe)
        code=r'''
#include <cassert>
#include <cstring>
#include "TlsMemory.h"
int scenario=0;
bool held=false,closed=false;
const char* root_ca="CA";
TlsMemoryStats stats={true,0,0,0,0,0,0};
TlsMemoryStats tlsMemorySnapshot() { return stats; }
struct TlsGateGuard {
 TlsGateGuard() { assert(!held);held=true; }
 ~TlsGateGuard() { held=false; }
};
struct CheckedTlsClient {
 void setCACert(const char* ca) { assert(ca==root_ca); }
 void setHandshakeTimeout(unsigned n) { assert(n==10); }
 bool connect(const char* host,unsigned port,int timeout) {
  assert(held && !strcmp(host,"api.tts.quest") && port==443 && timeout==5000);
  stats.externalAllocations+=2;
  if(scenario==2)++stats.failures;
  return scenario==0;
 }
 int lastError(char*,size_t) { return scenario==0 || scenario==3 ? 0 : -9984; }
 uint32_t verificationFlags() { return scenario==0?0:scenario==3?0xFFFFFFFFu:8; }
 bool allocationFailureInHandshake() { return scenario==2; }
 void stop() { closed=true; }
};
'''+declaration+probe+r'''
int main() {
 for(scenario=0;scenario<4;++scenario) {
  closed=false;
  auto result=voicevox_probe_tls();
  assert(closed && !held && result.externalAllocations==2);
  assert(result.connected==(scenario==0));
  assert(result.error==(scenario==0?0:scenario==3?-1:-9984));
  assert(result.allocationFailure==(scenario==2));
  assert(result.failedAllocations==(scenario==2?1:0));
 }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=pathlib.Path(tmp)
            (path/'probe.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-I',str(SRC),str(path/'probe.cpp'),'-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True,timeout=10)


if __name__ == '__main__': unittest.main()
