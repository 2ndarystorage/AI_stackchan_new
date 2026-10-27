# AI_StackChan2（Akr カスタム版）

AIｽﾀｯｸﾁｬﾝ2です。
<br><br>

![画像1](images/image1.png)<br><br>

robo8080 さんの AI_StackChan2 をフォークし、M5Stack Core2 向けにカスタムした版です。

## 動作確認の状況

| バージョン | 内容 | 状況 |
|---|---|---|
| 素の版（commit `d0dc6e1`） | フォーク直後のコード | ✅ 実機確認済み（2026-10-27、Core2 本体のみ） |
| カスタム版 第1弾 | ペンギン顔＋声切り替え＋モデル切り替え＋応答高速化 | ✅ 実機確認済み（2026-10-27） |
| カスタム版 第2弾（現在の main） | 話速調整＋ニュース読み上げを追加 | ⚠️ **ビルド済み・実機動作は未確認**（2026-10-27 時点） |

- 第1弾の成果物：firmware.bin 2,244,496 バイト / SHA-256 `d197a829d2b524e6fd4b85621ee87a3383605e6588f1099b8b6b1a7dad833877`
- 第2弾の成果物：firmware.bin 2,249,984 バイト / SHA-256 `792759f2a1a2399439a52488f08033ea01083edf86060f97e90df5c9d91b1e40`

## カスタム版で追加・変更したこと

- **ペンギン顔**（`src/PenguinFace.h`）：顔を自作のペンギンに変更。白い顔パッチ＋黒い丸目＋クチバシ。まばたき・口パク・表情エフェクトは従来どおり動きます
- **声の切り替えページ `/voice`**：VOICEVOX の話者14種（ずんだもん3種、四国めたん2種、春日部つむぎ、雨晴はう、波音リツ、冥鳴ひまり、九州そら、もち子さん、玄野武宏、青山龍星、剣崎雌雄）を名前で選択・試聴できます。選んだ声は本体（NVS）に保存され、再起動後も維持されます
- **話速の調整**：同じ `/voice` ページで 1.0／1.1／1.2／1.3 倍を選べます（初期値 1.2 倍）。再生側で速くする方式のため、倍率に応じて声が少し高くなります。本体に保存されます
- **AIモデルの切り替えページ `/model`**：gpt-4o-mini（初期設定）／gpt-4.1-mini／gpt-4o／gpt-3.5-turbo から選べます。本体に保存されます
- **応答の高速化**：録音は話し終わると無音検出で自動終了（固定時間まで待たない）。ChatGPT の回答は最大150トークンに制限（ロール側で指定がある場合はそちら優先）
- **ニュース読み上げ**：「ニュース」という言葉を含む発話をすると、NHKニュース RSS から最新見出し3件＋短い説明を取得して読み上げます。ブラウザの `/news` からも実行できます

## 手順

### 1. ビルド

必要なもの：VSCode＋PlatformIO（または PlatformIO Core の CLI）

```bash
cd M5Unified_AI_StackChan
pio run -e m5stack-core2
```

成果物は `.pio/build/m5stack-core2/firmware.bin` です。
環境：espressif32@6.3.2、M5Unified 0.1.16（詳細は `platformio.ini`）。

### 2. 本体への書き込み

**方法A（推奨・ブラウザで書き込み）**
書き込みページ一式が private リポジトリ `2ndarystorage/stackchan-core2` の `ai_stackchan_flasher/` フォルダにあります。

1. そのリポジトリを ZIP でダウンロードして解凍する
2. 解凍したフォルダ内の `ai_stackchan_flasher` で `python3 -m http.server 8000 --bind 127.0.0.1` を実行する
3. Chrome で `http://localhost:8000` を開く
4. Core2 を USB で接続し、ページの案内に従ってシリアルポートを選んで書き込む

書き込み内容：bootloader（0x1000）／パーティション（0x8000）／NVS初期化（0x9000）／boot_app0（0xe000）／アプリ本体（0x10000）／SPIFFS初期化（0xc90000）。
※ NVS と SPIFFS が初期化されるため、書き込み後は WiFi・APIキー・ウェイクワードの再設定が必要です。

**方法B（esptool で直接）**
上記のオフセットに各バイナリ（bootloader.bin / partitions.bin / boot_app0.bin / firmware.bin）を書き込んでください。

