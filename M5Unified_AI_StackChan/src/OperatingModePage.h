#pragma once

static const char OPERATING_MODE_HTML[] PROGMEM = R"MODEPAGE(<!doctype html>
<html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>StackChan 動作モード設定</title>
<h1>動作モード設定</h1><p>現在: 通常モード（BLE停止）</p>
<p>BLE転送専用モードへ切り替えると再起動し、Wi-Fi・会話・ニュース・音声は停止します。
受信完了の15秒後、3分の受信待ち時間切れ、または本体のCボタンで通常モードへ再起動します。
Wi-FiやAPIキーの設定は保持します。電源OFFからの起動機能ではありません。</p>
<button id="switch">BLE転送専用モードへ切替</button><p id="result"></p>
<p>本体に「BLE transfer only / WAIT」と表示されたら転送ページの「BLEで送信」を使ってください。
転送ページの旧「受信モードを開始」ボタンは使いません。BLE中はこのWeb画面には接続できません。</p>
<p><a href="/">本体ホーム</a></p>
<script>
document.getElementById('switch').onclick = async function () {
  if (!confirm('Wi-Fiと会話を停止し、BLE専用モードへ再起動しますか？')) return;
  this.disabled = true;
  const result = document.getElementById('result');
  try {
    const r = await fetch('/ble', {method:'POST',headers:{'X-Stackchan-Mode':'ble'}});
    const data = await r.json();
    if (!r.ok) throw Error(data.error || '切替に失敗しました');
    result.textContent = '切替を受け付けました。本体のWAIT表示を確認してください。Wi-Fi切断は正常です。';
  } catch(e) {
    result.textContent = e.message + '。応答が不明な場合は再操作する前に本体画面を確認してください。';
    this.disabled = false;
  }
};
</script></html>)MODEPAGE";
