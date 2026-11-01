# v9.18 TLSメモリ確保の修正と再現試験

更新: 2026-11-01。

**確定したのは「正しい証明書でも、メモリ確保失敗が -9984／flags=0x08 に化ける経路」と、その再現条件での修正効果です。実機v9.18でもTLS接続3回の成功を確認しました。ただし実機個体の原因がこれだけだったこと、およびニュース音声の復旧はまだ確認できていません。** 合成・音出しを行わない接続試験の成功だけを、実機での完全解消と表現しません。

## 証拠と原因の切り分け

今回のログは`api.tts.quest / 164.70.91.214`で、`flags=0x00000008`（信頼CAに対する署名検証失敗）。以前の実機ログには`BIGNUM -16 / Memory allocation failed`も記録されています。

1. 同じ接続先から公開証明書チェーンをTLS 1.2・SNI付きで取得。葉はapi.tts.quest、発行者YR1、Root YRのX1クロス署名を含む3枚でした。葉のDER SHA-256は`39c60b48335c3312c649470fa138831c32ec41f2b51418d03f9b0a999f4b9632`、有効期間は2026-10-22〜2027-01-20です。製品内のX1/YRを変更せずPCで検証できました。ただし、実機が受信した葉証明書自体を今回のログから取得できたわけではありません。
2. 固定Arduino環境はMbed TLS 2.28.1で、SDK設定は`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1`。デフォルトの`esp_mbedtls_mem_calloc`はPSRAMが空いていても内部RAMだけに割り当てます。TLS入出力バッファだけでも約32KiBを使い、証明書・RSA署名検証の追加確保と同時に必要になります。
3. Mbed TLS 2.28.1の証明書探索では、署名検証関数の失敗を`signature_is_good=false`に変換する箇所があります。RSAのメモリ確保失敗もここで不正署名扱いとなり、最終的に`MBEDTLS_X509_BADCERT_NOT_TRUSTED=0x08`として現れ得ます。**flags=0x08だけではCAが違うと断定できません。**
4. 同じ実証明書・同じCAを使い、C++試験で内部RAMの予算を46,400 bytesに制限。32KiBのTLSレコード相当バッファも同時に確保すると、旧方針は`-9984 / flags=0x08 / allocation_failures=4`を再現しました。製品と同じ新しい割当方針では`0 / flags=0 / failures=0`となり、20回連続で検証成功。誤CAと誤ホスト名は引き続き拒否し、全メモリを解放しました。

```text
REPRO internal budget=46400: old code=-9984 flags=0x00000008 allocation_failures=4
FIX same budget: code=0 flags=0x00000000 allocation_failures=0 external_calls=3
PASS: 20 fixed verifications; wrong CA/hostname rejected; zero memory leaks
```

これは実際のMbed TLS検証コードを動かした試験です。ただし内部予算・TLSレコード相当の同時確保をモデル化しており、ESP32のハードウェア暗号・断片化・他タスク・当該実機のメモリ状態をそのまま再現したものではありません。別の原因をこの試験だけで排除はできません。

## 修正内容

- 公開API`mbedtls_platform_set_calloc_free`を使い、Wi-Fi開始前に一度だけ割当方針を設定。
- **4KiB以上のMbed TLS割当はPSRAMを優先**。小さい割当は従来どおり内部RAM。PSRAM確保に失敗した場合だけ内部RAMへフォールバックします。主なTLSレコードバッファを外へ移し、内部RAMを署名検証等へ残します。
- 解放は従来と同じ`heap_caps_free`。既存内部RAMの割当も、新しいPSRAMの割当も同じ方法で安全に解放できます。サイズ乗算のオーバーフローを拒否。
- 再帰ログや動的Stringをアロケーター内で作りません。短いクリティカルセクションで割当回数・最終失敗サイズ・失敗タスクを記録し、ハンドシェイクの前後で集計します。
- 同じタスクのハンドシェイク中に確保失敗が発生した場合は、証明書失敗をCAの恒久エラーとして即停止する前に、メモリ失敗として既存の有限再試行へ振り分けます。単に検証flagsを消す処理ではありません。
- CA、SNI、必須証明書検証を維持。`setInsecure()`、HTTPへの変更、検証エラーの成功扱いはありません。v9.17の出力・音量修正、左上版表示も維持します。

起動ログ:

