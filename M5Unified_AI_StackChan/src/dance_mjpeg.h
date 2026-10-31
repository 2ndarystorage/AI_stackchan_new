// dance_mjpeg.h - ダンス動画（モーションJPEG）の宣言（第9弾）
//
// 元動画 pose_assets/kinako_dance.mp4 (960x960/24fps/10秒) を
// 12fps・320x240（中央 4:3 クロップ）・JPEG q5 で変換した連結JPEG。
// ESP32 は MP4 を再生できないため、この形式で埋め込んで順に描画する。
// 実データは dance_mjpeg.cpp（gen_v9_assets.py で生成）。
#ifndef DANCE_MJPEG_H_
#define DANCE_MJPEG_H_

#include <stdint.h>

extern const uint8_t dance_mjpeg[];
extern const uint32_t dance_mjpeg_len;

#endif  // DANCE_MJPEG_H_
