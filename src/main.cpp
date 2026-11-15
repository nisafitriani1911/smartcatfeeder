// ===== SMART CAT FEEDER - PASSWORD + MENU + PAKAN (GRAM) + NAMA KUCING + MQTT EMQX + KONTROL WEB =====
#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ---------- WIFI ----------
const char* WIFI_SSID = "Wokwi-GUEST";   // Wokwi: SSID ini, tanpa password
const char* WIFI_PASS = "";
const int   WIFI_CH   = 6;               // channel khusus Wokwi (di board asli hapus argumen ini)

// ---------- MQTT (EMQX CLOUD SERVERLESS) ----------
const char* MQTT_HOST = "ge188d00.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT = 8883;            // MQTT over TLS
const char* MQTT_USER = "catfeeder";     // <-- GANTI: user dari EMQX > Authentication
const char* MQTT_PASS = "catfeeder123"; // <-- GANTI

// true  = tanpa verifikasi sertifikat (paling mudah, cocok untuk Wokwi / belajar)
// false = verifikasi dengan CA cert (lebih aman, isi CA_CERT di bawah)
#define USE_INSECURE true

const char CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
(tempel isi file CA Certificate yang diunduh dari EMQX di sini)
-----END CERTIFICATE-----
)EOF";

// Topik (semua di bawah smartcat/feeder01/)
#define T_ONLINE  "smartcat/feeder01/online"        // retained + Last Will
#define T_STATUS  "smartcat/feeder01/status"        // retained, JSON status
#define T_EVENT   "smartcat/feeder01/event"         // JSON kejadian
#define T_CMD     "smartcat/feeder01/cmd/#"         // perintah masuk

// ---------- PIN ----------
#define TFT_CS    5     // TFT: SCK=18, MOSI=23, RST & LED -> 3V3 (MISO tidak dipakai)
#define TFT_DC    2
#define SERVO_PIN 16
#define TRIG_PIN  17    // HC-SR04 TRIG (JANGAN sambung LED ke pin ini)
#define ECHO_PIN  19    // HC-SR04 ECHO
#define BTN_PIN   0     // tombol feed manual
#define BUZZ_PIN  15
#define LED_R     32    // merah: servo TERTUTUP (kucing tidak makan)
#define LED_G     4     // hijau: servo TERBUKA (kucing makan)
#define JOY_X     35    // HORZ
#define JOY_Y     34    // VERT
// I2C RTC: SDA=21, SCL=22

// ---------- ARAH JOYSTICK ----------
#define JOY_INVERT_X true    // true supaya KANAN = pindah ke kotak berikutnya
#define JOY_INVERT_Y true    // ubah ke true/false kalau ATAS/BAWAH terbalik
#define JOY_LOW  1000
#define JOY_HIGH 3000

// ---------- PASSWORD ----------
const char* PASSWORD = "1010";
const int PASS_LEN = 4;

// ---------- SUDUT SERVO ----------
#define SERVO_OPEN   0      // sudut saat katup TERBUKA
#define SERVO_CLOSED 90     // sudut saat katup TERTUTUP

// ---------- PENGATURAN KUCING ----------
const float CAT_NEAR_CM  = 20.0;         // kucing dianggap datang jika < 20 cm
const float CAT_LEAVE_CM = 25.0;         // kucing dianggap masih ada jika < 25 cm
const unsigned long CONFIRM_MS = 1000;   // harus dekat 1 dtk baru servo buka
const unsigned long LEAVE_MS   = 2000;   // jauh 2 dtk baru servo tutup
const unsigned long MAX_EAT_MS = 60000;  // pengaman: maks makan 60 dtk

// ---------- PENGATURAN PAKAN (GRAM) ----------
const float MAX_GRAM         = 100.0;    // kapasitas wadah
const float FLOW_G_PER_SEC   = 2.0;      // pakan keluar 2 gram per detik saat katup terbuka
const int   GRAM_PER_PORSI   = 5;        // 1 porsi = 5 gram
const float EMPTY_GRAM       = 0.5;      // <= ini dianggap habis
float pakanGram = MAX_GRAM;              // stok awal PENUH (100 gram)
unsigned long lastConsume = 0;

// ---------- NAMA KUCING ----------
#define NAME_MAX 8
char namaKucing[NAME_MAX + 1] = "MIMI";
const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const int NCHARS = sizeof(CHARSET) - 1;

// Keypad 4x3 (kolom C4 tidak dipakai)
const byte ROWS = 4, COLS = 3;
char keys[4][3] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[4] = {13, 12, 14, 27};
byte colPins[3] = {26, 25, 33};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, -1);
RTC_DS1307 rtc;
Servo servo;
WiFiClientSecure netClient;
PubSubClient mqtt(netClient);

#define BG    ILI9341_BLACK
#define BLACK ILI9341_BLACK
#define WHITE ILI9341_WHITE
#define C565(r,g,b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define ORANGE C565(255,150,40)
#define DARKO  C565(200,100,20)
#define PINK   C565(255,150,170)
#define BROWN  C565(150,90,40)
#define TEAR   C565(80,160,255)

// ---------- DATA ----------
struct Jadwal { int h, m; bool on; };
Jadwal jadwal[3] = { {7, 0, true}, {12, 0, true}, {18, 0, true} };
int porsi = 3;                    // 1..5 (1 porsi = 5 gram)
long lastFeedKey = -1;
unsigned long lastManual = 0, lastScreen = 0, lastCat = 0, lastPing = 0, lastLog = 0;
bool needStatic = true;
int catFrame = 0, catMood = 0;    // mood: 0 santai, 1 makan, 2 lapar/sedih

