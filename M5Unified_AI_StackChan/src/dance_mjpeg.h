// dance_mjpeg.h - ダンス動画（モーションJPEG）の宣言（第9弾）
//
// 元動画 pose_assets/kinako_dance.mp4 (960x960/24fps/10秒) を
// 10fps・320x240（中央 4:3 クロップ）・JPEG q9 で変換した連結JPEG。
// （第9.1弾で軽量化。旧版は 12fps/q5 で 1,161,458 バイトだった）
// ESP32 は MP4 を再生できないため、この形式で埋め込んで順に描画する。
// 実データは dance_mjpeg.cpp（gen_v9_assets.py で生成）。
#ifndef DANCE_MJPEG_H_
#define DANCE_MJPEG_H_

#include <stdint.h>

extern const uint8_t dance_mjpeg[];
extern const uint32_t dance_mjpeg_len;

#endif  // DANCE_MJPEG_H_
