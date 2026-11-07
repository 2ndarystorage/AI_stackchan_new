#include <M5Unified.h>

#include <memory>

#include <esp_ns.h>
#include <SPIFFS.h>

#include "simplevox.h"
#include "FlashWriteGuard.h"  // フラッシュ書き込みガード（第9.31弾）
#include "SdCardAccess.h"     // SD 健全性管理の共有窓口（第9.32弾）
#include <SD.h>               // テンプレートの SD 保存確認（第9.32弾）

#define base_path "/spiffs"
#define file_name "/wakeword.bin"
// 第9.32弾: SD 上のテンプレートへの stdio パス。根拠: arduino-esp32
// 2.0.11 の SDFS::begin() の既定マウントポイントは "/sdcard" ではなく
// "/sd"（ライブラリ SD/src/SD.h のデフォルト引数）で、本プロジェクトの
// SD.begin（sdBeginCounted）はその既定値のまま使う。よってマウント中は
// VFS パス "/sd/wakeword.bin" で stdio（SimpleVox の saveFile/loadFile）
// から SD に読み書きできる。マウントは sdAccessBegin()/sdAccessEnd() の
// 内側でのみ触ること。万一この前提が崩れても、saveFile は false を返し、
// SD 保存は失敗として記録され、SPIFFS 保存で登録が完了する（安全側）。
#define sd_base_path "/sd"
constexpr int kSampleRate = 16000;
constexpr int audioLength = kSampleRate * 3;  // 3 seconds
constexpr int kRxBufferNum = 3;
int16_t* rawAudio;
int16_t* rxBuffer;
int16_t* rawBuffer;
ns_handle_t nsInst;
simplevox::VadEngine vadEngine;
simplevox::MfccEngine mfccEngine;
simplevox::MfccFeature* mfcc = nullptr;

// 第9.32弾: テンプレートファイルのサイズ（存在しなければ -1）。
// 保存結果のログ用。fs は SD / SPIFFS のどちらでもよい（SD は要マウント）。
static long wwFileSize(fs::FS &fs)
{
  File f = fs.open(file_name, "r");
  if (!f) { return -1; }
  long sz = (long)f.size();
  f.close();
  return sz;
}

// 第9.32弾: 登録モード中の診断ログ（約1秒に1回へ間引き）。
// v9.31 実機で「登録モードに入るが音声検出が反応しない」事象があり、
// 検出経路にはログが一切なかったため切り分けできなかった。フレームが
// 届いているか（frames/nulls）とピーク音量があれば、「マイクに音が
// 届いていない」のか「音はあるが VAD が検出しない」のかを分けられる。
// VAD の内部状態は SimpleVox の公開 API では取得できない（detect() が
// 内部で process() を呼ぶため、状態取得のために process() を追加で
// 呼ぶと二重にフレームを食わせてしまう）ので、無理に出さない。
// 本カウンタは wakeword_regist()（登録モードでのみ呼ばれる）からのみ
// 更新され、比較モード等の通常時にはログは出ない。
static uint32_t g_ww_diag_last_ms = 0;
static uint32_t g_ww_diag_frames = 0;  // rxMic がフレームを返した回数
static uint32_t g_ww_diag_nulls = 0;   // rxMic が nullptr を返した回数
static int32_t  g_ww_diag_peak = 0;    // 期間内の最大ピーク（絶対値）

static void wwRegistDiagFlush(bool detected)
{
  uint32_t now = millis();
  if (!detected && now - g_ww_diag_last_ms < 1000) { return; }
  g_ww_diag_last_ms = now;
  Serial.printf("[WW] regist frames=%lu nulls=%lu peak=%ld detected=%s\n",
                (unsigned long)g_ww_diag_frames, (unsigned long)g_ww_diag_nulls,
                (long)g_ww_diag_peak, detected ? "yes" : "no");
  g_ww_diag_frames = 0;
  g_ww_diag_nulls = 0;
  g_ww_diag_peak = 0;
}

LGFX_Button regButton;
LGFX_Button cmpButton;

/**
 * @brief 1サンプリングごとにMFCCを算出するexample
 * @details
 * VAD -> Buffer -> MFCCの流れで逐次的にMFCCの算出を行う。
 * この例では参考のためにREGISTではraw dataを残しているが、
 * COMPAREの処理だけにすればraw dataは必要なくなるためメモリサイズの削減が可能となる。
 * 処理としてHangbeforeおよびPreDetection時間のデータ処理が複雑な点と
 * frame_lengthがVADとMFCCで異なり、１つの処理単位がhop_lengthである点など
 * 構造を理解していないと分からない処理が多いのが難点…。
 */

