// dance_mjpeg.h - ダンス動画（モーションJPEG）の宣言（第9弾／第9.7弾で黒バック化）
//
// 元動画 pose_assets/kinako_dance.mp4 (960x960/24fps/10秒) を
// 10fps・320x240（中央 4:3 クロップ）で抜き出し、背景を黒に置き換えた
// 連結JPEG。実データは dance_mjpeg.cpp（gen_black_assets.py で生成）。
#ifndef DANCE_MJPEG_H_
#define DANCE_MJPEG_H_

#include <stdint.h>

extern const uint8_t dance_mjpeg[];
extern const uint32_t dance_mjpeg_len;

#endif  // DANCE_MJPEG_H_
