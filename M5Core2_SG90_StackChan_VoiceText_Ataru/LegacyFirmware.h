#include <Arduino.h>

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#if defined(ARDUINO_M5STACK_Core2)
  #define SERVO_PIN_X 13
  #define SERVO_PIN_Y 14
#elif defined( ARDUINO_M5STACK_FIRE )
  #define SERVO_PIN_X 21
  #define SERVO_PIN_Y 22
#elif defined( ARDUINO_M5Stack_Core_ESP32 )
  #define SERVO_PIN_X 21
  #define SERVO_PIN_Y 22
#endif

#include <Avatar.h> // https://github.com/meganetaaan/m5stack-avatar
#include <ServoEasing.hpp> // https://github.com/ArminJo/ServoEasing
#include "AtaruFace.h"
#include "RamFace.h"

#if defined(ARDUINO_M5STACK_Core2)
//#define USE_VOICE_TEXT //for M5STACK_Core2 Only
//#define USE_VOICE_CHAT //for M5STACK_Core2 Only
#endif

#ifdef USE_VOICE_TEXT
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2SLipSync.h"
#include "AudioFileSourceVoiceTextStream.h"

const char *SSID = "YOUR_WIFI_SSID";
const char *PASSWORD = "YOUR_WIFI_PASSWORD";

AudioGeneratorMP3 *mp3;
AudioFileSourceVoiceTextStream *file;
AudioFileSourceBuffer *buff;
AudioOutputI2SLipSync *out;
const int preallocateBufferSize = 40*1024;
uint8_t *preallocateBuffer;
#endif

#ifdef USE_VOICE_CHAT
// Voice chat server (run on your PC or a local server)
const char *CHAT_SERVER_HOST = "YOUR_SERVER_IP";
const uint16_t CHAT_SERVER_PORT = 8000;
const char *CHAT_SERVER_PATH = "/v1/chat";
const char *DEVICE_ID = "stackchan-core2-001";

const uint32_t MIC_SAMPLE_RATE = 16000;
const uint16_t MIC_RECORD_SECONDS = 4;

enum ChatBackend {
  CHAT_OPENAI = 0,
  CHAT_CLAUDE = 1,
};

ChatBackend currentBackend = CHAT_OPENAI;
#endif

using namespace m5avatar;
Avatar avatar;
Face* faces[3];
ColorPalette* cps[3];

#define START_DEGREE_VALUE_X 90
#define START_DEGREE_VALUE_Y 90

ServoEasing servo_x;
ServoEasing servo_y;

void restoreSpeakerConfig() {
  auto spk_config = M5.Speaker.config();
  spk_config.sample_rate = 88200;
  spk_config.stereo = false;
  M5.Speaker.config(spk_config);
}

void behavior(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
   for (;;)
  {
#ifdef USE_VOICE_TEXT
    int level = out->getLevel();
    level = abs(level);
    if(level > 10000)
    {
      level = 10000;
    }
    float open = (float)level/10000.0;
    avatar->setMouthOpenRatio(open);
#endif

    vTaskDelay(1/portTICK_PERIOD_MS);
//    delay(50);
  }
}

void servoloop(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  for (;;)
  {
    Avatar *avatar = ctx->getAvatar();
    avatar->getGaze(&gazeY, &gazeX);
    servo_x.setEaseTo(START_DEGREE_VALUE_X + (int)(20.0 * gazeX));
    if(gazeY < 0) {
      servo_y.setEaseTo(START_DEGREE_VALUE_Y + (int)(20.0 * gazeY));
    } else {
      servo_y.setEaseTo(START_DEGREE_VALUE_Y + (int)(10.0 * gazeY));
    }
    synchronizeAllServosStartAndWaitForAllServosToStop();
    vTaskDelay(33/portTICK_PERIOD_MS);
  }
}

void setup() {

#ifdef USE_VOICE_TEXT
  preallocateBuffer = (uint8_t*)ps_malloc(preallocateBufferSize);
#endif
  auto cfg = M5.config();
#ifdef USE_VOICE_CHAT
  cfg.internal_mic = true;
#endif
  cfg.internal_spk = true;
  M5.begin(cfg);

  restoreSpeakerConfig();
  //M5.Speaker.begin();

  if (servo_x.attach(SERVO_PIN_X, START_DEGREE_VALUE_X, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE)) {
    Serial.print("Error attaching servo x");
  }
  if (servo_y.attach(SERVO_PIN_Y, START_DEGREE_VALUE_Y, DEFAULT_MICROSECONDS_FOR_0_DEGREE, DEFAULT_MICROSECONDS_FOR_180_DEGREE)) {
    Serial.print("Error attaching servo y");
  }
  servo_x.setEasingType(EASE_QUADRATIC_IN_OUT);
  servo_y.setEasingType(EASE_QUADRATIC_IN_OUT);
  setSpeedForAllServos(60);

#ifdef USE_VOICE_TEXT
  M5.Lcd.setBrightness(100);
  M5.Lcd.clear();
  M5.Lcd.setTextSize(2);
  delay(1000);

  Serial.println("Connecting to WiFi");
  M5.Lcd.print("Connecting to WiFi");
  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    M5.Lcd.print(".");
  }
  Serial.println("\nConnected");
  M5.Lcd.println("\nConnected");
  M5.Speaker.tone(2000, 500);
  delay(500);
  M5.Speaker.tone(1000, 500);
  delay(1000);

  audioLogger = &Serial;
  out = new AudioOutputI2SLipSync(0, 0);
  out->SetPinout(12, 0, 2);           // ピン配列を指定（BCK, LRCK, DATA)BashCopy
  out->SetOutputModeMono(false);
  mp3 = new AudioGeneratorMP3();