/**
 * @brief 除算を行い演算結果を切り上げます（正の整数）
 * @param[in] dividend  被除数
 * @param[in]  divisor   除数
 * @return 切り上げた整数(3/2 -> 2)
 */
constexpr int divCeil(int dividend, int divisor)
{
  return (dividend + divisor - 1) / divisor;
}

/**
 * @brief １フレームの録音を行い読み取り可能なバッファを返します
 * @return  読み取り可能な１フレーム分のバッファ
 */
int16_t* rxMic()
{
  static int rxIndex = 0;
  const int frameLength = vadEngine.config().frame_length();

  if (!M5.Mic.record(&rxBuffer[frameLength * rxIndex], frameLength, kSampleRate))
  {
    return nullptr;
  }
  rxIndex++;
  rxIndex = (rxIndex >= kRxBufferNum) ? 0 : rxIndex;
  return &rxBuffer[rxIndex * frameLength];
}

int rawBufferMaxSize;
int rawBufferSize;
void raw_init(int length)
{
  rawBufferMaxSize = length;
  rawBufferSize = 0;
  rawBuffer = (int16_t*)heap_caps_calloc(rawBufferMaxSize, sizeof(*rawBuffer), MALLOC_CAP_8BIT);
}

int raw_size() { return rawBufferSize; }
void raw_reset() { rawBufferSize = 0; }
int16_t* raw_front() { return rawBuffer; }

void raw_pushBack(const int16_t* src, int length)
{
  if (rawBufferSize + length <= rawBufferMaxSize)
  {
    std::copy_n(src, length, &rawBuffer[rawBufferSize]);
    rawBufferSize += length;
  }
}

void raw_popFront(int length)
{
  std::copy_n(&rawBuffer[length], rawBufferSize - length, rawBuffer);
  rawBufferSize -= length;
}

float* features;
int mfccFrameNum;
int mfccCoefNum;
int mfccBeforeFrameNum;
void feature_init(const simplevox::MfccConfig& mfccConfig, const simplevox::VadConfig& vadConfig, int maxTimeMs)
{
  const int maxLength = maxTimeMs * mfccConfig.sample_rate / 1000;
  mfccFrameNum = (maxLength - (mfccConfig.frame_length() - mfccConfig.hop_length())) / mfccConfig.hop_length();
  mfccCoefNum = mfccConfig.coef_num;
  features = (float*)heap_caps_malloc(sizeof(*features) * mfccFrameNum * mfccCoefNum, MALLOC_CAP_8BIT);

  // VADのHangbeforeおよびPreDetectionに相当するデータサイズを算出
  const int vadBeforeLength = 
            vadConfig.frame_length() *
            ( divCeil(vadConfig.before_length(), vadConfig.frame_length())
            + divCeil(vadConfig.decision_length(), vadConfig.frame_length()));
  // VADのBefore区間に相当するフレーム数を算出
  mfccBeforeFrameNum = (vadBeforeLength - (mfccConfig.frame_length() - mfccConfig.hop_length())) / mfccConfig.hop_length();
}

float* feature_get(int number) { return &features[number * mfccCoefNum]; }

constexpr uint32_t memCaps = (CONFIG_SPIRAM) ? (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM) : MALLOC_CAP_8BIT;

void wakeword_setup(){
  // auto vadConfig = vadEngine.config();
  // vadConfig.sample_rate = kSampleRate;
  // auto mfccConfig = mfccEngine.config();
  // mfccConfig.sample_rate = kSampleRate;

  // rawAudio = (int16_t*)heap_caps_malloc(audioLength * sizeof(*rawAudio), memCaps);
  // rxBuffer = (int16_t*)heap_caps_malloc(kRxBufferNum * vadConfig.frame_length() * sizeof(*rxBuffer), MALLOC_CAP_8BIT);
  
  // raw_init(mfccConfig.frame_length() + vadConfig.frame_length());
  // feature_init(mfccConfig, vadConfig, 3000);
}

int mode = 0;   // 0: none, <0: REGIST, >0: COMPARE

void clear_buff(){
  // memset(rawAudio, 0 ,audioLength * sizeof(*rawAudio));
  auto vadConfig = vadEngine.config();
  auto mfccConfig = mfccEngine.config();
  // memset(rxBuffer, 0 ,kRxBufferNum * vadConfig.frame_length() * sizeof(*rxBuffer));
  raw_init(mfccConfig.frame_length() + vadConfig.frame_length());
//  feature_init(mfccConfig, vadConfig, 3000);
}

