# iPong V300 OTA

Robot tenis meja otomatis berbasis ESP32 dengan kontrol via web + GitHub OTA bertanda tangan digital.

## Struktur Repo

```
ipong-v300-ota/
├── src/
│   ├── main.cpp          ← Kode firmware lengkap
│   ├── ota_pubkey.h      ← Public key verifikasi OTA (hasil keygen, aman di-commit)
│   └── web_icons.h       ← Ikon PWA (hasil tools/make_icons.py)
├── tools/
│   ├── ota_release.py    ← Tanda tangan firmware + tulis version.json
│   └── make_icons.py     ← Generate ikon PWA ke src/web_icons.h
├── firmware/
│   ├── version.json      ← Metadata versi + sha256 + tanda tangan (dibaca ESP32 saat cek OTA)
│   └── firmware.bin      ← Binary hasil build (ditulis oleh ota_release.py)
├── android/              ← Aplikasi Android (WebView) untuk panel kontrol
├── .github/workflows/    ← Build APK otomatis di GitHub Actions
├── keys/                 ← Private key OTA + keystore APK (DI-IGNORE git, JANGAN di-commit)
└── platformio.ini        ← Konfigurasi PlatformIO (versi dikunci, partisi min_spiffs)
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

Cek sebelum dinyalakan:
- Resistor pull-down 10k di tiap gate MOSFET (gate floating saat boot/reset → motor bisa berputar sesaat).
- MOSFET logic-level (mis. IRLZ44N, AO3400). Modul IRF520 tidak menyala penuh dengan gate 3.3V dan akan panas.
- Dioda flyback di tiap motor yang di-switch MOSFET.
- GPIO14 mengeluarkan PWM sesaat saat boot → motor osilasi bisa berkedut. Jika mengganggu, pindahkan ke GPIO32/13 (ubah `PIN_OSC_L`).
- Adaptor 0.8A mepet untuk 4 motor + ESP32. Jika ESP32 sering reset saat motor start (brownout), tambahkan kapasitor besar / regulator terpisah untuk ESP32.

## Cara Pakai

### Setup Awal (Sekali, via USB)
1. `pip install platformio cryptography`
2. Key tanda tangan sudah dibuat di `keys/ota_private.pem`. **Backup file ini** (mis. password manager / flashdisk). Jika hilang, robot di lapangan hanya bisa di-update lewat USB. Untuk membuat baru: `python tools/ota_release.py keygen`.
3. Build & flash: `pio run -t upload`, lalu `pio device monitor`.
4. Hubungkan HP ke WiFi **`natsir'S-ipongv300`** (password: `44444444`). Halaman captive portal muncul otomatis; tekan **Buka di Browser** (atau buka `http://192.168.4.1` langsung di browser). Jika HP menampilkan "Jaringan tidak memiliki akses internet", pilih **Tetap terhubung**.
5. Di kartu "WiFi & Update Firmware" tekan **Pindai WiFi Sekitar**, pilih jaringan, isi password → **Simpan & Tes**. Robot mengetes koneksi lalu memutusnya lagi; koneksi internet hanya dipakai saat update. Setelah tersimpan, form disembunyikan dan diganti status koneksi + tombol **Tes Koneksi** / **Ganti WiFi**. Robot juga mengetes WiFi tersimpan sekali setiap kali menyala.

> Partisi flash (`min_spiffs.csv`) hanya bisa diubah lewat USB. Flash pertama wajib memakai `platformio.ini` di repo ini.

### Update Jarak Jauh (OTA)
1. Edit kode → naikkan `FW_VERSION` di `src/main.cpp` (misal `"1.0.0"` → `"1.0.1"`). Robot menolak versi yang sama atau lebih lama.
2. `pio run`
3. `python tools/ota_release.py release` → menyalin `firmware.bin`, menghitung sha256, menandatangani, dan menulis `firmware/version.json`.
4. `git add firmware/ src/ && git commit -m "Release vX.Y.Z" && git push` ke branch `main`.
5. Tunggu ±5 menit (cache `raw.githubusercontent.com`), lalu orang di lokasi tekan **"Cek & Update Firmware (GitHub)"**.

Catatan:
- Repo harus **public** agar `raw.githubusercontent.com` bisa diakses robot.
- Saat robot tersambung ke WiFi internet, AP robot pindah channel mengikuti router. HP bisa terputus beberapa detik lalu tersambung ulang sendiri; pesan status terakhir ditampilkan lagi setelah tersambung.

## Fitur
- **AP + STA on-demand** — STA hanya aktif saat tes WiFi / update, agar AP kontrol tetap stabil.
- **Captive portal** — pop-up login HP hanya menampilkan tombol untuk membuka panel kontrol di browser, plus peringatan bila HP berpindah jaringan otomatis.
- **Aplikasi Android** — PWA tidak bisa di-install dari `http://192.168.4.1` (browser mewajibkan HTTPS), jadi tersedia APK WebView di `android/`. Aplikasi memaksa koneksi lewat WiFi robot walau tanpa internet, layar tetap menyala, dan menampilkan halaman bantuan bila robot tidak terjangkau. Ikon dibuat ulang dengan `python tools/make_icons.py`.

## Aplikasi Android (APK)
- Setiap push yang mengubah `android/` memicu GitHub Actions membangun APK bertanda tangan dan mengunggahnya ke **Releases** (`android-vX.Y`).
- Unduh APK dari halaman Releases saat HP terhubung internet → pasang (izinkan "sumber tidak dikenal").
- Versi baru: naikkan `versionCode` dan `versionName` di `android/app/build.gradle`, lalu push.
- Keystore penanda tangan ada di `keys/android-release.p12` (password di `keys/android-release.password`) dan juga tersimpan sebagai secret GitHub `ANDROID_KEYSTORE_B64` / `ANDROID_KEYSTORE_PASSWORD`. **Backup** keduanya: tanpa keystore yang sama, APK baru tidak bisa meng-update aplikasi yang sudah terpasang (harus uninstall dulu).
- Hanya Android. Di iPhone, buka `http://192.168.4.1` lewat Safari.
- **Soft-Start PWM** — naik bertahap; feeder baru naik setelah roda mencapai kecepatan target.
- **Gear Protection** — jeda 80ms saat motor osilasi berbalik arah.
- **Osilasi terbatas** — posisi diestimasi dari waktu gerak; mode Sweep & Random tidak drift ke satu sisi. (Tanpa sensor posisi, estimasi bisa meleset jika motor tidak simetris; limit switch disarankan.)
- **10 Slot Preset** — tersimpan permanen di NVS, sinkron ke semua HP yang tersambung.
- **Safe OTA** — semua motor dimatikan, perintah kontrol diblokir selama update; firmware dicek ukuran, SHA256, dan tanda tangan ECDSA P-256 sebelum diaktifkan.
