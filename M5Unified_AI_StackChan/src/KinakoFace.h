// KinakoFace.h - きなこ写真の顔 for M5Stack-Avatar (AI_StackChan2 / Core2)
//
// 顔全体に「きなこ」の写真 (320x240 JPEG, kinako_img.h) を表示する Face。
// 写真は静止画なので、まばたき・口パク等の描画アニメーションは無くなる。
// 吹き出し (Balloon) と表情エフェクト (Effect)・バッテリー表示は Avatar 側が
// 顔の後に描画するため、そのまま有効。
//
// 描画順は mouth -> eyeR -> eyeL -> browR -> browL -> balloon/effect のため、
// 写真は mouth スロット (KinakoImage) が最初に全面描画し、
// 目・眉スロットは KinakoBlank (無描画) を置く。
//
// JPEG デコードは初回のみ行い、結果を PSRAM 上の M5Canvas にキャッシュする。
// 以後のフレームはキャッシュの pushSprite 転送のみ (デコードなし)。
// キャッシュ確保に失敗した場合は毎フレーム drawJpg で直接描くフォールバック。
#ifndef KINAKO_FACE_H_
#define KINAKO_FACE_H_

#include <Avatar.h>
#include "kinako_img.h"

namespace m5avatar {

// きなこ写真 (全面描画・mouth スロット)
class KinakoImage : public Drawable {
 private:
  M5Canvas cache;  // デコード済み画像キャッシュ
  bool cacheReady = false;
  bool cacheFailed = false;

 public:
  KinakoImage() : cache(&M5.Lcd) {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    (void)rect;
    (void)ctx;
    if (!cacheReady && !cacheFailed) {
      cache.setColorDepth(16);
      cache.setPsram(true);  // 320x240x16bit = 150KB は PSRAM に確保
      if (cache.createSprite(320, 240) != nullptr) {
        cache.drawJpg(kinako_jpg, kinako_jpg_len, 0, 0);
        cacheReady = true;
      } else {
        cacheFailed = true;  // 確保失敗。以後はフォールバック描画
      }
    }
    if (cacheReady) {
      cache.pushSprite(spi, 0, 0);
    } else {
      // フォールバック: 毎フレーム JPEG を直接デコード描画
      spi->drawJpg(kinako_jpg, kinako_jpg_len, 0, 0);
    }
  }
};

// 何も描かないパーツ (目・眉スロット用)
class KinakoBlank : public Drawable {
 public:
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    (void)spi;
    (void)rect;
    (void)ctx;
  }
};

class KinakoFace : public Face {
 public:
  KinakoFace()
      : Face(new KinakoImage(), new BoundingRect(150, 160),
             new KinakoBlank(), new BoundingRect(92, 107),
             new KinakoBlank(), new BoundingRect(92, 213),
             new KinakoBlank(), new BoundingRect(62, 107),
             new KinakoBlank(), new BoundingRect(62, 213)) {}
};

}  // namespace m5avatar

#endif  // KINAKO_FACE_H_