// status deteksi kucing
float lastDist = 400;
bool catNear = false, handled = false, eating = false;
bool servoOpen = false;
unsigned long nearSince = 0, farSince = 0, eatStart = 0, lastNearMs = 0;
long leaveLeft = 0;

const char* items[] = {"1 Tambah Pakan", "2 Nama Kucing", "3 Jadwal 1",
                       "4 Jadwal 2", "5 Jadwal 3", "6 Porsi",
                       "7 Set Jam", "8 Tes Servo", "9 Keluar"};
const int NITEMS = 9;

// ---------- MQTT STATE ----------
volatile int pendFeed = 0;                 // perintah feed dari MQTT (dijalankan di loop)
volatile int pendServo = 0;                // 1 = buka, 2 = tutup (dari web)
bool manualHold = false;                   // servo dibuka manual dari web
bool lastEatingPub = false, lastServoPub = false;
unsigned long lastMqttTry = 0, lastStatusPub = 0;

// ---------- PAKAN ----------
bool pakanHabis() { return pakanGram <= EMPTY_GRAM; }

int foodPercent() {
  return constrain((int)(pakanGram * 100.0 / MAX_GRAM), 0, 100);
}

// ---------- MQTT ----------
void publishStatus() {
  if (!mqtt.connected()) return;
  DateTime now = rtc.now();
  char j[3][6];
  for (int i = 0; i < 3; i++) {
    if (jadwal[i].on) snprintf(j[i], sizeof(j[i]), "%02d:%02d", jadwal[i].h, jadwal[i].m);
    else              snprintf(j[i], sizeof(j[i]), "OFF");
  }
  char js[360];
  snprintf(js, sizeof(js),
    "{\"nama\":\"%s\",\"pakan\":%.1f,\"persen\":%d,\"servo\":\"%s\",\"makan\":%s,"
    "\"jarak\":%d,\"porsi\":%d,\"jadwal\":[\"%s\",\"%s\",\"%s\"],\"jam\":\"%02d:%02d:%02d\"}",
    namaKucing, pakanGram, foodPercent(), servoOpen ? "BUKA" : "TUTUP",
    eating ? "true" : "false", (int)min(lastDist, 400.0f), porsi, j[0], j[1], j[2],
    now.hour(), now.minute(), now.second());
  mqtt.publish(T_STATUS, js, true);
  lastEatingPub = eating;
  lastServoPub  = servoOpen;
  lastStatusPub = millis();
}

void publishEvent(const char* ev, const char* detail) {
  if (!mqtt.connected()) return;
  char js[160];
  snprintf(js, sizeof(js), "{\"event\":\"%s\",\"detail\":\"%s\",\"pakan\":%.1f}",
           ev, detail, pakanGram);
  mqtt.publish(T_EVENT, js);
}

// Callback: HANYA set data / flag (tidak ada delay panjang di sini)
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char buf[48];
  unsigned int n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;
  String t = topic;
  String p = String(buf);
  p.trim();
  Serial.printf("[MQTT] %s = %s\n", topic, buf);

  if (t.endsWith("/cmd/feed")) {                       // payload: 1..5 (porsi), kosong = porsi default
    int v = p.toInt();
    pendFeed = (v >= 1 && v <= 5) ? v : porsi;
  } else if (t.endsWith("/cmd/porsi")) {               // payload: 1..5
    int v = p.toInt();
    if (v >= 1 && v <= 5) porsi = v;
  } else if (t.endsWith("/cmd/pakan")) {               // payload: gram yang ditambah
    int g = p.toInt();
    if (g > 0) {
      pakanGram += g;
      if (pakanGram > MAX_GRAM) pakanGram = MAX_GRAM;
      publishEvent("tambah_pakan", "via MQTT");
    }
  } else if (t.endsWith("/cmd/nama")) {                // payload: nama (A-Z, 0-9, maks 8)
    p.toUpperCase();
    char nb[NAME_MAX + 1];
    int k = 0;
    for (unsigned int i = 0; i < p.length() && k < NAME_MAX; i++)
      if (strchr(CHARSET, p[i]) && p[i] != ' ') nb[k++] = p[i];
    nb[k] = 0;
    if (k > 0) strcpy(namaKucing, nb);
  } else if (t.indexOf("/cmd/jadwal/") >= 0) {         // /cmd/jadwal/1..3  payload: "HH:MM" atau "OFF"
    int idx = t.charAt(t.length() - 1) - '1';
    if (idx >= 0 && idx < 3) {
      p.toUpperCase();
      if (p == "OFF") jadwal[idx].on = false;
      else {
        int h, m;
        if (sscanf(buf, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59)
          jadwal[idx] = {h, m, true};
      }
    }
  } else if (t.endsWith("/cmd/servo")) {               // payload: BUKA / TUTUP
    p.toUpperCase();
    if (p == "BUKA" || p == "1") pendServo = 1;
    else if (p == "TUTUP" || p == "0") pendServo = 2;
  } else if (t.endsWith("/cmd/jam")) {                 // payload: HH:MM
    int h, m;
    if (sscanf(buf, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
      DateTime nw = rtc.now();
      rtc.adjust(DateTime(nw.year(), nw.month(), nw.day(), h, m, 0));
    }
  }
  publishStatus();
}

void mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;
  if (millis() - lastMqttTry < 5000 && lastMqttTry != 0) return;
  lastMqttTry = millis();

  // ID acak per boot: di Wokwi MAC selalu sama, ID kembar membuat dua simulasi saling menendang
  static String cid = "catfeeder-" + String((uint32_t)esp_random(), HEX);
  Serial.print("[MQTT] connect... ");
  // Last Will: kalau ESP32 mati, broker otomatis kirim "offline"
  if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, T_ONLINE, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(T_ONLINE, "online", true);
    mqtt.subscribe(T_CMD, 1);
    publishStatus();
  } else {
    Serial.printf("GAGAL rc=%d\n", mqtt.state());
  }
}

