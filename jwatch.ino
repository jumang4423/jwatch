/*
 * AQM1602Y-RN-GBW (ST7032i, I2C 16x2) demo  —  XIAO ESP32-C6
 *
 *   3 s : 2D animation  (drawn with an ASCII density ramp, no CGRAM needed)
 *   3 s : clock         (normal 5x8 font: HH:MM:SS + date), 6 s full cycle
 *
 * No custom characters are used at all -> nothing to swap, no flicker.
 *
 * I2C uses the ESP32-C6 INTERNAL pull-ups only.
 *   SDA = GPIO22 (D4), SCL = GPIO23 (D5), addr 0x3E
 *
 * Wiring (LCD 9-pin module):
 *   1 V0 open | 2 VOUT --[1uF]-- 5 VDD | 3 CAP1N --[1uF]-- 4 CAP1P
 *   5 VDD->3V3 | 6 VSS->GND | 7 SDA->GPIO22 | 8 SCL->GPIO23 | 9 /RES->3V3
 *
 * Wi-Fi credentials live in "secrets.h".
 */

#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "secrets.h"

// ---- configuration ----
static const int      PIN_SDA = 22;
static const int      PIN_SCL = 23;
static const uint8_t  LCD_ADDR = 0x3E;
static const uint8_t  LCD_CONTRAST = 35;
static const char*    TZ_INFO = "JST-9";
static uint32_t       g_i2cHz = 50000;

#define LCD_COLS 16
#define LCD_ROWS 2
#define FRAME_MS 40
#define PHASE_MS 3000UL          // switch anim <-> clock every 3 s
#define CYCLE_MS 6000UL          // one full cycle = 6 s

// animation density ramp: level 0 (empty) .. 7 (dense), built-in ROM chars
static const char kRamp[8] = {' ', '.', ':', '=', '+', '*', '#', '@'};