**元に戻したい場合**
公式コミュニティ版 stack-chan v2.0.1 は公式 Web インストーラ（https://stack-chan.github.io/stack-chan/web/flash/）から書き直せます。

### 3. 初期設定（WiFi / APIキー）

microSD カードのルートに2つのファイルを作って入れる方法が確実です。

- `wifi.txt`：1行目に SSID、2行目にパスワード
- `apikey.txt`：1行目に OpenAI キー、2行目に VOICEVOX キー（tts.quest）、3行目に STT キー（Whisper を使う場合は OpenAI キーと同じもの）

カードを入れて電源を入れ直すと接続され、画面に IP アドレスが表示されます。
WiFi 未設定のまま起動すると「Waiting for SmartConfig」を表示して再起動を繰り返します（故障ではありません）。

### 4. 使い方

- **ウェイクワードの登録**（最初の1回だけ）：本体の **Bボタン（下段まん中）を2秒長押し** →「ウェイクワード登録開始」と表示されたら、呼び名にしたい言葉（例：「こむぎ」）を1回はっきり言う → 登録した声が再生され「登録終了」と出れば完了。ウェイクワードは文字ではなく自分の声で登録する方式です
- **会話の始め方**：**Aボタン（下段左）** を押して「ウェイクワード有効」にしてから登録した言葉で呼ぶ。または画面の「STT」表示あたりをタッチしても会話が始まります
- **ニュース**：「ニュース教えて」のように「ニュース」を含めて話しかけると、最新ニュースを読み上げます
- **電池残量**：Cボタン（下段右）で読み上げます
- **独り言モード**：放っておくと時々独り言を言います。Bボタン長押し（ウェイクワード登録）をすると止まります

### 5. ブラウザ設定ページ一覧

本体と同じ WiFi から `http://stack-chan.local`（開けない場合は本体表示の IP アドレス）で開けます。

| ページ | 内容 |
|---|---|
| `/voice` | 声（話者14種）の切り替え・試聴、話速（1.0〜1.3倍）の切り替え |
| `/model` | AIモデルの切り替え |
| `/news` | ニュース読み上げをその場で実行 |
| `/speech?say=文章` | 任意の文章を喋らせる（`&voice=話者ID` で一時的に声変更可） |
| `/face?expression=0〜5` | 表情変更（0:Neutral 1:Happy 2:Sleepy 3:Doubt 4:Sad 5:Angry） |
| `/setting?volume=&led=&speaker=` | 音量・LED・話者IDの直接設定 |
| `/apikey` `/role` | APIキー設定／ロール（キャラ設定）編集 |

### 顔と表情について

- 顔は M5Stack-Avatar ライブラリが描画しています（画像ファイルではありません）。カスタム版の顔は `src/PenguinFace.h` です
- 表情ごとにエフェクトが出ます：Happy=ハート（右上）、Angry=怒りマーク、Doubt=汗、Sad=チルマーク、Sleepy=鼻ちょうちん
- ハートは主に「発話中」「会話開始時（『御用でしょうか？』）」に表示されます
- サーボを使う場合の GPIO は `main.cpp` で設定（現在のビルドは Core2 PORT A：X=GPIO33 / Y=GPIO32）。サーボ未接続でも動作します

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

### 使い方（オリジナル版） ###

* [AI_StackChan2_README](https://github.com/robo8080/AI_StackChan2_README/ "Title")
<br>

---

## Program Summary
* M5Stack-based “AI Stack-chan” that records audio, runs speech-to-text (Google Cloud STT or OpenAI Whisper), sends chat to OpenAI, and speaks responses via VOICEVOX TTS.
* Includes on-device avatar/face animation, optional servo movement, wake-word handling, and a small HTTP server for settings (API keys, role, speech/face controls).
* Stores configuration in NVS/SPIFFS and can load Wi-Fi/API keys from SD card files.
* This fork adds: a penguin face, web pages for voice/speech-rate/model selection, faster responses (silence-based recording stop, max_tokens cap), and NHK news headline reading triggered by the word 「ニュース」.

## Completion Status
Stock code verified on real hardware (M5Stack Core2) on 2026-10-27. Custom v1 (penguin face, /voice, /model, speedups) also verified on device the same day. Custom v2 (speech-rate control, news reading) is built but **not yet verified on the device**.