// dipanggil di loop utama
void mqttHandle() {
  mqttConnect();
  if (!mqtt.connected()) return;
  mqtt.loop();
  if (eating != lastEatingPub || servoOpen != lastServoPub || millis() - lastStatusPub > 3000)
    publishStatus();
}

// dipanggil di loop blocking (menu, input, feed) supaya koneksi tidak putus
void mqttTick() {
  if (mqtt.connected()) mqtt.loop();
}

// ---------- LED + SERVO ----------
// servoOpen = true  -> HIJAU konstan, MERAH mati
// servoOpen = false -> MERAH konstan, HIJAU mati
void updateLED() {
  digitalWrite(LED_G, servoOpen ? HIGH : LOW);
  digitalWrite(LED_R, servoOpen ? LOW : HIGH);
}

void setServo(bool open) {
  servoOpen = open;
  servo.write(open ? SERVO_OPEN : SERVO_CLOSED);
  updateLED();
}

// ---------- JOYSTICK ----------
bool joyUp()    { int v = analogRead(JOY_Y); return JOY_INVERT_Y ? (v > JOY_HIGH) : (v < JOY_LOW);  }
bool joyDown()  { int v = analogRead(JOY_Y); return JOY_INVERT_Y ? (v < JOY_LOW)  : (v > JOY_HIGH); }
bool joyRight() { int v = analogRead(JOY_X); return JOY_INVERT_X ? (v < JOY_LOW)  : (v > JOY_HIGH); }
bool joyLeft()  { int v = analogRead(JOY_X); return JOY_INVERT_X ? (v > JOY_HIGH) : (v < JOY_LOW);  }

void waitIdle() {                            // tunggu joystick kembali ke tengah
  unsigned long t0 = millis();
  while ((analogRead(JOY_X) > JOY_HIGH || analogRead(JOY_X) < JOY_LOW ||
          analogRead(JOY_Y) > JOY_HIGH || analogRead(JOY_Y) < JOY_LOW) &&
         millis() - t0 < 2000) {
    updateLED();
    mqttTick();
    delay(20);
  }
  delay(100);
}

// ---------- TFT HELPER ----------
void txt(int x, int y, int sz, uint16_t fg, uint16_t bg, const char* s) {
  tft.setTextSize(sz);
  tft.setTextColor(fg, bg);
  tft.setCursor(x, y);
  tft.print(s);
}

void msg(const char* a, const char* b = "", int ms = 1200, uint16_t col = ILI9341_YELLOW) {
  tft.fillScreen(BG);
  tft.setTextSize(3); tft.setTextColor(col);
  tft.setCursor(10, 80); tft.print(a);
  tft.setTextSize(2); tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 130); tft.print(b);
  needStatic = true;
  if (ms) delay(ms);
}

// ---------- KUCING ANIMASI ----------
#define CAT_W 100
#define CAT_H 96
GFXcanvas16 canvas(CAT_W, CAT_H);

