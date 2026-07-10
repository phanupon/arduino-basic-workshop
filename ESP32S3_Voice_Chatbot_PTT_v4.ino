/*********************************************************************
 * ESP32-S3 AI Voice Chatbot (Push-to-Talk ที่ขา GPIO 17)  — v4
 * -------------------------------------------------------------------
 * อัปเดตจาก v3:
 *   [แก้] หลังเล่นเสียงจบทุกครั้ง สั่งถอนไดรเวอร์ I2S พอร์ต 0 ซ้ำเอง
 *         เพราะไลบรารีเคลียร์สถานะแต่ไม่ถอนไดรเวอร์จริง ทำให้เล่น
 *         รอบที่ 2 ขึ้น "register I2S object to platform failed"
 *   [แก้] สร้างตัวเล่นเสียงใหม่ทุกรอบ (สถานะสะอาดเสมอ)
 *   หมายเหตุ: ถ้าเห็นบรรทัดแดง "I2S port 0 has not installed"
 *   หลังเล่นจบ ไม่ต้องสนใจ เป็นการยืนยันว่าพอร์ตถูกถอนแล้ว
 *
 * การทำงาน:
 *   1) รอจนขา GPIO 17 = 1 (กดปุ่มค้าง)
 *   2) อัดเสียงจากไมค์ INMP441 ตลอดเวลาที่ขา 17 = 1 (สูงสุด 8 วินาที)
 *   3) ส่งไฟล์เสียง (WAV) ให้ Gemini ฟัง + ตอบเป็นข้อความไทย
 *   4) แปลงข้อความเป็นเสียงพูดไทย (Google Translate TTS)
 *   5) เล่นเสียงออกลำโพงผ่าน MAX98357A
 *   6) แสดงสถานะบนจอ OLED SSD1306
 *
 * ไลบรารี: Adafruit SSD1306 (+GFX), ESP8266Audio, ArduinoJson (v7)
 * บอร์ด: ESP32S3 Dev Module + Tools > PSRAM: OPI PSRAM (จำเป็น!)
 *********************************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>          // I2S legacy driver สำหรับไมค์ (RX)
#include "mbedtls/base64.h"

// ---- ESP8266Audio สำหรับเล่น MP3 ออกลำโพง ----
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

/**************** 1. ตั้งค่า WiFi / API KEY *****************/
const char* WIFI_SSID = "ssid";
const char* WIFI_PASS = "password";
const char* GEMINI_API_KEY = "Key";   // จาก https://aistudio.google.com

// alias "gemini-flash-latest" ชี้รุ่น Flash ล่าสุดเสมอ ไม่โดน 404 อีก
const char* GEMINI_MODEL   = "gemini-flash-latest";

/**************** 2. กำหนดขา (ปรับให้ตรงวงจรเดิมของคุณ!) *****************/
// ปุ่ม Push-to-Talk
#define PIN_TALK        17        // ขา 17 = 1 -> เริ่มอัดเสียง

// ไมโครโฟน INMP441 -> ใช้ I2S พอร์ต 1 (โหมดรับ)
#define I2S_MIC_SCK     4         // BCLK / SCK
#define I2S_MIC_WS      5         // WS  / LRCL
#define I2S_MIC_SD      6         // SD  / DOUT ของไมค์
#define MIC_I2S_PORT    I2S_NUM_1
// * ขา L/R ของ INMP441 ต่อลง GND (ช่องซ้าย)

// ลำโพง MAX98357A -> ใช้ I2S พอร์ต 0 (ติดตั้ง/ถอนใหม่ทุกครั้งที่พูด)
// เอาเลข 3 ตัวจาก SetPinout(BCLK, LRC, DIN) ของสเก็ตช์ Radio มาใส่ให้ตรง
#define I2S_SPK_BCLK    15        // ← เลขตัวที่ 1 จาก SetPinout ของ Radio
#define I2S_SPK_LRC     7        // ← เลขตัวที่ 2
#define I2S_SPK_DIN     16         // ← เลขตัวที่ 3

// จอ OLED SSD1306 (I2C)
#define OLED_SDA        8
#define OLED_SCL        9
#define OLED_ADDR       0x3C
#define SCREEN_W        128
#define SCREEN_H        64

