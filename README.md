# Core2 Stackchan 音声会話

M5Stack Core2 + SG90 ×2 のスタックチャンで、ChatGPTとClaudeに日本語で話しかけるためのファームウェアとPC用中継サーバーです。あたる・ラム・スタックちゃんの顔、視線に連動した首振り、音声に合わせた口の動きを使います。

元プロジェクト: [robo8080/M5Core2_SG90_StackChan_VoiceText_Ataru](https://github.com/robo8080/M5Core2_SG90_StackChan_VoiceText_Ataru)

## 現在の状況

更新日: **2026-10-27（日本時間）**

- 標準ファームは `USE_NETWORK_VOICE=1`。Web設定とネットワーク音声会話を使います。
- Web設定から **ChatGPT Realtime / ChatGPT / Claude** を選べます。
- ChatGPT Realtimeは音声を逐次送受信。ChatGPTとClaudeの通常モードは、無音検出 → OpenAIで文字起こし → 選んだAIの応答 → OpenAIで読み上げ、の順です。
- 音声は24kHz・16bit・モノラルPCM。録音と再生を交互に行い、読み上げ中の割り込みには対応していません。
- Wi-Fi、PC接続先、AI、共有トークン、音量をWebから設定し、本体に保存します。クラウドのAPIキーはPC側だけに置きます。
- Realtimeモードには、音声から本体タイマーの開始・確認・取消とPCの時刻取得を行う機能があります。通常のChatGPT／Claudeモードにはツール実行をまだ接続していません。
- 自動テスト: サーバー12件とC++タイマーテストが成功。クラウドAPIはモックで代替しています。
- Core2向けビルド: **成功**（PlatformIO、ESP32 Arduino 2.0.17）。USB接続されたCore2は検出されていません。書き込み・マイク・スピーカー・サーボ・有料APIを通した会話は未検証です。

「コードがあること」「テスト成功」「実機確認」を分けて記録します。[検証環境と結果](docs/VALIDATION.md)も参照してください。

## 必要なもの

- M5Stack Core2。録音には内蔵マイクを含む底面基板が必要です。
- SG90 ×2、対応するスタックチャンの組み立てキットと電源。
- 同じ信頼できるLANにつながったPC。会話中は中継サーバーを起動しておきます。
- OpenAI APIキー。Claudeの会話を選ぶ場合はAnthropic APIキーも必要です。
- Python 3.11以降とPlatformIO。API利用料は各提供元の契約に従います。

Core2のサーボ配線は **X=GPIO13、Y=GPIO14**。組み立て済み機体の配線と可動範囲を確認してください。CoreS3用の配線には置き換えないでください。

## PCサーバーの起動

リポジトリのルートで実行します。

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r server/requirements.txt

export OPENAI_API_KEY='自分のOpenAI APIキー'
export ANTHROPIC_API_KEY='自分のAnthropic APIキー'
export BRIDGE_TOKEN="$(python -c 'import secrets; print(secrets.token_urlsafe(32))')"
python server/voice_bridge_server.py
```

`BRIDGE_TOKEN` は本体にも入力する共有トークンです。16文字以上の印字可能なASCIIを使います。現在のシェルで値を確認して本体の設定画面に入力してください。`.env.example` は設定例で、自動読み込みはしません。

既定の待受は `0.0.0.0:8000`。PCのファイアウォールでは信頼できるLANからの接続を許可します。Core2との通信はトークン認証付きHTTP/WebSocketなので、インターネットに直接公開しないでください。

ヘルスチェック（APIキーやトークンは返しません）:

```sh
curl http://127.0.0.1:8000/health
```

### 主な環境変数

| 変数 | 既定値 | 用途 |
|---|---|---|
| `OPENAI_API_KEY` | 未設定 | すべての音声モードで必須 |
| `ANTHROPIC_API_KEY` | 未設定 | Claude選択時に追加で必要 |
| `BRIDGE_TOKEN` | 未設定 | 本体と共通の16文字以上のトークン |
| `OPENAI_REALTIME_MODEL` | `gpt-realtime` | Realtime会話モデル |
| `OPENAI_REALTIME_VOICE` | `marin` | Realtimeの声 |
| `OPENAI_CHAT_MODEL` | `gpt-4o-mini` | 通常のChatGPT会話モデル |
| `ANTHROPIC_MODEL` | `claude-sonnet-4-6` | Claude会話モデル |
| `OPENAI_TRANSCRIBE_MODEL` | `whisper-1` | 通常モードの文字起こし |
| `OPENAI_TTS_MODEL` | `gpt-4o-mini-tts` | 通常モードの読み上げ |
| `OPENAI_TTS_VOICE` | `coral` | 通常モードの声 |
| `VAD_SILENCE_MS` | `600` | 発話終了と判定する無音時間（200〜2000ms） |
| `PCM_VAD_THRESHOLD` | `500` | 通常モードの音声検出RMSしきい値。実機に合わせて調整 |
| `REALTIME_SESSION_SECONDS` | `300` | 各音声セッションの上限秒数 |
| `MAX_TURNS` | `6` | 通常モードの履歴の最大往復数 |
| `SYSTEM_PROMPT` | 短く日本語で話すStackchan | 会話の性格・指示 |
| `HOST` / `PORT` | `0.0.0.0` / `8000` | PCの待受アドレスとポート |

モデル名は環境変数で変更できます。キーの権限とモデルの提供状況は各API側で確認してください。通常モードの履歴はAIと端末IDごとにPCメモリへ保持し、サーバー再起動で消えます。

## 本体のビルドと設定

```sh
source .venv/bin/activate
python -m pip install platformio==6.2.0
pio run -e core2
```

`platformio.ini` でCore2ボードと依存ライブラリを固定しています。ビルド成果物は `.pio/build/core2/` に生成されます。

Core2をUSB接続し、ポートを確認したうえで書き込みます。

```sh
pio device list
pio run -e core2 -t upload --upload-port /dev/cu.実際のポート名
pio device monitor -b 115200 --port /dev/cu.実際のポート名
```

1. 初回起動時に本体の設定画面を開きます。通常時はCを1.5秒長押しでも開けます。
2. スマホまたはPCを、本体画面に表示された `Stackchan-xxxx` のWi-Fiに接続します。パスワードも本体画面に表示されます。
3. ブラウザで `http://192.168.4.1/` を開きます。ユーザー名は `admin`、パスワードは画面に出ているWi-Fiパスワードです。
4. 自宅のWi-Fi、PCのLAN内IPまたはホスト名、ポート、会話AI、`BRIDGE_TOKEN`、音量を入力します。ホストには `http://` やパスを含めません。
5. 保存すると再起動します。スマホ／PCも元のLANに戻し、Aで会話を開始します。

設定パスワードは本体再起動で変わります。Wi-Fi接続に15秒間失敗すると設定画面へ戻ります。通常モードでは、発話後に約600msの無音を置くと応答生成を開始します。1発話の録音上限は約15秒です。

### 操作

| ボタン | 動作 |
|---|---|
| A | 設定したAIとの音声セッションを開始／停止 |
| B | 顔をあたる → ラム → スタックちゃんの順に切替 |
| Cを短押し | 会話停止／設定画面を閉じる |
| Cを1.5秒長押し | Web設定を開く |

読み上げる声はAI生成です。録音中はサーボを止めて機械音を減らします。無音が長い場合や接続が切れた場合は停止するので、Aで再接続します。

Realtimeで試せる例: 「3分のタイマーをかけて」「あと何秒？」「タイマーを取り消して」「今何時？」。タイマーは本体の電源を切ると消え、同時に1つだけ動作します。

## テスト

APIキーや本体がなくても実行できます。

```sh
python -m unittest discover -s server -p 'test_*.py'
c++ -std=c++11 server/test_device_timer.cpp -o /tmp/stackchan-timer-test
/tmp/stackchan-timer-test
pio run -e core2
```

サーバーテストは認証、PCMの再生待ち制御、ツール引数と本体からの応答待ち、無音検出、Claudeへの振り分け、WAV生成を確認します。実際のクラウド応答やマイク音質は保証しません。GitHubへpushすると `.github/workflows/core2.yml` でもサーバー・タイマーのテストとCore2ビルドを実行します。

## 構成とAPI

- `NetworkVoice.h`: 標準ファームのWeb設定・録音・再生・顔・サーボ。
- `DeviceTimer.h`: 通信から独立した本体タイマー。
- `LegacyFirmware.h`: 従来の顔表示／VoiceText方式を保存したコード。
- `server/voice_bridge_server.py`: aiohttpによる音声中継。
- `server/.env.example`: PCの設定例。
- `docs/FEATURE_IDEAS.md`: 以前の拡張案調査を保存した記録。

| エンドポイント | 用途 |
|---|---|
| `GET /health` | サーバー稼働確認（認証不要） |
| `GET /v1/realtime` | OpenAI Realtimeとの音声WebSocket |
| `GET /v1/voice?backend=openai` | 通常ChatGPTの音声WebSocket |
| `GET /v1/voice?backend=claude` | Claudeの音声WebSocket |
| `POST /v1/chat` | 従来のWAV→応答テキストJSON API |

`/health` 以外は `Authorization: Bearer <BRIDGE_TOKEN>` が必要です。WAV APIでは `Content-Type: audio/wav`、`X-Backend: openai` または `claude`、任意の `X-User-Id` を渡します。音声WebSocketは1台ずつ接続します。

従来ファームを使う場合は `.ino` の `USE_NETWORK_VOICE` を0に変更します。VoiceTextの音声用 `.cpp` はビルド全体に `STACKCHAN_LEGACY_AUDIO` の定義が必要です。従来ファームは今回の検証対象外で、旧サーバーとの認証互換も別途確認が必要です。標準のネットワーク音声モードにVoiceTextキーは不要です。

## GitHubへの進捗保存

保存先: [2ndarystorage/AI_stackchan_new の専用ブランチ](https://github.com/2ndarystorage/AI_stackchan_new/tree/codex/core2-voice-20261027)。既存mainとは構成・履歴が異なるため、このブランチをCore2版の保存先として使います。

作業の区切りと、Codexの毎時の定期実行で、変更があれば関連ファイルをコミットし、専用ブランチにpushします。定期実行はCodexアプリ側の機能で、GitHub Actionsによる無人保存ではありません。実行できなかった場合は次回の状況報告で確認します。

保存するのはソース、設定例、テスト、説明書です。APIキー、Wi-Fi認証情報、`.env`、`.venv`、`.pio` は保存しません。変更のない回は空コミットを作りません。既存のmainへの統合やforce pushは行いません。保存先と実際の検証結果はこのREADMEとスレッドで記録します。

## 次の実機確認

1. Core2をUSB接続して、生成済みのファームを書き込み。
2. Web設定とLAN接続、マイク録音と読み上げを確認。
3. ChatGPTとClaudeの両方で会話を確認。
4. サーボの可動範囲、口の動き、タイマー、切断からの復帰を確認。
5. 音声検出しきい値と応答までの時間を実測して調整。

## 参考・ライセンス

- [M5Unified](https://github.com/m5stack/M5Unified)、[M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
- [Core2公式仕様](https://docs.m5stack.com/en/core/Core2)
- [OpenAI Realtime会話](https://developers.openai.com/api/docs/guides/realtime-conversations)、[音声合成](https://developers.openai.com/api/docs/guides/text-to-speech)
- [Claude Sonnet 4.6](https://platform.claude.com/docs/en/models/sonnet-4-6/overview)
- 本体組み立て: [M5GoBottom版キット](https://raspberrypi.mongonta.com/about-products-stackchan-m5gobottom-version/)

元リポジトリの `LICENSE` と、各ソースに記載された著作権・ライセンス表示を参照してください。VoiceText由来のファイルにはGPL表記があります。
