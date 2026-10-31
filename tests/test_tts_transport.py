"""Compile the production gate, wait, encoding and worker bodies with offline fakes."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'M5Unified_AI_StackChan'
SRC = PROJECT / 'src'


def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


class TtsTransportTests(unittest.TestCase):
    def compile_run(self, code, json=False):
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / 'transport.cpp'
            exe = pathlib.Path(tmp) / 'transport'
            source.write_text(code)
            cmd = ['c++', '-std=c++11', '-pthread', '-I', str(ROOT/'tests/native'),
                   '-I', str(SRC)]
            if json:
                cmd += ['-I', str(PROJECT/'.pio/libdeps/m5stack-core2/ArduinoJson/src')]
            subprocess.run(cmd + [str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)

    def test_recursive_tls_gate_serializes_competing_tasks(self):
        self.compile_run(r'''
#include <atomic>
#include <thread>
#include <cassert>
#include "TlsGate.h"
std::recursive_mutex mutex;
SemaphoreHandle_t g_tls_mutex = &mutex;
std::atomic<int> active{0};
void request() {
  for(int i=0;i<10000;++i) {
    TlsGateGuard gate;
    TlsGateGuard recursive;
    assert(active.fetch_add(1)==0);
    std::this_thread::yield();
    assert(active.fetch_sub(1)==1);
  }
}
int main() { std::thread a(request), b(request); a.join(); b.join(); assert(active==0); }
''')

    def test_completed_audio_polling_cancel_timeout_and_utf8(self):
        tts = (SRC/'WebVoiceVoxTTS.cpp').read_text()
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <ArduinoJson.h>
using String=std::string;
static std::atomic<bool> tts_cancel_requested{false};
uint32_t now=0, cancelAt=~0u;
uint32_t millis() { return now; }
void delay(uint32_t ms) { now+=ms; if(now>=cancelAt) tts_cancel_requested=true; }
struct SerialStub { void println(const char*) {} } Serial;
const int HTTP_CODE_OK=200;
String tts_status_url="https://audio.example/status";
std::vector<String> responses;
unsigned cursor=0, opened=0, live=0;
bool beginFails=false;
struct Client {} client, *tts_client=&client;
struct Http {
  bool begin(Client&,const String&) { return !beginFails; }
  int GET() { return 200; }
  String getString() { return responses.at(cursor++ % responses.size()); }
} http, *tts_https=&http;
bool tts_ensure_connection() { assert(live==0); ++live; ++opened; return true; }
void tts_forget_connection() { live=0; }
void tts_log_request(const char*,const String&) {}
void tts_log_tls_error(const char*) {}
'''
        code += body(tts, 'static bool tts_wait(uint32_t ms)')
        code += body(tts, 'static bool tts_wait_until_ready()')
        code += body(tts, 'static String URLEncode(const char* msg) {')
        code += r'''
void reset() { now=cursor=opened=live=0; cancelAt=~0u; tts_cancel_requested=false; beginFails=false; }
int main() {
  responses={R"({"isAudioReady":false})",R"({"isAudioReady":true})"};
  assert(tts_wait_until_ready()); assert(opened==2 && live==0 && now==2000);
  reset(); responses={R"({"isAudioError":true})"}; assert(!tts_wait_until_ready()); assert(live==0);
  reset(); responses={"bad JSON"}; assert(!tts_wait_until_ready()); assert(live==0);
  reset(); responses={R"({"isAudioReady":false})"}; cancelAt=80;
  assert(!tts_wait_until_ready()); assert(now==80 && opened==1 && live==0);
  reset(); assert(!tts_wait_until_ready()); assert(now==90000 && live==0);
  reset(); beginFails=true; assert(!tts_wait_until_ready()); assert(live==0);
  reset(); tts_cancel_requested=true; assert(!tts_wait_until_ready()); assert(opened==0);
  reset(); tts_status_url=""; assert(tts_wait_until_ready()); assert(opened==0);
  assert(URLEncode("hello world&") == "hello%20world%26");
  assert(URLEncode("\xe3\x81\x82") == "%E3%81%82");
}
'''
        self.compile_run(code, json=True)

    def test_worker_queues_only_valid_current_generation(self):
        main = (SRC/'main.cpp').read_text()
        worker = body(main, 'void ttsSynthTask(void *args)\n{')
        prefix = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include "TlsGate.h"
using String=std::string;
std::recursive_mutex mutex, queueMutex;
SemaphoreHandle_t g_tls_mutex=&mutex, g_tts_mutex=&queueMutex;
int xSemaphoreTake(SemaphoreHandle_t m,unsigned) { m->lock(); return 1; }
void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }
struct TtsChunk { String text,parms; };
struct TtsReadyItem { uint8_t* data; size_t len; };
std::deque<TtsChunk> g_tts_queue;
std::deque<TtsReadyItem> g_tts_ready_q;
const unsigned TTS_READY_MAX=3;
bool g_tts_synth_busy=false, g_tts_release_req=false;
uint32_t g_tts_gen=0;
unsigned g_tts_failed_chunks=0, freed=0, released=0;
int scenario=0;
uint8_t data[8];
struct SerialStub { template<class... T> void printf(T...) {} void println(const char*) {} } Serial;
const int WL_CONNECTED=1;
struct WifiStub { int status() { return WL_CONNECTED; } } WiFi;
void voicevox_reset_cancel() {}
String voicevox_synth_url(const char*, const char*) { assert(g_tts_synth_busy); return "url"; }
int voicevox_download_mp3(const String&,uint8_t** out,size_t* len) {
  assert(g_tts_synth_busy);
  if(scenario==1) return -1;
  *out=data; *len=sizeof(data);
  if(scenario==2) { ++g_tts_gen; assert(g_tts_synth_busy); }
  return 0;
}
void voicevox_release_connection() { ++released; }
void heap_caps_free(void* p) { assert(p==data); ++freed; }
struct Done {};
void delay(unsigned) { throw Done{}; }
'''
        checks = r'''
int main() {
  for(scenario=0;scenario<3;++scenario) {
    g_tts_queue.push_back({"text","parms"});
    try { ttsSynthTask(nullptr); } catch(Done&) {}
    assert(!g_tts_synth_busy && g_tts_queue.empty());
    if(scenario==0) { assert(g_tts_ready_q.size()==1); assert(g_tts_ready_q.front().data==data); g_tts_ready_q.clear(); }
    else assert(g_tts_ready_q.empty());
  }
  assert(g_tts_failed_chunks==1 && freed==1 && released==3);
}
'''
        self.compile_run(prefix + worker + checks)
        stop = body(main, 'static void stop_news_speech() {')
        self.assertNotIn('g_tts_synth_busy = false', stop)
        self.assertIn('voicevox_cancel();', stop)
        self.assertNotIn('voicevox_play_url(', main)
        tts = (SRC/'WebVoiceVoxTTS.cpp').read_text()
        self.assertIn('tts_https->setReuse(false)', tts)
        self.assertNotIn('setInsecure(', tts)


if __name__ == '__main__':
    unittest.main()