void drawCat(int mood, int frame) {
  canvas.fillScreen(0);
  const int oy = 12;
  bool blink = (frame % 28) < 2;

  // ekor (goyang)
  float sway = sin(frame * (mood == 1 ? 0.7 : 0.35)) * 8;
  float tipY = (mood == 2) ? oy + 58 : oy + 38 + sway;
  for (int i = 0; i <= 10; i++) {
    float t = i / 10.0;
    float x = (1 - t) * (1 - t) * 68 + 2 * (1 - t) * t * 94 + t * t * 88;
    float y = (1 - t) * (1 - t) * (oy + 70) + 2 * (1 - t) * t * (oy + 72) + t * t * tipY;
    canvas.fillCircle((int)x, (int)y, 4, ORANGE);
  }

  // badan + kaki
  canvas.fillRoundRect(26, oy + 46, 44, 36, 14, ORANGE);
  canvas.fillCircle(38, oy + 78, 6, ORANGE);
  canvas.fillCircle(58, oy + 78, 6, ORANGE);
  canvas.drawLine(38, oy + 78, 38, oy + 84, DARKO);
  canvas.drawLine(58, oy + 78, 58, oy + 84, DARKO);

  // kepala + telinga
  canvas.fillCircle(48, oy + 30, 22, ORANGE);
  canvas.fillTriangle(27, oy + 24, 29, oy + 2, 44, oy + 13, ORANGE);
  canvas.fillTriangle(69, oy + 24, 67, oy + 2, 52, oy + 13, ORANGE);
  canvas.fillTriangle(31, oy + 19, 32, oy + 8, 40, oy + 14, PINK);
  canvas.fillTriangle(65, oy + 19, 64, oy + 8, 56, oy + 14, PINK);

  // belang di dahi
  canvas.fillRect(47, oy + 9, 3, 7, DARKO);
  canvas.fillRect(40, oy + 10, 2, 5, DARKO);
  canvas.fillRect(56, oy + 10, 2, 5, DARKO);

  // mata
  int ex[2] = {39, 57};
  for (int e = 0; e < 2; e++) {
    int x = ex[e];
    if (mood == 1) {                       // makan: mata senyum ^ ^
      canvas.drawLine(x - 5, oy + 32, x, oy + 28, BLACK);
      canvas.drawLine(x, oy + 28, x + 5, oy + 32, BLACK);
      canvas.drawLine(x - 5, oy + 33, x, oy + 29, BLACK);
      canvas.drawLine(x, oy + 29, x + 5, oy + 33, BLACK);
    } else if (blink) {                    // kedip
      canvas.drawLine(x - 5, oy + 31, x + 5, oy + 31, BLACK);
      canvas.drawLine(x - 5, oy + 32, x + 5, oy + 32, BLACK);
    } else if (mood == 2) {                // lapar: mata besar memelas
      canvas.fillCircle(x, oy + 30, 6, WHITE);
      canvas.fillCircle(x, oy + 31, 4, BLACK);
      canvas.fillCircle(x - 1, oy + 29, 1, WHITE);
    } else {                               // santai
      canvas.fillCircle(x, oy + 30, 5, WHITE);
      canvas.fillCircle(x + 1, oy + 31, 3, BLACK);
    }
  }
  if (mood == 2) {                         // alis sedih + air mata
    canvas.drawLine(33, oy + 22, 44, oy + 25, DARKO);
    canvas.drawLine(63, oy + 22, 52, oy + 25, DARKO);
    canvas.fillCircle(33, oy + 38, 2, TEAR);
  }

  // hidung + mulut
  canvas.fillTriangle(45, oy + 37, 51, oy + 37, 48, oy + 41, PINK);
  if (mood == 1 && (frame % 2)) {          // mengunyah (mulut terbuka)
    canvas.fillCircle(48, oy + 47, 4, BLACK);
    canvas.fillCircle(48, oy + 49, 2, PINK);
  } else if (mood == 2) {                  // cemberut
    canvas.drawLine(43, oy + 47, 48, oy + 44, BLACK);
    canvas.drawLine(48, oy + 44, 53, oy + 47, BLACK);
  } else {                                 // senyum kecil
    canvas.drawLine(48, oy + 41, 48, oy + 43, BLACK);
    canvas.drawLine(48, oy + 43, 44, oy + 46, BLACK);
    canvas.drawLine(48, oy + 43, 52, oy + 46, BLACK);
  }

  // kumis
  canvas.drawLine(27, oy + 38, 13, oy + 34, WHITE);
  canvas.drawLine(27, oy + 41, 13, oy + 42, WHITE);
  canvas.drawLine(69, oy + 38, 83, oy + 34, WHITE);
  canvas.drawLine(69, oy + 41, 83, oy + 42, WHITE);

  // kibble jatuh saat makan
  if (mood == 1) {
    for (int k = 0; k < 3; k++) {
      int y = (frame * 6 + k * 12) % 38;
      canvas.fillCircle(45 + k * 3, y, 2, BROWN);
    }
  }

  // tanda seru saat lapar/sedih
  if (mood == 2 && (frame % 6) < 3) {
    canvas.setTextSize(2);
    canvas.setTextColor(ILI9341_RED);
    canvas.setCursor(86, 0);
    canvas.print("!");
  }
}

void showCat(int x, int y) {
  tft.drawRGBBitmap(x, y, canvas.getBuffer(), CAT_W, CAT_H);
}

// ---------- SPLASH ----------
int splashFrame = 0;
const int SPLASH_TOTAL = 34;

void splashStatic() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(64, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "SMART CAT FEEDER");
  tft.drawRect(60, 196, 200, 14, ILI9341_WHITE);
}

void splashAnim(int n, const char* status, bool meow) {
  char b[20];
  snprintf(b, sizeof(b), "%-16s", status);
  txt(48, 168, 2, ILI9341_CYAN, BG, b);
  for (int i = 0; i < n; i++) {
    drawCat(0, splashFrame);
    showCat(110, 62);
    int w = splashFrame * 196 / SPLASH_TOTAL;
    if (w > 196) w = 196;
    tft.fillRect(62, 198, w, 10, ILI9341_GREEN);
    if (meow) txt(218, 80, 2, ILI9341_YELLOW, BG, "Meow!");
    splashFrame++;
    delay(120);
  }
}

// ---------- BUZZER ----------
void buzzTone(int freq, int ms) {
  long period = 1000000L / freq;
  long cycles = (long)freq * ms / 1000;
  for (long i = 0; i < cycles; i++) {
    digitalWrite(BUZZ_PIN, HIGH);
    delayMicroseconds(period / 2);
    digitalWrite(BUZZ_PIN, LOW);
    delayMicroseconds(period / 2);
  }
}

void beep(int n, int ms) {
  for (int i = 0; i < n; i++) {
    buzzTone(2000, ms);
    delay(ms);
  }
}

// ---------- PASSWORD ----------
void drawPasswordStatic() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(64, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "SMART CAT FEEDER");
  txt(40, 50, 2, ILI9341_CYAN, BG, "Masukkan password:");
  tft.drawRect(60, 90, 200, 60, ILI9341_WHITE);
  txt(10, 190, 2, ILI9341_GREEN, BG, "#=OK   *=Hapus");
}

void passwordScreen() {
  String s = "";
  bool dirty = true;
  drawPasswordStatic();

  while (true) {
    updateLED();
    if (dirty) {
      char b[PASS_LEN + 1];
      for (int i = 0; i < PASS_LEN; i++) b[i] = (i < (int)s.length()) ? '*' : '_';
      b[PASS_LEN] = 0;
      txt(112, 106, 4, ILI9341_YELLOW, BG, b);
      dirty = false;
    }

    char k = keypad.getKey();
    if (!k) continue;

    if (k >= '0' && k <= '9') {
      if ((int)s.length() < PASS_LEN) { s += k; dirty = true; }
    } else if (k == '*') {
      if (s.length()) { s.remove(s.length() - 1); dirty = true; }
    } else if (k == '#') {
      if (s == PASSWORD) {
        msg("Password benar", "", 0, ILI9341_GREEN);
        beep(1, 150);
        delay(700);
        return;
      } else {
        msg("Password salah!", "Coba lagi", 0, ILI9341_RED);
        beep(2, 150);
        delay(1000);
        s = "";
        drawPasswordStatic();
        dirty = true;
      }
    }
  }
}

