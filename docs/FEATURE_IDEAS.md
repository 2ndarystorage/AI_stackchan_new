# 拡張機能の調査メモ

以下は2026-10-16時点の調査記録です。実装状況はルートのREADME.mdを参照してください。

## 今のハードで追加できる機能の提案

調査日：**2026-10-16**。ここでいう「追加できる」は、公開プロジェクトの対応表とCore2の仕様を根拠にした実現性の判断です。以下はこのリポジトリには未実装です。「追加部品なし」はCore2・SG90・既存PCを使う前提で、外部サービスの契約やAPI利用料が不要という意味ではありません。

### ハードウェアの前提

対象は **M5Stack Core2 + SG90 ×2 + M5GoBottom系の組み立てキット**です。Core2の標準仕様にはESP32、16MB Flash、8MB PSRAM、タッチ画面、Wi-Fi、スピーカー、RTC、マイク、IMUがあります。マイクとIMUは底面側の基板にあるため、実際の組み替え後も残っているか確認が必要です。[Core2公式仕様](https://docs.m5stack.com/en/core/Core2)

カメラは標準搭載されていません。CoreS3向けの画像認識や新型StackChan向けのシリアルサーボ制御を、そのまま適用することはできません。

### 優先順位付き候補

| 優先度 | 追加候補 | 得られる体験 | 今の構成での実現方法・条件 | 参考 |
|---|---|---|---|---|
| P1 | Web設定画面 | スマホからWi-Fi・接続先・音声設定を変更 | Core2に設定用APとWeb画面を追加し、内蔵ストレージへ保存。追加部品なし | [AI_StackChan_ExのWeb設定](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/realtime_api.md) |
| P1 | 音声合成の選択 | VoiceText以外の声で話す | 通常のSTT→LLM→TTS方式で音声プロバイダーを切替。PCで音声を生成しCore2で再生する案。再生形式の対応が必要 | [Core2のTTS対応表](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/basic_usage.md) |
| P1 | 時計・ポモドーロ・アラーム | 作業時間や終了時刻を顔・音・首振りで通知 | Core2の画面・RTCとSG90を利用。ネットなしの基本タイマーから着手可能 | [AI_StackChan_Exのアプリ例](https://github.com/ronron-gh/AI_StackChan_Ex) |
| P2 | 低遅延の音声会話 | 固定4秒録音より自然なやり取り | PC中継を音声ストリーミング対応に拡張し、Core2に送受信バッファと状態管理を追加。遅延は実測が必要 | [Realtime / Gemini Live実装例](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/realtime_api.md) |
| P2 | 会話から道具を使う | 「3分タイマー」などを会話で実行 | PC側でFunction Calling / MCPを処理し、許可した操作をCore2に返す設計案。外部機器の操作には対応サービスが必要 | [Function Calling / MCPの例](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/realtime_api.md) |
| P2 | Bluetoothスピーカー | PCやスマホの音に合わせて顔と首が動く | Core2対応のA2DP実装を移植。AI会話とはモードを分け、Wi-Fiとの共存とメモリ使用量を検証 | [stackchan-bluetooth-simple](https://github.com/mongonta0716/stackchan-bluetooth-simple) |
| P3 | 顔・手への追従 | 画面の向こうの人へ視線と首を向ける | PCのカメラでMediaPipeを実行し、BLE等で姿勢を送る。既存PCにカメラがある場合は本体への追加部品不要 | [本家のMediaPipe BLE tracking](https://github.com/stack-chan/stack-chan) |
| P3 | MOD・ブラウザ開発への移行 | 表情やミニアプリをブラウザで作る | 本家のModdable系へファームウェアを移行する別案。現在のArduinoコードへ直接追加する方式ではない | [stack-chan/stack-chan](https://github.com/stack-chan/stack-chan) |

### 互換性で確認すべき点

- **SG90配線を維持する**：このコードのCore2用ピンはX=13、Y=14です。参考プロジェクトの標準値が別のポートやシリアルサーボになっていても、実配線に合わせます。AI_StackChan_ExはPWM方式のSG90設定を掲載しています。[サーボ設定例](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/basic_usage.md)
- **RealtimeとTTSの組み合わせは別判定**：AI_StackChan_ExのRealtime＋外部TTS対応表では、Core2のVOICEVOX併用は非対応、AquesTalkは対応です。通常会話モードのVOICEVOX対応とは区別します。PCでTTSを生成する案も、この構成での実装・検証が必要です。[Realtime対応表](https://github.com/ronron-gh/AI_StackChan_Ex/blob/main/doc/realtime_api.md)
- **本家の対応範囲を確認する**：本家はCore2対応とTakao Core2＋SG90向けソースビルドを記載していますが、物理デバイスでのリリース検証対象はM5StackChan CoreS3です。現在のM5GoBottom配線への適合は別途確認します。[対応ハード](https://github.com/stack-chan/stack-chan)
- **常時待受は後から評価する**：ボタンで会話を開始する方式を先に安定させます。ウェイクワード、発話の割り込み、同時録音・再生は、マイク構成と音響処理まで含めて検証が必要です。

### おすすめの実装順

1. **現行機能の動作を確立する**：依存ライブラリを固定し、通常表示・TTS・録音・AI会話を個別に確認。通信のタイムアウト、Wi-Fi再接続、録音完了待ち、エラー表示を整える。
2. **日常的に使える機能を増やす**：Web設定、音声合成の切替、時計・ポモドーロを追加。あたる・ラムの顔と既存の首振りを活かす。
3. **会話を自然にする**：ストリーミング音声と無音検出を導入。録音開始から最初の応答音声までを計測して改善する。
4. **外部連携を広げる**：PC側の道具実行、必要ならカメラ追従を追加。Bluetoothは独立モードとして検証する。

最初の拡張は **「Web設定＋時計／ポモドーロ＋音声合成切替」** を推奨します。現行のArduino構成を段階的に拡張でき、追加ハードなしで使い勝手を改善できます。低遅延音声会話はその次の重点候補です。

### GitHub調査の参照先

| リポジトリ／資料 | 調査で確認した内容 | このプロジェクトへの位置づけ |
|---|---|---|
| [ronron-gh/AI_StackChan_Ex](https://github.com/ronron-gh/AI_StackChan_Ex) | Core2対応、複数の音声サービス、Realtime、Web設定、ツール実行、ミニアプリ | 機能追加の主な参考。完成ファームの無条件互換を意味しない |
| [mongonta0716/stackchan-bluetooth-simple](https://github.com/mongonta0716/stackchan-bluetooth-simple) | Core2対応、A2DPスピーカーとアバター・サーボ | Bluetooth専用モードの参考 |
| [stack-chan/stack-chan](https://github.com/stack-chan/stack-chan) | Core2、SG90向けビルド、ブラウザツール、外部カメラ追従 | 大きく開発方式を変える場合の比較対象 |
| [m5stack/StackChan](https://github.com/m5stack/StackChan) | CoreS3と専用サーボ等を使う新型製品の公開コード | 現Core2＋SG90用のファームウェアとして扱わない |

参照先は調査時点の公開ブランチの内容です。移植時には採用コミット、依存関係、ライセンスを確認し、この機体での確認結果を記録してください。

---

