#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Avatar.h>
#include <ServoEasing.hpp>
#include "AtaruFace.h"
#include "RamFace.h"
#include "DeviceTimer.h"

// ESP32 Core2 / M5GoBottom SG90 wiring. Do not copy CoreS3 UART servo defaults.
constexpr int SERVO_X = 13, SERVO_Y = 14;
constexpr uint32_t SAMPLE_RATE = 24000;
constexpr size_t FRAME_SAMPLES = 960, FRAME_BYTES = FRAME_SAMPLES * 2;
constexpr size_t RING_SAMPLES = 24000; // 1 second; bridge allows at most 500 ms in flight
using namespace m5avatar;
Avatar avatar;
Face* faces[3];
ColorPalette palettes[3];
ServoEasing servoX, servoY;
WebServer settingsServer(80);
WebSocketsClient voiceSocket;
Preferences preferences;
DeviceTimer deviceTimer;
String timerLabel;

struct Settings {
  String ssid, password, host, token, backend = "realtime";
  uint16_t port = 8000;
  uint8_t volume = 100;
} settings;
String adminPassword, csrfToken, apName, authHeader, uiText, lastToolId, lastToolReply;
bool configMode = false, sessionActive = false, sessionReady = false;
bool microphoneRunning = false, playbackEnded = false, resuming = false;
bool networkWasConnected = false;
volatile bool pauseServos = false;
volatile int mouthLevel = 0;
uint32_t connectionStarted = 0, restartAt = 0, lastActivity = 0, lastMicAt = 0;
int faceIndex = 0;
int16_t micBuffers[2][FRAME_SAMPLES];
size_t micHead = 0, micQueued = 0;
int16_t* audioRing = nullptr;
size_t ringRead = 0, ringWrite = 0, ringCount = 0;
int16_t playBuffers[3][FRAME_SAMPLES];
size_t playLengths[3] = {}, playRead = 0, playWrite = 0, playQueued = 0;