// ---------- SENSOR ----------
float readDistance() {                       // HC-SR04: jarak kucing
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return 400;
  return dur * 0.0343 / 2.0;
}

// ---------- MAKAN OTOMATIS: BUKA SELAMA KUCING ADA ----------
void refuse(const char* l1, const char* l2) {
  Serial.printf(">> DITOLAK: %s - %s\n", l1, l2);
  publishEvent("ditolak", "pakan habis");
  updateLED();
  tft.fillScreen(BG);
  txt(10, 8, 3, ILI9341_RED, BG, l1);
  txt(10, 50, 2, ILI9341_WHITE, BG, l2);
  beep(2, 120);
  for (int i = 0; i < 14; i++) { drawCat(2, catFrame++); showCat(110, 80); mqttTick(); delay(120); }
  needStatic = true;
}

void startEating() {
  eating = true;
  eatStart = lastNearMs = lastConsume = millis();
  farSince = 0;
  leaveLeft = 0;
  setServo(true);                                 // BUKA katup + HIJAU konstan
  beep(1, 150);
  Serial.println(">> Servo BUKA");
  publishEvent("kucing_makan", manualHold ? "servo dibuka manual" : "servo buka");
  publishStatus();
}

void stopEating(const char* why) {
  manualHold = false;
  setServo(false);                                // TUTUP katup + MERAH konstan
  eating = false;
  leaveLeft = 0;
  beep(2, 100);
  Serial.print(">> Servo TUTUP, alasan: ");
  Serial.print(why);
  Serial.printf(" | sisa pakan %.1f g\n", pakanGram);
  publishEvent("selesai_makan", why);
  publishStatus();
}

void catRequest() {
  if (pakanHabis()) {
    refuse("PAKAN HABIS!", "Isi: Menu > Tambah Pakan");
    return;
  }
  startEating();
}

void checkCat() {
  if (millis() - lastPing < 100) return;
  lastPing = millis();
  lastDist = readDistance();

  if (millis() - lastLog > 1000) {
    lastLog = millis();
    Serial.printf("Jarak: %.1f cm | %s | pakan %.1f g | MQTT %s\n",
                  lastDist, eating ? "MAKAN" : "standby", pakanGram,
                  mqtt.connected() ? "OK" : "putus");
  }

  // --- sedang makan: servo terbuka, pakan berkurang ---
  if (eating) {
    unsigned long nowMs = millis();
    unsigned long dt = nowMs - lastConsume;
    if (dt > 500) dt = 500;
    lastConsume = nowMs;
    pakanGram -= FLOW_G_PER_SEC * dt / 1000.0;
    if (pakanGram < 0) pakanGram = 0;

    if (!manualHold) {                            // kalau dibuka manual dari web, jangan tutup karena jarak
      if (lastDist < CAT_LEAVE_CM) {
        lastNearMs = millis();
        leaveLeft = 0;
      } else {
        leaveLeft = (long)LEAVE_MS - (long)(millis() - lastNearMs);
        if (leaveLeft <= 0) { stopEating("kucing pergi"); return; }
      }
    }
    if (millis() - eatStart > MAX_EAT_MS)  stopEating("waktu makan habis");
    else if (pakanHabis())                 stopEating("pakan habis");
    return;
  }

  // --- standby: tunggu kucing mendekat ---
  if (lastDist < CAT_NEAR_CM) {
    farSince = 0;
    if (nearSince == 0) nearSince = millis();
    catNear = !handled;
    if (!handled && millis() - nearSince >= CONFIRM_MS) {
      handled = true;
      catNear = false;
      catRequest();
    }
  } else {
    nearSince = 0;
    catNear = false;
    if (handled) {
      if (farSince == 0) farSince = millis();
      if (millis() - farSince > 1500) handled = false;
    }
  }
}

// ---------- MAKAN TERUKUR (JADWAL / MANUAL / TES / MQTT) ----------
bool feed(const char* reason, int p) {
  if (eating) return false;
  tft.fillScreen(BG);

  if (pakanHabis()) {
    updateLED();
    publishEvent("ditolak", "pakan habis");
    txt(30, 8, 3, ILI9341_RED, BG, "PAKAN HABIS!");
    txt(10, 44, 2, ILI9341_WHITE, BG, "Isi: Menu > Tambah Pakan");
    drawCat(2, 3); showCat(110, 80);
    beep(3, 300);
    for (int i = 0; i < 8; i++) { drawCat(2, catFrame++); showCat(110, 80); mqttTick(); delay(120); }
    needStatic = true;
    return false;
  }

  float gram = (float)p * GRAM_PER_PORSI;         // gram yang diminta
  if (gram > pakanGram) gram = pakanGram;         // tidak boleh lebih dari stok
  unsigned long durMs = (unsigned long)(gram / FLOW_G_PER_SEC * 1000.0);

  char b[30];
  snprintf(b, sizeof(b), "%s - %d gram", reason, (int)(gram + 0.5));
  txt(10, 8, 3, ILI9341_GREEN, BG, "Memberi makan");
  txt(10, 44, 2, ILI9341_WHITE, BG, b);
  txt(100, 190, 2, ILI9341_CYAN, BG, "Nyam nyam~");

  setServo(true);                                 // BUKA katup + HIJAU konstan
  beep(1, 300);
  publishEvent("feed_mulai", reason);
  publishStatus();

  unsigned long t0 = millis();
  while (millis() - t0 < durMs) {
    drawCat(1, catFrame++); showCat(110, 80); mqttTick(); delay(100);
  }
  setServo(false);                                // TUTUP katup + MERAH konstan
  for (int i = 0; i < 5; i++) { drawCat(1, catFrame++); showCat(110, 80); mqttTick(); delay(100); }

  pakanGram -= gram;
  if (pakanGram < 0) pakanGram = 0;

  publishEvent("feed_selesai", reason);
  publishStatus();

  needStatic = true;
  return true;
}

