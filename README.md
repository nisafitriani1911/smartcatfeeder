# 🐱 Smart Cat Feeder - Pemberi Makan Kucing Otomatis berbasis ESP32

Proyek Internet of Things (IoT) menggunakan mikrokontroler **ESP32** untuk memberi makan kucing secara otomatis. Katup pakan digerakkan oleh **servo** ketika kucing terdeteksi mendekat (sensor *Ultrasonic*) atau sesuai **jadwal** dari RTC. Alat dapat dipantau dan dikontrol dari jarak jauh melalui **dashboard web** dengan protokol **MQTT** (EMQX Cloud).

---

## 🚀 Fitur Utama
1. **Deteksi Kucing Otomatis:** Servo membuka katup saat kucing berada < 20 cm selama 1 detik, lalu menutup kembali setelah kucing pergi (> 25 cm selama 2 detik).
2. **Jadwal Makan (3 Jadwal):** Pemberian pakan otomatis berdasarkan jam RTC DS1307, dengan jumlah porsi 1-5 (1 porsi = 5 gram).
3. **Stok Pakan dalam Gram:** Kapasitas wadah 100 gram, berkurang saat katup terbuka. Pakan habis akan menolak permintaan makan dan membunyikan buzzer.
4. **Menu di Alat:** Password keypad, menu joystick untuk tambah pakan, nama kucing, jadwal, porsi, set jam, dan tes servo.
5. **Layar TFT ILI9341:** Menampilkan jam, tanggal, status servo, jarak, level pakan, jadwal berikutnya, dan animasi kucing (santai, makan, lapar).
6. **Indikator LED & Buzzer:** LED hijau = katup terbuka, LED merah = katup tertutup. Buzzer memberi tanda saat buka/tutup dan saat ditolak.
7. **Kontrol Jarak Jauh via MQTT:** Beri makan, buka/tutup servo, ganti nama, tambah pakan, ubah jadwal, dan samakan jam dari web.
8. **Dashboard Web:** Pantau status alat secara *real-time*, riwayat kejadian, dan notifikasi (kucing belum makan, pakan menipis, alat offline).

---

## 🛠️ Komponen yang Digunakan
* **Mikrokontroler:** ESP32 DevKit
* **Aktuator / Output:**
  * Servo sebagai katup pakan (`GPIO 16`)
  * Buzzer (`GPIO 15`)
  * LED Merah (`GPIO 32`) dan LED Hijau (`GPIO 4`)
* **Sensor:**
  * HC-SR04 untuk jarak kucing (`TRIG: GPIO 17`, `ECHO: GPIO 19`)
  * RTC DS1307 (`SDA: GPIO 21`, `SCL: GPIO 22`)
* **Input:**
  * Keypad 4x3 (`Baris: 13, 12, 14, 27` | `Kolom: 26, 25, 33`)
  * Joystick analog (`HORZ: GPIO 35`, `VERT: GPIO 34`)
  * Tombol feed manual (`GPIO 0`)
* **Display:** TFT ILI9341 SPI (`CS: GPIO 5`, `DC: GPIO 2`, `SCK: 18`, `MOSI: 23`)

---

## 📌 Logika Kerja Sistem
1. **Kucing mendekat (`< 20 cm` selama 1 dtk)** ➔ Servo **Terbuka**, LED hijau menyala, stok pakan berkurang 2 gram/detik.
2. **Kucing menjauh (`> 25 cm` selama 2 dtk)** ➔ Servo **Tertutup**, LED merah menyala.
3. **Jadwal tiba (RTC)** ➔ Servo terbuka sesuai porsi yang dipilih, lalu menutup otomatis.
4. **Pakan habis (`<= 0.5 g`)** ➔ Permintaan makan **Ditolak** + buzzer berbunyi.
5. **Pengaman waktu** ➔ Servo menutup paksa jika terbuka lebih dari 60 detik.

---

## 📡 Komunikasi MQTT
Alat terhubung ke broker **EMQX Cloud (Serverless)** melalui TLS (port `8883`). Dashboard web terhubung melalui WebSocket Secure (port `8084`).

**Topik status (dikirim alat):**