/**************** 3. ค่าคงที่การอัดเสียง *****************/
#define SAMPLE_RATE     16000     // 16 kHz เหมาะกับงานรู้จำเสียงพูด
#define MAX_RECORD_SEC  8         // อัดได้สูงสุด 8 วินาที
#define WAV_HDR_SIZE    44
#define MAX_SAMPLES     (SAMPLE_RATE * MAX_RECORD_SEC)
#define MAX_WAV_BYTES   (WAV_HDR_SIZE + MAX_SAMPLES * 2)   // 16-bit mono

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// บัฟเฟอร์ใน PSRAM
uint8_t*  wavBuf   = nullptr;     // ไฟล์ WAV ที่อัดได้
uint8_t*  mp3Buf   = nullptr;     // ไฟล์ MP3 จาก TTS
size_t    wavLen   = 0;

// เก็บบทสนทนาย้อนหลังสั้น ๆ เพื่อให้คุยต่อเนื่อง
String historyUser[3];
String historyBot[3];
int    historyCount = 0;

/*====================================================================
 *  OLED: แสดงสถานะ
 *===================================================================*/
void showStatus(const char* line1, const char* line2 = "") {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 8);
  oled.println(line1);
  oled.setTextSize(1);
  oled.setCursor(0, 40);
  oled.println(line2);
  oled.display();
}

/*====================================================================
 *  ไมโครโฟน: อัดเสียงขณะขา 17 = 1  ->  สร้างไฟล์ WAV ใน wavBuf
 *  (ใช้ I2S พอร์ต 1 ติดตั้งเฉพาะช่วงอัดแล้วถอนทิ้ง)
 *===================================================================*/
void writeWavHeader(uint8_t* buf, uint32_t dataBytes) {
  uint32_t byteRate = SAMPLE_RATE * 2;
  memcpy(buf, "RIFF", 4);
  uint32_t chunk = dataBytes + 36;           memcpy(buf + 4,  &chunk, 4);
  memcpy(buf + 8, "WAVEfmt ", 8);
  uint32_t sub1 = 16;                        memcpy(buf + 16, &sub1, 4);
  uint16_t fmt = 1, ch = 1;                  memcpy(buf + 20, &fmt, 2);
                                             memcpy(buf + 22, &ch, 2);
  uint32_t sr = SAMPLE_RATE;                 memcpy(buf + 24, &sr, 4);
                                             memcpy(buf + 28, &byteRate, 4);
  uint16_t align = 2, bits = 16;             memcpy(buf + 32, &align, 2);
                                             memcpy(buf + 34, &bits, 2);
  memcpy(buf + 36, "data", 4);               memcpy(buf + 40, &dataBytes, 4);
}

bool recordWhileTalkPin() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,     // INMP441 ส่ง 32 บิต
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num  = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };
  if (i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[MIC] ติดตั้งไดรเวอร์ I2S ไม่สำเร็จ");
    return false;
  }
  i2s_set_pin(MIC_I2S_PORT, &pins);
  i2s_zero_dma_buffer(MIC_I2S_PORT);

  int32_t  raw[256];
  int16_t* pcm = (int16_t*)(wavBuf + WAV_HDR_SIZE);
  size_t   sampleCount = 0;
  uint32_t t0 = millis();

  // อัดตราบใดที่ขา 17 ยังเป็น 1 และไม่เกินเวลาสูงสุด
  while (digitalRead(PIN_TALK) == HIGH &&
         sampleCount < MAX_SAMPLES &&
         millis() - t0 < MAX_RECORD_SEC * 1000UL) {
    size_t br = 0;
    i2s_read(MIC_I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    int n = br / 4;
    for (int i = 0; i < n && sampleCount < MAX_SAMPLES; i++) {
      pcm[sampleCount++] = (int16_t)(raw[i] >> 14);   // 32 บิต -> 16 บิต (ขยายเสียง)
    }
  }
  i2s_driver_uninstall(MIC_I2S_PORT);

  if (sampleCount < SAMPLE_RATE / 2) return false;    // สั้นกว่า 0.5 วิ ไม่เอา
  uint32_t dataBytes = sampleCount * 2;
  writeWavHeader(wavBuf, dataBytes);
  wavLen = WAV_HDR_SIZE + dataBytes;
  Serial.printf("[MIC] อัดเสียงได้ %.1f วินาที (%u ไบต์)\n",
                (float)sampleCount / SAMPLE_RATE, wavLen);
  return true;
}

/*====================================================================
 *  Gemini: ส่งไฟล์เสียง -> ได้ข้อความตอบกลับภาษาไทย
 *===================================================================*/