void checkSchedule() {
  DateTime now = rtc.now();
  long key = (long)now.day() * 1440 + now.hour() * 60 + now.minute();
  for (int i = 0; i < 3; i++) {
    if (jadwal[i].on && jadwal[i].h == now.hour() && jadwal[i].m == now.minute()
        && key != lastFeedKey) {
      lastFeedKey = key;
      feed("Jadwal", porsi);
    }
  }
}

// ---------- INPUT ANGKA (KEYPAD) ----------
// Angka = ketik, # = OK, * = hapus 1 angka; kalau sudah kosong = batal (return -1)
int readNumber(const char* title, int maxDigits) {
  String s = "";
  bool dirty = true;
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(10, 7, 2, ILI9341_WHITE, ILI9341_NAVY, title);
  tft.drawRect(60, 80, 200, 80, ILI9341_WHITE);
  txt(10, 200, 2, ILI9341_GREEN, BG, "#=OK  *=Hapus/Batal");
  while (true) {
    updateLED();
    mqttTick();
    if (dirty) {
      char b[8];
      String d = s + "_";
      snprintf(b, sizeof(b), "%-5s", d.c_str());
      txt(80, 102, 5, ILI9341_YELLOW, BG, b);
      dirty = false;
    }
    char k = keypad.getKey();

    if (k >= '0' && k <= '9' && (int)s.length() < maxDigits) { s += k; dirty = true; }
    else if (k == '#' && s.length() > 0) return s.toInt();
    else if (k == '*') {
      if (s.length()) { s.remove(s.length() - 1); dirty = true; }
      else return -1;                             // batal
    }
  }
}

// ---------- MENU 1: TAMBAH PAKAN (GRAM) ----------
void tambahPakan() {
  int g = readNumber("Tambah pakan (gram)", 3);
  if (g <= 0) { msg("Batal / invalid"); return; }

  float ruang = MAX_GRAM - pakanGram;
  if (ruang < 0.5) { msg("Wadah penuh!", "", 1500, ILI9341_RED); return; }

  float tambah = (g > ruang) ? ruang : (float)g;
  pakanGram += tambah;
  publishEvent("tambah_pakan", "via menu");
  publishStatus();

  char a[24], b[28];
  if (g > ruang) snprintf(a, sizeof(a), "Penuh! +%d g", (int)(tambah + 0.5));
  else           snprintf(a, sizeof(a), "Pakan +%d g", g);
  snprintf(b, sizeof(b), "Total %d/%d g", (int)(pakanGram + 0.5), (int)MAX_GRAM);
  msg(a, b, 1800, ILI9341_GREEN);
}

// ---------- MENU 2: ATUR NAMA KUCING ----------
// Joystick atas/bawah = ganti huruf, kiri/kanan = pindah kotak, # = simpan, * = batal
void drawNameRow(const char* buf, int pos) {
  const int cw = 36;
  const int x0 = (320 - NAME_MAX * cw) / 2;
  for (int i = 0; i < NAME_MAX; i++) {
    int x = x0 + i * cw;
    uint16_t col = (i == pos) ? ILI9341_CYAN : ILI9341_DARKGREY;
    tft.fillRect(x, 80, cw - 4, 50, BG);
    tft.drawRect(x, 80, cw - 4, 50, col);
    if (i == pos) tft.drawRect(x + 1, 81, cw - 6, 48, col);
    char c[2] = { buf[i], 0 };
    txt(x + 7, 93, 3, ILI9341_YELLOW, BG, c);
  }
}

void editNama() {
  char buf[NAME_MAX + 1];
  int len = strlen(namaKucing);
  for (int i = 0; i < NAME_MAX; i++) buf[i] = (i < len) ? namaKucing[i] : ' ';
  buf[NAME_MAX] = 0;
  int pos = 0;
  bool dirty = true;

  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(10, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "NAMA KUCING");
  txt(10, 150, 1, ILI9341_LIGHTGREY, BG, "Joystick atas/bawah: huruf");
  txt(10, 162, 1, ILI9341_LIGHTGREY, BG, "Joystick kiri/kanan: pindah kotak");
  txt(10, 195, 2, ILI9341_GREEN, BG, "#=Simpan  *=Batal");

  waitIdle();

  while (true) {
    updateLED();
    mqttTick();
    if (dirty) { drawNameRow(buf, pos); dirty = false; }

    char k = keypad.getKey();

    if (joyUp()) {                                // atas = huruf berikutnya
      int ci = strchr(CHARSET, buf[pos]) - CHARSET;
      buf[pos] = CHARSET[(ci + 1) % NCHARS];
      dirty = true; delay(180);
    } else if (joyDown()) {                       // bawah = huruf sebelumnya
      int ci = strchr(CHARSET, buf[pos]) - CHARSET;
      buf[pos] = CHARSET[(ci + NCHARS - 1) % NCHARS];
      dirty = true; delay(180);
    } else if (joyRight()) {                      // kanan = kotak berikutnya
      if (pos < NAME_MAX - 1) pos++;
      dirty = true; waitIdle();
    } else if (joyLeft()) {                       // kiri = kotak sebelumnya
      if (pos > 0) pos--;
      dirty = true; waitIdle();
    }

    if (k == '*') { msg("Batal", "", 800); return; }

    if (k == '#') {                               // simpan
      int last = NAME_MAX - 1;
      while (last >= 0 && buf[last] == ' ') last--;
      if (last < 0) { msg("Nama kosong!", "", 1200, ILI9341_RED); return; }
      int first = 0;
      while (buf[first] == ' ') first++;
      int n = 0;
      for (int i = first; i <= last; i++) namaKucing[n++] = buf[i];
      namaKucing[n] = 0;
      publishStatus();
      msg("Nama tersimpan", namaKucing, 1500, ILI9341_GREEN);
      return;
    }
  }
}