// -------------------------------------------------------------- low level
static void lcdCmd(uint8_t v) {
  Wire.beginTransmission(LCD_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(v);
  Wire.endTransmission();
  delayMicroseconds(30);
}

static bool i2cPing(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static void i2cStart(uint32_t hz) {
  Wire.begin(PIN_SDA, PIN_SCL, hz);
  gpio_set_pull_mode((gpio_num_t)PIN_SDA, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode((gpio_num_t)PIN_SCL, GPIO_PULLUP_ONLY);
}

static void busLevels() {
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delayMicroseconds(100);
  Serial.printf("  idle: SDA=%d SCL=%d\n", digitalRead(PIN_SDA), digitalRead(PIN_SCL));
}

static void probeSpeeds() {
  static const uint32_t s[] = {100000UL, 50000UL, 20000UL, 10000UL};
  Serial.println(F("  speed probe:"));
  for (unsigned i = 0; i < 4; i++) {
    Wire.end(); delay(20);
    i2cStart(s[i]); delay(20);
    bool ok = i2cPing(LCD_ADDR);
    Serial.printf("    %6lu Hz : %s\n", (unsigned long)s[i], ok ? "ACK" : "no-ack");
    if (ok) { g_i2cHz = s[i]; return; }
  }
}

static void lcdInit() {
  delay(50);
  lcdCmd(0x38); lcdCmd(0x39); lcdCmd(0x14);
  lcdCmd(0x70 | (LCD_CONTRAST & 0x0F));
  lcdCmd(0x54 | ((LCD_CONTRAST >> 4) & 0x03));
  lcdCmd(0x6C); delay(200);
  lcdCmd(0x38); lcdCmd(0x0C); lcdCmd(0x01); delay(2);
  lcdCmd(0x06);
}

// blit one 16-char row in a single transaction (raw char codes)
static void lcdWriteRow(uint8_t row, const uint8_t* codes) {
  lcdCmd(0x80 | (row ? 0x40 : 0x00));
  Wire.beginTransmission(LCD_ADDR);
  for (uint8_t i = 0; i < LCD_COLS; i++) {
    Wire.write(i == LCD_COLS - 1 ? 0x40 : 0xC0);
    Wire.write(codes[i]);
  }
  Wire.endTransmission();
}

// ------------------------------------------------------------- effects
static uint8_t g_frame[LCD_ROWS][LCD_COLS];

static inline uint8_t quant(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return 7;
  int l = (int)(v * 8.0f);
  return (uint8_t)(l > 7 ? 7 : l);
}

static void fxPlasma(float t) {
  for (int y = 0; y < 2; y++) {
    float fy = y * 4.0f;
    for (int x = 0; x < 16; x++) {
      float v = sinf(x * 0.7f + t * 2.0f)
              + sinf(fy + t * 1.3f)
              + sinf((x * 0.5f - fy * 0.8f) + t * 1.7f);
      g_frame[y][x] = quant((v + 3.0f) / 6.0f);
    }
  }
}

static void fxBands(float t) {
  float ang = t * 0.35f, ca = cosf(ang), sa = sinf(ang);
  for (int y = 0; y < 2; y++) {
    float py = y * 4.0f;
    for (int x = 0; x < 16; x++)
      g_frame[y][x] = quant(0.5f + 0.5f * sinf((x * ca + py * sa) * 0.9f - t * 2.5f));
  }
}

static void fxRipple(float t) {
  float cx = 7.5f + 5.5f * cosf(t * 0.7f);
  float cy = 0.5f + 0.5f * sinf(t * 1.1f);
  for (int y = 0; y < 2; y++) {
    float dy = (y - cy) * 4.0f;
    for (int x = 0; x < 16; x++) {
      float dx = x - cx, d = sqrtf(dx * dx + dy * dy);
      g_frame[y][x] = quant(0.5f + 0.5f * sinf(d * 1.8f - t * 4.0f));
    }
  }
}

static float mbx[3] = {2.0f, 8.0f, 13.0f};
static float mby[3] = {0.0f, 1.0f, 0.4f};
static float mbvx[3] = {1.3f, -0.9f, 0.8f};
static float mbvy[3] = {0.35f, -0.45f, 0.55f};

static void fxMeta(float dt) {
  for (int i = 0; i < 3; i++) {
    mbx[i] += mbvx[i] * dt; mby[i] += mbvy[i] * dt;
    if (mbx[i] < 0) { mbx[i] = 0; mbvx[i] = -mbvx[i]; }
    if (mbx[i] > 15) { mbx[i] = 15; mbvx[i] = -mbvx[i]; }
    if (mby[i] < 0) { mby[i] = 0; mbvy[i] = -mbvy[i]; }
    if (mby[i] > 1) { mby[i] = 1; mbvy[i] = -mbvy[i]; }
  }
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 16; x++) {
      float s = 0.0f;
      for (int i = 0; i < 3; i++) {
        float dx = x - mbx[i], dy = (y - mby[i]) * 4.0f;
        s += 2.4f / (dx * dx + dy * dy + 0.9f);
      }
      g_frame[y][x] = quant(s * 0.5f);
    }
}

static void drawAnim() {
  uint8_t c0[LCD_COLS], c1[LCD_COLS];
  for (int i = 0; i < LCD_COLS; i++) {
    c0[i] = (uint8_t)kRamp[g_frame[0][i]];
    c1[i] = (uint8_t)kRamp[g_frame[1][i]];
  }
  lcdWriteRow(0, c0);
  lcdWriteRow(1, c1);
}

// ---------------------------------------------------------------- clock
static void setTimeFromCompile() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char dbuf[12];
  strncpy(dbuf, __DATE__, sizeof(dbuf) - 1); dbuf[sizeof(dbuf) - 1] = 0;
  char mon[4] = {dbuf[0], dbuf[1], dbuf[2], 0};
  const char* p = strstr(months, mon);
  if (!p) return;
  struct tm t = {};
  t.tm_mon = (int)(p - months) / 3;
  t.tm_mday = atoi(dbuf + 4);
  t.tm_year = atoi(dbuf + 7) - 1900;
  t.tm_hour = atoi(__TIME__);
  t.tm_min = atoi(__TIME__ + 3);
  t.tm_sec = atoi(__TIME__ + 6);
  t.tm_isdst = -1;
  time_t tt = mktime(&t);
  struct timeval tv = { tt, 0 };
  settimeofday(&tv, nullptr);
}

static void startWifiAndNtp() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println(F("WiFi: no credentials -> using build time (approx)"));
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
    struct tm tm; uint32_t t1 = millis();
    while (!getLocalTime(&tm, 500) && millis() - t1 < 10000) Serial.print('*');
    Serial.println();
    if (getLocalTime(&tm, 0))
      Serial.printf("NTP synced: %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    Serial.println(F("WiFi: failed -> using build time (approx)"));
  }
}

