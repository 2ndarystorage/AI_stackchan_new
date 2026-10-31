# Vendored library note (第9.2弾・第9.3弾)

This is a vendored copy of meganetaaan/M5Stack-Avatar 0.9.2 (the same version
that PlatformIO previously fetched into `.pio/libdeps`). Placing it under the
project `lib/` folder makes PlatformIO prefer it over the registry copy.

## Changes from upstream

1. (第9.2弾) `src/Avatar.cpp`, `Avatar::start()`: the `drawLoop` task stack
   size was raised from **2048 to 8192**. On ESP-IDF the stack-size argument
   of `xTaskCreateUniversal` is in **bytes** (despite the upstream comment
   saying "words"), so the stock draw task had only 2048 bytes of stack.
   With the photo-face (KinakoFace) draw path this overflowed and corrupted
   the stack canary, causing `Stack canary watchpoint triggered (drawLoop)`
   panics on the real device. The library exposes no API to change this
   value, hence the vendored copy.

2. (第9.3弾) `src/Avatar.cpp`: `#include "../../../src/GfxLock.h"` was added
   and `Avatar::draw()` now takes the application-wide graphics lock
   (`GfxLockGuard`) for the whole draw. On the real device, an
   `xQueueGenericSend` mutex-ownership assert fired inside
   `Face::draw → … → spiEndTransaction` (resolved with addr2line): the
   LGFX panel transaction flag is shared per panel object while the Arduino
   SPI bus mutex is taken/given by whichever task happens to run the
   outermost begin/end, so concurrent drawing from the loop task (dance
   video, news menu list, pose pre-decode) and drawLoop corrupts the
   pairing. The application serializes all LGFX access with one recursive
   mutex (see project `src/GfxLock.h`); wrapping `Avatar::draw()` covers
   everything drawLoop draws.

Everything else is byte-identical to the 0.9.2 registry package (examples/
and .github/ were dropped). If the upstream library is ever upgraded, this
folder must be re-vendored and both changes re-applied.