| Topik | Isi |
|---|---|
| `smartcat/feeder01/online` | `online` / `offline` (retained + Last Will) |
| `smartcat/feeder01/status` | JSON status (nama, pakan, servo, jarak, jadwal, jam) |
| `smartcat/feeder01/event` | JSON kejadian (kucing makan, feed selesai, ditolak, dll.) |

**Topik perintah (dikirim web):**

| Topik | Payload |
|---|---|
| `smartcat/feeder01/cmd/feed` | `1`-`5` (porsi) |
| `smartcat/feeder01/cmd/porsi` | `1`-`5` |
| `smartcat/feeder01/cmd/pakan` | gram yang ditambah |
| `smartcat/feeder01/cmd/nama` | nama (A-Z, 0-9, maks 8) |
| `smartcat/feeder01/cmd/jadwal/1..3` | `HH:MM` atau `OFF` |
| `smartcat/feeder01/cmd/servo` | `BUKA` / `TUTUP` |
| `smartcat/feeder01/cmd/jam` | `HH:MM` |

---

## 📦 Pustaka (Library) yang Dibutuhkan
Dikelola otomatis oleh PlatformIO melalui `platformio.ini`. Jika memakai Arduino IDE / Wokwi, pastikan *library* berikut terpasang:

* `RTClib` (oleh Adafruit)
* `Adafruit GFX Library`
* `Adafruit ILI9341`
* `Keypad` (oleh Mark Stanley / Alexander Brevig)
* `ESP32Servo`
* `PubSubClient` (oleh Nick O'Leary)

---

## ⚙️ Cara Instalasi & Penggunaan

### 1. Siapkan broker MQTT (EMQX Cloud)
1. Buat deployment **Serverless** di EMQX Cloud.
2. Di **Access Control → Authentication**, tambahkan user dan password untuk alat dan web.
3. Catat alamat broker (`xxxx.ala.asia-southeast1.emqxsl.com`).

### 2. Jalankan firmware (Wokwi atau ESP32 asli)
1. Buka `src/main.cpp`, lalu isi `MQTT_HOST`, `MQTT_USER`, dan `MQTT_PASS` sesuai broker Anda.
2. **Wokwi:** salin kode ke proyek Wokwi, pastikan `diagram.json` dan daftar *library* sesuai, lalu jalankan simulasi.
3. **ESP32 asli (PlatformIO):** ubah `WIFI_SSID` / `WIFI_PASS`, hapus argumen channel pada `WiFi.begin`, lalu unggah dengan `pio run -t upload`.
4. Masukkan password keypad, lalu buka **Serial Monitor** (`115200 baud`). Pastikan muncul `[MQTT] connect... OK`.

### 3. Jalankan dashboard web
1. Ubah alamat broker (`HOST`) pada `web/index.html` agar sama dengan broker Anda.
2. Deploy folder `web` ke hosting statis (misalnya **Netlify Drop**) atau buka langsung `index.html` di browser.
3. Login dengan user dan password MQTT, lalu kontrol alat dari tab **Kontrol**.

### Struktur Folder
```
├── src/main.cpp        # Firmware ESP32
├── web/index.html      # Dashboard web (MQTT over WebSocket)
├── diagram.json        # Rangkaian Wokwi
├── wokwi.toml          # Konfigurasi simulasi Wokwi
└── platformio.ini      # Konfigurasi PlatformIO
```

---

## 💡 Catatan Tambahan
* Alat tampil **offline** di web jika simulasi Wokwi tidak berjalan atau belum terhubung ke broker.
* Kode `rc=5` pada Serial Monitor berarti user/password MQTT salah, sedangkan `rc=-2` berarti alamat broker salah atau jaringan bermasalah.
* Verifikasi sertifikat TLS dimatikan (`USE_INSECURE true`) agar mudah dipakai di Wokwi. Untuk alat sungguhan, isi `CA_CERT` dan ubah menjadi `false`.
* Jangan menyimpan password MQTT asli di repositori publik. Gunakan password khusus proyek ini.
* Jika arah joystick terbalik, ubah `JOY_INVERT_X` / `JOY_INVERT_Y` di bagian atas `main.cpp`.
