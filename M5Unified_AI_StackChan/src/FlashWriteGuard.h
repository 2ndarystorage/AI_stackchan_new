#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include "Forensics.h"

// ==== フラッシュ書き込みと側面LED（RMT）の相互排他（第9.27弾） ====
// ESP32 のフラッシュ書き込み/消去中はキャッシュが無効化され、その間に
// フラッシュ上のコードを実行すると "Cache disabled but cached memory
// region accessed" パニックになる。FastLED 3.10 / IDF 4.4 の RMT 補充
// 割り込み経路はフラッシュ上の HAL コードに触れるため、SPIFFS / NVS への
// 書き込みと FastLED.show()（RMT 送信）が重なると踏み得る。
// 対策として、内蔵フラッシュへの書き込みは必ず FlashWriteGuard で囲み、
// ledTask 側の show() も同じミューテックス（StorageLedGuard）で囲むことで
// 「書き込みと RMT 送信が同時に走らない」ことを保証する。ガードは書き込み
// 前に ledRmtDrain() で RMT の完全アイドルを待ち、フォレンジックへ開始/
// 終了を記録する（第9.28弾）。詳細は FlashWriteGuard.cpp 側のコメント参照。
// ミューテックスは setup のタスク起動前に生成する。生成前（起動初期の
// 単独実行区間）や未生成時はロックなしで通す（競合相手が存在しない）。
//
// 第9.31弾: 実体は当初 main.cpp 内にあったが、WakeWord.cpp のウェイクワード
// 登録保存（SimpleVox の saveFile）がガードの対象外で、同型パニックが
// 登録中に発生した。ガードを本ヘッダ＋FlashWriteGuard.cpp へ切り出して
// 全ファイルから共有する。ミューテックス／ドレイン状態の実体は
// FlashWriteGuard.cpp の単一箇所だけに置く（二重定義禁止）。
extern SemaphoreHandle_t g_storage_led_mutex;
void storageLedLock();
void storageLedUnlock();
void ledRmtDrain();

struct StorageLedGuard {
  StorageLedGuard() { storageLedLock(); }
  ~StorageLedGuard() { storageLedUnlock(); }
};

// フラッシュ書き込み専用のガード。相互排他ロックの取得後に RMT の完全
// アイドルを待ち、それからフォレンジックへ開始/終了を記録する。
// ledTask 側も show() の直後に（ロック保持のまま）ledRmtDrain() するため、
// 「ロックが空いている＝RMT 送信は完全に終わっている」が常に成り立つ。
struct FlashWriteGuard {
  uint8_t tag;
  explicit FlashWriteGuard(uint8_t t) : tag(t) {
    storageLedLock();
    ledRmtDrain();
    fxLog(FXK_WRITE_BEGIN, tag);
  }
  ~FlashWriteGuard() {
    fxLog(FXK_WRITE_END, tag);
    storageLedUnlock();
  }
  FlashWriteGuard(const FlashWriteGuard &) = delete;
  FlashWriteGuard &operator=(const FlashWriteGuard &) = delete;
};
