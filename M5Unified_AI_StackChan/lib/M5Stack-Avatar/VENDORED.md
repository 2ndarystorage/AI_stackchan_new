# Vendored library note (第9.2弾)

This is a vendored copy of meganetaaan/M5Stack-Avatar 0.9.2 (the same version
that PlatformIO previously fetched into `.pio/libdeps`). Placing it under the
project `lib/` folder makes PlatformIO prefer it over the registry copy.

## Only change from upstream

- `src/Avatar.cpp`, `Avatar::start()`: the `drawLoop` task stack size was
  raised from **2048 to 8192**. On ESP-IDF the stack-size argument of
  `xTaskCreateUniversal` is in **bytes** (despite the upstream comment saying
  "words"), so the stock draw task had only 2048 bytes of stack. With the
  photo-face (KinakoFace) draw path this overflowed and corrupted the stack
  canary, causing `Stack canary watchpoint triggered (drawLoop)` panics on
  the real device. The library exposes no API to change this value, hence
  the vendored copy.

Everything else is byte-identical to the 0.9.2 registry package (examples/
and .github/ were dropped). If the upstream library is ever upgraded, this
folder must be re-vendored and the same one-line change re-applied.