// ---------- MENU LAIN ----------
void editJadwal(int i) {
  int h = readNumber("Jam (0-23)", 2);   if (h < 0 || h > 23) { msg("Batal / invalid"); return; }
  int m = readNumber("Menit (0-59)", 2); if (m < 0 || m > 59) { msg("Batal / invalid"); return; }
  int a = readNumber("Aktif? 1=Ya 0=Tidak", 1); if (a < 0) return;
  jadwal[i] = {h, m, a == 1};
  publishStatus();
  msg("Jadwal tersimpan", "", 1200, ILI9341_GREEN);
}

bool doItem(int idx) {          // return true = keluar menu
  switch (idx) {
    case 0: tambahPakan(); break;
    case 1: editNama(); break;
    case 2: case 3: case 4: editJadwal(idx - 2); break;
    case 5: { int p = readNumber("Porsi (1-5)", 1);
              if (p >= 1 && p <= 5) {
                porsi = p;
                publishStatus();
                char b[24];
                snprintf(b, sizeof(b), "= %d gram", p * GRAM_PER_PORSI);
                msg("Porsi tersimpan", b, 1500, ILI9341_GREEN);
              } else msg("Invalid");
              break; }
    case 6: { int h = readNumber("Set Jam (0-23)", 2);
              int m = readNumber("Set Menit (0-59)", 2);
              if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                DateTime n = rtc.now();
                rtc.adjust(DateTime(n.year(), n.month(), n.day(), h, m, 0));
                msg("Jam diupdate", "", 1200, ILI9341_GREEN);
              } else msg("Invalid");
              break; }
    case 7: feed("Tes", porsi); break;
    case 8: return true;
  }
  return false;
}

// ---------- MENU ----------
// Joystick ATAS/BAWAH = gerakkan pilihan, # = masuk, * = kembali
void drawMenuRow(int i, bool sel) {
  int y = 34 + i * 22;
  uint16_t bg = sel ? ILI9341_CYAN : BG;
  tft.fillRect(0, y, 320, 21, bg);
  txt(12, y + 3, 2, sel ? ILI9341_BLACK : ILI9341_WHITE, bg, items[i]);
}

void drawMenuFull(int idx) {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(10, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "MENU");
  txt(80, 12, 1, ILI9341_WHITE, ILI9341_NAVY, "joystick=pilih  #=OK  *=kembali");
  for (int i = 0; i < NITEMS; i++) drawMenuRow(i, i == idx);
}

void menu() {
  int idx = 0;
  waitIdle();
  drawMenuFull(idx);
  while (true) {
    updateLED();
    mqttTick();
    int old = idx;
    if (joyUp())        idx = (idx + NITEMS - 1) % NITEMS;   // ATAS = naik
    else if (joyDown()) idx = (idx + 1) % NITEMS;            // BAWAH = turun
    if (idx != old) { drawMenuRow(old, false); drawMenuRow(idx, true); delay(200); }

    char k = keypad.getKey();
    if (k == '#') {                               // # = masuk menu yang dipilih
      if (doItem(idx)) return;
      drawMenuFull(idx);
    } else if (k == '*') {                        // * = kembali
      return;
    }
  }
}

// ---------- LAYAR UTAMA ----------
void drawStatic() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(64, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "SMART CAT FEEDER");
  tft.drawRect(10, 180, 300, 22, ILI9341_WHITE);
  txt(10,  208, 1, ILI9341_LIGHTGREY, BG, "JADWAL 1");
  txt(115, 208, 1, ILI9341_LIGHTGREY, BG, "JADWAL 2");
  txt(220, 208, 1, ILI9341_LIGHTGREY, BG, "JADWAL 3");
}

