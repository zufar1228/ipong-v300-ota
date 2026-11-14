# iPong V300 OTA

Robot tenis meja otomatis berbasis ESP32 dengan kontrol via web + GitHub OTA.

## Struktur Repo

```
ipong-v300-ota/
├── src/
│   └── main.cpp          ← Kode firmware lengkap
├── firmware/
│   ├── version.json      ← Metadata versi (dibaca ESP32 saat cek OTA)
│   └── firmware.bin      ← Binary hasil build (upload setiap update)
└── platformio.ini        ← Konfigurasi PlatformIO
```

## Hardware

| Komponen | Pin ESP32 | Keterangan |
|---|---|---|
| MOSFET 1 (Topspin) | GPIO 25 | Roda atas |
| MOSFET 2 (Backspin) | GPIO 26 | Roda bawah |
| MOSFET 3 (Ball Feeder) | GPIO 27 | Frekuensi jatuh bola |
| H-Bridge IN1 (Osilasi L) | GPIO 14 | Tengok kiri |
| H-Bridge IN2 (Osilasi R) | GPIO 33 | Tengok kanan |

Kompatibel dengan **L9110S** maupun **MX1508**.

## Cara Pakai

### Setup Awal (Sekali)
1. Build di PlatformIO → ambil `.pio/build/esp32doit-devkit-v1/firmware.bin`
2. Flash ke ESP32 via USB (pakai [Adafruit WebSerial ESPTool](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/))
3. Hubungkan HP ke WiFi **`iPong-V300-Pro`** (password: `12345678`)
4. Buka browser → `192.168.4.1`
5. Masukkan WiFi internet di bagian bawah halaman

### Update Jarak Jauh (OTA)
1. Edit kode → naikkan `FW_VERSION` (misal `"1.0.0"` → `"1.0.1"`)
2. Update `firmware/version.json` dengan versi baru
3. Build → timpa `firmware/firmware.bin` dengan hasil build terbaru
4. `git push` ke branch `main`
5. Orang di lokasi tekan **"Cek & Update Firmware (GitHub)"** di halaman web HP

## Fitur
- **AP + STA WiFi Mode** — tetap bisa dikontrol via AP lokal saat update OTA
- **Soft-Start PWM** — naik bertahap, adaptor 0.8A tidak drop
- **Gear Protection** — jeda 80ms saat motor osilasi berbalik arah
- **5 Slot Preset** — tersimpan permanen di NVS (flash internal ESP32)
- **Safe OTA** — semua motor dimatikan sebelum flashing
