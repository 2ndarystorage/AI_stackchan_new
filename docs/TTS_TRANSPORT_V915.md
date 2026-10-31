# v9.15 音声取得・TLS処理の修正

更新: 2026-11-01。v9.14の実機ログで「合成成功後も再生されない」ことを受けた修正です。
**実機再生の成功はまだ確認していません。**

## ログで確認できたこと

- NHK RSSはHTTP 200、最初の合成もHTTP 200で成功。その後の音声取得でX509 -9984が発生しています。初回の合成以前に失敗した前回のログとは異なります。
- `isApiKeyValid: NG` はAPIキーが受理されていない表示ですが、その応答でも合成は成功しています。無料枠の制限・待ち時間は別に確認が必要で、今回の未再生をこの表示だけのせいにはできません。
- 取得失敗後、旧処理がストリーミング再生へ切り替え、合成タスクと同時にTLSを確保。続けてSSL -32512（メモリ確保失敗）が出ています。中断後にも古いダウンロードが続いたまま新しいRSS取得が始まっています。
- 空の合成結果もreadyキューへ積まれ、先読み待ちの後にスキップされていました。
- GoogleニュースはX509エラー。当日のPCからの観測ではWE2→GTS Root R4のチェーンで、旧ファームはR1だけを持っていました。
- 音声処理中の確認音がマイク／スピーカーを切り替え、I2S登録エラーも出ています。これをTLS取得失敗の唯一の原因とは判断していません。

## 修正内容

1. TLSのクライアントとHTTPClientを、リクエストごとに対で破棄・再生成。keep-aliveを無効にし、合成APIのTLSコンテキストを解放してから音声ホストへ接続します。
2. 共通の再帰ミューテックスで合成・音声取得・RSS・会話/STTのTLS処理を直列化。**取得失敗からのストリーム再生は廃止**し、完全に取得・検証できたMP3だけをPSRAMから再生します。失敗した文はエラーカウントに記録し、空のready項目を作りません。
3. APIが`audioStatusUrl`と`mp3DownloadUrl`を返すときは、完成通知を確認してから完成版を取得。完成待ちは最大90秒で、進行中のHTTP要求には別途タイムアウトがあります。両フィールドがない応答のみ従来のMP3 URLを全量取得します。取得中の無着信許容も5秒から20秒に変更しました。
4. 中断時はキャンセル要求と世代変更だけを行い、処理中フラグを先に消しません。待機・読み出しはキャンセルを確認し、処理が終了するまでは新しいニュース・録音を開始しません。進行中の同期HTTP/TLS呼び出し自体は即時中止できず、タイムアウトまで待つ場合があります。
5. 音声処理中の確認音は別チャンネルを使い、I2Sをマイクへ戻しません。
6. TTSに公式の自己署名Root YRを追加（ISRG Root X1も保持）。Googleに公式の自己署名GTS Root R4を追加（R1も保持）。証明書／ホスト名検証は維持し、`setInsecure()`は使っていません。
7. TLS診断に接続先ホスト名・epoch・空きヒープ・エラーコードを追加。URL全文・APIキー・読み上げ本文は追加ログに出しません。日本語URLエンコードの符号付きcharによる範囲外参照も修正しました。

文の生成完了を待つため、開始までの待ち時間は従来の先読み20秒より長くなる場合があります。文間無音や無料枠429を完全になくす修正ではありません。

## 証明書の取得元