void updateMain() {
  DateTime now = rtc.now();
  int pct = foodPercent();
  char b[34], n[34];

  // indikator MQTT di header
  bool on = mqtt.connected();
  txt(272, 11, 1, on ? ILI9341_GREEN : ILI9341_RED, ILI9341_NAVY, on ? "MQTT:OK" : "MQTT:--");

  // jam besar
  snprintf(b, sizeof(b), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  txt(8, 40, 4, ILI9341_YELLOW, BG, b);

  // tanggal
  snprintf(b, sizeof(b), "%02d/%02d/%04d", now.day(), now.month(), now.year());
  txt(8, 82, 2, ILI9341_CYAN, BG, b);

  // jarak kucing + status servo
  int d = (int)lastDist; if (d > 400) d = 400;
  snprintf(b, sizeof(b), "Jrk:%3dcm  ", d);
  txt(140, 82, 1, ILI9341_LIGHTGREY, BG, b);

  char sv[18];
  if (!eating)            snprintf(sv, sizeof(sv), "Servo:TUTUP   ");
  else if (leaveLeft > 0) snprintf(sv, sizeof(sv), "Tutup %lds     ", (leaveLeft + 999) / 1000);
  else                    snprintf(sv, sizeof(sv), "Servo:BUKA    ");
  txt(140, 93, 1, eating ? ILI9341_GREEN : ILI9341_LIGHTGREY, BG, sv);

  // status kucing (dengan nama)
  catMood = eating ? 1 : (pct < 20 ? 2 : 0);
  const char* st = eating ? "makan~" :
                   (pakanHabis() ? "habis!" :
                   (pct < 20 ? "lapar!" :
                   (catNear ? "datang!" : "santai")));
  uint16_t mc = eating ? ILI9341_GREEN :
                (pct < 20 ? ILI9341_RED : (catNear ? ILI9341_YELLOW : ILI9341_GREEN));
  snprintf(n, sizeof(n), "%s %s", namaKucing, st);
  snprintf(b, sizeof(b), "%-16s", n);
  txt(8, 106, 2, mc, BG, b);

  // jadwal berikutnya
  int nowMin = now.hour() * 60 + now.minute(), best = -1, early = 9999, ei = -1;
  for (int i = 0; i < 3; i++) {
    if (!jadwal[i].on) continue;
    int t = jadwal[i].h * 60 + jadwal[i].m;
    if (t > nowMin && (best < 0 || t < best)) best = t;
    if (t < early) { early = t; ei = i; }
  }
  if (best < 0 && ei >= 0) best = early;
  if (best >= 0) snprintf(n, sizeof(n), "Next %02d:%02d  Porsi %d", best / 60, best % 60, porsi);
  else           snprintf(n, sizeof(n), "Semua jadwal OFF");
  snprintf(b, sizeof(b), "%-26s", n);
  txt(0, 136, 2, ILI9341_WHITE, BG, b);

  // level pakan (gram)
  snprintf(n, sizeof(n), "Pakan: %d/%d g", (int)(pakanGram + 0.5), (int)MAX_GRAM);
  snprintf(b, sizeof(b), "%-26s", n);
  txt(0, 158, 2, pct < 20 ? ILI9341_RED : ILI9341_WHITE, BG, b);

  int w = (int)(pakanGram * 296.0 / MAX_GRAM);
  w = constrain(w, 0, 296);
  uint16_t c = pct >= 50 ? ILI9341_GREEN : (pct >= 20 ? ILI9341_YELLOW : ILI9341_RED);
  tft.fillRect(12, 182, w, 18, c);
  tft.fillRect(12 + w, 182, 296 - w, 18, BG);

  // daftar jadwal
  for (int i = 0; i < 3; i++) {
    int x = 10 + 105 * i;
    if (jadwal[i].on) {
      snprintf(b, sizeof(b), "%02d:%02d", jadwal[i].h, jadwal[i].m);
      txt(x, 220, 2, ILI9341_GREEN, BG, b);
    } else {
      txt(x, 220, 2, ILI9341_DARKGREY, BG, " OFF ");
    }
  }
}

// ---------- WIFI ----------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CH);
  Serial.print("[WiFi] konek");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    splashAnim(1, "Konek WiFi...", false);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] OK, IP %s\n", WiFi.localIP().toString().c_str());
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");   // WIB, dipakai untuk validasi sertifikat
  } else {
    Serial.println("\n[WiFi] gagal (alat tetap jalan offline)");
  }
}

// ---------- SETUP & LOOP ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  servoOpen = false;
  updateLED();                                   // awal: MERAH konstan, hijau mati

  Wire.begin(21, 22);
  tft.begin();
  tft.setRotation(1);           // landscape 320x240
  pinMode(ECHO_PIN, INPUT);     // set SETELAH tft.begin() supaya pin 19 tetap input biasa

  passwordScreen();             // minta password dulu (1010)

  splashStatic();
  splashAnim(6, "Memulai...", false);

  servo.attach(SERVO_PIN);
  setServo(false);              // katup tertutup + LED merah
  beep(2, 150);

  if (!rtc.begin()) { msg("RTC ERROR!", "", 0, ILI9341_RED); while (1); }
  if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  // --- WiFi + MQTT ---
  wifiConnect();
#if USE_INSECURE
  netClient.setInsecure();
#else
  netClient.setCACert(CA_CERT);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(512);
  splashAnim(2, "Konek MQTT...", false);
  mqttConnect();

  splashAnim(5, "Cek sensor...", false);
  splashAnim(8, "Siap!", true);
  delay(500);
}

void loop() {
  if (needStatic) { drawStatic(); needStatic = false; lastScreen = 0; lastCat = 0; }

  checkCat();                                 // deteksi kucing + kontrol servo
  updateLED();                                // buka = HIJAU konstan; tutup = MERAH konstan
  mqttHandle();                               // koneksi MQTT + kirim status

  if (millis() - lastScreen > 500) { lastScreen = millis(); updateMain(); }

  if (millis() - lastCat > 150) {             // animasi kucing di layar utama
    lastCat = millis();
    drawCat(catMood, catFrame++);
    showCat(212, 34);
  }

  checkSchedule();

  if (pendFeed > 0 && !eating) {              // perintah feed dari web/MQTT
    int p = pendFeed;
    pendFeed = 0;
    feed("MQTT", p);
  }

  if (pendServo == 1) {                       // BUKA dari web
    pendServo = 0;
    if (!eating && !pakanHabis()) { manualHold = true; startEating(); }
    else if (pakanHabis()) publishEvent("ditolak", "pakan habis");
  } else if (pendServo == 2) {                // TUTUP dari web
    pendServo = 0;
    if (eating) stopEating("ditutup manual");
  }

  if (digitalRead(BTN_PIN) == LOW && millis() - lastManual > 10000) {   // cooldown 10 dtk
    lastManual = millis();
    feed("Manual", porsi);
  }

  char k = keypad.getKey();
  if (k == '#') {                             // # = buka menu
    menu();
    needStatic = true;
  }
}