# v9.14 TTS証明書更新

更新: 2026-11-01。

## 原因と差し替え

TTSの `WebVoiceVoxRootCA.h` に、ISRG Root X1のDST Root CA X3クロス署名版（notAfter=2024-09-30）が残っていました。TTSへ接続する前のTLS検証で `X509 - Certificate verification failed` となり、HTTPの音声合成リクエストに到達せず、各文を再試行後にスキップしていました。

Let’s Encrypt公式の自己署名版ISRG Root X1へ置き換えました。証明書の期限欄だけを書き換える方法ではなく、公式PEM全体の差し替えです。

- 取得元: https://letsencrypt.org/certs/isrgrootx1.pem
- 公式説明: https://letsencrypt.org/certificates/
- Subject / Issuer: C=US, O=Internet Security Research Group, CN=ISRG Root X1
- DERのSHA-256: `96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6`
- notBefore: 2015-06-04 11:04:38 UTC
- notAfter: 2035-06-04 11:04:38 UTC（信頼ポリシー上の利用期間とは別です）

TLS証明書・ホスト名検証は維持し、`setInsecure()` は使用していません。時刻が誤っている場合もTLS検証は失敗するため、本体のNTP同期は引き続き必要です。接続先のCAやチェーンが変われば、その時点で再確認が必要です。

## 検証

- 取得したPEMのDER SHA-256が公式ISRG Root X1の識別値と一致することを確認。
- このCAだけを指定し、PCから `api.tts.quest:443` にTLS 1.2・SNI付きで接続。証明書とホスト名の検証に成功しました。発行者はLet's Encrypt YR1でした。
- 接続試験はTLSハンドシェイクだけで、音声合成のHTTP要求・APIキー送信・課金API試験はしていません。音声ダウンロード先ホストやESP32実機のTLS処理は、このPC試験だけでは保証しません。
- `pio run -d M5Unified_AI_StackChan -e m5stack-core2`: 成功。
- Pythonテスト6件とC++音声回帰テスト成功。証明書のフィンガープリント・自己署名・現在の有効性・setCACert維持も試験。
- 配布base64全パーツの復元サイズ・SHA-256を検証。アプリはローカル生成物とバイト一致。
- パッケージ生成時にも、新CAのフィンガープリントとv9.14のTTS起動マーカー、Avatarロックのマーカーを検証します。

## 生成物

- firmware.bin: **3,268,912 bytes**
- firmware.bin SHA-256: `20c263741056bf9d2dc8ab98463d3848395da6c494b08be3640ad01240b552a8`
- ELF SHA-256: `6abeb5825f383390dc2b57a918abc8af45cea30691ba1f748b9faca2d0a1b6ed`
- ブートローダー・パーティション・boot_app0は前版と同じハッシュ。
- Avatarロック修正とMP3専用タスクの修正を維持しています。Avatarのロック版表示はv9.13のまま、配布ファームはv9.14です。

## 実機で確認すること

書き込みページにv9.14と表示され、起動ログに以下が出ることを確認してください。

```text
[BOOT] TTS CA: ISRG Root X1 self-signed (v9.14)
```

ニュースを開始し、`[TTS] synth code: 200`、`[TTS] download OK`、音声再生まで進むことを確認します。`isApiKeyValid: NG` やHTTP429が残る場合は、証明書とは別にVOICEVOXキーとサービス側の制限を確認してください。

**本体への書き込み・実機の読み上げ確認は未実施です。** フル書き込みページは従来通りNVS/SPIFFSを初期化するので、Wi-Fi／APIキーを再設定できる状態で書き込んでください。
