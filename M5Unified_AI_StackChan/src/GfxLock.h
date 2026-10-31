// GfxLock.h - グラフィックス直列化ロック（第9.3弾）
//
// M5GFX/LovyanGFX のパネル transaction は、パネルオブジェクト共有の
// フラグ (Panel_LCD::_in_transaction) で入れ子を管理し、実際の SPI バス
// mutex (Arduino コアの spi->lock) は「最初に startWrite したタスク」が
// 取り、「最後に endWrite したタスク」が返す前提になっている。drawLoop と
// loop 等の別タスクが同じ M5.Display へ同時に描くと、begin/end の対応が
// タスクをまたいで崩れ、endWrite を実行したタスクと mutex の保持タスクが
// 食い違って、実機で
// assert failed: xQueueGenericSend (mutex 所有権の不一致)
// が発生した（第9.2弾の実機ログで確認。addr2line で drawLoop の
// Face::draw → LGFXBase::endWrite → Panel_LCD::endTransaction →
// Bus_SPI::endTransaction → spiEndTransaction と特定）。
//
// アプリ全体で共有する再帰 mutex を1つ用意し、LGFX オブジェクト
// （ディスプレイ・キャンバス）に触るすべての経路でこれを取る:
// - Avatar::draw() 全体（drawLoop タスク側。vendored ライブラリ側で取得）
// - loop 側: ポーズの先行デコード (pose_prepare_tick)、ダンス動画再生
// (play_dance_video)、ニュースのカテゴリ一覧描画 (drawMenuList)
// 再帰 mutex なので、同じタスク内での入れ子取得でもデッドロックしない。
// 保持中に他タスクが待たされるのは最長でもダンス動画再生中（約10秒）で、
// その間 drawLoop は Avatar::draw() の入口でブロックされるだけ
// （顔は静止画なので実害はない）。
#ifndef GFX_LOCK_H_
#define GFX_LOCK_H_

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 実体は main.cpp。setup() の最初で生成される。
extern SemaphoreHandle_t g_gfx_mutex;

inline bool gfxLock() {
if (g_gfx_mutex == nullptr) return false;
return xSemaphoreTakeRecursive(g_gfx_mutex, portMAX_DELAY) == pdTRUE;
}

inline void gfxUnlock() {
if (g_gfx_mutex!= nullptr) xSemaphoreGiveRecursive(g_gfx_mutex);
}

// RAII ガード（スコープを抜けると自動で解放する）
class GfxLockGuard {
public:
GfxLockGuard(): locked_(gfxLock()) {}
~GfxLockGuard() { if (locked_) gfxUnlock();}
GfxLockGuard(const GfxLockGuard &) = delete;
GfxLockGuard &operator=(const GfxLockGuard &) = delete;

private:
bool locked_;
};

#endif // GFX_LOCK_H_