String askGeminiWithAudio() {
  // 1) เข้ารหัส WAV เป็น base64 (เก็บใน PSRAM)
  size_t b64Cap = ((wavLen + 2) / 3) * 4 + 8;
  uint8_t* b64 = (uint8_t*)ps_malloc(b64Cap);
  if (!b64) return "";
  size_t b64Len = 0;
  mbedtls_base64_encode(b64, b64Cap, &b64Len, wavBuf, wavLen);

  // 2) สร้างพรอมป์ + ประวัติสนทนา
  String prompt =
    "คุณคือผู้ช่วย AI พูดภาษาไทย ฟังเสียงคำถามในไฟล์แนบ "
    "แล้วตอบเป็นภาษาไทยแบบเป็นกันเอง กระชับ ไม่เกิน 2-3 ประโยค "
    "ตอบเป็นข้อความล้วน ห้ามใช้สัญลักษณ์ * # หรือ emoji";
  if (historyCount > 0) {
    prompt += "\n\nบทสนทนาก่อนหน้า:";
    for (int i = 0; i < historyCount; i++) {
      prompt += "\nผู้ใช้: " + historyUser[i] + "\nคุณ: " + historyBot[i];
    }
  }
  // escape ข้อความสำหรับ JSON
  prompt.replace("\\", "\\\\"); prompt.replace("\"", "\\\"");
  prompt.replace("\n", "\\n");

  String prefix =
    "{\"contents\":[{\"parts\":["
    "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
  String suffix =
    "\"}},{\"text\":\"" + prompt + "\"}]}],"
    "\"generationConfig\":{\"maxOutputTokens\":256}}";

  // 3) รวม body ทั้งหมดไว้ใน PSRAM
  size_t bodyLen = prefix.length() + b64Len + suffix.length();
  uint8_t* body = (uint8_t*)ps_malloc(bodyLen + 1);
  if (!body) { free(b64); return ""; }
  memcpy(body, prefix.c_str(), prefix.length());
  memcpy(body + prefix.length(), b64, b64Len);
  memcpy(body + prefix.length() + b64Len, suffix.c_str(), suffix.length());
  free(b64);

  // 4) ส่งไปยัง Gemini API
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" +
               String(GEMINI_MODEL) + ":generateContent?key=" + GEMINI_API_KEY;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(30000);
  int code = http.POST(body, bodyLen);
  free(body);

  String answer = "";
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      const char* t = doc["candidates"][0]["content"]["parts"][0]["text"];
      if (t) answer = String(t);
    }
  } else {
    Serial.printf("[GEMINI] HTTP error %d: %s\n", code,
                  http.getString().substring(0, 200).c_str());
  }
  http.end();
  answer.trim();
  return answer;
}

/*====================================================================
 *  TTS: ข้อความไทย -> เสียงพูด (Google Translate TTS) -> ลำโพง
 *===================================================================*/
String urlEncodeUTF8(const String& s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c)) out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F]; }
  }
  return out;
}

// เล่นข้อความ 1 ท่อน: ดาวน์โหลด MP3 มาไว้ใน PSRAM แล้วเล่นออกลำโพง
bool speakChunk(const String& text) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=th&q="
               + urlEncodeUTF8(text);
  http.begin(client, url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // ชื่อสำหรับ core 2.0.x
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  int code = http.GET();
  int len  = http.getSize();          // อาจเป็น -1 ถ้า Google ส่งแบบ chunked
  Serial.printf("[TTS] HTTP=%d size=%d\n", code, len);
  if (code != 200) { http.end(); return false; }

  const int MAXMP3 = 400000;
  if (mp3Buf) { free(mp3Buf); mp3Buf = nullptr; }
  mp3Buf = (uint8_t*)ps_malloc(MAXMP3);
  if (!mp3Buf) { http.end(); return false; }

  WiFiClient* stream = http.getStreamPtr();
  int got = 0;
  uint32_t tLast = millis();
  while ((len < 0 || got < len) && got < MAXMP3) {
    int avail = stream->available();
    if (avail) {
      got += stream->read(mp3Buf + got, min(avail, MAXMP3 - got));
      tLast = millis();
    } else if (!stream->connected() || millis() - tLast > 3000) {
      break;                          // โหลดครบ / หมดเวลา
    } else delay(2);
  }
  http.end();
  Serial.printf("[TTS] ได้ MP3 %d ไบต์\n", got);
  if (got < 500) return false;        // เล็กเกินไป = โหลดล้มเหลว

  // เล่น MP3: สร้างชุดเล่นเสียงใหม่ทุกรอบ เพื่อให้สถานะสะอาดเสมอ
  AudioFileSourcePROGMEM* file = new AudioFileSourcePROGMEM(mp3Buf, got);
  AudioOutputI2S* out = new AudioOutputI2S(0);        // I2S พอร์ต 0
  out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DIN);
  out->SetGain(0.9);
  AudioGeneratorMP3* mp3 = new AudioGeneratorMP3();
  mp3->begin(file, out);
  Serial.println("[TTS] กำลังเล่นเสียง...");
  while (mp3->isRunning()) {
    if (!mp3->loop()) mp3->stop();
  }
  delete mp3;
  delete out;
  delete file;
  // การันตีว่าพอร์ต 0 ว่างจริงก่อนรอบถัดไป (ไลบรารีบางเวอร์ชันถอนไม่ครบ)
  i2s_driver_uninstall(I2S_NUM_0);
  Serial.println("[TTS] เล่นจบ");
  return true;
}

