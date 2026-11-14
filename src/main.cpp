// ===== SMART CAT FEEDER - SERVO BUKA SELAMA KUCING MAKAN =====
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

// ---------- WIFI + MQTT (EMQX Cloud Serverless) ----------
#define WIFI_SSID   "Wokwi-GUEST"          // WiFi bawaan simulator Wokwi
#define WIFI_PASS   ""
#define MQTT_HOST   "ge188d00.ala.asia-southeast1.emqxsl.com"
#define MQTT_PORT   8883                   // MQTT over TLS
#define MQTT_USER   "ISI_USERNAME"         // buat di EMQX Console > Access Control > Authentication
#define MQTT_PASS   "ISI_PASSWORD"
#define TOPIC_STATUS "smartcat/status"     // ESP32 -> HP (retained, tiap 2 dtk)
#define TOPIC_CMD    "smartcat/cmd"        // HP -> ESP32: "feed"
#define TOPIC_ONLINE "smartcat/online"     // "1"/"0" (last will)

#define TOPIC_EVENT  "smartcat/event"      // ESP32 -> HP: kejadian (makan, ditolak, pakan menipis)

WiFiClientSecure netClient;
PubSubClient mqtt(netClient);
String pendingCmd = "";                    // perintah dari HP, dijalankan di loop()
unsigned long lastMqttTry = 0, lastPub = 0;
bool remoteHold = false;                   // katup dibuka manual dari HP
unsigned long remoteStart = 0;
const unsigned long REMOTE_MAX_MS = 30000; // pengaman: menutup otomatis setelah 30 dtk
uint32_t bootTs = 0, lastFeedTs = 0;       // waktu RTC (unix) saat nyala & terakhir kucing makan
char lastSrc[12] = "-";
int lowState = 0;                          // 0 ok, 1 menipis, 2 habis

// ---------- PIN ----------
#define TFT_CS    5     // TFT: SCK=18, MOSI=23, RST & LED -> 3V3 (MISO tidak dipakai)
#define TFT_DC    2
#define SERVO_PIN 16
#define TRIG_PIN  17    // HC-SR04 TRIG
#define ECHO_PIN  19    // HC-SR04 ECHO
#define POT_PIN   39    // slide potentiometer: level pakan (label "VN")
#define BTN_PIN   0     // tombol feed manual
#define BUZZ_PIN  15
#define LED_R     32    // merah: servo TERTUTUP (kucing tidak makan)
#define LED_G     4     // hijau: servo TERBUKA (kucing makan)
#define JOY_X     35
#define JOY_Y     34
// I2C RTC: SDA=21, SCL=22

// ---------- SUDUT SERVO ----------
#define SERVO_OPEN   0      // sudut saat katup TERBUKA
#define SERVO_CLOSED 90     // sudut saat katup TERTUTUP (ubah angka ini kalau posisinya kurang pas)

// ---------- PENGATURAN ----------
const float CAT_NEAR_CM  = 20.0;         // kucing dianggap datang jika < 20 cm
const float CAT_LEAVE_CM = 25.0;         // kucing dianggap masih ada jika < 25 cm
const unsigned long CONFIRM_MS = 1000;   // harus dekat 1 dtk baru servo buka
const unsigned long LEAVE_MS   = 2000;   // jauh 2 dtk baru servo tutup
const unsigned long MAX_EAT_MS = 60000;  // pengaman: maks makan 60 dtk
const unsigned long MIN_GAP_MS = 20000;  // jeda antar makan (simulasi 20 dtk; asli mis. 4 jam = 14400000)
const int REQ_MAX = 3;                   // maksimal sesi makan "minta" per hari

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
int porsi = 3;                    // 1..5 (jadwal/manual: servo buka porsi x 1 detik)
long lastFeedKey = -1;
unsigned long lastManual = 0, lastScreen = 0, lastCat = 0, lastPing = 0, lastLog = 0;
unsigned long lastFeedMs = 0;     // kapan terakhir selesai makan (0 = belum pernah)
bool needStatic = true;
int catFrame = 0, catMood = 0;    // mood: 0 santai, 1 makan, 2 lapar/sedih

