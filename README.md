# AI_StackChan2

AIｽﾀｯｸﾁｬﾝ2です。
<br><br>

![画像1](images/image1.png)<br><br>

## ✅ 実動作確認済み（2026-10-27）

このリポジトリのコードは、M5Stack Core2 実機での動作確認済みです。

| 項目 | 内容 |
|---|---|
| 確認日 | 2026-10-27 |
| 対象コード | commit `d0dc6e126e9ad5ac90a7d90efe59440c0d36af4e`（このREADME更新時点の main） |
| 対象機種 | M5Stack Core2（本体のみ。サーボ未接続でも起動・動作します） |
| ビルド | PlatformIO / env `m5stack-core2`（espressif32@6.3.2、M5Unified 0.1.16）<br>コマンド: `pio run -e m5stack-core2`（`M5Unified_AI_StackChan/` フォルダで実行） |
| 成果物 | firmware.bin 2,237,888 バイト<br>SHA-256: `a27bcbd761ac48069f0e67b28dcb1499c84102035fc3e1a6a3b6cbc609c4e596` |
| 書き込み方法 | Mac の Chrome（Web Serial）から書き込みページで書き込み。<br>ページ一式は private リポジトリ `2ndarystorage/stackchan-core2` の `ai_stackchan_flasher/` フォルダにあります（bootloader 0x1000 / partitions 0x8000 / boot_app0 0xe000 / firmware 0x10000、NVS・SPIFFS初期化付き） |
| 確認内容 | 書き込み完了→自動再起動→顔表示→正常動作を所有者が実機で確認 |

※ この確認ステータスは上記コミットのコードに対するものです。以後の変更は別途確認が必要です。

### 起動後の設定（WiFi / APIキー）

- microSD カードのルートに置く方法が確実です。
- `wifi.txt`：1行目に SSID、2行目にパスワード
- `apikey.txt`：1行目に OpenAI キー、2行目に VOICEVOX キー、3行目に STT キー（Whisper を使う場合は OpenAI キーと同じもの）
- カードを本体に入れて電源を入れ直すと接続され、画面に IP アドレスが表示されます
- WiFi 未設定のまま起動すると「Waiting for SmartConfig」を表示して再起動を繰り返します（故障ではありません）。スマホアプリ（EspTouch 等）で SmartConfig 送信も可能です
- 接続後はブラウザで `http://stack-chan.local`（または表示された IP）を開き、`/apikey`（APIキー設定）、`/role`（ロール設定）等のページが使えます

### 顔と表情について

- 顔は M5Stack-Avatar ライブラリが描画しています（画像ファイルではありません）
- 表情（Expression）ごとに右上等にエフェクトが出ます：Happy=ハート、Angry=怒りマーク、Doubt=汗、Sad=チルマーク、Sleepy=鼻ちょうちん
- ハートは主に「発話中」「会話開始時（『御用でしょうか？』）」に表示されます
- ブラウザから `http://stack-chan.local/face?expression=0〜5` で表情を切り替えられます（0:Neutral 1:Happy 2:Sleepy 3:Doubt 4:Sad 5:Angry）

---

AIｽﾀｯｸﾁｬﾝ2の特徴<br>

* 音声合成にWeb版 VOICEVOXを使います。
* 音声認識に"Google Cloud STT"か"OpenAI Whisper"のどちらかを選択できます。
<br>

Google Cloud STTは、”MhageGH”さんの [esp32_CloudSpeech](https://github.com/MhageGH/esp32_CloudSpeech/ "Title") を参考にさせて頂きました。ありがとうございました。<br>
"OpenAI Whisper"が使えるようにするにあたって、多大なご助言を頂いた”イナバ”さん、”kobatan”さんに感謝致します。<br>
ウェイクワードには、”MechaUma”さんの[SimpleVox](https://github.com/MechaUma/SimpleVox/ "Title")ライブラリを使わせていただきました。

---

### M5GoBottom版ｽﾀｯｸﾁｬﾝ本体を作るのに必要な物、及び作り方 ###
こちらを参照してください。<br>
* [ｽﾀｯｸﾁｬﾝ M5GoBottom版組み立てキット](https://raspberrypi.mongonta.com/about-products-stackchan-m5gobottom-version/ "Title")

### プログラムをビルドするのに必要な物 ###
* [M5Stack Core2](http://www.m5stack.com/)
* VSCode
* PlatformIO

使用しているライブラリ等は"platformio.ini"を参照してください。

~~~~

---

### サーボモーターを使用するGPIO番号の設定 ###
* main.cppの46行目付近、サーボモーターを使用するGPIO番号を設定してください。
* 現在のビルドでは Core2 PORT A（X: GPIO33 / Y: GPIO32）が有効です。

### 使い方 ###

こちらを参照してください。

* [AI_StackChan2_README](https://github.com/robo8080/AI_StackChan2_README/ "Title")
<br>
<br>
<br>

---

## Program Summary
* M5Stack-based “AI Stack-chan” that records audio, runs speech-to-text (Google Cloud STT or OpenAI Whisper), sends chat to OpenAI, and speaks responses via VOICEVOX TTS.
* Includes on-device avatar/face animation, optional servo movement, wake-word handling, and a small HTTP server for settings (API keys, role, speech/face controls).
* Stores configuration in NVS/SPIFFS and can load Wi-Fi/API keys from SD card files.

## Completion Status
Verified on real hardware (M5Stack Core2) on 2026-10-27 — see the verification section at the top of this README. Core features (STT, chat, TTS, avatar, wake word, settings server) are implemented; full conversation use requires Wi-Fi and API keys.