```text
[BOOT] TLS memory: PSRAM >=4096B, internal small allocations
[BOOT] Firmware v9.18
```

接続ごとの`[TLS] memory policy=1 external_allocs=... failures=... last_failed_bytes=... this_task_failure=...`で、実際に方針が使われたかと割当失敗を確認できます。`last_failed_bytes`は累積の最終失敗値であり、当該接続の失敗有無は`failures`差分と`this_task_failure`を確認します。

### メモリ上の注意

TLSの大きい一時バッファには平文の通信データも含まれ得るため、この方針ではそれもPSRAMへ置かれます。証明書検証と通信路の暗号化を維持する修正であって、Core2の外部RAMを物理観測する攻撃への耐性を保証するものではありません。SDK公式も内部RAM以外の方針を提供していますが、内部RAMをセキュリティ面で推奨しています。APIキーをログや診断JSONへ追加出力することはありません。

## APIを呼ばない実機TLSテスト

v9.18を書き込んだ後、同じWi-Fiで`http://stack-chan.local/audio`（または本体IPの`/audio`）を開くと、**TLS接続テスト（HTTP・API要求なし）**を実行できます。

- `/tls_test`はPOST＋専用ヘッダーを要求。会話／合成／再生中は409で拒否。
- 接続先はapi.tts.quest:443固定。CAとホスト名を検証するTLSハンドシェイクと切断だけを行い、HTTP要求・本文・APIキーを一切送りません。HTTP接続前のエラーを、課金APIなしで切り分けます。
- JSONの期待値は`firmware=v9.18, connected=true, error=0, verify_flags=0, allocation_failure=false, failed_allocations=0`。`external_allocations`で大きい割当がPSRAMへ進んだことも確認します。DNS／接続失敗は成功と扱いません。
- `/audio_status`にも方針の有効性・累積外部割当数・失敗数・最終失敗サイズを追加。音を鳴らす従来のボタンとは独立しています。
- 信頼するLAN内専用です。認証・暗号化のない本体Webページをインターネットへ公開しないでください。ホストを任意入力して外部へ接続させる機能はありません。

## 検証結果

- Core2製品ビルド成功。SDKの実際のMbed TLSフックとリンクできることを確認。
- Python/native回帰試験**20件成功**。新方針の境界・ゼロサイズ・オーバーフロー・PSRAM失敗時のフォールバック・既存ポインターの解放互換性・失敗タスク・2スレッド計2,000回の割当を試験。実際のTLSプローブ関数も抽出コンパイルし、成功／検証失敗／メモリ失敗／DNS相当失敗と、HTTP・資格情報送信がないことを確認。
- 前述のMbed TLS 2.28.1・実チェーンでの旧方式失敗／新方式20回成功と、誤CA／ホスト名拒否。
- 同じ方針・内部RAM予算46,400 bytesで実際のTLS 1.2ハンドシェイクのみをPCから実行。api.tts.quest、audio1.tts.quest、audio2.tts.quest、news.google.comの4ホストで成功、外部割当各2回・確保失敗0。内部RAMのみへ戻す負の試験はX509 allocation failed（-10368）で失敗。全コンテキスト解放後のモデル上のリークは0。
- 試験コードは`tests/native/tls_memory_repro.cpp`、`tests/native/tls_memory_handshake.cpp`、共有`TlsTestHeap.h`。公式Mbed TLS 2.28.1を`MBEDTLS_PLATFORM_MEMORY`有効でビルドし、製品srcとMbed TLSのincludeを付け、libmbedtls/libmbedx509/libmbedcryptoへリンクして実行できます。証明書チェーンは当日取得した公開PEMを使い、秘密鍵・APIキーは不要です。
- 配布パーツと製品ビルドのバイト一致・サイズ・SHA-256、版整合、旧v9.17拒否、JS構文、localhostのHTTP 200・MIME・バイト一致を確認しました。製品ELFにも実際のTLS割当関数・公開フックのリンクを確認。音声診断ページ内のJS構文も成功しています。
- 最初のUSB調査ではv9.17稼働を確認しましたが、PCと本体が別ネットワークでHTTP診断はタイムアウトしました。その後同一LANで接続可能となり、下記の許可済み実機検証を行いました。課金API・実機スピーカーテストはまだ実施していません。

## 実機検証（2026-11-01）

