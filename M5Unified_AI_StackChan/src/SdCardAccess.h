#pragma once
// ==== SD カード健全性管理への共有アクセス窓口（第9.32弾） ====
// SD の健全性管理（第9.28弾: 一度使えたカードが応答を止めたら、その起動中は
// SD を二度と触らない）の実体 g_sd_present_once / g_sd_dead /
// sdBeginCounted() は main.cpp 内にある。ウェイクワードのテンプレートを
// SD へ永続化する WakeWord.cpp からも同じ枠組みで SD を使うため、薄い
// ラッパだけを本ヘッダで共有する（状態の実体は main.cpp の単一箇所のみ。
// FlashWriteGuard の共有化と同じ作法）。
//
// sdAccessBegin() が true を返したら、使い終わりに必ず sdAccessEnd() を
// 呼ぶこと（既存の memfs_close / seenfs_close と同じ begin/end の作法）。
bool sdAccessBegin();
void sdAccessEnd();
bool sdAccessDead();  // 一度使えたカードが応答停止（この起動中は SD 無効）
