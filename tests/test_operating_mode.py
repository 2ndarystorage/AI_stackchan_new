"""Offline execution of v9.24 mode policy and actual boot/HTTP control bodies."""
import pathlib
import subprocess
import tempfile
import unittest
from test_tts_transport import body

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / 'M5Unified_AI_StackChan/src'
MAIN = (SRC / 'main.cpp').read_text()

NVS = r'''
#pragma once
#include <cstdint>
#include <cstring>
#include <cassert>
using esp_err_t=int;
using nvs_handle_t=unsigned;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
int openError=0,getError=0,setError=0,commitError=0;
int commits=0,writes=0,closes=0;
bool exists=false;
uint8_t value=0;
int nvs_open(const char* name,int access,nvs_handle_t* h) {
 assert(std::strcmp(name,"runmode")==0); *h=7;
 if(openError) return openError;
 return access==NVS_READONLY && !exists ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
int nvs_get_u8(nvs_handle_t,const char* key,uint8_t* v) {
 assert(std::strcmp(key,"next")==0); if(getError) return getError; *v=value; return 0;
}
int nvs_set_u8(nvs_handle_t,const char* key,uint8_t v) {
 assert(std::strcmp(key,"next")==0); ++writes; if(setError) return setError;
 exists=true;value=v;return 0;
}
int nvs_commit(nvs_handle_t) { ++commits;return commitError; }
void nvs_close(nvs_handle_t) { ++closes; }
'''


class OperatingModeTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp)
            (path / 'nvs.h').write_text(NVS)
            (path / 'check.cpp').write_text(code)
            subprocess.run(['c++', '-std=c++11', '-pthread', '-I', temp,
                            '-I', str(SRC), str(path / 'check.cpp'),
                            '-o', str(path / 'check')], check=True)
            subprocess.run([str(path / 'check')], check=True, timeout=10)

    def test_one_shot_selection_preserves_other_namespaces_and_fails_closed(self):
        self.compile_run(r'''
#include "OperatingMode.h"
int main() {
 esp_err_t err=0;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && !err && !writes);
 assert(saveNextOperatingMode(OperatingMode::BleTransfer)==0 && value==1 && commits==1);
 assert(consumeNextOperatingMode(err)==OperatingMode::BleTransfer && value==0 && commits==2);
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && commits==2);
 value=99; assert(consumeNextOperatingMode(err)==OperatingMode::Normal && value==0);
 value=1; commitError=8;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && err==8);
 commitError=0; value=1; setError=9;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && err==9);
 int before=commits; assert(saveNextOperatingMode(OperatingMode::BleTransfer)==9 && commits==before);
 setError=0; getError=10;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && err==10);
 getError=ESP_ERR_NVS_NOT_FOUND;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && err==0);
 getError=0; openError=11;
 assert(consumeNextOperatingMode(err)==OperatingMode::Normal && err==11);
 assert(saveNextOperatingMode(OperatingMode::BleTransfer)==11);
}
''')

    def test_busy_policy_and_wrap_safe_deadlines(self):
        self.compile_run(r'''
#include "OperatingMode.h"
int main() {
 for(int i=0;i<8;++i) assert(canEnterBleMode(i&1,i&2,i&4)==(i==0));
 assert(!modeDeadlineReached(10,11)); assert(modeDeadlineReached(11,11));
 assert(modeDeadlineReached(12,11));
 uint32_t start=0xfffffff0U,deadline=start+15000;
 assert(!modeDeadlineReached(start,deadline));
 assert(!modeDeadlineReached(deadline-1,deadline));
 assert(modeDeadlineReached(deadline,deadline));
}
''')

    def test_http_switch_requires_intent_idle_and_persistent_save(self):
        self.compile_run(r'''
#include "OperatingMode.h"
#include <string>
uint32_t now=100; uint32_t millis(){return now;}
bool audio=false,tts=false,g_mode_switch_pending=false;
uint32_t g_mode_restart_at=0;
std::string speech_text,speech_text_buffer;
bool audioRunning(){return audio;} bool tts_processing(){return tts;}
struct { template<class... A> void printf(const char*,A...){} } Serial;
struct {
 std::string intent; int code=0;
 std::string header(const char* name){assert(std::string(name)=="X-Stackchan-Mode");return intent;}
 void send(int c,const char*,const char*){code=c;}
} server;
''' + body(MAIN, 'static void handle_ble()') + r'''
int main() {
 handle_ble();assert(server.code==403 && writes==0 && !g_mode_switch_pending);
 server.intent="ble";
 audio=true;handle_ble();assert(server.code==409 && writes==0);
 audio=false;tts=true;handle_ble();assert(server.code==409 && writes==0);
 tts=false;speech_text="news";handle_ble();assert(server.code==409 && writes==0);
 speech_text="";speech_text_buffer="pending";handle_ble();assert(server.code==409 && writes==0);
 speech_text_buffer="";commitError=8;handle_ble();
 assert(server.code==500 && !g_mode_switch_pending);
 commitError=0;handle_ble();assert(server.code==202 && g_mode_switch_pending && value==1);
 assert(g_mode_restart_at==850);int count=writes;handle_ble();
 assert(server.code==409 && writes==count);
}
''')

    def test_boot_failure_timeout_ready_and_button_return_to_normal(self):
        self.compile_run(r'''
#include "OperatingMode.h"
#include <atomic>
#include <string>
uint32_t now=0;uint32_t millis(){return now;}
void delay(unsigned n){now+=n;}
std::atomic<int> g_ble_boot_result{0};uint32_t g_ble_boot_deadline=15000;
int restarted=0,ticks=0,screens=0,updates=0;
struct { bool pressed=false; bool wasPressed(){bool p=pressed;pressed=false;return p;} } button;
struct { decltype(button)& BtnC=button; void update(){++updates;} } M5;
void ble_digest_stop(const char*){++restarted;}
void ble_mode_screen(const char*){++screens;}
void ble_digest_tick(){++ticks;}
''' + body(MAIN, 'static void ble_mode_loop()') + r'''
int main() {
 ble_mode_loop();assert(!restarted && !ticks && now==10);
 now=14999;ble_mode_loop();assert(!restarted && !ticks);
 now=15000;ble_mode_loop();assert(restarted==1 && !ticks);
 g_ble_boot_result=-1;now=0;ble_mode_loop();assert(restarted==2 && !ticks);
 g_ble_boot_result=1;ble_mode_loop();assert(ticks==1 && restarted==2);
 int oldScreens=screens;ble_mode_loop();assert(ticks==2 && screens==oldScreens);
 button.pressed=true;ble_mode_loop();assert(restarted==3 && ticks==2);
 assert(updates==7);
}
''')

    def test_ble_initializer_checks_failures_without_normal_subsystems(self):
        self.compile_run(r'''
#include <atomic>
#include <string>
#include <cassert>
#include <cstdint>
constexpr int ESP_OK=0,MALLOC_CAP_INTERNAL=1,DG_WAIT=1;
constexpr uint8_t FXN_BLE=10;
void netActivityBeginTag(uint8_t) {}
constexpr uint32_t BLE_DG_WINDOW_MS=180000;
const char *BLE_DG_DEVICE_NAME="test",*BLE_DG_SERVICE_UUID="svc",*BLE_DG_CTRL_UUID="ctrl",
 *BLE_DG_DATA_UUID="data",*BLE_DG_STATUS_UUID="status";
int nvsError=0,fail=0,initCalls=0,deleted=0,chars=0;
int nvs_flash_init(){return nvsError;}
void vTaskDelete(void* p){assert(!p);++deleted;}
uint32_t millis(){return 200;}
unsigned heap_caps_get_largest_free_block(int){return 90000;}
struct {unsigned getFreeHeap(){return 120000;} } ESP;
struct {template<class... A> void printf(const char*,A...){} } Serial;
std::atomic<int> g_ble_boot_result{0};
uint32_t g_ble_dg_deadline=0;int g_ble_dg_state=0;
int g_ble_dg_server_cb,g_ble_dg_ctrl_cb,g_ble_dg_data_cb;
namespace NIMBLE_PROPERTY { constexpr int WRITE=1,READ=2,NOTIFY=4; }
struct NimBLECharacteristic {
 void setCallbacks(int*){} void setValue(const std::string&){}
};
NimBLECharacteristic characteristic,*g_ble_dg_status_chr=nullptr;
struct NimBLEService {
 NimBLECharacteristic* createCharacteristic(const char*,int){return fail==3 && ++chars==1 ? nullptr : &characteristic;}
 bool start(){return fail!=4;}
} service;
struct NimBLEServer {
 void setCallbacks(int*){}
 NimBLEService* createService(const char*){return fail==2 ? nullptr : &service;}
} bleServer;
struct NimBLEAdvertising {
 void addServiceUUID(const char*){} void setScanResponse(bool){}
 bool start(){return fail!=6;}
} advertising;
struct NimBLEDevice {
 static void init(const char*){++initCalls;} static void setMTU(int){}
 static NimBLEServer* createServer(){return fail==1 ? nullptr : &bleServer;}
 static NimBLEAdvertising* getAdvertising(){return fail==5 ? nullptr : &advertising;}
};
''' + body(MAIN, 'static void ble_boot_task(void *)') + r'''
int main() {
 nvsError=8;ble_boot_task(nullptr);assert(g_ble_boot_result==-1 && !initCalls && deleted==1);
 nvsError=0;
 for(fail=1;fail<=6;++fail){
   chars=0;g_ble_boot_result=0;ble_boot_task(nullptr);assert(g_ble_boot_result==-1);
 }
 fail=0;g_ble_boot_result=0;ble_boot_task(nullptr);
 assert(g_ble_boot_result==1 && deleted==8 && initCalls==7);
 assert(g_ble_dg_state==DG_WAIT && g_ble_dg_deadline==180200);
}
''')

    def test_early_boot_and_loop_isolation_and_non_destructive_storage(self):
        setup = body(MAIN, 'void setup()')
        ble = setup[setup.index('if (g_operating_mode'):setup.index('// Memory release')]
        self.assertIn('ble_mode_setup();\n    return;', ble)
        self.assertIn('ble_cfg.internal_mic = false', ble)
        self.assertIn('ble_cfg.internal_spk = false', ble)
        for forbidden in ('WiFi.begin(', 'installTlsMemoryPolicy(', 'wakeword_init(',
                          'ttsSynthTask,', 'avatar.init(', 'init_schedule('):
            self.assertNotIn(forbidden, ble)
            self.assertGreater(setup.index(forbidden), setup.index('esp_bt_mem_release('))
        loop = body(MAIN, 'void loop()')
        self.assertLess(loop.index('ble_mode_loop(); return;'), loop.index('wifi_watchdog();'))
        self.assertLess(loop.index('g_mode_switch_pending'), loop.index('wifi_watchdog();'))
        tick = body(MAIN, 'static void ble_digest_tick()')
        self.assertNotIn('avatar.', tick)
        self.assertNotIn('sw_tone(', tick)
        self.assertNotIn('NimBLEDevice::deinit', MAIN)
        self.assertIn('SPIFFS.begin(false)', body(MAIN, 'static fs::FS *digest_open('))
        self.assertNotIn('nvs_flash_erase', body(MAIN, 'static void ble_boot_task(void *)'))
        self.assertIn('server.on("/ble", HTTP_GET, handle_mode)', MAIN)
        self.assertIn('server.on("/ble", HTTP_POST, handle_ble)', MAIN)
        self.assertIn('if (g_mode_switch_pending) return;', loop[loop.index('server.handleClient();'):])

    def test_commit_timeout_and_completion_reboot_without_audio(self):
        self.compile_run(r'''
#include "OperatingMode.h"
#include <atomic>
#include <string>
using String=std::string;
enum { DG_OFF,DG_WAIT,DG_RECV,DG_COMMIT,DG_DONE,DG_ERROR };
std::atomic<uint8_t> g_ble_dg_state{DG_OFF};
std::atomic<uint32_t> g_ble_dg_received{0},g_ble_dg_expect{0};
uint8_t buffer[4]={};uint8_t* g_ble_dg_buf=buffer;
uint32_t now=100,g_ble_dg_deadline=200,g_ble_dg_off_at=0;
constexpr uint32_t BLE_DG_HOLD_MS=15000;
int g_ble_dg_mutex=1,portMAX_DELAY=1;
bool saveOk=true,raceCommit=false;int saves=0,restarts=0;
String status,screen;
uint32_t millis(){return now;}
void xSemaphoreTake(int,int){if(raceCommit){g_ble_dg_state=DG_COMMIT;raceCommit=false;}}
void xSemaphoreGive(int){}
bool digest_store(uint8_t*,uint32_t,String& name){++saves;name="dg_0001.txt";return saveOk;}
void ble_dg_set_status(const String& s){status=s;}
void ble_mode_screen(const char* s){screen=s;}
void ble_digest_stop(const char*){++restarts;}
struct {template<class... A> void printf(const char*,A...){} } Serial;
''' + body(MAIN, 'static void ble_digest_tick()') + r'''
int main() {
 g_ble_dg_state=DG_COMMIT;g_ble_dg_received=3;g_ble_dg_expect=3;
 ble_digest_tick();assert(g_ble_dg_state==DG_DONE && saves==1 && !restarts);
 assert(status=="DONE dg_0001.txt" && g_ble_dg_off_at==15100);
 now=15099;ble_digest_tick();assert(!restarts);
 now=15100;ble_digest_tick();assert(restarts==1);
 g_ble_dg_state=DG_COMMIT;g_ble_dg_received=2;ble_digest_tick();
 assert(g_ble_dg_state==DG_ERROR && saves==1 && status=="ERR store");
 g_ble_dg_state=DG_COMMIT;g_ble_dg_received=3;saveOk=false;ble_digest_tick();
 assert(g_ble_dg_state==DG_ERROR && saves==2);
 now=199;g_ble_dg_state=DG_RECV;ble_digest_tick();assert(g_ble_dg_state==DG_RECV);
 now=200;ble_digest_tick();assert(g_ble_dg_state==DG_ERROR && status=="ERR timeout");
 now=3199;ble_digest_tick();assert(restarts==1);
 now=3200;ble_digest_tick();assert(restarts==2);
 // END winning the race with the timeout must still get its commit attempt.
 g_ble_dg_state=DG_RECV;raceCommit=true;ble_digest_tick();
 assert(g_ble_dg_state==DG_COMMIT && saves==2);
 ble_digest_tick();assert(saves==3);
}
''')


if __name__ == '__main__':
    unittest.main()
