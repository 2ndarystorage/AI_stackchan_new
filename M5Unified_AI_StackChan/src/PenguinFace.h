// PenguinFace.h - ペンギン顔 for M5Stack-Avatar (AI_StackChan2 / Core2)
//
// 描画方式はライブラリ標準と同じ1bit（白=primary / 黒=background）。
// 黒い画面全体をペンギンの頭と見立て、中央に白い顔パッチ（楕円）を描き、
// その上に黒い丸目とクチバシを置く。
// 描画順は mouth -> eyeR -> eyeL -> browR -> browL -> effect/balloon のため、
// 顔パッチは mouth スロット（PenguinBeak）が最初に描く。
#ifndef PENGUIN_FACE_H_
#define PENGUIN_FACE_H_

#include <Avatar.h>

namespace m5avatar {

// クチバシ＋顔パッチ（mouth スロット。最初に描かれる）
class PenguinBeak : public Drawable {
 public:
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    const uint16_t patchColor =
        ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
    const uint16_t darkColor =
        ctx->getColorDepth() == 1 ? 0 : ctx->getColorPalette()->get(COLOR_BACKGROUND);
    const uint16_t beakColor = ctx->getColorDepth() == 1 ? darkColor : TFT_ORANGE;
    const float breath = ctx->getBreath();
    const int by = (int)(breath * 3);  // 他パーツと同じ呼吸オフセット

    // 白い顔パッチ（楕円）: 中心 (160, 130+by), rx=115, ry=102
    spi->fillEllipse(160, 130 + by, 115, 102, patchColor);

    // クチバシ: rect の中心がクチバシ中心 (main 側で (150,160) を指定)
    const int cx = rect.getCenterX();
    const int cy = rect.getCenterY() + by;
    const float open = ctx->getMouthOpenRatio();
    const int gap = (int)(open * 12);  // 発話に合わせて下クチバシが下がる
    // 上クチバシ（ひし形の上半分）
    spi->fillTriangle(cx - 27, cy - 6, cx + 27, cy - 6, cx, cy + 10, beakColor);
    spi->fillRect(cx - 27, cy - 9, 54, 4, beakColor);
    // 下クチバシ（小さめの三角）
    spi->fillTriangle(cx - 18, cy + 10 + gap, cx + 18, cy + 10 + gap,
                      cx, cy + 22 + gap, beakColor);
  }
};

// ペンギンの目（白パッチの上に黒い丸目＋白いハイライト）
class PenguinEye : public Drawable {
 private:
  bool isLeft;

 public:
  explicit PenguinEye(bool isLeft) : isLeft{isLeft} {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    const uint16_t patchColor =
        ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
    const uint16_t darkColor =
        ctx->getColorDepth() == 1 ? 0 : ctx->getColorPalette()->get(COLOR_BACKGROUND);
    const Expression exp = ctx->getExpression();
    const Gaze g = ctx->getGaze();
    const float openRatio = ctx->getEyeOpenRatio();
    const int cx = rect.getCenterX() + (int)(g.getHorizontal() * 4);
    const int cy = rect.getCenterY() + (int)(g.getVertical() * 3);
    const int r = 16;

    if (openRatio == 0) {
      // まばたき（閉じ目）: 黒い横線
      spi->fillRect(cx - 15, cy - 2, 30, 5, darkColor);
      return;
    }
    // 丸目（開き具合で縦方向に潰れる）
    int ry = (int)(r * openRatio);
    if (ry < 3) ry = 3;
    spi->fillEllipse(cx, cy, r, ry, darkColor);
    if (openRatio > 0.6 && exp != Expression::Happy) {
      // ハイライト
      spi->fillCircle(cx - 5, cy - 5, 4, patchColor);
    }
    if (exp == Expression::Happy) {
      // 笑い目: 下半分をパッチ色で消して山型にする
      spi->fillRect(cx - r - 2, cy + 1, (r + 2) * 2, r + 2, patchColor);
    } else if (exp == Expression::Sleepy) {
      // 眠い目: 上半分を消して半目にする
      spi->fillRect(cx - r - 2, cy - r - 2, (r + 2) * 2, r + 1, patchColor);
    } else if (exp == Expression::Angry || exp == Expression::Sad) {
      // 上端を斜めにカット（標準 Eye と同じ向き規則）
      const int x0 = cx - r;
      const int y0 = cy - r;
      const int x1 = x0 + r * 2;
      const int y1 = y0;
      const int x2 = (!isLeft != !(exp == Expression::Sad)) ? x0 : x1;
      const int y2 = y0 + r;
      spi->fillTriangle(x0, y0, x1, y1, x2, y2, patchColor);
    }
  }
};

// 眉（ペンギンの顔の模様として、怒り/悲しみ/疑問の時だけパッチ内に描く）
class PenguinBrow : public Drawable {
 private:
  bool isLeft;

 public:
  explicit PenguinBrow(bool isLeft) : isLeft{isLeft} {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    const Expression exp = ctx->getExpression();
    if (exp != Expression::Angry && exp != Expression::Sad &&
        exp != Expression::Doubt) {
      return;  // 通常時は模様なし
    }
    const uint16_t darkColor =
        ctx->getColorDepth() == 1 ? 0 : ctx->getColorPalette()->get(COLOR_BACKGROUND);
    const int x = rect.getCenterX();
    const int y = rect.getCenterY();
    const int w = 36;
    const int h = 5;
    const int a = isLeft ^ (exp == Expression::Sad) ? -1 : 1;
    const int dx = a * 3;
    const int dy = a * 6;
    const int x1 = x - w / 2;
    const int x2 = x1 - dx;
    const int x4 = x + w / 2;
    const int x3 = x4 + dx;
    const int y1 = y - h / 2 - dy;
    const int y2 = y + h / 2 - dy;
    const int y3 = y - h / 2 + dy;
    const int y4 = y + h / 2 + dy;
    spi->fillTriangle(x1, y1, x2, y2, x3, y3, darkColor);
    spi->fillTriangle(x2, y2, x3, y3, x4, y4, darkColor);
  }
};

class PenguinFace : public Face {
 public:
  PenguinFace()
      : Face(new PenguinBeak(), new BoundingRect(150, 160),
             new PenguinEye(false), new BoundingRect(92, 107),
             new PenguinEye(true), new BoundingRect(92, 213),
             new PenguinBrow(false), new BoundingRect(62, 107),
             new PenguinBrow(true), new BoundingRect(62, 213)) {}
};

}  // namespace m5avatar

#endif  // PENGUIN_FACE_H_
