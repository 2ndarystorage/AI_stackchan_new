// pose_img.h - ポーズ画像12種 (320x240 JPEG) の宣言（第9弾／第9.7弾で黒バック化）
//
// 素材は pose_assets/pose01..12.jpg。背景を黒に置き換えた版を
// gen_black_assets.py で変換・埋め込みした。実データは pose_img.cpp。
#ifndef POSE_IMG_H_
#define POSE_IMG_H_

#include <stdint.h>

enum PoseId : int {
  POSE_WAVE = 0,
  POSE_BOW = 1,
  POSE_JUMP = 2,
  POSE_DANCE = 3,
  POSE_HEART = 4,
  POSE_THUMBSUP = 5,
  POSE_SURPRISE = 6,
  POSE_LAUGH = 7,
  POSE_SLEEP = 8,
  POSE_RUN = 9,
  POSE_PEACE = 10,
  POSE_FISTS = 11,
  POSE_COUNT
};

extern const uint8_t *const pose_jpgs[POSE_COUNT];
extern const uint32_t pose_jpg_lens[POSE_COUNT];

#endif  // POSE_IMG_H_