#endif

  faces[0] = new AtaruFace();
  faces[1] = new RamFace();
  faces[2] = avatar.getFace();
  cps[0] = new ColorPalette();
  cps[1] = new ColorPalette();
  cps[2] = new ColorPalette();
  cps[0]->set(COLOR_PRIMARY, TFT_BLACK);
  cps[0]->set(COLOR_BACKGROUND, TFT_WHITE);
  cps[0]->set(COLOR_SECONDARY, TFT_WHITE);
  cps[1]->set(COLOR_PRIMARY, TFT_BLACK);
  cps[1]->set(COLOR_BACKGROUND, TFT_WHITE);
  cps[1]->set(COLOR_SECONDARY, TFT_WHITE);
  cps[2]->set(COLOR_PRIMARY, TFT_WHITE);
  cps[2]->set(COLOR_BACKGROUND, TFT_BLACK);
  cps[2]->set(COLOR_SECONDARY, TFT_WHITE);

  avatar.init(8);
  avatar.setFace(faces[0]);
  avatar.setColorPalette(*cps[0]);
  avatar.addTask(behavior, "behavior");
  avatar.addTask(servoloop, "servoloop");
}

#ifdef USE_VOICE_TEXT
//char *text1 = "こんにちは、世界！";
char *text1 = "こんにちは、僕の名前はあたるです。よろしくね！";
char *text2 = "こんにちは、私の名前はラムちゃんです。よろしくね！";
char *text3 = "こんにちは、私の名前はスタックちゃんです。よろしくね！";
char *tts_parms1 ="&emotion_level=2&emotion=happiness&format=mp3&speaker=takeru&volume=200&speed=100&pitch=130";
char *tts_parms2 ="&emotion_level=2&emotion=happiness&format=mp3&speaker=hikari&volume=200&speed=120&pitch=130";
char *tts_parms3 ="&emotion_level=4&emotion=anger&format=mp3&speaker=bear&volume=200&speed=120&pitch=100";

void VoiceText_tts(const char *text,const char *tts_parms) {
    file = new AudioFileSourceVoiceTextStream( text, tts_parms);
    buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
    delay(100);
    mp3->begin(buff, out);
}
#endif

#ifdef USE_VOICE_CHAT
static String last_tts_text;

size_t buildWavHeader(uint8_t *wav, size_t pcm_bytes, uint32_t sample_rate) {
  const uint16_t channels = 1;
  const uint16_t bits_per_sample = 16;
  const uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
  const uint16_t block_align = channels * (bits_per_sample / 8);
  const uint32_t chunk_size = 36 + pcm_bytes;

  memcpy(wav, "RIFF", 4);
  wav[4] = chunk_size & 0xff;
  wav[5] = (chunk_size >> 8) & 0xff;
  wav[6] = (chunk_size >> 16) & 0xff;
  wav[7] = (chunk_size >> 24) & 0xff;
  memcpy(wav + 8, "WAVE", 4);
  memcpy(wav + 12, "fmt ", 4);
  wav[16] = 16; wav[17] = 0; wav[18] = 0; wav[19] = 0;
  wav[20] = 1; wav[21] = 0;
  wav[22] = channels & 0xff; wav[23] = (channels >> 8) & 0xff;
  wav[24] = sample_rate & 0xff; wav[25] = (sample_rate >> 8) & 0xff;
  wav[26] = (sample_rate >> 16) & 0xff; wav[27] = (sample_rate >> 24) & 0xff;
  wav[28] = byte_rate & 0xff; wav[29] = (byte_rate >> 8) & 0xff;
  wav[30] = (byte_rate >> 16) & 0xff; wav[31] = (byte_rate >> 24) & 0xff;
  wav[32] = block_align & 0xff; wav[33] = (block_align >> 8) & 0xff;
  wav[34] = bits_per_sample & 0xff; wav[35] = (bits_per_sample >> 8) & 0xff;
  memcpy(wav + 36, "data", 4);
  wav[40] = pcm_bytes & 0xff;
  wav[41] = (pcm_bytes >> 8) & 0xff;
  wav[42] = (pcm_bytes >> 16) & 0xff;
  wav[43] = (pcm_bytes >> 24) & 0xff;
  return 44;
}

