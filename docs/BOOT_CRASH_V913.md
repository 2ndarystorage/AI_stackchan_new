# v9.13 起動ダンス中のクラッシュ修正

更新: 2026-11-01。v9.12は起動クラッシュの退行があるため、書き込みにはv9.13を使ってください。

## 確定した原因

報告された `ELF file SHA256: d69211fa89ace9d6` は配布したv9.12のELFと一致しました（完全なSHA-256は `d69211fa89ace9d6f421eebdbc128b807763f334c48859e56ffb40f9741b702b`）。同じELFでバックトレースをaddr2lineに通すと、次の経路です。

```text
m5avatar::drawLoop
  Avatar::draw                 .pio/libdeps/.../M5Stack-Avatar/src/Avatar.cpp:201
    Face::draw                Face.cpp:164
      LGFXBase::endWrite
        Panel_LCD::endTransaction
          Bus_SPI::endTransaction
            spiEndTransaction
              xQueueGenericSend assertion
```

ビルド設定の `meganetaaan/M5Stack-Avatar@^0.9.2` により、同梱の修正済みライブラリではなくレジストリの通常版が選ばれていました。通常版のAvatar::drawには共通グラフィックスロックがなく、描画スタックも2048バイトです。同梱版にあった共有ロックと8192バイトスタックの修正が、v9.12の生成物へ入っていませんでした。

ダンス側だけが `g_gfx_mutex` を取っても、顔描画側が同じmutexを取らなければ相互排他になりません。ダンスと顔が同じLCD/SPI transactionに同時に触り、mutexを取得したタスクとは別のタスクが解放してFreeRTOSの所有権assertに至りました。音声用タスクのPSRAM不足とは別の問題です。

## 修正

- Core2／CoreS3の両ビルド環境で `symlink://lib/M5Stack-Avatar` を明示。通常版への依存を削除。
- 同梱ヘッダーの `STACKCHAN_AVATAR_GFX_LOCKED=0x0913` をアプリからチェック。通常版・旧版ならコンパイル失敗にする。
- 同梱実装に `stackchan_avatar_gfx_lock_revision()` を追加し、起動時に呼び出す。ヘッダーだけ修正版で実装が通常版ならリンク失敗にする。
- mutex生成失敗と版不一致は、描画タスクを開始する前に検出。
- `setExpression()` も共通ロックへ変更。描画タスクをSPI transaction途中でsuspendしない。
- ダンスは削除せず、既存の共有ロックで顔描画と直列化。ニュース音声の専用タスク・PCM保持等のv9.12修正は維持。
- 配布パッケージ生成時も、v9.13のAvatarロックとMP3タスクの起動メッセージを含まないバイナリを拒否。

## 検証結果

- Core2ビルド成功。実際のELFの `Avatar::draw()` が `lib/M5Stack-Avatar/src/Avatar.cpp` を指すことをaddr2lineで確認。
- `Avatar::draw()` の機械語を逆アセンブルし、Face::drawの前後に `gfxLock()` と `gfxUnlock()` が呼ばれることを確認。
- ELFに `stackchan_avatar_gfx_lock_revision` の実装が存在することをnmで確認。
- Pythonテスト3件成功。実際のdraw／setExpressionの実装を抽出し、実際のGfxLockGuardと模擬FreeRTOS mutexを使って2スレッド各1万回の顔・ダンス競合を試験。入れ子の表情変更も停止せず完了。
- 実装の版チェックをC++プリプロセッサで試験。版なし／0x0912を拒否し、0x0913だけ受理。
- 既存の音声回帰テスト成功。配布データの復元サイズ・SHA-256も照合。
- 実機への書き込みと起動ダンス、ニュース、音質の再確認は未実施。ネイティブの競合試験は実際のESP32 SPIドライバーを実行するものではありません。

## 生成物と確認方法

- firmware.bin: **3,268,784 bytes**
- firmware.bin SHA-256: `639c3963ff59e064ab959b7916a74c560dfafbeefda83f52bf1b2d6a46b5281f`
- ELF SHA-256: `b376730a6d2c80e57d411024e1228ba90f7184bf2d56bdbde1a8651a07e2714f`
- ブートローダー、パーティション、boot_app0はv9.12と同じハッシュ。

書き込みページにv9.13と表示され、起動ログに以下が出ることを確認してください。

```text
[BOOT] Avatar gfx lock: vendored-v9.13 (shared mutex, draw stack=8192)
[BOOT] MP3 decoder task ready (core=1 priority=2 v9.13)
```

続いてダンスが完了し `[DANCE] done` が出ること、通常の顔表示・表情変更・ニュース再生が続くことを確認します。VOICEVOXの `isApiKeyValid: NG` / 429については引き続きキーとサービス側の確認が必要です。

既存のフル書き込みページはNVS/SPIFFSを初期化します。Wi-Fi／APIキーを再設定できる状態で書き込んでください。