// status deteksi kucing
float lastDist = 400;
bool catNear = false, handled = false, eating = false;
bool servoOpen = false;           // status katup: true = terbuka
unsigned long nearSince = 0, farSince = 0, eatStart = 0, lastNearMs = 0;
int reqCount = 0, lastDay = -1;
long leaveLeft = 0;               // sisa ms sebelum servo menutup (untuk tampilan layar)

const char* items[] = {"1 Jadwal 1", "2 Jadwal 2", "3 Jadwal 3",
                       "4 Porsi", "5 Set Jam", "6 Tes Servo", "7 Keluar"};
const int NITEMS = 7;

// ---------- EVENT KE HP ----------
void pubEvent(const char* type, const char* src, int val, const char* txt) {
  if (!mqtt.connected()) return;
  char j[220];
  snprintf(j, sizeof(j), "{\"type\":\"%s\",\"src\":\"%s\",\"val\":%d,\"txt\":\"%s\",\"ts\":%lu}",
           type, src, val, txt, (unsigned long)rtc.now().unixtime());
  mqtt.publish(TOPIC_EVENT, j);
}

void markFed(const char* src) {
  lastFeedTs = rtc.now().unixtime();
  strncpy(lastSrc, src, sizeof(lastSrc) - 1);
  lastSrc[sizeof(lastSrc) - 1] = 0;
}

// ---------- LED + SERVO ----------
// open=true  -> servo BUKA,  HIJAU nyala konstan, MERAH mati
// open=false -> servo TUTUP, MERAH nyala konstan, HIJAU mati
void ledEating(bool e) {
  digitalWrite(LED_G, e ? HIGH : LOW);
  digitalWrite(LED_R, e ? LOW : HIGH);
}

void setServo(bool open) {
  servoOpen = open;
  servo.write(open ? SERVO_OPEN : SERVO_CLOSED);
  ledEating(open);
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
const int SPLASH_TOTAL = 26;

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

// ---------- SENSOR ----------
float readDistance() {                       // HC-SR04: jarak kucing
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return 400;
  return dur * 0.0343 / 2.0;
}

int foodPercent() {                          // slide potentiometer: level pakan
  return map(analogRead(POT_PIN), 0, 4095, 0, 100);
}

// ---------- MAKAN OTOMATIS: BUKA SELAMA KUCING ADA ----------
void refuse(const char* l1, const char* l2) {
  Serial.printf(">> DITOLAK: %s - %s\n", l1, l2);
  pubEvent("refused", "kucing", 0, l1);
  ledEating(false);                               // MERAH nyala, hijau mati
  tft.fillScreen(BG);
  txt(10, 8, 3, ILI9341_RED, BG, l1);
  txt(10, 50, 2, ILI9341_WHITE, BG, l2);
  beep(2, 120);
  for (int i = 0; i < 14; i++) { drawCat(2, catFrame++); showCat(110, 80); delay(120); }
  needStatic = true;
}

void startEating() {
  eating = true;
  eatStart = lastNearMs = millis();
  farSince = 0;
  leaveLeft = 0;
  reqCount++;
  setServo(true);                                 // BUKA katup + HIJAU konstan, merah mati
  beep(1, 150);
  Serial.println(">> Kucing makan: servo BUKA");
  pubEvent("start", "kucing", 0, "kucing mulai makan");
}

void stopEating(const char* why) {
  setServo(false);                                // TUTUP katup + MERAH konstan, hijau mati
  eating = false;
  leaveLeft = 0;
  lastFeedMs = millis();
  markFed("kucing");
  pubEvent("ate", "kucing", (int)((millis() - eatStart) / 1000), why);
  beep(2, 100);
  Serial.print(">> Servo TUTUP, alasan: ");
  Serial.println(why);
}

void catRequest() {
  if (foodPercent() <= 5) {
    refuse("PAKAN HABIS!", "Isi wadah dulu");
    return;
  }
  if (reqCount >= REQ_MAX) {
    refuse("Jatah habis", "Besok lagi ya!");
    return;
  }
  unsigned long since = millis() - lastFeedMs;
  if (lastFeedMs != 0 && since < MIN_GAP_MS) {
    char b[28];
    snprintf(b, sizeof(b), "Tunggu %lu detik", (MIN_GAP_MS - since) / 1000 + 1);
    refuse("Belum waktunya", b);
    return;
  }
  startEating();
}

void checkCat() {
  if (millis() - lastPing < 100) return;
  lastPing = millis();
  lastDist = readDistance();

  if (millis() - lastLog > 1000) {                // log jarak ke Serial Monitor
    lastLog = millis();
    Serial.printf("Jarak: %.1f cm | %s\n", lastDist, eating ? "MAKAN" : "standby");
  }

  // --- sedang makan: servo tetap terbuka sampai kucing pergi ---
  if (eating) {
    if (lastDist < CAT_LEAVE_CM) {
      lastNearMs = millis();
      leaveLeft = 0;
    } else {
      leaveLeft = (long)LEAVE_MS - (long)(millis() - lastNearMs);
      if (leaveLeft <= 0) { stopEating("kucing pergi"); return; }
    }
    if (millis() - eatStart > MAX_EAT_MS)  stopEating("waktu makan habis");
    else if (foodPercent() <= 5)           stopEating("pakan habis");
    return;
  }

  if (remoteHold) return;                         // katup sedang dibuka manual dari HP

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
    if (handled) {                                // kucing harus pergi dulu 1.5 dtk
      if (farSince == 0) farSince = millis();
      if (millis() - farSince > 1500) handled = false;
    }
  }
}