// centered 16-char line from a string
static void centerInto(uint8_t* row, const char* s) {
  for (int i = 0; i < LCD_COLS; i++) row[i] = ' ';
  int n = strlen(s); if (n > LCD_COLS) n = LCD_COLS;
  int pad = (LCD_COLS - n) / 2;
  for (int i = 0; i < n; i++) row[pad + i] = (uint8_t)s[i];
}

static void drawClock(const struct tm* t) {
  char buf[17];
  uint8_t r0[LCD_COLS], r1[LCD_COLS];

  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  centerInto(r0, buf);

  snprintf(buf, sizeof(buf), "%04d/%02d/%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  centerInto(r1, buf);

  lcdWriteRow(0, r0);
  lcdWriteRow(1, r1);
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println(F("\n=== AQM1602Y anim + clock (XIAO ESP32-C6) ==="));
  busLevels();
  probeSpeeds();

  Wire.beginTransmission(LCD_ADDR);
  uint8_t err = Wire.endTransmission();
  Serial.printf("LCD 0x%02X ACK: %s @ %lu Hz\n",
                LCD_ADDR, err == 0 ? "OK" : "FAIL", (unsigned long)g_i2cHz);

  lcdInit();
  lcdCmd(0x01); delay(2);

  configTzTime(TZ_INFO, "ntp.nict.jp", "pool.ntp.org");
  setTimeFromCompile();
  startWifiAndNtp();
  Serial.println(F("ready: 3s anim / 3s clock (6s cycle)"));
}

// ----------------------------------------------------------------- loop
void loop() {
  static uint32_t lastAnimMs = 0;
  static float    t = 0.0f;
  static int      lastClockSec = -1;
  static bool     wasClock = false;

  uint32_t now = millis();

  struct tm now_tm;
  bool have = getLocalTime(&now_tm, 0);
  if (!have) {
    uint32_t s = now / 1000;
    now_tm.tm_hour = (s / 3600) % 24; now_tm.tm_min = (s / 60) % 60;
    now_tm.tm_sec = s % 60;
  }

  bool showClock = ((now / PHASE_MS) % 2UL) == 1UL;   // 3 s anim / 3 s clock

  if (showClock) {                                    // -------- clock window
    wasClock = true;
    if (now_tm.tm_sec != lastClockSec) {
      lastClockSec = now_tm.tm_sec;
      drawClock(&now_tm);
    }
    return;
  }

  // -------------------------------------- animation window
  if (wasClock) { wasClock = false; lastAnimMs = now; lastClockSec = -1; }
  if (now - lastAnimMs < FRAME_MS) return;
  float dt = (now - lastAnimMs) / 1000.0f;
  if (dt > 0.2f) dt = 0.2f;
  lastAnimMs = now;
  t += dt; if (t > 1000.0f) t -= 1000.0f;

  uint8_t mode = (uint8_t)((now / CYCLE_MS) % 4);     // new effect each window
  switch (mode) {
    case 0: fxPlasma(t); break;
    case 1: fxBands(t);  break;
    case 2: fxRipple(t); break;
    default: fxMeta(dt); break;
  }
  drawAnim();
}
