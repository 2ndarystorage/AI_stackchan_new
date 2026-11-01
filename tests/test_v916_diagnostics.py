"""Native tests for production I2S handover, badge, MP3 logging and TLS diagnostics."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / 'M5Unified_AI_StackChan/src'

PRINT = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
class Print {
 public:
  virtual size_t write(uint8_t)=0;
  size_t write(const uint8_t* p,size_t n) { for(size_t i=0;i<n;i++) write(p[i]); return n; }
  size_t print(const char* p) { return write((const uint8_t*)p,strlen(p)); }
  void println(const char* p) { print(p); write('\n'); }
};
'''


class DiagnosticsTests(unittest.TestCase):
    def compile_run(self, code, stubs=None):
        with tempfile.TemporaryDirectory() as tmp:
            folder = pathlib.Path(tmp)
            for name, text in (stubs or {}).items():
                p = folder/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
            source = folder/'check.cpp'
            source.write_text(code)
            subprocess.run(['c++', '-std=c++11', '-pthread', '-I', tmp, '-I', str(SRC),
                            str(source), '-o', str(folder/'check')], check=True)
            subprocess.run([str(folder/'check')], check=True, timeout=10)

    def test_i2s_driver_is_released_once_before_microphone_begin(self):
        self.compile_run(r'''
#include <cassert>
#include "AudioHandover.h"
bool installed=false;
unsigned releases=0;
struct Config { int i2s_port=0; };
struct Speaker {
  bool running=false;
  bool isRunning() { return running; }
  Config config() { return {}; }
  void end() { running=false; } // production 0.1.16 leaves installed=true
  void begin() { assert(!installed); installed=true; running=true; }
} speaker;
struct Mic {
  bool running=false;
  bool begin() { if(running) return true; assert(!installed); installed=true; running=true; return true; }
  void end() { if(running) installed=false; running=false; }
} mic;
void release(int port) { assert(port==0 && installed && !speaker.running); ++releases; installed=false; }
int main() {
  assert(handoverToMicrophone(speaker,mic,release));
  assert(handoverToMicrophone(speaker,mic,release)); assert(releases==0);
  for(int i=0;i<100;++i) {
    mic.end(); speaker.begin();
    assert(handoverToMicrophone(speaker,mic,release));
  }
  assert(releases==100 && mic.running);
}
''')

    def test_badge_is_top_left_and_restores_text_state(self):
        self.compile_run(r'''
#include <cassert>
#include <cstring>
namespace lgfx { enum class textdatum_t { top_left, other }; }
namespace fonts { const int Font0=0; }
#include "VersionBadge.h"
struct Style { lgfx::textdatum_t datum=lgfx::textdatum_t::other; int size=3, color=7; };
struct Display {
  Style style; int otherFont=9; const int* font=&otherFont; bool drew=false;
  Style getTextStyle() { return style; }
  const int* getFont() { return font; }
  void setFont(const int* f) { font=f; }
  void setTextStyle(Style s) { style=s; }
  void fillRoundRect(int x,int y,int w,int h,int,int) { assert(x==2&&y==2&&w==52&&h==16); }
  void setTextDatum(lgfx::textdatum_t d) { style.datum=d; }
  void setTextSize(int n) { style.size=n; }
  void setTextColor(int c,int) { style.color=c; }
  void drawString(const char* label,int x,int y,const int* f) {
    assert(!strcmp(label,"v9.16") && x==6 && y==4 && f==&fonts::Font0);
    assert(style.datum==lgfx::textdatum_t::top_left && style.size==1); font=f; drew=true;
  }
};
int main() {
  Display display; drawFirmwareVersionBadge(display);
  assert(display.drew && display.font==&display.otherFont);
  assert(display.style.size==3 && display.style.color==7 && display.style.datum==lgfx::textdatum_t::other);
}
''')
        main = (SRC/'main.cpp').read_text()
        avatar = (ROOT/'M5Unified_AI_StackChan/lib/M5Stack-Avatar/src/Avatar.cpp').read_text()
        self.assertIn('::stackchan_draw_version_badge();', avatar)
        self.assertGreater(avatar.index('::stackchan_draw_version_badge();'), avatar.index('face->draw(ctx);'))
        self.assertIn('drawFirmwareVersionBadge(M5.Lcd);\n    consec_errors', main)

    def test_only_first_buflen_at_source_eof_is_reclassified(self):
        self.compile_run(r'''
#include <cassert>
#include "Mp3DiagnosticLogger.h"
bool eof=false; bool ended() { return eof; }
struct Output : Print { std::string text; size_t write(uint8_t ch) override { text+=char(ch); return 1; } } output;
int main() {
  Mp3DiagnosticLogger logger(output,ended);
  logger.print("MP3:ERROR_BUFLEN 0\n"); assert(output.text=="MP3:ERROR_BUFLEN 0\n"); output.text.clear();
  eof=true; logger.print("MP3:ERROR_BUFLEN 0\n");
  assert(output.text=="[MP3] buffer refill at source EOF (decoder continues)\n"); output.text.clear();
  logger.print("MP3:ERROR_BUFLEN 1\n"); assert(output.text=="MP3:ERROR_BUFLEN 1\n"); output.text.clear();
  logger.print("some other failure\n"); assert(output.text=="some other failure\n"); output.text.clear();
  std::string longline(200,'x'); longline+="MP3:ERROR_BUFLEN 0\n";
  logger.print(longline.c_str()); assert(output.text==longline);
}
''', {'Arduino.h': PRINT})

    def test_tls_flags_are_captured_before_stop_and_verification_is_required(self):
        tls_stub = r'''
#pragma once
#include <cstdint>
#include <string>
#include <cassert>
struct Context {};
struct sslclient_context { Context ssl_ctx; };
struct Time { int year=2026,mon=11,day=1; };
struct mbedtls_x509_crt { int issuer; Time valid_from,valid_to; };
extern bool stopped, dnsFail, insecureUsed;
extern int outcome;
struct IPAddress { std::string toString() { return "192.0.2.10"; } };
struct Wifi { bool hostByName(const char*,IPAddress&) { return !dnsFail; } };
extern Wifi WiFi;
struct SerialStub { template<class... T> void printf(T...) {} void print(const char*) {} };
extern SerialStub Serial;
int start_ssl_client(sslclient_context*,IPAddress,uint16_t,const char*,int,const char*,bool,const char*,const char*,const char*,const char*,bool,const char**);
uint32_t mbedtls_ssl_get_verify_result(Context*);
const mbedtls_x509_crt* mbedtls_ssl_get_peer_cert(Context*);
void mbedtls_x509_crt_verify_info(char*,size_t,const char*,uint32_t);
void mbedtls_x509_dn_gets(char*,size_t,const int*);
class WiFiClientSecure {
 protected:
  sslclient_context context; sslclient_context* sslclient=&context;
  int _lastError=0,_timeout=0; const char *_CA_cert="CA",*_cert=nullptr,*_private_key=nullptr;
  const char** _alpn_protos=nullptr; bool _connected=false;
 public:
  virtual int connect(const char*,uint16_t) { return 0; }
  virtual int connect(const char*,uint16_t,int32_t) { return 0; }
  void stop() { stopped=true; }
};
'''
        self.compile_run(r'''
#include "CheckedTlsClient.h"
bool stopped=false,dnsFail=false,insecureUsed=false;
int outcome=-9984;
Wifi WiFi; SerialStub Serial;
int start_ssl_client(sslclient_context*,IPAddress,uint16_t,const char* host,int timeout,
  const char* ca,bool bundle,const char*,const char*,const char*,const char*,bool insecure,const char**) {
  assert(std::string(host)=="api.tts.quest" && timeout==25000 && ca && !bundle && !insecure);
  insecureUsed=insecure; return outcome;
}
uint32_t mbedtls_ssl_get_verify_result(Context*) { assert(!stopped); return 8; }
const mbedtls_x509_crt* mbedtls_ssl_get_peer_cert(Context*) { assert(!stopped); return nullptr; }
void mbedtls_x509_crt_verify_info(char*,size_t,const char*,uint32_t flags) { assert(flags==8 && !stopped); }
void mbedtls_x509_dn_gets(char*,size_t,const int*) {}
int main() {
 CheckedTlsClient client;
 assert(!client.connect("api.tts.quest",443,25000) && stopped && !insecureUsed);
 stopped=false; outcome=1;
 assert(client.connect("api.tts.quest",443,25000)==1 && !stopped);
 dnsFail=true; assert(!client.connect("api.tts.quest",443,25000));
}
''', {'WiFiClientSecure.h': tls_stub, 'mbedtls/x509_crt.h': '#pragma once\n'})


if __name__ == '__main__':
    unittest.main()
