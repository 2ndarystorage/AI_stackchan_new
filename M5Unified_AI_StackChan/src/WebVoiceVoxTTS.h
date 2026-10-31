#pragma once

#include <Arduino.h>

// 第8弾: 文単位パイプラインTTS 用のAPI。
// voicevox_synth_url() は tts.quest へ1文（チャンク）を合成依頼し、
// 再生用ストリーミングURLを返す。失敗/タイムアウト時は空文字を返す。
// 合成専用タスクからのみ呼ぶこと（内部で接続を使い回すため）。
String voicevox_synth_url(const char* text, const char* tts_parms);

// 第9.6弾: 合成済みURLのMP3をPSRAMへ丸ごとダウンロードする（TLS一本化）。
// 再生用にTLS接続を張りっぱなしにせず、ダウンロード中だけTLSを使い、
// 接続を閉じてからメモリ上のデータで再生するための前処理。
// 第9.8弾: チャンク形式はプロトコルどおり解読して終端で完了を確定し、
// Content-Length と受信バイト数を突き合わせ、受信後にMP3フレーム列の
// 完全性も検証する。*outLen は末尾のガード用ゼロパディングを含む長さ。
// 戻り値: 0=成功（*outData/*outLen にPSRAMバッファ。所有権は呼び出し側へ移る）
//         1=上限超過でダウンロード中止（呼び出し側はストリーム再生へフォールバック）
//        -1=失敗（失敗時は内部で全解放＋1回だけ再試行済み）
// 合成専用タスクからのみ呼ぶこと（合成と同じクライアントペアを使い回すため）。
int voicevox_download_mp3(const String& url, uint8_t** outData, size_t* outLen);

// 第9.8弾: 合成/ダウンロードで使い回している接続ペアを完全に破棄し、
// TLSバッファ等を内蔵ヒープへ返却する。合成専用タスクからのみ呼ぶこと
// （他タスクは main.cpp 側のフラグ経由で依頼する）。
void voicevox_release_connection();

// 第9.6弾: PSRAM上のMP3データ（data/len）から再生を開始する。
// data は再生終了まで呼び出し側が保持すること（ソースはポインタ参照のみ）。
bool voicevox_play_memory(const uint8_t* data, size_t len);

// 取得済みのストリーミングURLから MP3 再生を開始する
// （第9.6弾以降はダウンロード上限超過時のフォールバック用）。
bool voicevox_play_url(const String& url);