bool recordPcm(int16_t *pcm, size_t samples, uint32_t sample_rate) {
  M5.Mic.begin();
  M5.Mic.setSampleRate(sample_rate);
  bool ok = M5.Mic.record(pcm, samples, sample_rate, false);
  M5.Mic.end();
  return ok;
}

bool postWavToServer(const uint8_t *wav_data, size_t wav_len, ChatBackend backend, String *out_text) {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + CHAT_SERVER_HOST + ":" + String(CHAT_SERVER_PORT) + CHAT_SERVER_PATH;
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Backend", backend == CHAT_OPENAI ? "openai" : "claude");
  http.addHeader("X-User-Id", DEVICE_ID);

  int code = http.POST((uint8_t*)wav_data, wav_len);
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return false;
  }
  const char *text = doc["text"];
  if (!text) {
    return false;
  }
  *out_text = String(text);
  return true;
}

void recordAndChat(ChatBackend backend) {
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
    out->setLevel(0);
  }

  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Recording...");

  const size_t samples = MIC_SAMPLE_RATE * MIC_RECORD_SECONDS;
  const size_t pcm_bytes = samples * sizeof(int16_t);
  int16_t *pcm = (int16_t*)ps_malloc(pcm_bytes);
  if (!pcm) {
    M5.Lcd.println("No PCM memory");
    return;
  }

  M5.Speaker.end();
  bool ok = recordPcm(pcm, samples, MIC_SAMPLE_RATE);
  M5.Speaker.begin();
  restoreSpeakerConfig();

  if (!ok) {
    free(pcm);
    M5.Lcd.println("Mic record failed");
    return;
  }

  uint8_t *wav = (uint8_t*)ps_malloc(pcm_bytes + 44);
  if (!wav) {
    free(pcm);
    M5.Lcd.println("No WAV memory");
    return;
  }
  buildWavHeader(wav, pcm_bytes, MIC_SAMPLE_RATE);
  memcpy(wav + 44, pcm, pcm_bytes);
  free(pcm);

  M5.Lcd.println("Sending...");
  String reply;
  if (!postWavToServer(wav, pcm_bytes + 44, backend, &reply)) {
    free(wav);
    M5.Lcd.println("Server error");
    return;
  }
  free(wav);

  M5.Lcd.println("Speaking...");
  last_tts_text = reply;
  const char *tts_parms = (backend == CHAT_OPENAI) ? tts_parms1 : tts_parms2;
  VoiceText_tts(last_tts_text.c_str(), tts_parms);
}
#endif

void loop() {
  M5.update();
#if defined(USE_VOICE_TEXT) && !defined(USE_VOICE_CHAT)
  static int lastms = 0;
  if (M5.BtnA.wasPressed())
  {
    M5.Speaker.tone(2000, 500);
    avatar.setFace(faces[0]);
    avatar.setColorPalette(*cps[0]);
    delay(1000);
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text1, tts_parms1);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (M5.BtnB.wasPressed())
  {
    M5.Speaker.tone(2000, 500);
    avatar.setFace(faces[1]);
    avatar.setColorPalette(*cps[1]);
    delay(1000);
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text2, tts_parms2);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (M5.BtnC.wasPressed())
  {
    M5.Speaker.tone(2000, 500);
    avatar.setFace(faces[2]);
    avatar.setColorPalette(*cps[2]);
    delay(1000);
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text3, tts_parms3);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (mp3->isRunning()) {
    if (millis()-lastms > 1000) {
      lastms = millis();
      Serial.printf("Running for %d ms...\n", lastms);
      Serial.flush();
     }
    if (!mp3->loop()) {
      mp3->stop();
      out->setLevel(0);
      delete file;
      delete buff;
      Serial.println("mp3 stop");
    }
  }
#elif defined(USE_VOICE_CHAT)
  if (M5.BtnA.wasPressed())
  {
    avatar.setFace(faces[0]);
    avatar.setColorPalette(*cps[0]);
    recordAndChat(CHAT_OPENAI);
  }
  if (M5.BtnB.wasPressed())
  {
    avatar.setFace(faces[1]);
    avatar.setColorPalette(*cps[1]);
    recordAndChat(CHAT_CLAUDE);
  }
  if (M5.BtnC.wasPressed())
  {
    avatar.setFace(faces[2]);
    avatar.setColorPalette(*cps[2]);
  }
#else
  if (M5.BtnA.wasPressed())
  {
    avatar.setFace(faces[0]);
    avatar.setColorPalette(*cps[0]);
  }
  if (M5.BtnB.wasPressed())
  {
    avatar.setFace(faces[1]);
    avatar.setColorPalette(*cps[1]);
  }
  if (M5.BtnC.wasPressed())
  {
    avatar.setFace(faces[2]);
    avatar.setColorPalette(*cps[2]);
  }
#endif
}