- [Let's Encryptの証明書一覧](https://letsencrypt.org/certificates/)
- [ISRG Root X1](https://letsencrypt.org/certs/isrgrootx1.pem): DER SHA-256 `96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6`
- [Root YR](https://letsencrypt.org/certs/gen-y/root-yr.pem): DER SHA-256 `e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f`
- [GTS Root R4](https://pki.goog/repo/certs/gtsr4.pem): DER SHA-256 `349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d`

## 検証結果

- Core2ビルド: `pio run -d M5Unified_AI_StackChan -e m5stack-core2` 成功。既存依存ライブラリのdeprecated警告は残ります。
- `python3 -m unittest discover -s tests -v`: **10件成功**。実際のTLSゲートで2タスク各1万回の競合・再帰取得、製品コードから抽出した完成待ち関数（本物のArduinoJson＋HTTPモック）で未完成→完成・サービスエラー・不正JSON・キャンセル・タイムアウト・begin失敗を試験。実際のワーカー本体でも成功・失敗・古い世代の破棄を確認。日本語UTF-8エンコード、証明書、描画ロック、文章分割も試験しています。
- `c++ -std=c++11 -I tests/native tests/native/audio_regression.cpp -o /tmp/stackchan-audio-regression && /tmp/stackchan-audio-regression`: 成功。
- ファームに埋め込むCAバンドルだけを指定し、PCからTLS 1.2／SNI／ホスト名検証付きで`api.tts.quest`・`audio1.tts.quest`・`audio2.tts.quest`・`news.google.com`へ接続: 4ホスト成功。**TLSハンドシェイクのみ**で、合成HTTP要求・APIキー送信はしていません。
- 配布生成スクリプトはv9.13描画ロック・v9.15 MP3/TTSマーカーとX1/YR/R4の実証明書指紋を要求。v9.14の旧バイナリを渡す負の試験は、想定どおり拒否し配布ファイルを変更しませんでした。
- `node scripts/verify-firmware.mjs BUILD_DIR BOOT_APP0_BIN`: 最終再ビルド後、再梱包前の試験では旧生成物との不一致を検出しました。再生成後は全パーツのサイズ・SHA-256・アドレス・初期化サイズ・ローカルビルドとのバイト一致に成功。
- 配布の4つの対象JSモジュールの構文検査成功。localhostでindex/main/firmware-dataのHTTP 200、モジュールMIME、原ファイルとのバイト一致を確認。

PCではTTSの断続的なX509失敗そのものを再現できていません。X1だけでも現在のTTSホストをPCから検証できるため、**TTSの全X509失敗をCA不足と断定はしていません**。接続再生成・直接のRoot YR信頼・診断ログを追加した対策版です。ESP32側のTLS検証、未観測の配信先ホスト、実際のサービス応答／生成待ち／再生を上記PC・モック試験だけで保証することはできません。

## 生成物

- firmware.bin: **3,271,024 bytes**
- firmware.bin SHA-256: `d1ebe70bec4546c7340033b31444ccb8329f2b298d00fc008aac7e8caa63fbc0`
- ELF SHA-256: `30aff6b64b7773e217f5bf215ffcee241d6e23dc58bca8d93dd30587e5310e8b`
- ブートローダー・パーティション・boot_app0・NVS/SPIFFS初期化範囲は前版と同じです。
- [ソース](https://github.com/2ndarystorage/AI_stackchan_new/tree/codex/news-audio-v912)と[配布](https://github.com/2ndarystorage/stackchan-core2/tree/codex/news-audio-v912)は専用ブランチ`codex/news-audio-v912`を継続。mainへの統合・force pushはしていません。

## 実機での確認手順

1. [専用ブランチのZIP](https://github.com/2ndarystorage/stackchan-core2/archive/refs/heads/codex/news-audio-v912.zip)を取得・解凍し、`ai_stackchan_flasher`内で`python3 -m http.server 8000 --bind 127.0.0.1`を実行。Chromeの`http://localhost:8000/`で**v9.15**の表示を確認します。ルートのMOD書き込みページではありません。
2. フル書き込みは従来どおり**NVS/SPIFFSを初期化**します。Wi-Fi／APIキーを再設定できる状態でユーザーが書き込みます。
3. 起動ログを確認します。Avatarのv9.13はロック実装の版で、配布ファーム版はv9.15です。

```text
[BOOT] Avatar gfx lock: vendored-v9.13 (shared mutex, draw stack=8192)
[BOOT] TTS CA: ISRG Root X1 + Root YR (v9.15)
[BOOT] MP3 decoder task ready (core=1 priority=2 v9.15)
```

4. ニュースを1回だけ開始し、`synth code: 200`→`completed audio ready`（対応応答の場合）→`download OK`→`play start (memory)`まで進むか確認します。生成待ちは上記上限までかかる場合があります。
5. 失敗時は`[TLS]`行を含めて起動からのログを確認します。APIキーを含むURLや設定値は共有しないでください。`isApiKeyValid: NG`／429が続く場合のキー・サービス制限確認は、この修正と別に必要です。

**課金API試験・本体への書き込み・実機の起動／音声／中断操作確認は未実施**です。
