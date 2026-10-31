// KinakoFace.h - きなこ写真の顔 for M5Stack-Avatar (AI_StackChan2 / Core2)
//
// 顔全体に「きなこ」の写真 (320x240 JPEG) を表示する Face。
// 写真は静止画なので、まばたき・口パク等の描画アニメーションは無くなる。
// 吹き出し (Balloon) と表情エフェクト (Effect)・バッテリー表示は Avatar 側が
// 顔の後に描画するため、そのまま有効。
//
// 描画順は mouth -> eyeR -> eyeL -> browR -> browL -> balloon/effect のため、
// 写真は mouth スロット (KinakoImage) が最初に全面描画し、
// 目・眉スロットは KinakoBlank (無描画) を置く。
//
// 第9弾: 表示画像はポーズ12種 (pose_img.h) から選ぶ。選択の優先順位は
//   一時ポーズ (g_pose_timed、期限内) > 状態ポーズ (g_pose_state)
//   > 表情連動 (Happy=ハート/Sleepy=寝る/Doubt=驚き/Sad=お辞儀/Angry=両拳上げ)
//   > 待機 (ピース)
// の順。
//
// 第9.2弾: JPEG デコードを draw コールバック（drawLoop タスク上）から追い出した。
// 第9.1弾までの実装はポーズ変更時に draw 内で drawJpg デコードしており、
// drawLoop のスタック（2048 バイトしかない）では収まらず、実機で
// 「Stack canary watchpoint triggered (drawLoop)」panic が発生した。
// 現在の方式:
//   - PSRAM 上に M5Canvas スプライトを2枚（ダブルバッファ）保持する。
//   - loop/setup 側が kinakoPreparePose() で次のポーズを裏スプライトへ
//     先にデコードし、完了したら staged フラグを立てる。
//   - draw 側は staged フラグを見て表裏を入れ替えるだけ（アトミックな
//     インデックス切替）で、あとは pushSprite のみ。デコードは一切しない。
//     デコード完了までは前のポーズを表示し続ける。
//   - スプライト確保に失敗した場合のみ、従来どおり毎フレーム drawJpg で
//     直接描くフォールバックに落ちる（PSRAM 枯渇時用の非常手段）。
// ポーズ画像のデコードに失敗した場合は従来の単一写真 (kinako_img.h) を描く。
//
// 第9.3弾: LGFX へのアクセスはアプリ共有の再帰 mutex (GfxLock.h) で
// 直列化される。draw 側は Avatar::draw()（vendored ライブラリ）が冒頭で
// ロックを取り、prepare() 側は loop の pose_prepare_tick() がロックを
// 取ってから呼ぶ前提。そのため prepare() と draw() が同時に走ることは
// なくなった（ダブルバッファの切替プロトコル自体は従来どおり）。
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
#include "pose_img.h"

// 読み上げ字幕の共有状態（実体は main.cpp）
// loop 側が裏バッファ (front^1) へ2行を書いてから front を切り替え、
// 描画側は front のバッファを読むことで、書換え途中の行を見せない。
extern volatile bool g_caption_show;
extern volatile uint8_t g_caption_front;
extern char g_caption_buf[2][2][128];

// バッテリーアイコン・音量表示の共有状態（実体は main.cpp、第9.6弾）
// バッテリーは loop 側が数秒おきに取得して更新し、描画側は読むだけ。
// 音量は描画時に現在のレベルを読むため、変更が即座に反映される。
extern volatile int g_batt_pct;        // 残量 %（-1=未取得・アイコンを出さない）
extern volatile bool g_batt_charging;  // 充電中か
extern uint8_t g_vol_level;            // 音量レベル（0〜5、第8弾の6段階）

// ポーズ切り替えの共有状態（実体は main.cpp、優先順位管理も main.cpp 側）
extern volatile int g_pose_state;            // 状態ポーズ (-1=なし)
extern volatile int g_pose_timed;            // 一時ポーズ (-1=なし)
extern volatile uint32_t g_pose_timed_until; // 一時ポーズの終了時刻 (millis)

namespace m5avatar {

// きなこ写真 (全面描画・mouth スロット)
class KinakoImage : public Drawable {
 private:
  M5Canvas sprA;  // ダブルバッファ用スプライト (PSRAM)
  M5Canvas sprB;
  bool sprReady = false;    // スプライト2枚の確保が完了したか
  bool sprFailed = false;   // 確保失敗（フォールバック描画へ）
  int frontIdx = 0;         // 表示中スプライト (0=sprA, 1=sprB)
  int displayedPose = -1;   // front スプライトに表示中のポーズ (-1=なし)
  volatile int stagedPose = -1;     // デコード済み・切替待ちのポーズ
  volatile bool stagedReady = false;  // 切替待ちがあるか

  M5Canvas &sprAt(int i) { return (i == 0) ? sprA : sprB; }

  // スプライト2枚を PSRAM に確保する（draw の外からだけ呼ぶこと）
  bool ensureSprites() {
    if (sprReady) return true;
    if (sprFailed) return false;
    for (int i = 0; i < 2; i++) {
      M5Canvas &s = sprAt(i);
      s.setColorDepth(16);
      s.setPsram(true);  // 320x240x16bit = 150KB/枚 は PSRAM に確保
      if (s.createSprite(320, 240) == nullptr) {
        sprFailed = true;  // 確保失敗。以後はフォールバック描画
        return false;
      }
    }
    sprReady = true;
    return true;
  }