void wakeword_init(){
  auto vadConfig = vadEngine.config();
  vadConfig.sample_rate = kSampleRate;
  auto mfccConfig = mfccEngine.config();
  mfccConfig.sample_rate = kSampleRate;

  rawAudio = (int16_t*)heap_caps_malloc(audioLength * sizeof(*rawAudio), memCaps);
  rxBuffer = (int16_t*)heap_caps_malloc(kRxBufferNum * vadConfig.frame_length() * sizeof(*rxBuffer), MALLOC_CAP_8BIT);
  
  raw_init(mfccConfig.frame_length() + vadConfig.frame_length());
  feature_init(mfccConfig, vadConfig, 3000);

  nsInst = ns_pro_create(vadConfig.frame_time_ms, 1, vadConfig.sample_rate);
  if (nsInst == NULL)
  {
    M5.Display.println("Failed to initialize ns.");
    while(true) delay(10);
  }
  if (!vadEngine.init(vadConfig))
  {
    M5.Display.println("Failed to initialize vad.");
    while(true) delay(10);
  }
  if (!mfccEngine.init(mfccConfig))
  {
    M5.Display.println("Failed to initialize mfcc.");
    while(true) delay(10);
  }
  
  SPIFFS.begin(true);
  // 第9.32弾: テンプレートの読み込みは SD を主、SPIFFS を保険とする。
  // SD 側はファーム書き込みでも消えないため、書き込みをまたいで登録が
  // 残る。SD アクセスは健全性管理（sdAccessBegin/End）の枠組みに従い、
  // 応答停止したカードへは繰り返し試行しない。wakeword_init() は
  // setup 内（LED タスク起動前・ミューテックス生成後）から呼ばれる
  // 単独実行区間で、ここでの読み取りに書き込みガードは要らない。
  bool ww_loaded = false;
  if (sdAccessBegin())
  {
    if (SD.exists(file_name))
    {
      long sz = wwFileSize(SD);
      mfcc = mfccEngine.loadFile(sd_base_path file_name);
      if (mfcc != nullptr)
      {
        Serial.printf("[WW] template loaded from SD (%ld bytes)\n", sz);
        ww_loaded = true;
      } else {
        Serial.println("[WW] SD template failed validation; falling back to SPIFFS");
      }
    }
    sdAccessEnd();
  } else if (sdAccessDead()) {
    Serial.println("[WW] SD unavailable (card dead); trying SPIFFS");
  }
  if (!ww_loaded && SPIFFS.exists(file_name))
  {
    long sz = wwFileSize(SPIFFS);
    mfcc = mfccEngine.loadFile(base_path file_name);
    if (mfcc != nullptr)
    {
      Serial.printf("[WW] template loaded from SPIFFS (%ld bytes)\n", sz);
      ww_loaded = true;
    } else {
      Serial.println("[WW] SPIFFS template failed validation");
    }
  }
  if (ww_loaded)
  {
    M5.Display.println("File exists !!");
    //mode = 1;
  } else {
    Serial.println("[WW] no wakeword template (not registered)");
  }
}