// แบ่งข้อความยาวเป็นท่อนละไม่เกิน ~180 ไบต์ (ไม่ตัดกลางตัวอักษรไทย UTF-8)
void speakThai(const String& text) {
  const size_t CHUNK = 180;
  size_t pos = 0, len = text.length();
  while (pos < len) {
    size_t end = min(pos + CHUNK, len);
    if (end < len) {
      // ถอยกลับจนถึงจุดเริ่มต้นตัวอักษร UTF-8 (ไบต์ที่ไม่ใช่ 10xxxxxx)
      while (end > pos && ((uint8_t)text[end] & 0xC0) == 0x80) end--;
      // ถ้ามีช่องว่างใกล้ ๆ ให้ตัดตรงช่องว่างแทน (อ่านลื่นกว่า)
      size_t sp = text.lastIndexOf(' ', end);
      if (sp != (size_t)-1 && sp > pos + CHUNK / 2) end = sp;
    }
    String part = text.substring(pos, end);
    part.trim();
    if (part.length() > 0) speakChunk(part);
    pos = end;
  }
}

/*====================================================================
 *  SETUP / LOOP
 *===================================================================*/
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TALK, INPUT_PULLDOWN);   // ขา 17: ปุ่มต่อไป 3.3V (กด = 1)

  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  showStatus("BOOT...", "Connecting WiFi");

  // ตรวจสอบ PSRAM
  Serial.printf("PSRAM found: %d\n", psramFound());
  Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());

  // จองบัฟเฟอร์เสียงใน PSRAM
  wavBuf = (uint8_t*)ps_malloc(MAX_WAV_BYTES);
  if (!wavBuf) {
    showStatus("ERROR", "No PSRAM! Enable OPI PSRAM");
    while (1) delay(1000);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\n[WiFi] " + WiFi.localIP().toString());

  // ---- ทดสอบระบบเสียงทันทีหลังบูต (ลบบรรทัดนี้ออกได้เมื่อระบบนิ่งแล้ว) ----
  showStatus("SPK TEST", "Playing test voice");
  speakChunk("สวัสดีครับ ทดสอบระบบเสียง หนึ่ง สอง สาม");

  showStatus("READY", "Hold GPIO17 = talk");
}

void loop() {
  if (digitalRead(PIN_TALK) == HIGH) {
    delay(30);                                    // กันสัญญาณเด้ง
    if (digitalRead(PIN_TALK) != HIGH) return;

    // ---- 1) อัดเสียง ----
    showStatus("LISTEN...", "Release pin to stop");
    if (!recordWhileTalkPin()) {
      showStatus("TOO SHORT", "Try again");
      delay(1000);
      showStatus("READY", "Hold GPIO17 = talk");
      return;
    }

    // ---- 2) ถาม Gemini ----
    showStatus("THINKING", "Asking Gemini...");
    String answer = askGeminiWithAudio();
    if (answer.length() == 0) {
      showStatus("ERROR", "No answer / check API");
      speakChunk("ขอโทษค่ะ เกิดข้อผิดพลาด กรุณาลองใหม่อีกครั้ง");
      showStatus("READY", "Hold GPIO17 = talk");
      return;
    }
    Serial.println("[AI] " + answer);

    // เก็บประวัติสนทนา (เลื่อนทิ้งอันเก่าสุดเมื่อเต็ม)
    if (historyCount == 3) {
      for (int i = 0; i < 2; i++) {
        historyUser[i] = historyUser[i + 1];
        historyBot[i]  = historyBot[i + 1];
      }
      historyCount = 2;
    }
    historyUser[historyCount] = "(คำถามด้วยเสียง)";
    historyBot[historyCount]  = answer;
    historyCount++;

    // ---- 3) พูดตอบ ----
    showStatus("SPEAKING", "Playing answer...");
    speakThai(answer);

    showStatus("READY", "Hold GPIO17 = talk");
  }
  delay(20);
}
