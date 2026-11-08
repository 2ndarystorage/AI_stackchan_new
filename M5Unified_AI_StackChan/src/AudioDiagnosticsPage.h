#pragma once
static const char AUDIO_DIAGNOSTICS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stackchan 音声診断</title>
<style>body{max-width:700px;margin:2em auto;padding:0 1em;font-family:serif}pre{background:#f1eee7;padding:1em;white-space:pre-wrap}button{padding:.7em 1em}</style>
<h1>音声診断</h1>
<p>合成APIを使わず、スピーカーと音量を確認できます。同じWi-Fi内で使用してください。</p>
<p>テストはミュート時でも一時的に中程度の音量で2音鳴らします。会話・ニュース中は実行しません。終了後は元の音量へ戻します。</p>
<button id="test">スピーカーテスト（API不要）</button>
<button id="tls">TLS接続テスト（HTTP・API要求なし）</button>
<p id="message" role="status"></p><pre id="state">読み込み中</pre>
<script>
const state=document.getElementById('state'),message=document.getElementById('message'),test=document.getElementById('test');
async function refresh(){try{const r=await fetch('/audio_status',{cache:'no-store'});state.textContent=JSON.stringify(await r.json(),null,2)}catch(e){message.textContent='接続を確認してください'}}
test.onclick=async()=>{test.disabled=true;try{const r=await fetch('/audio_test',{method:'POST',headers:{'X-Stackchan-Audio-Test':'1'}});message.textContent=await r.text()}catch(e){message.textContent='テスト要求に失敗しました'}finally{test.disabled=false;refresh()}};
const tls=document.getElementById('tls');
tls.onclick=async()=>{tls.disabled=true;try{const r=await fetch('/tls_test',{method:'POST',headers:{'X-Stackchan-TLS-Test':'1'}});message.textContent=await r.text()}catch(e){message.textContent='TLS接続テストに失敗しました'}finally{tls.disabled=false;refresh()}};
refresh();setInterval(refresh,3000);
</script></html>)HTML";