  // 表示すべきポーズを決める（フォールバック描画専用。
  // 通常経路の解決は main.cpp の resolve_target_pose() が同じ規則で行う）
  int resolvePose(DrawContext *ctx) {
    uint32_t now = millis();
    int timed = g_pose_timed;
    if (timed >= 0 && timed < POSE_COUNT &&
        (int32_t)(g_pose_timed_until - now) > 0) {
      return timed;
    }
    int st = g_pose_state;
    if (st >= 0 && st < POSE_COUNT) return st;
    switch (ctx->getExpression()) {
      case Expression::Happy:  return POSE_HEART;
      case Expression::Sleepy: return POSE_SLEEP;
      case Expression::Doubt:  return POSE_SURPRISE;
      case Expression::Sad:    return POSE_BOW;
      case Expression::Angry:  return POSE_FISTS;
      default:                 return POSE_PEACE;  // Neutral は待機ポーズ
    }
  }

 public:
  KinakoImage();  // 定義はクラス外（グローバル登録のため）

  // 次のポーズを裏スプライトへ先にデコードする。draw タスク以外の文脈
  // （loop / setup）から呼ぶこと。immediate=true の場合は切替まで即時に
  // 行う（描画タスクがまだ動いていない setup 中の初回表示用）。
  // 戻り値: スプライト経路が使える状態なら true。
  bool prepare(int pose, bool immediate) {
    if (pose < 0 || pose >= POSE_COUNT) return false;
    if (!ensureSprites()) return false;
    if (pose == displayedPose) return true;
    if (!immediate && stagedReady && pose == stagedPose) return true;
    int back = frontIdx ^ 1;
    M5Canvas &s = sprAt(back);
    // ポーズ画像のデコード。失敗時は従来の単一写真で代替
    if (!s.drawJpg(pose_jpgs[pose], pose_jpg_lens[pose], 0, 0)) {
      s.drawJpg(kinako_jpg, kinako_jpg_len, 0, 0);
    }
    if (immediate) {
      frontIdx = back;
      displayedPose = pose;
      stagedReady = false;
    } else {
      stagedPose = pose;
      stagedReady = true;  // 最後に立てる（draw 側の切替トリガ）
    }
    return true;
  }

  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    (void)rect;
    if (sprFailed) {
      // フォールバック: 毎フレーム JPEG を直接デコード描画（従来方式）
      int pose = resolvePose(ctx);
      if (!spi->drawJpg(pose_jpgs[pose], pose_jpg_lens[pose], 0, 0)) {
        spi->drawJpg(kinako_jpg, kinako_jpg_len, 0, 0);
      }
    } else {
      // デコード済みの次ポーズがあれば表裏を入れ替える（軽い処理のみ）
      if (stagedReady) {
        frontIdx ^= 1;
        displayedPose = stagedPose;
        stagedReady = false;
      }
      if (sprReady && displayedPose >= 0) {
        sprAt(frontIdx).pushSprite(spi, 0, 0);
      } else {
        // 初回デコードが終わるまでの仮表示
        spi->fillRect(0, 0, 320, 240, 0x0000);
      }
    }
    // バッテリーアイコン（右上・4段階）と音量表示（その下）を写真の上に重ねる（第9.6弾）
    // 字幕と同じく顔合成のキャンバスへ描くため、ちらつきなく常時表示できる。
    // 下地は字幕パネルと同じ黒の角丸。表示のみでタッチ判定には影響しない。
    {
      int pct = g_batt_pct;
      if (pct >= 0) {
        // 段階: 75%以上=4、50%以上=3、25%以上=2、25%未満=1
        int bars = (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : 1;
        uint32_t barCol = g_batt_charging ? 0x00FF00u : (bars <= 1 ? 0xFF0000u : 0xFFFFFFu);
        spi->fillRoundRect(284, 2, 34, 16, 3, 0x000000);
        spi->drawRoundRect(288, 4, 24, 12, 2, 0xFFFFFF);  // 電池の外枠（24x12）
        spi->fillRect(313, 7, 3, 6, 0xFFFFFF);             // 端子の出っ張り
        for (int i = 0; i < bars; i++) {
          spi->fillRect(290 + i * 5, 6, 4, 8, barCol);     // 残量バー（幅4px＋間隔1px）
        }
      }
      // 音量: スピーカーグリフ＋5目盛りバー（右ほど高い）。レベル0は×付きミュート表示
      int vol = (int)g_vol_level;
      if (vol < 0) vol = 0;
      if (vol > 5) vol = 5;
      spi->fillRoundRect(284, 20, 34, 14, 3, 0x000000);
      if (vol == 0) {
        spi->fillRect(287, 25, 3, 5, 0x808080);
        spi->fillTriangle(290, 23, 290, 32, 295, 27, 0x808080);
        spi->drawLine(286, 22, 297, 32, 0xFF0000);
        spi->drawLine(297, 22, 286, 32, 0xFF0000);
      } else {
        spi->fillRect(287, 25, 3, 5, 0xFFFFFF);
        spi->fillTriangle(290, 23, 290, 32, 295, 27, 0xFFFFFF);
      }
      for (int i = 0; i < 5; i++) {
        int h = 3 + i;  // 目盛りの高さ 3〜7px
        spi->fillRect(299 + i * 4, 31 - h, 3, h, (i < vol) ? 0xFFFFFFu : 0x404040u);
      }
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

// 生成中の KinakoImage へのポインタ（コンストラクタで登録される。
// 実体の定義は main.cpp 側にある）
extern KinakoImage *g_kinako_image;

inline KinakoImage::KinakoImage() : sprA(&M5.Lcd), sprB(&M5.Lcd) {
  g_kinako_image = this;
}

// loop / setup からポーズの先行デコードを依頼する入口
inline bool kinakoPreparePose(int pose, bool immediate = false) {
  if (g_kinako_image == nullptr) return false;
  return g_kinako_image->prepare(pose, immediate);
}

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
