"""Exercise the actual vendored draw/setExpression bodies against a competing dance task."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'M5Unified_AI_StackChan'

class GraphicsLockTests(unittest.TestCase):
    def test_build_uses_only_patched_avatar(self):
        ini = (PROJECT / 'platformio.ini').read_text()
        self.assertEqual(ini.count('symlink://lib/M5Stack-Avatar'), 2)
        self.assertNotIn('meganetaaan/M5Stack-Avatar@', ini)
        main = (PROJECT / 'src/main.cpp').read_text()
        self.assertIn('#error "Use the vendored M5Stack-Avatar', main)
        self.assertIn('stackchan_avatar_gfx_lock_revision()', main)
        guard = main[main.index('#if !defined(STACKCHAN_AVATAR_GFX_LOCKED)'):]
        guard = guard[:guard.index('#endif')+len('#endif')]
        for define, expected in [('', False), ('#define STACKCHAN_AVATAR_GFX_LOCKED 0x0912\n', False),
                                 ('#define STACKCHAN_AVATAR_GFX_LOCKED 0x0913\n', True)]:
            check = subprocess.run(['c++','-E','-x','c++','-'],input=define+guard,
                                   text=True,capture_output=True)
            self.assertEqual(check.returncode == 0, expected)

    def test_avatar_and_dance_use_same_lock(self):
        avatar = (PROJECT / 'lib/M5Stack-Avatar/src/Avatar.cpp').read_text()
        draw = avatar[avatar.index('void Avatar::draw()'):avatar.index('bool Avatar::isDrawing()')]
        expression = avatar[avatar.index('void Avatar::setExpression('):avatar.index('Expression Avatar::getExpression()')]
        prefix = r'''
#include <atomic>
#include <cassert>
#include <thread>
#include "GfxLock.h"
std::recursive_mutex graphicsMutex;
SemaphoreHandle_t g_gfx_mutex = &graphicsMutex;
std::atomic<int> active{0};
struct Gaze { Gaze(float,float) {} };
struct DrawContext { template<class... T> DrawContext(T...) {} };
struct Face {
  void draw(DrawContext*) {
    assert(active.fetch_add(1)==0);
    std::this_thread::yield();
    assert(active.fetch_sub(1)==1);
  }
};
using Expression = int;
class Avatar {
 public:
  float gazeV=0,gazeH=0,breath=0,palette=0,eyeOpenRatio=0,mouthOpenRatio=0;
  float speechText=0,rotation=0,scale=0,colorDepth=0,batteryIconStatus=0,batteryLevel=0,speechFont=0;
  int expression=0;
  Face storage; Face* face=&storage;
  void draw(); void setExpression(Expression);
};
'''
        checks = r'''
int main() {
  Avatar avatar;
  std::thread face([&] { for(int i=0;i<10000;++i) avatar.draw(); });
  std::thread dance([&] {
    for(int i=0;i<10000;++i) {
      GfxLockGuard guard;
      assert(active.fetch_add(1)==0);
      avatar.setExpression(i); // recursive acquisition must not freeze the owner.
      std::this_thread::yield();
      assert(active.fetch_sub(1)==1);
    }
  });
  face.join(); dance.join();
  assert(active==0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=pathlib.Path(tmp)/'graphics.cpp'; exe=pathlib.Path(tmp)/'graphics'
            src.write_text(prefix+draw+expression+checks)
            subprocess.run(['c++','-std=c++11','-pthread','-I',str(ROOT/'tests/native'),
                            '-I',str(PROJECT/'src'),str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=15)

if __name__ == '__main__': unittest.main()