void showStatus(const String& text) {
  uiText = text;
  avatar.setSpeechText(uiText.c_str());
}
void selectFace(int index) {
  faceIndex = index;
  avatar.setFace(faces[index]);
  avatar.setColorPalette(palettes[index]);
}
void faceBehavior(void* args) {
  auto* ctx = static_cast<DriveContext*>(args);
  for (;;) {
    ctx->getAvatar()->setMouthOpenRatio(min(1.0f, mouthLevel / 10000.0f));
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
void servoBehavior(void* args) {
  auto* ctx = static_cast<DriveContext*>(args);
  for (;;) {
    if (!pauseServos) {
      float x, y;
      ctx->getAvatar()->getGaze(&y, &x);
      servoX.setEaseTo(90 + int(20 * x));
      servoY.setEaseTo(90 + int((y < 0 ? 20 : 10) * y));
      synchronizeAllServosStartAndWaitForAllServosToStop();
    }
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}
void stopMicrophone() {
  if (microphoneRunning) M5.Mic.end(); // joins capture task before buffers are reused
  microphoneRunning = false;
  micQueued = 0;
}
void clearPlayback() {
  M5.Speaker.stop();
  M5.Speaker.end(); // release I2S before capture; synchronizes buffer ownership
  ringRead = ringWrite = ringCount = 0;
  playRead = playWrite = playQueued = 0;
  mouthLevel = 0;
  playbackEnded = resuming = false;
}
void stopSession(const String& reason) {
  sessionActive = sessionReady = false;
  voiceSocket.disconnect();
  stopMicrophone();
  clearPlayback();
  pauseServos = configMode;
  showStatus(reason);
}
bool startMicrophone() {
  if (!sessionActive || !sessionReady) return false;
  M5.Speaker.end();
  if (!M5.Mic.begin()) return false;
  microphoneRunning = true;
  micHead = 0; micQueued = 0;
  for (int i = 0; i < 2; ++i) {
    if (!M5.Mic.record(micBuffers[i], FRAME_SAMPLES, SAMPLE_RATE, false)) {
      stopMicrophone(); return false;
    }
    ++micQueued;
  }
  lastMicAt = millis();
  pauseServos = true; // reduce SG90 mechanical noise during speech capture
  showStatus("Listening... (C: stop)");
  return true;
}
void serviceMicrophone() {
  if (!microphoneRunning) return;
  // A queue slot becomes free only after the capture task has filled its buffer.
  for (int limit = 0; limit < 2 && M5.Mic.isRecording() < micQueued; ++limit) {
    size_t index = micHead;
    --micQueued;
    if (!voiceSocket.sendBIN(reinterpret_cast<uint8_t*>(micBuffers[index]), FRAME_BYTES)) {
      stopSession("Audio send failed. Press A to retry."); return;
    }
    micHead = (micHead + 1) % 2;
    if (!M5.Mic.record(micBuffers[index], FRAME_SAMPLES, SAMPLE_RATE, false)) {
      stopSession("Mic queue failed. Press A to retry."); return;
    }
    ++micQueued;
    lastMicAt = millis();
  }
  if (millis() - lastMicAt > 2000) stopSession("Microphone timeout. Check bottom board.");
}
void sendCredit(size_t bytes) {
  String message = "{\"type\":\"credit\",\"bytes\":" + String(bytes) + "}";
  voiceSocket.sendTXT(message);
}
void receiveAudio(const uint8_t* data, size_t length) {
  if (!sessionActive || !sessionReady) return;
  if (!length || length > FRAME_BYTES || length % 2 || ringCount + length / 2 > RING_SAMPLES) {
    stopSession("Audio buffer error. Press A to retry."); return;
  }
  stopMicrophone();
  pauseServos = false;
  if (!M5.Speaker.begin()) { stopSession("Speaker failed."); return; }
  M5.Speaker.setVolume(settings.volume);
  for (size_t i = 0; i < length; i += 2) {
    audioRing[ringWrite] = int16_t(uint16_t(data[i]) | (uint16_t(data[i+1]) << 8));
    ringWrite = (ringWrite + 1) % RING_SAMPLES;
    ++ringCount;
  }
  showStatus("Speaking... (AI voice)");
}
void servicePlayback() {
  if (!sessionActive) return;
  // Runtime audio pointers stay valid until the M5 speaker queue releases them.
  size_t occupied = M5.Speaker.isPlaying(uint8_t(0));
  while (playQueued > occupied) {
    sendCredit(playLengths[playRead] * 2);
    playRead = (playRead + 1) % 3;
    --playQueued;
  }
  while (ringCount && playQueued < 2) {
    size_t count = min(FRAME_SAMPLES, ringCount);
    int32_t level = 0;
    for (size_t i = 0; i < count; ++i) {
      int16_t sample = audioRing[ringRead];
      playBuffers[playWrite][i] = sample;
      level += abs(int(sample));
      ringRead = (ringRead + 1) % RING_SAMPLES;
    }
    ringCount -= count;
    if (!M5.Speaker.playRaw(playBuffers[playWrite], count, SAMPLE_RATE, false, 1, 0)) {
      stopSession("Speaker queue failed."); return;
    }
    mouthLevel = level / count * 3;
    playLengths[playWrite] = count;
    playWrite = (playWrite + 1) % 3;
    ++playQueued;
  }
  if (!ringCount && !playQueued) mouthLevel = 0;
  if (playbackEnded && !ringCount && !playQueued && !resuming) {
    playbackEnded = false;
    resuming = true;
    M5.Speaker.end();
    String message = "{\"type\":\"playback_done\"}";
    voiceSocket.sendTXT(message);
  }
}
void executeTool(JsonDocument& request) {
  String id = request["call_id"] | "";
  if (id.isEmpty() || id.length() > 128) return;
  if (id == lastToolId) { voiceSocket.sendTXT(lastToolReply); return; }
  StaticJsonDocument<768> reply;
  reply["type"] = "tool_result";
  reply["call_id"] = id;
  JsonObject result = reply.createNestedObject("result");
  String name = request["name"] | "";
  if (name == "start_timer") {
    JsonVariant value = request["arguments"]["seconds"];
    uint32_t seconds = value.is<uint32_t>() ? value.as<uint32_t>() : 0;
    String label = request["arguments"]["label"] | "Timer";
    if (label.length() > 160 || !deviceTimer.start(seconds, millis())) {
      result["error"] = "Invalid duration or an existing timer is still active";
    } else {
      timerLabel = label;
      result["ok"] = true;
    }
  } else if (name == "cancel_timer") {
    deviceTimer.cancel(); result["ok"] = true;
  } else if (name != "timer_status") {
    result["error"] = "Unsupported tool";
  }
  result["active"] = deviceTimer.active;
  result["remaining_seconds"] = deviceTimer.remaining(millis());
  result["label"] = timerLabel;
  lastToolId = id;
  lastToolReply = "";
  serializeJson(reply, lastToolReply);
  voiceSocket.sendTXT(lastToolReply);
}
void socketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_DISCONNECTED) {
    if (sessionActive) stopSession("Disconnected. Press A to reconnect.");
    return;
  }
  if (!sessionActive) return;
  if (type == WStype_BIN) { receiveAudio(payload, length); return; }
  if (type != WStype_TEXT) return;
  StaticJsonDocument<2048> event;
  if (deserializeJson(event, payload, length)) { stopSession("Invalid bridge message."); return; }
  String kind = event["type"] | "";
  if (kind == "ready") {
    if (event["sample_rate"] != SAMPLE_RATE || event["window"].as<size_t>() > RING_SAMPLES * 2) {
      stopSession("Audio format mismatch."); return;
    }
    sessionReady = true;
    lastActivity = millis();
    if (!startMicrophone()) stopSession("Microphone failed. Check bottom board.");
  } else if (kind == "listening") {
    resuming = false;
    lastActivity = millis();
    if (!startMicrophone()) stopSession("Microphone failed.");
  } else if (kind == "speech_started") {
    lastActivity = millis();
  } else if (kind == "thinking") {
    stopMicrophone(); pauseServos = false;
    showStatus("Thinking...");
  } else if (kind == "audio_end") {
    playbackEnded = true;
  } else if (kind == "tool_call") {
    executeTool(event);
  } else if (kind == "error") {
    stopSession(event["message"] | "Voice server error.");
  }
}
void startSession() {
  if (configMode || WiFi.status() != WL_CONNECTED || settings.host.isEmpty() || settings.token.length() < 16) {
    showStatus("Hold C for Web settings."); return;
  }
  if (!audioRing) { showStatus("PSRAM missing. Enable PSRAM in build."); return; }
  stopSession("");
  sessionActive = true;
  lastActivity = millis();
  authHeader = "Authorization: Bearer " + settings.token;
  voiceSocket.setExtraHeaders(authHeader.c_str());
  String path = settings.backend == "realtime" ? "/v1/realtime" : "/v1/voice?backend=" + settings.backend;
  voiceSocket.begin(settings.host, settings.port, path);
  voiceSocket.onEvent(socketEvent);
  // Reconnection is explicit: prevents unattended API sessions after an outage.
  voiceSocket.setReconnectInterval(60000);
  voiceSocket.enableHeartbeat(15000, 3000, 2);
  showStatus("Connecting to voice bridge...");
}
String escapeHtml(String value) {
  value.replace("&", "&amp;"); value.replace("<", "&lt;"); value.replace(">", "&gt;");
  value.replace("\"", "&quot;"); value.replace("'", "&#39;"); return value;
}
bool authorizeSettings() {
  if (settingsServer.authenticate("admin", adminPassword.c_str())) return true;
  settingsServer.requestAuthentication(); return false;
}
bool validHost(const String& host) {
  if (host.isEmpty() || host.length() > 253) return false;
  for (size_t i = 0; i < host.length(); ++i) {
    char c = host[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
  }
  return true;
}
bool decimalValue(const String& value, uint32_t minimum, uint32_t maximum) {
  if (value.isEmpty() || value.length() > 5) return false;
  for (size_t i = 0; i < value.length(); ++i) if (!isDigit(value[i])) return false;
  return value.toInt() >= long(minimum) && value.toInt() <= long(maximum);
}
void settingsPage() {
  if (!authorizeSettings()) return;
  String html = R"HTML(<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Stackchan 設定</title><style>body{max-width:520px;margin:32px auto;padding:16px;font:16px sans-serif;background:#f4f5f7;color:#17202a}label{display:block;margin:18px 0}input{box-sizing:border-box;width:100%;padding:10px;margin-top:6px}input[type=checkbox]{width:auto}button{padding:12px 24px}small{display:block;color:#485460}</style><h1>Stackchan 設定</h1><p>Core2の接続情報を保存します。APIキーはPC側だけで設定します。</p><form action="/save" method="post">)HTML";
  html += "<input type='hidden' name='csrf' value='" + csrfToken + "'>";
  html += "<label>Wi-Fi SSID<input name='ssid' maxlength='32' required value='" + escapeHtml(settings.ssid) + "'></label>";
  html += "<label>Wi-Fiパスワード<input type='password' name='password' maxlength='63' autocomplete='new-password'><small>空欄なら現在の値を保持</small></label>";
  html += "<label><input type='checkbox' name='open_wifi'> パスワードなしのWi-Fiを使う</label>";
  html += "<label>PCのIPアドレス／ホスト名<input name='host' maxlength='253' required value='" + escapeHtml(settings.host) + "'><small>例: 192.168.1.20（http:// は付けない）</small></label>";
  html += "<label>PC側ポート<input name='port' type='number' min='1' max='65535' required value='" + String(settings.port) + "'></label>";
  html += "<label>会話AI<select name='backend'>";
  const char* backends[] = {"realtime", "openai", "claude"};
  const char* labels[] = {"ChatGPT Realtime", "ChatGPT（文字起こし＋読み上げ）", "Claude（文字起こし＋読み上げ）"};
  for (int i = 0; i < 3; ++i) {
    html += String("<option value='") + backends[i] + "'" + (settings.backend == backends[i] ? " selected" : "") + ">" + labels[i] + "</option>";
  }
  html += "</select><small>Claudeでも音声認識と読み上げにはOpenAIを使用します。</small></label>";
  html += "<label>BRIDGE_TOKEN<input type='password' name='token' minlength='16' maxlength='128' autocomplete='new-password'><small>PCと同じ16文字以上の値。保存済みなら空欄で保持</small></label>";
  html += "<label>音量（0〜255）<input name='volume' type='number' min='0' max='255' required value='" + String(settings.volume) + "'></label>";
  html += "<button>保存して再起動</button></form><p>音声はAIによって生成されます。</p></html>";
  settingsServer.sendHeader("Cache-Control", "no-store");
  settingsServer.send(200, "text/html; charset=utf-8", html);
}
void saveSettings() {
  if (!authorizeSettings()) return;
  if (settingsServer.arg("csrf") != csrfToken) { settingsServer.send(403, "text/plain", "Reload settings page."); return; }
  String ssid = settingsServer.arg("ssid"), host = settingsServer.arg("host"); host.trim();
  String password = settingsServer.arg("password");
  if (settingsServer.hasArg("open_wifi")) password = "";
  else if (password.isEmpty()) password = settings.password;
  String token = settingsServer.arg("token");
  String backend = settingsServer.arg("backend");
  if (backend != "realtime" && backend != "openai" && backend != "claude") {
    settingsServer.send(400, "text/plain", "Invalid backend."); return;
  }
  if (token.isEmpty()) token = settings.token;
  bool tokenValid = token.length() >= 16 && token.length() <= 128;
  for (size_t i = 0; i < token.length(); ++i) if (token[i] < 33 || token[i] > 126) tokenValid = false;
  if (ssid.isEmpty() || ssid.length() > 32 || (!password.isEmpty() && (password.length() < 8 || password.length() > 63)) || !validHost(host) || !tokenValid || !decimalValue(settingsServer.arg("port"), 1, 65535) || !decimalValue(settingsServer.arg("volume"), 0, 255)) {
    settingsServer.send(400, "text/plain; charset=utf-8", "入力値を確認してください。SSID・PCホスト・16文字以上のトークンが必要です。"); return;
  }
  StaticJsonDocument<1024> doc;
  doc["ssid"] = ssid; doc["password"] = password; doc["host"] = host;
  doc["backend"] = backend;
  doc["token"] = token; doc["port"] = settingsServer.arg("port").toInt(); doc["volume"] = settingsServer.arg("volume").toInt();
  String serialized; serializeJson(doc, serialized);
  if (!preferences.begin("stackchan", false)) { settingsServer.send(500, "text/plain", "NVS open failed"); return; }
  size_t written = preferences.putString("settings", serialized);
  preferences.end();
  if (!written) { settingsServer.send(500, "text/plain", "NVS save failed"); return; }
  stopSession("Restarting...");
  settingsServer.sendHeader("Cache-Control", "no-store");
  settingsServer.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><p>保存しました。本体を再起動します。</p>");
  restartAt = millis() + 1000;
}
void openConfiguration() {
  stopSession("");
  configMode = true; pauseServos = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), adminPassword.c_str());
  avatar.stop();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8, 20);
  M5.Display.printf("Web settings\n\n WiFi: %s\n Password: %s\n\n http://192.168.4.1/\n Login: admin\n Password: same as WiFi\n\n C: close settings", apName.c_str(), adminPassword.c_str());
}
void loadSettings() {
  if (!preferences.begin("stackchan", true)) return;
  String data = preferences.getString("settings", "{}"); preferences.end();
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, data)) return;
  settings.ssid = doc["ssid"] | ""; settings.password = doc["password"] | "";
  settings.host = doc["host"] | ""; settings.token = doc["token"] | "";
  settings.backend = doc["backend"] | "realtime";
  if (settings.backend != "openai" && settings.backend != "claude") settings.backend = "realtime";
  settings.port = doc["port"] | 8000; settings.volume = doc["volume"] | 100;
}
void setup() {
  Serial.begin(115200);
  auto cfg = M5.config(); cfg.internal_mic = true; cfg.internal_spk = true;
  M5.begin(cfg);
  auto micCfg = M5.Mic.config(); micCfg.sample_rate = SAMPLE_RATE; M5.Mic.config(micCfg);
  audioRing = static_cast<int16_t*>(ps_malloc(RING_SAMPLES * sizeof(int16_t)));
  servoX.attach(SERVO_X, 90, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE);
  servoY.attach(SERVO_Y, 90, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE);
  servoX.setEasingType(EASE_QUADRATIC_IN_OUT); servoY.setEasingType(EASE_QUADRATIC_IN_OUT); setSpeedForAllServos(60);
  faces[0] = new AtaruFace(); faces[1] = new RamFace(); faces[2] = avatar.getFace();
  for (int i = 0; i < 3; ++i) {
    palettes[i].set(COLOR_PRIMARY, i == 2 ? TFT_WHITE : TFT_BLACK);
    palettes[i].set(COLOR_BACKGROUND, i == 2 ? TFT_BLACK : TFT_WHITE);
    palettes[i].set(COLOR_SECONDARY, TFT_WHITE);
  }
  avatar.init(8); selectFace(0);
  avatar.addTask(faceBehavior, "face"); avatar.addTask(servoBehavior, "servo");
  loadSettings();
  char password[17]; snprintf(password, sizeof(password), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  adminPassword = password;
  char nonce[17]; snprintf(nonce, sizeof(nonce), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random()); csrfToken = nonce;
  apName = "Stackchan-" + String(uint32_t(ESP.getEfuseMac()) & 0xffff, HEX);
  settingsServer.on("/", HTTP_GET, settingsPage);
  settingsServer.on("/save", HTTP_POST, saveSettings);
  settingsServer.begin();
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  connectionStarted = millis();
  if (settings.ssid.isEmpty()) openConfiguration();
  else { WiFi.begin(settings.ssid.c_str(), settings.password.c_str()); showStatus("Connecting WiFi... Hold C: settings"); }
}
void loop() {
  M5.update(); settingsServer.handleClient();
  if (restartAt && int32_t(millis() - restartAt) >= 0) ESP.restart();
  if (M5.BtnC.pressedFor(1500) && !configMode) openConfiguration();
  if (M5.BtnC.wasClicked()) {
    if (configMode) {
      configMode = false; WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); avatar.start();
      pauseServos = false; connectionStarted = millis();
      showStatus("A: voice / B: face / Hold C: settings");
    } else stopSession("Stopped. A: voice / Hold C: settings");
  }
  if (!configMode) {
    if (M5.BtnA.wasClicked()) {
      if (sessionActive) stopSession("Stopped. Press A to talk."); else startSession();
    }
    if (M5.BtnB.wasClicked()) selectFace((faceIndex + 1) % 3);
  }
  if (sessionActive) {
    voiceSocket.loop();
    servicePlayback(); serviceMicrophone();
    if (sessionActive && millis() - lastActivity > (sessionReady ? 60000u : 20000u)) stopSession("Voice timed out. Press A to reconnect.");
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !networkWasConnected && !configMode) showStatus("A: voice / B: face / Hold C: settings");
  if (connected) connectionStarted = millis();
  if (!connected && sessionActive) stopSession("WiFi disconnected.");
  if (!connected && !configMode && millis() - connectionStarted > 15000) openConfiguration();
  networkWasConnected = connected;
  if (deviceTimer.update(millis())) {
    stopSession("Timer finished! Press A to talk.");
    avatar.setExpression(Expression::Happy);
    M5.Speaker.begin(); M5.Speaker.setVolume(settings.volume); M5.Speaker.tone(1800, 700);
  }
  delay(1);
}
