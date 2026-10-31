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
//
// 読み上げ字幕（2行表示）：写真を描いた直後に、同じキャンバスへ画面下部の
// 字幕パネル（黒帯＋白文字2行）を重ねて描く。Avatar のフレーム合成に
// 含まれるため、loop 側から LCD へ直接描く方式と違いちらつきがない。
// 字幕の内容と表示状態は main.cpp 側が共有バッファ経由で渡す。
#ifndef KINAKO_FACE_H_
#define KINAKO_FACE_H_

#include <Avatar.h>
#include <string.h>
#include "kinako_img.h"

// 読み上げ字幕の共有状態（実体は main.cpp）
// loop 側が裏バッファ (front^1) へ2行を書いてから front を切り替え、
// 描画側は front のバッファを読むことで、書換え途中の行を見せない。
extern volatile bool g_caption_show;
extern volatile uint8_t g_caption_front;
extern char g_caption_buf[2][2][128];

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
    // 読み上げ字幕（2行）を写真の上に重ねる
    // （フォント efontJA_16 等倍：全角16px、1行は全角19文字相当まで）
    if (g_caption_show) {
      uint8_t f = g_caption_front;
      char line1[128];
      char line2[128];
      strncpy(line1, g_caption_buf[f][0], sizeof(line1) - 1);
      line1[sizeof(line1) - 1] = '\0';
      strncpy(line2, g_caption_buf[f][1], sizeof(line2) - 1);
      line2[sizeof(line2) - 1] = '\0';
      spi->fillRoundRect(0, 196, 320, 44, 6, 0x000000);
      spi->setFont(&fonts::efontJA_16);
      spi->setTextSize(1);
      spi->setTextColor(0xFFFFFF);
      spi->setCursor(8, 200);
      spi->print(line1);
      spi->setCursor(8, 218);
      spi->print(line2);
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