// ---------- MAKAN TERUKUR (JADWAL / MANUAL / TES) ----------
bool feed(const char* reason, int p) {
  if (eating)     { pubEvent("refused", reason, 0, "kucing sedang makan"); return false; }
  if (remoteHold) { pubEvent("refused", reason, 0, "katup sedang dibuka dari HP"); return false; }
  tft.fillScreen(BG);

  if (foodPercent() <= 5) {                       // pakan habis
    ledEating(false);                             // MERAH nyala, hijau mati
    pubEvent("refused", reason, 0, "pakan habis");
    txt(30, 8, 3, ILI9341_RED, BG, "PAKAN HABIS!");
    txt(50, 44, 2, ILI9341_WHITE, BG, "Isi wadah dulu");
    drawCat(2, 3); showCat(110, 80);
    beep(3, 300);
    for (int i = 0; i < 8; i++) { drawCat(2, catFrame++); showCat(110, 80); delay(120); }
    needStatic = true;
    return false;
  }

  char b[30];
  snprintf(b, sizeof(b), "%s - Porsi %d", reason, p);
  txt(10, 8, 3, ILI9341_GREEN, BG, "Memberi makan");
  txt(10, 44, 2, ILI9341_WHITE, BG, b);
  txt(100, 190, 2, ILI9341_CYAN, BG, "Nyam nyam~");

  setServo(true);                                 // BUKA katup + HIJAU konstan, merah mati
  beep(1, 300);

  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)p * 1000) {   // porsi x 1 detik
    drawCat(1, catFrame++); showCat(110, 80); delay(100);
  }
  setServo(false);                                // TUTUP katup + MERAH konstan, hijau mati
  for (int i = 0; i < 5; i++) { drawCat(1, catFrame++); showCat(110, 80); delay(100); }

  lastFeedMs = millis();
  markFed(reason);
  pubEvent("fed", reason, p, "porsi selesai");
  needStatic = true;
  return true;
}

void checkSchedule() {
  DateTime now = rtc.now();
  if (now.day() != lastDay) { lastDay = now.day(); reqCount = 0; }   // reset harian
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
int readNumber(const char* title, int maxDigits) {
  String s = "";
  bool dirty = true;
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(10, 7, 2, ILI9341_WHITE, ILI9341_NAVY, title);
  tft.drawRect(60, 80, 200, 80, ILI9341_WHITE);
  txt(10, 200, 2, ILI9341_GREEN, BG, "#=OK   *=Hapus/Batal");
  while (true) {
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
      else return -1;           // batal
    }
  }
}

