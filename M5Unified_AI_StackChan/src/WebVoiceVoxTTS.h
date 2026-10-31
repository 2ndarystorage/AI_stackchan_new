#pragma once

#include <Arduino.h>

void voicevox_log_ca();
void voicevox_cancel();
void voicevox_reset_cancel();

// 第8弾: 文単位パイプラインTTS 用のAPI。
// voicevox_synth_url() は tts.quest へ1文（チャンク）を合成依頼し、
// 完成版MP3のURLを優先して返す。失敗/タイムアウト時は空文字を返す。
// 合成専用タスクからのみ呼ぶこと。
String voicevox_synth_url(const char* text, const char* tts_parms);

// 第9.6弾: 合成済みURLのMP3をPSRAMへ丸ごとダウンロードする（TLS一本化）。
// 再生用にTLS接続を張りっぱなしにせず、ダウンロード中だけTLSを使い、
// 接続を閉じてからメモリ上のデータで再生するための前処理。
// 第9.8弾: チャンク形式はプロトコルどおり解読して終端で完了を確定し、
// Content-Length と受信バイト数を突き合わせ、受信後にMP3フレーム列の
// 完全性も検証する。*outLen は末尾のガード用ゼロパディングを含む長さ。
// 戻り値: 0=成功（*outData/*outLen にPSRAMバッファ。所有権は呼び出し側へ移る）
//         1=上限超過でダウンロード中止（その文はスキップ）
//        -1=失敗（失敗時は内部で全解放＋1回だけ再試行済み）
// 完成通知があれば最大90秒待ち（進行中HTTPは別途タイムアウト）、その後に取得する。
// 合成専用タスクからのみ呼ぶこと。
int voicevox_download_mp3(const String& url, uint8_t** outData, size_t* outLen);

// 合成/ダウンロードの接続ペアを完全に破棄し、
// TLSバッファ等を内蔵ヒープへ返却する。合成専用タスクからのみ呼ぶこと
// （他タスクは main.cpp 側のフラグ経由で依頼する）。
void voicevox_release_connection();

// 第9.6弾: PSRAM上のMP3データ（data/len）から再生を開始する。
// data は再生終了まで呼び出し側が保持すること（ソースはポインタ参照のみ）。
bool voicevox_play_memory(const uint8_t* data, size_t len);

// v9.15: playback is memory-only; failed downloads are never streamed on a second TLS client.