bool wakeword_regist() // 検出した音声を再生しMFCCを登録及びファイルに保存します
{
  bool ret =false;
  // M5.Speaker.end();
  // M5.Mic.begin();
  auto* data = rxMic();
  // M5.Mic.end();
  // M5.Speaker.begin();
  if (data == nullptr) {
    g_ww_diag_nulls++;  // 第9.32弾: 診断ログ用カウンタ
    wwRegistDiagFlush(false);
    return ret;
  }
  // 第9.32弾: 診断用に生フレームのピークを測る（ns_process の前＝
  // マイクに届いたままの音量）。検出のしきい値自体は変えていない。
  {
    const int frameLength = vadEngine.config().frame_length();
    int32_t peak = 0;
    for (int i = 0; i < frameLength; i++) {
      int32_t v = data[i];
      if (v < 0) { v = -v; }
      if (v > peak) { peak = v; }
    }
    g_ww_diag_frames++;
    if (peak > g_ww_diag_peak) { g_ww_diag_peak = peak; }
  }
  ns_process(nsInst, data, data);

  int length = vadEngine.detect(rawAudio, audioLength, data);
  if (length <= 0) {
    wwRegistDiagFlush(false);
    return ret;
  }
  Serial.printf("[WW] regist: utterance detected (%d samples)\n", length);  // 第9.32弾
  wwRegistDiagFlush(true);
//  if (length <= 100) { return ret; }
  M5.Mic.end();
  if (M5.Speaker.begin())
  {
    M5.Speaker.playRaw(rawAudio, length, kSampleRate);
    while (M5.Speaker.isPlaying()) { delay(10); }
    M5.Speaker.end();
  }
  M5.Mic.begin();
  if (mfcc != nullptr){ delete mfcc; }
  mfcc = mfccEngine.create(rawAudio, length);
  if (mfcc)
  {
    // 第9.31弾: SPIFFS へのテンプレート保存をフラッシュ書き込みガードで
    // 囲む。従来はガードの対象外で、保存中のキャッシュ無効化と側面LED
    // （RMT）送信が重なり、登録中に "Cache disabled" パニックが発生した
    // （v9.30 実機）。保存は "wb" で開き直すため、前回パニックで中途半端に
    // 残ったファイルも次回登録でそのまま上書きされる。
    // 第9.32弾: 保存先を SD（主）＋ SPIFFS（保険）に二重化し、ファーム
    // 書き込みで SPIFFS が消えても SD から登録が残るようにした。両保存
    // とも同じガードの内側で行う。片方でも成功すれば登録は完了として
    // 扱い、両方失敗したときだけ登録失敗（ret=false）とする。
    FlashWriteGuard guard(FXW_WAKEWORD);
    bool saved_any = false;
    if (sdAccessBegin())
    {
      bool ok_sd = mfccEngine.saveFile(sd_base_path file_name, *mfcc);
      long sz_sd = ok_sd ? wwFileSize(SD) : -1;
      Serial.printf("[WW] save to SD: %s (%ld bytes)\n", ok_sd ? "ok" : "FAILED", sz_sd);
      saved_any = saved_any || ok_sd;
      sdAccessEnd();
    } else {
      Serial.printf("[WW] save to SD: skipped (%s)\n",
                    sdAccessDead() ? "card dead" : "not mounted");
    }
    bool ok_spiffs = mfccEngine.saveFile(base_path file_name, *mfcc);
    long sz_spiffs = ok_spiffs ? wwFileSize(SPIFFS) : -1;
    Serial.printf("[WW] save to SPIFFS: %s (%ld bytes)\n", ok_spiffs ? "ok" : "FAILED", sz_spiffs);
    saved_any = saved_any || ok_spiffs;
    if (saved_any) {
      ret = true;
    } else {
      Serial.println("[WW] regist: save failed on both SD and SPIFFS");
    }
    //mode = 1;
  }
  vadEngine.reset();
//  mode = 0;
//  M5.Mic.end();
  return ret;
}

bool wakeword_compare()  
{
  // M5.Speaker.end();
  // M5.Mic.begin();
  bool ret = false;
  auto* data = rxMic();
  // M5.Speaker.begin();
  if (data == nullptr) {
    return false; 
  }
  ns_process(nsInst, data, data);

  if (mfcc == nullptr) {
     return false; 
  }
  static int mfccFrameCount = 0;
  const auto state = vadEngine.process(data);
  const int vadFrameLength = vadEngine.config().frame_length();
  const int mfccFrameLength = mfccEngine.config().frame_length();
  const int mfccHopLength = mfccEngine.config().hop_length();
  // Silence以上ならrawBufferにデータを追加
  if (state >= simplevox::VadState::Silence)
  {
    raw_pushBack(data, vadFrameLength);
  }
  // rawBufferにMFCCのframe_length()以上のデータがある場合はMFCCを算出
  while (raw_size() >= mfccFrameLength && mfccFrameCount < mfccFrameNum)
  {
    // feature_pushBack
    mfccEngine.calculate(raw_front(), feature_get(mfccFrameCount));
    mfccFrameCount++;
    raw_popFront(mfccHopLength);  // hop_length分シフト
  }
  // Speech以前(Silence, PreDetection)の場合はmfccBeforeFrameNumを超えた分は取り除く(シフトする)
  if (state < simplevox::VadState::Speech && mfccFrameCount > mfccBeforeFrameNum)
  {
    const int shiftCount = mfccFrameCount - mfccBeforeFrameNum;
    const int shiftLength = shiftCount * mfccCoefNum;
    std::copy_n(&features[shiftLength], mfccFrameCount * mfccCoefNum - shiftLength, features);
    mfccFrameCount -= shiftCount;
  }
  // 検出完了もしくは最大フレームに到達した場合は判定終了
  if (state == simplevox::VadState::Detected
      || (state >= simplevox::VadState::Speech && mfccFrameNum <= mfccFrameCount))
  {
    std::unique_ptr<simplevox::MfccFeature> feature(mfccEngine.create(features, mfccFrameCount, mfccCoefNum));
    const auto dist = simplevox::calcDTW(*mfcc, *feature);
    char cbuf[64];
    char pass = (dist < 180) ? '!': '?';  // 180未満で一致と判定, しきい値は要調整
    sprintf(cbuf, "Dist: %6lu, %c", dist, pass);
    //M5.Display.drawString(cbuf, 0, 50);
    Serial.println(String(cbuf));
    raw_reset();
    mfccFrameCount = 0;
    vadEngine.reset();
    //mode = 0;
//    M5.Mic.end();
    if(dist < 180) ret = true;
  }    
  return ret;
}
