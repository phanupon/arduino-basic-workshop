/*
 * ESP32-S3 Internet Radio v4.1 — ฉบับแก้เสถียรภาพ + วินิจฉัยตัวเอง
 * --------------------------------------------------------------------
 * แก้จาก v4 (อาการ: ไม่มีเสียง + ปุ่มไม่ทำงาน = ไปไม่ถึง loop):
 *  1. Metadata callback แค่ "ตั้งธง" — ย้ายการวาดจอมาทำใน loop()
 *     (เดิมวาดจอจากใน callback ที่ถูกเรียกลึกในตัวถอดรหัส MP3 = เสี่ยงล้ม)
 *  2. ปุ่มแบบ non-blocking ตรวจขอบสัญญาณ (เดิมมี while ค้าง)
 *  3. สถานีเปิดไม่ติด -> ข้ามไปสถานีถัดไปอัตโนมัติใน 3 วิ (เดิมค้างรอ)
 *  4. พิมพ์ [BOOT-x] ทุกขั้น -> เปิด Serial Monitor (115200) จะรู้ทันที
 *     ว่าโปรแกรมเดินถึงไหน / ตายตรงไหน
 *
 * ไลบรารี: ESP8266Audio 2.x + Adafruit SSD1306 + ESP32 Core 3.x
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ===================== การตั้งค่า =====================
const char *SSID = "phanupon";
const char *PASSWORD = "123456";

#define I2S_BCLK 15
#define I2S_LRC 7
#define I2S_DOUT 16
#define OLED_SDA 8
#define OLED_SCL 9
#define BUTTON_PIN 17

// ===================== รายการสถานี =====================
struct Station {
  const char *name;
  const char *url;
};

Station stations[] = {
  { "SomaFM U80s",       "http://ice1.somafm.com/u80s-128-mp3" },
  { "SomaFM GrooveSalad","http://ice1.somafm.com/groovesalad-128-mp3" },
  { "SomaFM DEF CON",    "http://ice1.somafm.com/defcon-128-mp3" },
  { "SomaFM Lush",       "http://ice1.somafm.com/lush-128-mp3" },
  { "SomaFM Indie Pop",  "http://ice1.somafm.com/indiepop-128-mp3" },
};
const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);
int currentStation = 0;

// ===================== ตัวแปรระบบ =====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOK = false;

AudioGeneratorMP3 *mp3 = NULL;
AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioOutputI2S *out = NULL;

uint8_t *streamBuffer = NULL;
size_t bufferSize = 0;

String songTitle = "";
volatile bool titleChanged = false;   // [FIX 1] ธงจาก callback
bool playFailed = false;              // [FIX 3] สถานีเปิดไม่ติด
unsigned long failTime = 0;

int scrollX = 0;
unsigned long lastScroll = 0;
unsigned long lastReport = 0;
int lastButtonState = HIGH;           // [FIX 2] ตรวจขอบสัญญาณ
unsigned long lastButtonTime = 0;

// ===================== Callbacks =====================
void StatusCallback(void *cbData, int code, const char *string) {
  static unsigned long lastPrint = 0;
  static uint32_t suppressed = 0;
  if (millis() - lastPrint > 1000) {
    if (suppressed > 0) {
      Serial.printf("  (...ข้าม log %u รายการ...)\n", suppressed);
      suppressed = 0;
    }
    Serial.printf("STATUS(%d): %s\n", code, string);
    lastPrint = millis();
  } else suppressed++;
}

// [FIX 1] callback ทำงานให้น้อยที่สุด: เก็บข้อความ + ตั้งธง แล้วจบ
// ห้ามวาดจอ/ทำงานหนักในนี้ เพราะถูกเรียกจากลึกในตัวถอดรหัส MP3
void MetadataCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  (void)cbData; (void)isUnicode;
  if (strstr(type, "Title") != NULL) {
    songTitle = String(string);
    songTitle.trim();
    titleChanged = true;
  }
}

// ===================== จอ OLED =====================
void drawScreen() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("[%d/%d] %s", currentStation + 1, NUM_STATIONS,
                 stations[currentStation].name);
  display.drawLine(0, 12, 127, 12, SSD1306_WHITE);

  display.setTextSize(2);
  if (songTitle.length() == 0) {
    display.setCursor(0, 24);
    display.print("...");
  } else {
    int textWidth = songTitle.length() * 12;
    if (textWidth <= 128) {
      display.setCursor((128 - textWidth) / 2, 24);
      display.print(songTitle);
    } else {
      display.setCursor(-scrollX, 24);
      display.print(songTitle);
    }
  }

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("BOOT btn = next station");
  display.display();
}

void drawMessage(const char *line1, const char *line2) {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20); display.println(line1);
  display.setCursor(0, 36); display.println(line2);
  display.display();
}

// ===================== ระบบเสียง =====================
void stopPlayback() {
  if (mp3)  { if (mp3->isRunning()) mp3->stop(); delete mp3;  mp3 = NULL; }
  if (buff) { delete buff; buff = NULL; }
  if (file) { delete file; file = NULL; }
}

void startPlayback() {
  Station &st = stations[currentStation];
  Serial.printf("\n>>> Station [%d/%d]: %s\n", currentStation + 1, NUM_STATIONS, st.name);
  songTitle = "";
  scrollX = 0;
  playFailed = false;
  drawMessage("Connecting...", st.name);

  Serial.println("    opening stream...");
  file = new AudioFileSourceICYStream(st.url);
  if (!file->isOpen()) {
    Serial.println("    FAILED to open! Auto-next in 3s...");
    drawMessage("Stream FAILED", "Next station in 3s");
    delete file; file = NULL;
    playFailed = true; failTime = millis();   // [FIX 3]
    return;
  }
  file->RegisterMetadataCB(MetadataCallback, NULL);

  Serial.println("    creating buffer...");
  buff = new AudioFileSourceBuffer(file, streamBuffer, bufferSize);
  buff->RegisterStatusCB(StatusCallback, NULL);

  Serial.println("    starting decoder...");
  mp3 = new AudioGeneratorMP3();
  mp3->RegisterStatusCB(StatusCallback, NULL);

  if (!mp3->begin(buff, out)) {
    Serial.println("    Decoder FAILED! Auto-next in 3s...");
    drawMessage("Decoder FAILED", "Next station in 3s");
    stopPlayback();
    playFailed = true; failTime = millis();   // [FIX 3]
    return;
  }
  Serial.println("=== Playing! ===");
  drawScreen();
}

void nextStation() {
  Serial.println("\n[Next station]");
  stopPlayback();
  currentStation = (currentStation + 1) % NUM_STATIONS;
  startPlayback();
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Internet Radio v4.1 ===");
  Serial.println("[BOOT-1] Button pin...");
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println("[BOOT-2] OLED init...");
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) oledOK = true;
  else if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) oledOK = true;
  Serial.printf("[BOOT-2] OLED %s\n", oledOK ? "OK" : "NOT FOUND (continue without)");
  drawMessage("Internet Radio v4.1", "Connecting WiFi...");

  Serial.println("[BOOT-3] WiFi connect...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(SSID, PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 15000) {
      Serial.println("\n[BOOT-3] WiFi FAILED! Restarting...");
      drawMessage("WiFi FAILED!", "Check SSID/PASS");
      delay(3000);
      ESP.restart();
    }
  }
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("\n[BOOT-3] WiFi OK | IP: %s | RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  Serial.println("[BOOT-4] Allocating stream buffer...");
  #ifdef BOARD_HAS_PSRAM
    bufferSize = 131072;
    streamBuffer = (uint8_t *)ps_malloc(bufferSize);
  #endif
  if (streamBuffer == NULL) { bufferSize = 65536; streamBuffer = (uint8_t *)malloc(bufferSize); }
  if (streamBuffer == NULL) { bufferSize = 32768; streamBuffer = (uint8_t *)malloc(bufferSize); }
  if (streamBuffer == NULL) {
    Serial.println("[BOOT-4] FATAL: out of memory!");
    drawMessage("FATAL", "Out of memory");
    while (true) delay(1000);
  }
  Serial.printf("[BOOT-4] Buffer %u KB %s\n", bufferSize / 1024,
                psramFound() ? "(PSRAM)" : "(SRAM)");

  Serial.println("[BOOT-5] I2S output...");
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.8);

  Serial.println("[BOOT-6] Start first station...");
  startPlayback();
  Serial.println("[BOOT-7] Entering loop()");
}

// ===================== Loop =====================
void loop() {
  // 1) ถอดรหัสเสียง
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("Stream lost. Reconnecting...");
      stopPlayback();
      playFailed = true; failTime = millis() - 2000;  // ลองใหม่ใน 1 วิ
    }
  }

  // 2) [FIX 3] สถานีล้มเหลว -> ไปสถานีถัดไปอัตโนมัติใน 3 วิ
  if (playFailed && millis() - failTime > 3000) {
    playFailed = false;
    currentStation = (currentStation + 1) % NUM_STATIONS;
    startPlayback();
  }

  // 3) [FIX 2] ปุ่มแบบ non-blocking: ตรวจ "ขอบขาลง" (เพิ่งกด)
  int btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastButtonState == HIGH && millis() - lastButtonTime > 300) {
    lastButtonTime = millis();
    Serial.println("[Button pressed]");
    nextStation();
  }
  lastButtonState = btn;

  // 4) [FIX 1] ชื่อเพลงเปลี่ยน -> วาดจอจากใน loop (ปลอดภัย)
  if (titleChanged) {
    titleChanged = false;
    scrollX = 0;
    Serial.printf("♪ Now Playing: %s\n", songTitle.c_str());
    drawScreen();
  }

  // 5) ตัวหนังสือวิ่ง (เฉพาะตอนกำลังเล่นและชื่อยาวเกินจอ)
  if (mp3 && mp3->isRunning() &&
      songTitle.length() * 12 > 128 && millis() - lastScroll > 150) {
    lastScroll = millis();
    scrollX += 4;
    if (scrollX > (int)(songTitle.length() * 12)) scrollX = -128;
    drawScreen();
  }

  // 6) รายงานสุขภาพทุก 10 วิ
  if (millis() - lastReport > 10000) {
    lastReport = millis();
    if (buff) {
      uint32_t filled = buff->getFillLevel();
      Serial.printf("[Buffer] %u%% | RSSI: %d dBm | Heap: %u KB | ♪ %s\n",
                    (filled * 100) / bufferSize, WiFi.RSSI(),
                    ESP.getFreeHeap() / 1024, songTitle.c_str());
    }
  }
}