void editJadwal(int i) {
  int h = readNumber("Jam (0-23)", 2);   if (h < 0 || h > 23) { msg("Batal / invalid"); return; }
  int m = readNumber("Menit (0-59)", 2); if (m < 0 || m > 59) { msg("Batal / invalid"); return; }
  int a = readNumber("Aktif? 1=Ya 0=Tidak", 1); if (a < 0) return;
  jadwal[i] = {h, m, a == 1};
  msg("Jadwal tersimpan", "", 1200, ILI9341_GREEN);
}

bool doItem(int idx) {          // return true = keluar menu
  switch (idx) {
    case 0: case 1: case 2: editJadwal(idx); break;
    case 3: { int p = readNumber("Porsi (1-5)", 1);
              if (p >= 1 && p <= 5) { porsi = p; msg("Porsi tersimpan", "", 1200, ILI9341_GREEN); }
              else msg("Invalid"); break; }
    case 4: { int h = readNumber("Set Jam (0-23)", 2);
              int m = readNumber("Set Menit (0-59)", 2);
              if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                DateTime n = rtc.now();
                rtc.adjust(DateTime(n.year(), n.month(), n.day(), h, m, 0));
                msg("Jam diupdate", "", 1200, ILI9341_GREEN);
              } else msg("Invalid");
              break; }
    case 5: feed("Tes", porsi); break;
    case 6: return true;
  }
  return false;
}

// ---------- MENU (JOYSTICK) ----------
void drawMenuRow(int i, bool sel) {
  int y = 36 + i * 28;
  uint16_t bg = sel ? ILI9341_CYAN : BG;
  tft.fillRect(0, y, 320, 26, bg);
  txt(12, y + 5, 2, sel ? ILI9341_BLACK : ILI9341_WHITE, bg, items[i]);
}

void drawMenuFull(int idx) {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, 320, 30, ILI9341_NAVY);
  txt(10, 7, 2, ILI9341_WHITE, ILI9341_NAVY, "MENU");
  txt(90, 12, 1, ILI9341_WHITE, ILI9341_NAVY, "atas/bawah=pilih  kanan=OK  kiri=keluar");
  for (int i = 0; i < NITEMS; i++) drawMenuRow(i, i == idx);
}

void waitCenter() {
  while (analogRead(JOY_X) > 3000 || analogRead(JOY_X) < 1000) delay(20);
  delay(100);
}