ユーザーからアプリ領域だけのUSB書き込み許可を受けて実施しました。ニュース再生に伴うAPI利用・音出しの追加確認は未回答のため、合成・ニュース再生・スピーカーテストは実行していません。

- 実機からパーティション表のみを読み、app0の位置`0x10000`・サイズ`0x640000`を製品設定と照合。NVSや資格情報は読み出していません。
- 下記SHA-256に一致する3,284,032 bytesの既存v9.18を`0x10000`だけへ書き込み。esptoolの書き込み後ハッシュ照合成功。消去対象は`0x10000`〜`0x331fff`のみで、NVS・OTA選択情報・SPIFFS・ブートローダー・パーティション表は未書き込みです。
- 再起動後の`[BOOT] Firmware v9.18`とTLSメモリ方針の起動ログ、既存IPでのWi-Fi接続、音量レベル2・master=102・channel0=255を確認。秘密情報そのものの読出しや照合はしていません。
- ダンスは`drawn=100 skipped=0 errors=0`で完走、再生後ガードも解除。観測中の起動クラッシュはありませんでした。
- 実機`/tls_test`を3回、各回1秒以上あけて実行。接続先は以前失敗した`api.tts.quest / 164.70.91.214`、所要時間3.04／2.80／3.34秒。3回とも次の結果で、シリアルにも`verified host=api.tts.quest`を確認しました。

```json
{"firmware":"v9.18","connected":true,"error":0,"verify_flags":0,"allocation_failure":false,"external_allocations":2,"failed_allocations":0,"http_requests_sent":0}
```

- 3回後の状態は`tls_memory_policy=true`、`tls_external_allocations=6`、`tls_allocation_failures=0`、`tls_last_failed_bytes=0`。マイクは稼働、デコードは停止、`output_fault=false`。PCM送信数とピークは0であり、**これは音声未試験を示し、音出し成功の証拠ではありません**。
- `/tls_test`と`/audio_test`への専用ヘッダーなしPOSTは403で拒否されました。最初の連続要求中にTCPリセットを1回観測しましたが、1秒間隔での再試験では両方403、続く状態取得10回もすべて成功（約0.13〜0.28秒）。初回の接続切断の原因は未確定で、解消済みとは扱いません。
- 同じ実機・設定で旧版の検証失敗から新版の接続成功へ改善したことは確認できました。ただし合成タスク・RSS取得後のメモリ負荷・長時間連続再生は未試験です。旧版へ戻す実機A/B試験も行っておらず、原因が一つだけだったという証明ではありません。
- ローカル回帰20件、配布パーツのビルド／ハッシュ一致、版整合・キャッシュ対策試験も再実行し成功しました。製品コード・配布バイナリの追加変更はありません。

## 生成物・残作業

- firmware.bin: **3,284,032 bytes**
- SHA-256: `86ca79a8a8884e7d45e164e676f5b7ba09b179756a3c0fcc47474d033f170163`
- ELF SHA-256: `d1880b976105b0b99921597c37f3d73b764ea310ee2347b4e818a83b92ee0b74`
- 両リポジトリの専用ブランチ`codex/news-audio-v912`を継続。main統合・force pushはしません。
- 新しいZIPの`ai_stackchan_flasher`から書き込み、左上／起動バナーのv9.18を確認。フル書き込みは従来どおり**NVS/SPIFFSを初期化するので、Wi-Fi・APIキーを再設定**してください。
- 残作業は、許可後のAPI不要スピーカーテスト、回数を限定したニュース合成・再生とPCM／終了ログの確認、実際に聞こえるかの確認です。合成負荷でTLS失敗が残り`this_task_failure=0`なら、提示証明書・実機暗号処理・その他の検証原因を継続調査します。初回のTCP切断も再発時に調査が必要です。
- キーNG／429は別問題で、キーの受理・サービス制限の確認が必要です。

## 参考資料

- [Mbed TLS 2.28.1 x509_crt.c](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-2.28.1/library/x509_crt.c)
- [ESP-IDF v4.4.4 esp_mem.c](https://github.com/espressif/esp-idf/blob/v4.4.4/components/mbedtls/port/esp_mem.c)
- [ESP-IDF v4.4 メモリ方針](https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32/api-reference/kconfig.html#config-mbedtls-mem-alloc-mode)
