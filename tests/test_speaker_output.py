"""Output initialization, gain and no-API test endpoint regression checks."""
import pathlib
import subprocess
import tempfile
import unittest
from test_tts_transport import body

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT/'M5Unified_AI_StackChan/src'


class SpeakerOutputTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            (p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-I',str(SRC),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=10)

    def test_volume_is_applied_once_and_mute_is_preserved(self):
        self.compile_run(r'''
#include <cassert>
#include "SpeakerVolume.h"
struct Speaker {
 unsigned master=0,channels[8]={};
 void setVolume(uint8_t v) { master=v; }
 void setChannelVolume(uint8_t ch,uint8_t v) { channels[ch]=v; }
};
int main() {
 Speaker speaker;
 for(unsigned v=0;v<256;++v) {
   setSpeechVolume(speaker,v);
   assert(speaker.master==v && speaker.channels[0]==255 && speaker.channels[7]==255);
   assert(volumeToLevel(v)<=5);
   assert((volumeToLevel(v)==0)==(v==0));
 }
 setSpeechVolume(speaker,51);
 const double oldGain=.2*.2*.2*.2;
 const double newGain=.2*.2;
 assert(newGain/oldGain>24.99 && newGain/oldGain<25.01);
 setSpeechVolume(speaker,0); assert(speaker.master==0);
}
''')

    def test_audio_test_requires_deliberate_request_and_restores_mute(self):
        main = (SRC/'main.cpp').read_text()
        function = body(main,'static void handle_audio_test() {')
        self.compile_run(r'''
#include <cassert>
#include <string>
#include "SpeakerVolume.h"
using String=std::string;
constexpr int HTTP_POST=2;
struct Server {
 int verb=HTTP_POST,code=0;
 String key="1",response;
 int method() { return verb; }
 String header(const char*) { return key; }
 void send(int c,const char*,const char* text) { code=c; response=text; }
} server;
bool busy=false;
bool tts_processing() { return busy; }
bool audioRunning() { return false; }
String speech_text,speech_text_buffer;
struct Mic { bool running=true; void end() { running=false; } };
struct Speaker {
 uint8_t master=0,channels[8]={};
 bool running=false,canStart=true;
 int tones=0;
 uint8_t getVolume() { return master; }
 uint8_t getChannelVolume(int c) { return channels[c]; }
 void setVolume(uint8_t v) { master=v; }
 void setChannelVolume(int c,uint8_t v) { channels[c]=v; }
 bool begin() { running=canStart; return running; }
 bool isRunning() { return running; }
 bool tone(int,int,int ch,bool) { assert(master==102 && channels[ch]==255); ++tones; return true; }
};
struct Device { struct Speaker Speaker; struct Mic Mic; } M5;
bool speakerBackendReady(const Speaker& speaker) { return speaker.running; }
void audioBeginMicrophone() { M5.Speaker.running=false; M5.Mic.running=true; }
void delay(unsigned) {}
struct Logger { template<class... T> void printf(T...) {} } Serial;
'''+function+r'''
int main() {
 M5.Speaker.channels[7]=77;
 server.key=""; handle_audio_test(); assert(server.code==403 && M5.Speaker.tones==0);
 server.key="1"; busy=true; handle_audio_test(); assert(server.code==409 && M5.Mic.running);
 busy=false; handle_audio_test();
 assert(server.code==200 && M5.Speaker.tones==2 && M5.Speaker.master==0);
 assert(M5.Speaker.channels[7]==77 && M5.Mic.running && !M5.Speaker.running);
 M5.Speaker.canStart=false; handle_audio_test();
 assert(server.code==500 && M5.Speaker.tones==2 && M5.Speaker.master==0 && M5.Mic.running);
}
''')
        self.assertNotIn('voicevox_',function)
        self.assertNotIn('exec_news(',function)

    def test_output_regression_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp)/'audio'
            subprocess.run(['c++','-std=c++11','-I',str(ROOT/'tests/native'),
                            str(ROOT/'tests/native/audio_regression.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
        main = (SRC/'main.cpp').read_text()
        self.assertIn('if (!decoding || out.hasFault())',main)
        self.assertIn('reason = out.hasFault() ? "output-error"',main)


if __name__ == '__main__': unittest.main()