void menu() {
  int idx = 0;
  waitCenter();
  drawMenuFull(idx);
  while (true) {
    int y = analogRead(JOY_Y), x = analogRead(JOY_X);
    int old = idx;
    if (y < 1000)      idx = (idx + NITEMS - 1) % NITEMS;
    else if (y > 3000) idx = (idx + 1) % NITEMS;
    if (idx != old) { drawMenuRow(old, false); drawMenuRow(idx, true); delay(200); }

    if (x > 3000) {                       // kanan = pilih
      waitCenter();
      if (doItem(idx)) return;
      waitCenter();
      drawMenuFull(idx);
    }
    if (x < 1000 || keypad.getKey() == '*') {   // kiri / * = keluar
      waitCenter();
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

  // jam besar
  snprintf(b, sizeof(b), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  txt(8, 40, 4, ILI9341_YELLOW, BG, b);

  // tanggal
  snprintf(b, sizeof(b), "%02d/%02d/%04d", now.day(), now.month(), now.year());
  txt(8, 82, 2, ILI9341_CYAN, BG, b);

  // jarak kucing + status servo (dengan hitung mundur tutup)
  int d = (int)lastDist; if (d > 400) d = 400;
  snprintf(b, sizeof(b), "Jrk:%3dcm  ", d);
  txt(140, 82, 1, ILI9341_LIGHTGREY, BG, b);

  char sv[18];
  if (!eating)            snprintf(sv, sizeof(sv), "Servo:TUTUP   ");
  else if (leaveLeft > 0) snprintf(sv, sizeof(sv), "Tutup %lds     ", (leaveLeft + 999) / 1000);
  else                    snprintf(sv, sizeof(sv), "Servo:BUKA    ");
  txt(140, 93, 1, eating ? ILI9341_GREEN : ILI9341_LIGHTGREY, BG, sv);

  // status kucing
  catMood = eating ? 1 : (pct < 20 ? 2 : 0);
  const char* ms = eating ? "Kucing MAKAN~" :
                   (pct <= 5 ? "Pakan habis!" :
                   (pct < 20 ? "Lapar nih...!" :
                   (catNear ? "Kucing datang!" : "Kucing santai")));
  uint16_t mc = eating ? ILI9341_GREEN :
                (pct < 20 ? ILI9341_RED : (catNear ? ILI9341_YELLOW : ILI9341_GREEN));
  snprintf(b, sizeof(b), "%-15s", ms);
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

  // level pakan
  snprintf(n, sizeof(n), "Pakan: %d%%  Minta:%d/%d", pct, reqCount, REQ_MAX);
  snprintf(b, sizeof(b), "%-26s", n);
  txt(0, 158, 2, pct < 20 ? ILI9341_RED : ILI9341_WHITE, BG, b);

  int w = pct * 296 / 100;
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

// ---------- MQTT ----------
void onMqtt(char* topic, byte* payload, unsigned int len) {
  String m;
  for (unsigned int i = 0; i < len; i++) m += (char)payload[i];
  Serial.printf("MQTT cmd: %s\n", m.c_str());
  if (pendingCmd.length() == 0) pendingCmd = m;     // dijalankan di loop() (bukan di callback)
}

// Perintah dari website:
//   feed | feed:N | servo:open | servo:close | porsi:N | sched:i,h,m,on | time:h,m | beep
void runCmd(String c) {
  Serial.printf("CMD: %s\n", c.c_str());
  if (c == "feed" || c.startsWith("feed:")) {
    int p = c.length() > 5 ? c.substring(5).toInt() : porsi;
    if (p < 1 || p > 5) p = porsi;
    feed("HP", p);
  } else if (c == "servo:open") {
    if (eating)                { pubEvent("refused", "HP", 0, "kucing sedang makan"); return; }
    if (foodPercent() <= 5)    { pubEvent("refused", "HP", 0, "pakan habis"); return; }
    remoteHold = true; remoteStart = millis();
    setServo(true);
    beep(1, 150);
    pubEvent("servo", "HP", 1, "katup dibuka dari HP");
  } else if (c == "servo:close") {
    if (!remoteHold) return;
    remoteHold = false;
    setServo(false);
    beep(2, 100);
    lastFeedMs = millis();
    markFed("HP");
    pubEvent("servo", "HP", 0, "katup ditutup, pakan sudah keluar");
  } else if (c.startsWith("porsi:")) {
    int p = c.substring(6).toInt();
    if (p >= 1 && p <= 5) porsi = p;
  } else if (c.startsWith("sched:")) {
    int i, h, m, on;
    if (sscanf(c.c_str() + 6, "%d,%d,%d,%d", &i, &h, &m, &on) == 4 &&
        i >= 0 && i < 3 && h >= 0 && h < 24 && m >= 0 && m < 60) {
      jadwal[i] = {h, m, on == 1};
    }
  } else if (c.startsWith("time:")) {
    int h, m;
    if (sscanf(c.c_str() + 5, "%d,%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
      DateTime n = rtc.now();
      rtc.adjust(DateTime(n.year(), n.month(), n.day(), h, m, 0));
    }
  } else if (c == "beep") {
    beep(2, 150);
  }
  lastPub = 0;                                       // kirim status terbaru segera
}

void mqttLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastMqttTry > 10000) { lastMqttTry = millis(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry < 5000) return;
    lastMqttTry = millis();
    String cid = "smartcat-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, TOPIC_ONLINE, 1, true, "0")) {
      mqtt.publish(TOPIC_ONLINE, "1", true);
      mqtt.subscribe(TOPIC_CMD);
      Serial.println("MQTT terhubung");
    } else {
      Serial.printf("MQTT gagal, rc=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();

  if (millis() - lastPub > 2000) {
    lastPub = millis();
    DateTime now = rtc.now();
    int pct = foodPercent();

    // peringatan pakan menipis / habis (sekali per turun)
    if (pct <= 5 && lowState < 2)       { lowState = 2; pubEvent("empty", "sensor", pct, "pakan habis"); }
    else if (pct < 20 && lowState < 1)  { lowState = 1; pubEvent("low", "sensor", pct, "pakan menipis"); }
    else if (pct >= 30)                 { lowState = 0; }

    long rleft = remoteHold ? (long)(REMOTE_MAX_MS - (millis() - remoteStart)) / 1000 : 0;
    if (rleft < 0) rleft = 0;

    char j[700];
    snprintf(j, sizeof(j),
      "{\"ts\":%lu,\"boot\":%lu,\"last\":%lu,\"src\":\"%s\","
      "\"time\":\"%02d:%02d:%02d\",\"date\":\"%02d/%02d/%04d\",\"dist\":%.0f,\"food\":%d,"
      "\"eating\":%s,\"servo\":\"%s\",\"remote\":%s,\"rleft\":%ld,"
      "\"req\":%d,\"maxreq\":%d,\"porsi\":%d,"
      "\"sched\":[{\"h\":%d,\"m\":%d,\"on\":%d},{\"h\":%d,\"m\":%d,\"on\":%d},{\"h\":%d,\"m\":%d,\"on\":%d}]}",
      (unsigned long)now.unixtime(), (unsigned long)bootTs, (unsigned long)lastFeedTs, lastSrc,
      now.hour(), now.minute(), now.second(), now.day(), now.month(), now.year(),
      lastDist, pct, eating ? "true" : "false", servoOpen ? "BUKA" : "TUTUP",
      remoteHold ? "true" : "false", rleft,
      reqCount, REQ_MAX, porsi,
      jadwal[0].h, jadwal[0].m, jadwal[0].on ? 1 : 0,
      jadwal[1].h, jadwal[1].m, jadwal[1].on ? 1 : 0,
      jadwal[2].h, jadwal[2].m, jadwal[2].on ? 1 : 0);
    mqtt.publish(TOPIC_STATUS, j, true);
  }
}

// ---------- SETUP & LOOP ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  ledEating(false);                              // awal: servo tutup -> MERAH nyala, hijau mati

  Wire.begin(21, 22);
  tft.begin();
  tft.setRotation(1);           // landscape 320x240
  pinMode(ECHO_PIN, INPUT);     // set SETELAH tft.begin() supaya pin 19 tetap input biasa

  splashStatic();
  splashAnim(6, "Memulai...", false);

  servo.attach(SERVO_PIN);
  setServo(false);              // katup tertutup + LED merah
  beep(2, 150);

  if (!rtc.begin()) { msg("RTC ERROR!", "", 0, ILI9341_RED); while (1); }
  if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  lastDay = rtc.now().day();
  bootTs = rtc.now().unixtime();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  netClient.setInsecure();                       // tanpa verifikasi sertifikat (cukup untuk demo)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  splashAnim(8, "Cek sensor...", false);
  splashAnim(12, "Siap!", true);
  delay(500);
}

void loop() {
  if (needStatic) { drawStatic(); needStatic = false; lastScreen = 0; lastCat = 0; }

  mqttLoop();                                 // kirim status / terima perintah dari HP
  if (pendingCmd.length()) { String c = pendingCmd; pendingCmd = ""; runCmd(c); }
  if (remoteHold && millis() - remoteStart > REMOTE_MAX_MS) runCmd("servo:close");   // pengaman

  checkCat();                                 // deteksi kucing + kontrol servo
  ledEating(servoOpen);                      // buka = HIJAU konstan (merah mati); tutup = MERAH konstan (hijau mati)

  if (millis() - lastScreen > 500) { lastScreen = millis(); updateMain(); }

  if (millis() - lastCat > 150) {             // animasi kucing di layar utama
    lastCat = millis();
    drawCat(catMood, catFrame++);
    showCat(212, 34);
  }

  checkSchedule();

  if (digitalRead(BTN_PIN) == LOW && millis() - lastManual > 10000) {   // cooldown 10 dtk
    lastManual = millis();
    feed("Manual", porsi);
  }

  char k = keypad.getKey();
  if (k == '*' || analogRead(JOY_X) > 3000) {   // masuk menu
    menu();
    needStatic = true;
  }
}