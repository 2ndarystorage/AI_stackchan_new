# 検証記録

検証日: 2026-10-27（日本時間）

## ローカルで確認した結果

| 対象 | 結果 | 確認した範囲 |
|---|---|---|
| Pythonサーバーテスト | 12件成功 | 認証、入力拒否、PCMの流量制御、タイマー用ツールの検証、無音検出、Claude振り分け、WAV生成、WebSocket音声往復と再開 |
| C++タイマーテスト | 成功 | 無効な秒数、既存タイマーの上書き拒否、残り時間、終了通知、millisの周回、取消 |
| Core2ファームウェア | ビルド成功 | 標準の `USE_NETWORK_VOICE=1`、リンクとESP32用バイナリ生成 |
| USBシリアル列挙 | Core2を検出せず | debug-consoleとBluetooth用ポートのみ |

APIプロバイダーはテスト中にモックへ置き換えています。APIキーを送信したクラウド試験、実機への書き込み、マイク入力、スピーカー出力、サーボの可動範囲、実測の会話遅延は未確認です。

## 環境

- macOS / Apple Silicon
- Python 3.15、aiohttp 3.14.5、OpenAI Python SDK 2.54.0、requests 2.34.2
- PlatformIO Core 6.2.0
- Espressif32 platform 6.9.0 / Arduino ESP32 2.0.17
- M5Unified 0.2.7、M5GFX 0.2.35、M5Stack-Avatar 0.9.2
- ServoEasing 3.4.0、ESP32Servo 1.1.1
- ArduinoJson 6.21.5、WebSockets 2.6.1、ESP8266Audio 1.9.7

ファームのバージョンは `platformio.ini` に固定しています。サーバー依存は `server/requirements.txt` の範囲指定で、上記は今回実際にインストールされたバージョンです。GitHub ActionsはPython 3.12で別途確認します。

## 実行コマンド

```sh
python -m unittest discover -s server -p 'test_*.py'
c++ -std=c++11 server/test_device_timer.cpp -o /tmp/stackchan-timer-test
/tmp/stackchan-timer-test
pio run -e core2
pio device list
```

ビルド結果: Flash使用量 1,224,145 bytes。本体の設定保存用領域を含むボード定義の最大プログラムサイズは6,553,600 bytes。生成物は `.pio/build/core2/firmware.bin` です。生成物はGitへ含めません。

ビルドにはESP32フレームワーク内の `ADC_ATTEN_DB_11` 非推奨警告が2件あります。今回のビルドは終了コード0で完了しました。

## 実機で次に確認すること

1. 初回設定APへ接続し、Wi-Fi・PC・トークン・AI設定が再起動後も残るか。
2. Aで録音を開始し、無音検出・応答・読み上げ・再録音へ進めるか。
3. Realtime、通常ChatGPT、Claudeそれぞれで会話できるか。
4. 録音中のサーボ停止、再生中のリップシンク、Bの顔切替が働くか。
5. Cによる停止、Wi-Fi断、PC停止、APIエラーの後に再開できるか。
6. Realtimeのタイマーが本体で作動し、終了時に鳴るか。

従来の `USE_NETWORK_VOICE=0` 構成は今回のビルド対象に含めていません。
