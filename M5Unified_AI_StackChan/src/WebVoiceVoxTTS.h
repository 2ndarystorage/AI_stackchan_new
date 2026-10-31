#pragma once

#include <Arduino.h>

// 第8弾: 文単位パイプラインTTS 用のAPI。
// voicevox_synth_url() は tts.quest へ1文（チャンク）を合成依頼し、
// 再生用ストリーミングURLを返す。失敗/タイムアウト時は空文字を返す。
// 合成専用タスクからのみ呼ぶこと（内部で接続を使い回すため）。
String voicevox_synth_url(const char* text, const char* tts_parms);

// 取得済みのストリーミングURLから MP3 再生を開始する。
bool voicevox_play_url(const String& url);
