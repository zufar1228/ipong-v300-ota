#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include "ota_pubkey.h"

// ================= KONFIGURASI VERSI & GITHUB =================
// Naikkan setiap rilis (format semver X.Y.Z). OTA hanya menerima versi yang LEBIH BARU.
#define FW_VERSION "1.0.1"
static const char* VERSION_URL = "https://raw.githubusercontent.com/zufar1228/ipong-v300-ota/main/firmware/version.json";
static const char* AP_SSID = "natsir'S-ipongv300";
static const char* AP_PASS = "44444444";

// ================= PIN HARDWARE =================
#define PIN_TOP   25
#define PIN_BACK  26
#define PIN_FEED  27
#define PIN_OSC_L 14   // Catatan: GPIO14 mengeluarkan PWM sesaat saat boot. Pertimbangkan pindah ke GPIO32/13.
#define PIN_OSC_R 33

#define PWM_FREQ 18000 // 18 kHz (senyap, tanpa bunyi berdenging)
#define PWM_RES  8     // 0 - 255

// ================= BATASAN =================
static const uint8_t  NUM_PRESETS            = 5;
static const uint8_t  PRESET_LAYOUT_VER      = 1;     // Naikkan jika struct Preset berubah
static const uint16_t OSC_TIME_MIN           = 400;
static const uint16_t OSC_TIME_MAX           = 3000;
static const uint16_t OSC_DEADTIME_MS        = 80;    // Jeda sebelum balik arah (proteksi gir)
static const uint16_t RAMP_INTERVAL_MS       = 15;
static const uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t HTTP_TIMEOUT_MS        = 15000;
static const size_t   WS_MAX_MSG             = 512;
static const uint8_t  CMD_QUEUE_LEN          = 16;

// ================= STRUKTUR DATA =================
struct Preset {
  char name[20];
  uint8_t top;
  uint8_t back;
  uint8_t feed;
  uint8_t oscSpeed;
  uint8_t oscMode;     // 0: Off, 1: Sweep, 2: Random
  uint16_t oscTime;    // ms (400 - 3000), waktu tempuh satu sapuan penuh kiri<->kanan
};

// Perintah dari WebSocket (task AsyncTCP) ke loop(). Semua state hanya diubah di loop().
enum CmdType : uint8_t { CMD_HELLO, CMD_SET, CMD_SAVE, CMD_WIFI, CMD_OTA };
struct Command {
  CmdType type;
  uint32_t clientId;
  bool run;
  uint8_t slot;
  Preset data;
  char s1[33];  // SSID
  char s2[65];  // password
};

static Preset presets[NUM_PRESETS];
static Preset activeState = {"Manual", 0, 0, 0, 0, 0, 1000};
static bool isRunning = false;

// Soft-start
static float curTop = 0, curBack = 0, curFeed = 0;
static unsigned long lastRampMs = 0;

// Osilasi: posisi diestimasi dalam ms waktu gerak (0 = tengah saat boot)
static long oscPos = 0;
static long oscTarget = 0;
static bool oscTargetValid = false;
static int8_t oscDir = 1;            // 1: kiri, -1: kanan
static bool oscDriving = false;
static bool oscDeadtime = false;
static unsigned long oscDeadStart = 0;
static unsigned long lastOscTick = 0;
static uint8_t lastOscMode = 255;
static int16_t oscOutL = -1, oscOutR = -1;

// WiFi & OTA
static String staSsid = "";
static String staPass = "";
static String lastMsg = "";
static volatile bool otaBusy = false;
static bool staTesting = false;
static unsigned long staTestStart = 0;

static QueueHandle_t cmdQueue;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static DNSServer dnsServer;
static Preferences prefs;

// ================= HELPER PWM (Kompatibel Core 2.x & 3.x) =================
static void setupPwmPin(uint8_t pin, uint8_t ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(pin, PWM_FREQ, PWM_RES);
#else
  ledcSetup(ch, PWM_FREQ, PWM_RES);
  ledcAttachPin(pin, ch);
#endif
}

static void writePwmPin(uint8_t pin, uint8_t ch, uint8_t val) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, val);
#else
  ledcWrite(ch, val);
#endif
}

static void writeOsc(uint8_t l, uint8_t r) {
  if (oscOutL != l) { writePwmPin(PIN_OSC_L, 3, l); oscOutL = l; }
  if (oscOutR != r) { writePwmPin(PIN_OSC_R, 4, r); oscOutR = r; }
}

static void stopAllMotorsImmediate() {
  isRunning = false;
  curTop = curBack = curFeed = 0;
  oscDriving = false;
  oscDeadtime = false;
  writePwmPin(PIN_TOP, 0, 0);
  writePwmPin(PIN_BACK, 1, 0);
  writePwmPin(PIN_FEED, 2, 0);
  oscOutL = oscOutR = -1;  // paksa tulis ulang
  writeOsc(0, 0);
}

static void setRunning(bool run) {
  if (run == isRunning) return;
  if (!run) {
    stopAllMotorsImmediate();
    return;
  }
  isRunning = true;
  lastRampMs = lastOscTick = millis();
  oscDriving = false;
  oscDeadtime = false;
  oscTargetValid = false;
}

// ================= VALIDASI INPUT =================
static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Potong byte UTF-8 yang tidak lengkap di akhir string. Teks WebSocket wajib UTF-8 valid;
// jika tidak, browser menutup koneksi dan UI tidak bisa dipakai.
static void utf8TrimTail(char* s) {
  size_t n = strlen(s);
  size_t i = n, cont = 0;
  while (i > 0 && cont < 4 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) { i--; cont++; }
  if (i == 0) { if (cont) s[0] = '\0'; return; }
  uint8_t lead = (uint8_t)s[i - 1];
  size_t need = (lead & 0x80) == 0x00 ? 1 :
                (lead & 0xE0) == 0xC0 ? 2 :
                (lead & 0xF0) == 0xE0 ? 3 :
                (lead & 0xF8) == 0xF0 ? 4 : 0;
  if (need != cont + 1) s[i - 1] = '\0';
}

static void sanitizePreset(Preset& p) {
  p.name[sizeof(p.name) - 1] = '\0';
  utf8TrimTail(p.name);
  if (p.oscMode > 2) p.oscMode = 0;
  p.oscTime = clampInt(p.oscTime, OSC_TIME_MIN, OSC_TIME_MAX);
}

static void parsePreset(JsonObjectConst d, Preset& p, const char* defName) {
  strlcpy(p.name, d["name"] | defName, sizeof(p.name));
  p.top      = clampInt(d["top"] | 0, 0, 255);
  p.back     = clampInt(d["back"] | 0, 0, 255);
  p.feed     = clampInt(d["feed"] | 0, 0, 255);
  p.oscSpeed = clampInt(d["oscSpeed"] | 0, 0, 255);
  p.oscMode  = clampInt(d["oscMode"] | 0, 0, 2);
  p.oscTime  = clampInt(d["oscTime"] | 1000, OSC_TIME_MIN, OSC_TIME_MAX);
  sanitizePreset(p);
}

// ================= NVS PRESETS & WIFI =================
static void loadStorage() {
  prefs.begin("ipong", false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");

  const Preset defaults[NUM_PRESETS] = {
    {"1. Warmup Top",  140,   0, 100, 130, 1, 1200},
    {"2. Heavy Back",    0, 185,  90, 120, 1, 1400},
    {"3. Fast Drill",  180,  40, 150, 170, 2, 1000},
    {"4. Forehand",    160,   0, 120,   0, 0, 1000},
    {"5. Custom",      120, 120, 110, 140, 1, 1100}
  };

  bool layoutOk = prefs.getUChar("pver", 0) == PRESET_LAYOUT_VER;
  for (int i = 0; i < NUM_PRESETS; i++) {
    char key[4];
    snprintf(key, sizeof(key), "p%d", i);
    if (layoutOk && prefs.getBytesLength(key) == sizeof(Preset)) {
      prefs.getBytes(key, &presets[i], sizeof(Preset));
      sanitizePreset(presets[i]);
    } else {
      presets[i] = defaults[i];
      prefs.putBytes(key, &presets[i], sizeof(Preset));
    }
  }
  if (!layoutOk) prefs.putUChar("pver", PRESET_LAYOUT_VER);
  activeState = presets[0];
}

static void savePreset(uint8_t idx) {
  if (idx >= NUM_PRESETS) return;
  presets[idx] = activeState;
  char key[4];
  snprintf(key, sizeof(key), "p%d", idx);
  prefs.putBytes(key, &presets[idx], sizeof(Preset));
}

// ================= EMBEDDED WEB UI (MOBILE-FIRST) =================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#0a1a33">
<title>iPong V300</title>
<style>
:root{--bg:#0a1a33;--tbl:#12407a;--tbl2:#0e3263;--card:rgba(13,34,66,.82);--line:rgba(255,255,255,.10);
--txt:#f4f7fb;--mut:#9fb3cf;--ball:#ff8a1f;--ball2:#ffb35c;--ok:#22c55e;--err:#ef4444;--red:#e23a3a}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0}
body{min-height:100vh;color:var(--txt);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
background:radial-gradient(1200px 600px at 50% -10%,#1d5aa3 0,transparent 60%),
linear-gradient(180deg,var(--tbl) 0,var(--bg) 70%);background-attachment:fixed}
.wrap{max-width:480px;margin:auto;padding:14px 14px 28px;position:relative}

/* Header: meja + net + bola memantul */
.hero{position:relative;border-radius:20px;overflow:hidden;padding:16px 16px 14px;margin-bottom:14px;
background:linear-gradient(160deg,#1a5bb0,#0f3b74);border:2px solid rgba(255,255,255,.85);
box-shadow:0 10px 30px rgba(0,0,0,.35)}
.hero:before{content:"";position:absolute;left:0;right:0;top:50%;height:2px;background:rgba(255,255,255,.55)}
.hero .net{position:absolute;top:0;bottom:0;left:50%;width:4px;margin-left:-2px;
background:repeating-linear-gradient(0deg,rgba(255,255,255,.9) 0 2px,transparent 2px 6px);opacity:.55}
.hero .ballfly{position:absolute;width:14px;height:14px;border-radius:50%;top:60%;left:8%;
background:radial-gradient(circle at 35% 30%,#fff3 0,var(--ball2) 30%,var(--ball) 70%);
box-shadow:0 4px 8px rgba(0,0,0,.35);animation:rally 2.4s cubic-bezier(.45,.05,.55,.95) infinite}
@keyframes rally{0%{left:8%;top:62%}25%{top:12%}50%{left:86%;top:62%}75%{top:12%}100%{left:8%;top:62%}}
.top{position:relative;display:flex;align-items:center;gap:12px;z-index:1}
.logo{width:46px;height:46px;flex:none;filter:drop-shadow(0 3px 5px rgba(0,0,0,.4))}
.ttl{flex:1;min-width:0}
.ttl h1{margin:0;font-size:20px;letter-spacing:.3px;font-weight:800}
.ttl h1 span{color:var(--ball)}
.ttl p{margin:2px 0 0;font-size:12px;color:#d6e4f7;opacity:.85}
.chip{display:inline-flex;align-items:center;gap:6px;font-size:11px;font-weight:600;padding:5px 10px;border-radius:99px;
background:rgba(0,0,0,.28);border:1px solid rgba(255,255,255,.18);white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--err);box-shadow:0 0 0 3px rgba(239,68,68,.25)}
.chip.on .dot{background:var(--ok);box-shadow:0 0 0 3px rgba(34,197,94,.25)}

/* Banner terputus */
#conn{display:none;margin-bottom:12px;padding:10px 12px;border-radius:12px;font-size:12px;
background:rgba(239,68,68,.15);border:1px solid rgba(239,68,68,.45);color:#fecaca;text-align:center}

/* Tombol power */
.power{position:relative;width:100%;border:none;border-radius:18px;padding:18px 16px;margin-bottom:14px;cursor:pointer;
display:flex;align-items:center;justify-content:center;gap:12px;color:#fff;font-size:18px;font-weight:800;letter-spacing:1px;
transition:transform .12s,box-shadow .2s,background .2s;user-select:none}
.power:active{transform:scale(.98)}
.power.start{background:linear-gradient(135deg,#22c55e,#15803d);box-shadow:0 10px 24px rgba(34,197,94,.35)}
.power.stop{background:linear-gradient(135deg,#ef4444,#b91c1c);box-shadow:0 10px 24px rgba(239,68,68,.4);animation:pulse 1.6s infinite}
@keyframes pulse{0%,100%{box-shadow:0 0 0 0 rgba(239,68,68,.55),0 10px 24px rgba(239,68,68,.4)}50%{box-shadow:0 0 0 10px rgba(239,68,68,0),0 10px 24px rgba(239,68,68,.4)}}
.pball{width:26px;height:26px;border-radius:50%;flex:none;
background:radial-gradient(circle at 35% 30%,#fff 0,var(--ball2) 25%,var(--ball) 70%);box-shadow:inset -3px -3px 6px rgba(0,0,0,.2)}
.power.stop .pball{animation:spin 1s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}

/* Kartu */
.card{background:var(--card);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);border:1px solid var(--line);
border-radius:18px;padding:16px;margin-bottom:14px;box-shadow:0 8px 22px rgba(0,0,0,.25)}
.ch{display:flex;align-items:center;gap:10px;margin-bottom:14px}
.ic{width:32px;height:32px;border-radius:10px;display:grid;place-items:center;flex:none;background:rgba(255,138,31,.15);color:var(--ball)}
.ic svg{width:18px;height:18px}
.ch h2{flex:1;margin:0;font-size:15px;font-weight:700}
.badge{font-size:11px;font-weight:600;padding:4px 10px;border-radius:99px;background:rgba(255,255,255,.08);color:var(--mut);
max-width:150px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

/* Preset */
.grid-p{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;margin-bottom:12px}
.btn-p{position:relative;padding:10px 2px 8px;border-radius:12px;border:1px solid var(--line);background:rgba(0,0,0,.22);
color:var(--txt);cursor:pointer;display:flex;flex-direction:column;align-items:center;gap:3px;transition:all .15s;min-width:0}
.btn-p b{font-size:14px}
.btn-p small{font-size:9px;color:var(--mut);max-width:100%;padding:0 3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.btn-p.act{background:linear-gradient(160deg,var(--ball2),var(--ball));border-color:transparent;color:#2a1200;box-shadow:0 6px 14px rgba(255,138,31,.35)}
.btn-p.act small{color:#4a2400}
.inrow{display:flex;gap:8px}

/* Input */
input[type=text],input[type=password]{width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);
background:rgba(0,0,0,.28);color:#fff;font-size:14px;outline:none;transition:border .15s}
input[type=text]:focus,input[type=password]:focus{border-color:var(--ball)}
.fld{margin-bottom:10px}
.fld label{display:block;font-size:12px;color:var(--mut);margin:0 0 6px 2px}

/* Slider */
.row{margin-bottom:16px}
.row:last-child{margin-bottom:0}
.lbl{display:flex;justify-content:space-between;align-items:baseline;font-size:13px;margin-bottom:8px;color:var(--mut)}
.lbl b{color:var(--txt);font-size:14px;font-variant-numeric:tabular-nums}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:8px;border-radius:99px;outline:none;margin:0;
background:linear-gradient(90deg,var(--ball) 0,var(--ball) var(--p,0%),rgba(255,255,255,.12) var(--p,0%))}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;cursor:pointer;
background:radial-gradient(circle at 35% 30%,#fff 0,#fff 20%,#f1f1f1 60%,#d9d9d9 100%);border:2px solid var(--ball);box-shadow:0 2px 6px rgba(0,0,0,.4)}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;cursor:pointer;background:#fff;border:2px solid var(--ball)}

/* Segmented mode */
.seg{display:grid;grid-template-columns:repeat(3,1fr);gap:4px;padding:4px;border-radius:12px;background:rgba(0,0,0,.28);border:1px solid var(--line)}
.seg button{border:none;background:transparent;color:var(--mut);padding:10px 4px;border-radius:9px;font-size:13px;font-weight:600;cursor:pointer;transition:all .15s}
.seg button.act{background:var(--tbl);color:#fff;box-shadow:0 2px 8px rgba(0,0,0,.3)}
#sOscM{display:none}

/* Tombol kecil */
.btn{width:100%;padding:12px;border-radius:12px;border:1px solid var(--line);background:rgba(255,255,255,.07);color:#fff;
font-size:14px;font-weight:600;cursor:pointer;transition:transform .1s,background .15s;display:flex;align-items:center;justify-content:center;gap:8px}
.btn:active{transform:scale(.98)}
.btn svg{width:16px;height:16px}
.btn-pri{background:linear-gradient(135deg,var(--ball2),var(--ball));border-color:transparent;color:#2a1200}
.btn-save{width:auto;flex:none;padding:12px 18px}
.stack{display:grid;gap:8px;margin-top:4px}
#otaMsg{margin-top:12px;font-size:12px;color:var(--mut);text-align:center;min-height:16px;line-height:1.45}

/* Toast */
#toast{position:fixed;left:50%;bottom:22px;transform:translate(-50%,30px);opacity:0;pointer-events:none;max-width:90%;
background:#0b1f3d;border:1px solid rgba(255,138,31,.5);color:#fff;font-size:13px;padding:10px 16px;border-radius:12px;
box-shadow:0 10px 30px rgba(0,0,0,.45);transition:all .25s;z-index:9}
#toast.show{opacity:1;transform:translate(-50%,0)}
.foot{text-align:center;font-size:11px;color:var(--mut);opacity:.7;margin-top:6px}
</style>
</head>
<body>
<div class="wrap">
  <div class="hero">
    <div class="net"></div><div class="ballfly"></div>
    <div class="top">
      <svg class="logo" viewBox="0 0 64 64" aria-hidden="true">
        <rect x="35" y="38" width="9" height="22" rx="4" transform="rotate(-40 39 49)" fill="#8b5a2b"/>
        <circle cx="26" cy="27" r="20" fill="#e23a3a"/>
        <circle cx="26" cy="27" r="20" fill="none" stroke="#111" stroke-width="3"/>
        <circle cx="50" cy="12" r="7" fill="#ff8a1f"/><circle cx="48" cy="10" r="2.2" fill="#fff" opacity=".7"/>
      </svg>
      <div class="ttl"><h1>iPong <span>V300</span></h1><p>Robot Latihan Tenis Meja</p></div>
      <div id="chip" class="chip"><span class="dot"></span><span id="chipTx">Offline</span></div>
    </div>
  </div>

  <div id="conn">Terputus dari robot, menyambung ulang...</div>

  <button id="btnPower" class="power start" onclick="toggleRun()"><span class="pball"></span><span id="pwTx">START ROBOT</span></button>

  <div class="card">
    <div class="ch">
      <div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M19 21l-7-5-7 5V5a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2z"/></svg></div>
      <h2>Preset Latihan</h2><span id="pTitle" class="badge">Slot 1</span>
    </div>
    <div class="grid-p" id="pGrid"></div>
    <div class="inrow">
      <input type="text" id="pName" placeholder="Nama preset" maxlength="18">
      <button class="btn btn-pri btn-save" onclick="saveP()">Simpan</button>
    </div>
  </div>

  <div class="card">
    <div class="ch">
      <div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></svg></div>
      <h2>Tuning Motor</h2><span class="badge">Real-time</span>
    </div>
    <div class="row"><div class="lbl"><span>Topspin &middot; roda atas</span><b id="vTop">0</b></div>
      <input type="range" id="sTop" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Backspin &middot; roda bawah</span><b id="vBack">0</b></div>
      <input type="range" id="sBack" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Ball feeder &middot; frekuensi bola</span><b id="vFeed">0</b></div>
      <input type="range" id="sFeed" min="0" max="255" oninput="sendVal()"></div>
  </div>

  <div class="card">
    <div class="ch">
      <div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M7 8l-4 4 4 4M17 8l4 4-4 4M3 12h18"/></svg></div>
      <h2>Osilasi Kanan-Kiri</h2>
    </div>
    <div class="row">
      <div class="seg" id="seg"><button data-v="0">Off</button><button data-v="1">Sweep</button><button data-v="2">Random</button></div>
      <select id="sOscM" onchange="sendVal(true)"><option value="0">Off</option><option value="1">Sweep</option><option value="2">Random</option></select>
    </div>
    <div class="row"><div class="lbl"><span>Kecepatan tengok</span><b id="vOscS">0</b></div>
      <input type="range" id="sOscS" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Durasi ayun</span><b id="vOscT">1000</b></div>
      <input type="range" id="sOscT" min="400" max="3000" step="100" oninput="sendVal()"></div>
  </div>

  <div class="card">
    <div class="ch">
      <div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 3v12M7 10l5 5 5-5M5 21h14"/></svg></div>
      <h2>WiFi &amp; Update Firmware</h2><span id="fwVer" class="badge">v...</span>
    </div>
    <div class="fld"><label for="wSsid">WiFi internet (hanya untuk update)</label>
      <input type="text" id="wSsid" placeholder="Nama WiFi" maxlength="32" autocomplete="off"></div>
    <div class="fld"><label for="wPass">Password WiFi</label>
      <input type="password" id="wPass" placeholder="Password" maxlength="63"></div>
    <div class="stack">
      <button class="btn" onclick="saveWifi()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M2 9a15 15 0 0 1 20 0"/><circle cx="12" cy="19.5" r="1"/></svg>Simpan &amp; Tes WiFi</button>
      <button class="btn btn-pri" onclick="startOta()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-3-6.7L21 8M21 3v5h-5"/></svg>Cek &amp; Update Firmware</button>
    </div>
    <div id="otaMsg"></div>
  </div>
  <div class="foot">Firmware diverifikasi tanda tangan digital sebelum dipasang</div>
</div>
<div id="toast"></div>

<script>
const $ = id => document.getElementById(id);
let ws, myId = 0, selSlot = 0, run = false, lastTx = 0, txTimer = null, toastT = null;
function showMsg(m){
  $('otaMsg').innerText = m;
  const t = $('toast'); t.innerText = m; t.classList.add('show');
  clearTimeout(toastT); toastT = setTimeout(()=>t.classList.remove('show'), 3500);
}
function setConn(on){
  $('conn').style.display = on ? 'none' : 'block';
  $('chip').className = 'chip' + (on ? ' on' : '');
  $('chipTx').innerText = on ? 'Online' : 'Offline';
}
function initWs(){
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = ()=>setConn(true);
  ws.onmessage = (e)=>{
    let d; try { d = JSON.parse(e.data); } catch(_) { return; }
    if(d.type === 'init'){
      myId = d.you;
      $('fwVer').innerText = 'v' + d.ver;
      $('wSsid').value = d.ssid || '';
      window.presets = d.presets;
      renderPresets();
      applyUi(d.active, d.run);
      if(d.msg) showMsg(d.msg);
    } else if(d.type === 'state'){
      // Perubahan dari HP sendiri tidak di-apply ulang agar slider tidak loncat saat digeser
      if(d.src !== myId) applyUi(d.active, d.run);
    } else if(d.type === 'presets'){
      window.presets = d.presets; renderPresets();
    } else if(d.type === 'msg'){
      showMsg(d.msg);
    }
  };
  ws.onclose = ()=>{ setConn(false); setTimeout(initWs, 1500); };
}
function tx(o){
  if(ws && ws.readyState === 1){ ws.send(JSON.stringify(o)); return true; }
  showMsg('Tidak terhubung ke robot. Coba lagi sebentar.');
  return false;
}
function renderPresets(){
  const g = $('pGrid'); g.innerHTML = '';
  window.presets.forEach((p,i)=>{
    const b = document.createElement('button');
    b.className = 'btn-p' + (i===selSlot?' act':'');
    const n = document.createElement('b'); n.innerText = 'P' + (i+1);
    const s = document.createElement('small'); s.innerText = p.name || '';
    b.append(n, s);
    b.onclick = ()=>{ selSlot = i; renderPresets(); applyUi(window.presets[i], run); sendVal(true); };
    g.appendChild(b);
  });
}
function applyUi(p, r){
  run = r;
  $('btnPower').className = 'power ' + (run ? 'stop' : 'start');
  $('pwTx').innerText = run ? 'STOP ROBOT' : 'START ROBOT';
  $('pTitle').innerText = p.name;
  $('pName').value = p.name;
  $('sTop').value = p.top;
  $('sBack').value = p.back;
  $('sFeed').value = p.feed;
  $('sOscS').value = p.oscSpeed;
  $('sOscM').value = p.oscMode;
  $('sOscT').value = p.oscTime;
  updLabels();
}
function updLabels(){
  ['Top','Back','Feed','OscS','OscT'].forEach(k=>{
    const s = $('s'+k), v = +s.value, mn = +s.min, mx = +s.max;
    s.style.setProperty('--p', ((v-mn)*100/(mx-mn)) + '%');
    $('v'+k).innerText = k === 'OscT' ? (v/1000).toFixed(1) + ' s' : Math.round(v*100/255) + '%';
  });
  const m = $('sOscM').value;
  document.querySelectorAll('#seg button').forEach(b=>b.classList.toggle('act', b.dataset.v === m));
}
document.querySelectorAll('#seg button').forEach(b=>{
  b.onclick = ()=>{ $('sOscM').value = b.dataset.v; sendVal(true); };
});
function toggleRun(){
  const nr = !run;
  clearTimeout(txTimer);
  if(tx({cmd:'set', run:nr, data:getForm()})){ lastTx = Date.now(); applyUi(getForm(), nr); }
}
function getForm(){
  return {
    name: $('pName').value || ('Preset '+(selSlot+1)),
    top: +$('sTop').value,
    back: +$('sBack').value,
    feed: +$('sFeed').value,
    oscSpeed: +$('sOscS').value,
    oscMode: +$('sOscM').value,
    oscTime: +$('sOscT').value
  };
}
// Throttle 60ms + kirim nilai terakhir (trailing) agar posisi akhir slider selalu sampai ke robot
function sendVal(force=false){
  updLabels();
  clearTimeout(txTimer);
  const wait = 60 - (Date.now() - lastTx);
  if(!force && wait > 0){ txTimer = setTimeout(()=>sendVal(true), wait); return; }
  lastTx = Date.now();
  tx({cmd:'set', run:run, data:getForm()});
}
function saveP(){
  const d = getForm();
  if(tx({cmd:'save', slot:selSlot, data:d})) showMsg('Menyimpan preset...');
}
function saveWifi(){
  if(tx({cmd:'wifi', ssid:$('wSsid').value, pass:$('wPass').value})) showMsg('Menyimpan & mengetes WiFi...');
}
function startOta(){
  if(tx({cmd:'ota'})) showMsg('Memulai update...');
}
updLabels();
initWs();
</script>
</body>
</html>
)rawliteral";

// ================= PESAN KE CLIENT (hanya dipanggil dari loop) =================
static void fillPreset(JsonObject o, const Preset& p) {
  o["name"] = p.name;
  o["top"] = p.top;
  o["back"] = p.back;
  o["feed"] = p.feed;
  o["oscSpeed"] = p.oscSpeed;
  o["oscMode"] = p.oscMode;
  o["oscTime"] = p.oscTime;
}

static void fillPresets(JsonDocument& doc) {
  JsonArray arr = doc["presets"].to<JsonArray>();
  for (int i = 0; i < NUM_PRESETS; i++) fillPreset(arr.add<JsonObject>(), presets[i]);
}

static void sendInit(uint32_t clientId) {
  JsonDocument doc;
  doc["type"] = "init";
  doc["you"] = clientId;
  doc["ver"] = FW_VERSION;
  doc["ssid"] = staSsid;
  doc["run"] = isRunning;
  doc["msg"] = lastMsg;
  fillPreset(doc["active"].to<JsonObject>(), activeState);
  fillPresets(doc);
  String out;
  serializeJson(doc, out);
  ws.text(clientId, out);
}

static void broadcastState(uint32_t srcClientId) {
  JsonDocument doc;
  doc["type"] = "state";
  doc["src"] = srcClientId;
  doc["run"] = isRunning;
  fillPreset(doc["active"].to<JsonObject>(), activeState);
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

static void broadcastPresets() {
  JsonDocument doc;
  doc["type"] = "presets";
  fillPresets(doc);
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

static void broadcastMsg(const String& msg) {
  lastMsg = msg;  // dikirim ulang di "init" untuk HP yang tersambung ulang (mis. setelah AP pindah channel)
  Serial.println("[msg] " + msg);
  JsonDocument doc;
  doc["type"] = "msg";
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ================= WEBSOCKET HANDLER (berjalan di task AsyncTCP) =================
// Hanya mem-parsing & mengantrekan perintah. Tidak menyentuh state, PWM, NVS, atau WiFi.
static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
  Command c = {};
  c.clientId = client->id();

  if (type == WS_EVT_CONNECT) {
    c.type = CMD_HELLO;
    xQueueSend(cmdQueue, &c, 0);
    return;
  }
  if (type != WS_EVT_DATA) return;

  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > WS_MAX_MSG) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "set") == 0) {
    c.type = CMD_SET;
    c.run = doc["run"] | false;
    parsePreset(doc["data"], c.data, "Manual");
  } else if (strcmp(cmd, "save") == 0) {
    c.type = CMD_SAVE;
    c.slot = clampInt(doc["slot"] | -1, -1, 255);
    parsePreset(doc["data"], c.data, "Preset");
  } else if (strcmp(cmd, "wifi") == 0) {
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    if (strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(pass) > 63) {
      client->text("{\"type\":\"msg\",\"msg\":\"SSID 1-32 karakter, password maks 63.\"}");
      return;
    }
    c.type = CMD_WIFI;
    strlcpy(c.s1, ssid, sizeof(c.s1));
    strlcpy(c.s2, pass, sizeof(c.s2));
  } else if (strcmp(cmd, "ota") == 0) {
    c.type = CMD_OTA;
  } else {
    return;
  }

  if (otaBusy) {
    client->text("{\"type\":\"msg\",\"msg\":\"Sedang update firmware, perintah diabaikan.\"}");
    return;
  }
  xQueueSend(cmdQueue, &c, 0);  // antrean penuh => perintah dibuang (slider akan mengirim ulang)
}

// ================= WIFI STA (hanya aktif saat tes / OTA) =================
// Saat STA tersambung, AP ikut pindah ke channel router dan HP bisa terputus sebentar.
// Karena itu STA hanya dinyalakan sementara, bukan terus-menerus.
static void startWifiTest() {
  WiFi.disconnect(false);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  staTesting = true;
  staTestStart = millis();
  broadcastMsg("Mencoba terhubung ke \"" + staSsid + "\"... (HP mungkin terputus sebentar)");
}

static void pollWifiTest() {
  if (!staTesting) return;
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    WiFi.disconnect(false);
    staTesting = false;
    broadcastMsg("WiFi OK (IP " + ip + "). Robot akan tersambung otomatis saat update.");
  } else if (millis() - staTestStart > STA_CONNECT_TIMEOUT_MS) {
    WiFi.disconnect(false);
    staTesting = false;
    broadcastMsg("Gagal terhubung ke \"" + staSsid + "\". Cek nama & password WiFi.");
  }
}

static bool connectStaBlocking() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(false);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
    dnsServer.processNextRequest();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ================= GITHUB OTA (download + verifikasi tanda tangan ECDSA) =================
static bool parseVer(const char* s, int v[3]) {
  return s && sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

static int cmpVer(const int a[3], const int b[3]) {
  for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

static bool isHex(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  return (tolower((unsigned char)c) - 'a') + 10;
}

// Pesan yang ditandatangani: "<version>|<size>|<sha256 hex lowercase>" (lihat tools/ota_release.py)
static bool verifySignature(const String& msg, const char* sigHex) {
  size_t hexLen = strlen(sigHex);
  uint8_t sig[80];
  if (hexLen % 2 || hexLen / 2 > sizeof(sig) || hexLen < 16 || !isHex(sigHex, hexLen)) return false;
  size_t sigLen = hexLen / 2;
  for (size_t i = 0; i < sigLen; i++) sig[i] = (hexNibble(sigHex[2 * i]) << 4) | hexNibble(sigHex[2 * i + 1]);

  uint8_t hash[32];
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md(mdInfo, (const uint8_t*)msg.c_str(), msg.length(), hash) != 0) return false;

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBKEY_PEM, strlen(OTA_PUBKEY_PEM) + 1);
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen);
  mbedtls_pk_free(&pk);
  return rc == 0;
}

static String downloadAndFlash(WiFiClientSecure& client, const char* url, size_t size,
                               const char* shaHex, const char* sigHex, const char* ver) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return "Gagal membuka URL firmware.";
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return "HTTP error firmware: " + String(code);
  }
  int len = http.getSize();
  if (len != (int)size) {
    http.end();
    return "Ukuran firmware tidak cocok (server " + String(len) + ", version.json " + String(size) + ").";
  }
  if (!Update.begin(size, U_FLASH)) {
    http.end();
    return "Partisi OTA tidak cukup / error: " + String(Update.errorString());
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&md);

  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[2048];
  size_t done = 0;
  int lastPct = -1;
  unsigned long lastData = millis();
  bool writeErr = false;

  while (done < size) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = min(min(avail, sizeof(buf)), size - done);
      size_t n = stream->readBytes(buf, want);
      if (n == 0) continue;
      if (Update.write(buf, n) != n) { writeErr = true; break; }
      mbedtls_md_update(&md, buf, n);
      done += n;
      lastData = millis();
      int pct = (int)(done * 100 / size);
      if (pct / 10 != lastPct / 10) {
        lastPct = pct;
        broadcastMsg("Mengunduh firmware... " + String(pct) + "%");
      }
    } else {
      if (!stream->connected() || millis() - lastData > HTTP_TIMEOUT_MS) break;
      delay(2);
    }
  }
  http.end();

  uint8_t hash[32];
  mbedtls_md_finish(&md, hash);
  mbedtls_md_free(&md);

  if (writeErr) { Update.abort(); return "Gagal menulis flash: " + String(Update.errorString()); }
  if (done != size) { Update.abort(); return "Unduhan terputus (" + String(done) + "/" + String(size) + " byte)."; }

  char actual[65];
  for (int i = 0; i < 32; i++) snprintf(actual + 2 * i, 3, "%02x", hash[i]);
  if (strcasecmp(actual, shaHex) != 0) { Update.abort(); return "Checksum SHA256 tidak cocok. Update dibatalkan."; }

  String signedMsg = String(ver) + "|" + String((unsigned)size) + "|" + String(actual);
  if (!verifySignature(signedMsg, sigHex)) {
    Update.abort();
    return "Tanda tangan firmware TIDAK valid. Update dibatalkan.";
  }

  if (!Update.end()) return "Finalisasi update gagal: " + String(Update.errorString());

  broadcastMsg("Update ke v" + String(ver) + " sukses. Robot restart...");
  delay(1500);
  ESP.restart();
  return "";
}

static String doGitHubOta() {
  if (staSsid.isEmpty()) return "Gagal: WiFi internet belum diatur.";
  broadcastMsg("Menghubungkan ke \"" + staSsid + "\"... (HP mungkin terputus sebentar)");
  if (!connectStaBlocking()) return "Gagal: tidak bisa terhubung ke WiFi internet.";

  // Kanal TLS tidak diverifikasi (sertifikat GitHub bisa berganti sewaktu-waktu).
  // Keaslian firmware dijamin oleh tanda tangan ECDSA P-256 yang dicek sebelum firmware diaktifkan.
  WiFiClientSecure client;
  client.setInsecure();

  broadcastMsg("Mengecek versi di GitHub...");
  HTTPClient http;
  if (!http.begin(client, VERSION_URL)) return "Gagal membuka URL version.json.";
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return "HTTP error version.json: " + String(code);
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return "Format version.json tidak valid.";

  const char* ver = doc["version"];
  int remote[3], local[3];
  if (!parseVer(ver, remote)) return "version.json: field 'version' tidak valid.";
  parseVer(FW_VERSION, local);
  if (cmpVer(remote, local) <= 0) return "Firmware sudah versi terbaru (v" FW_VERSION ").";

  const char* url = doc["url"];
  const char* sha = doc["sha256"];
  const char* sig = doc["sig"];
  long size = doc["size"] | 0L;
  if (!url || strncmp(url, "https://", 8) != 0) return "version.json: 'url' harus https://";
  if (!sha || strlen(sha) != 64 || !isHex(sha, 64)) return "version.json: 'sha256' tidak valid.";
  if (!sig || !*sig) return "version.json: 'sig' tidak ada (firmware belum ditandatangani).";
  if (size <= 0) return "version.json: 'size' tidak valid.";

  broadcastMsg("Menemukan v" + String(ver) + ". Mengunduh firmware...");
  return downloadAndFlash(client, url, (size_t)size, sha, sig, ver);
}

static void runOta() {
  otaBusy = true;
  setRunning(false);  // Wajib matikan semua motor sebelum flashing
  broadcastState(0);
  staTesting = false;

  String result = doGitHubOta();  // hanya kembali jika gagal / tidak ada update
  broadcastMsg(result);
  WiFi.disconnect(false);

  // Buang perintah kontrol yang sempat masuk antrean; HELLO tetap dilayani.
  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
    if (c.type == CMD_HELLO) sendInit(c.clientId);
  }
  otaBusy = false;
}

// ================= PEMROSESAN PERINTAH (loop) =================
static void processCommands() {
  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
    switch (c.type) {
      case CMD_HELLO:
        sendInit(c.clientId);
        break;
      case CMD_SET:
        activeState = c.data;
        setRunning(c.run);
        broadcastState(c.clientId);
        break;
      case CMD_SAVE:
        if (c.slot >= NUM_PRESETS) break;
        activeState = c.data;
        savePreset(c.slot);
        broadcastPresets();
        broadcastState(0);
        broadcastMsg("Preset P" + String(c.slot + 1) + " tersimpan ke memori robot.");
        break;
      case CMD_WIFI:
        staSsid = c.s1;
        staPass = c.s2;
        prefs.putString("ssid", staSsid);
        prefs.putString("pass", staPass);
        startWifiTest();
        break;
      case CMD_OTA:
        runOta();
        return;  // antrean sudah dikuras di runOta()
    }
  }
}

// ================= KONTROL MOTOR NON-BLOCKING =================
static void rampTo(float& cur, uint8_t target) {
  if (cur < target) cur = min((float)target, cur + 4.0f);
  else if (cur > target) cur = max((float)target, cur - 8.0f);
}

static void pickRandomTarget(long half) {
  // Selalu ke sisi berlawanan dengan amplitudo acak => posisi tetap dalam batas, tidak drift.
  long amp = random(half / 4, half + 1);
  oscTarget = (oscPos >= 0) ? -amp : amp;
  oscTargetValid = true;
}

static void updateOscillation(unsigned long now) {
  unsigned long dt = now - lastOscTick;
  lastOscTick = now;
  if (oscDriving) oscPos += oscDir * (long)dt;

  const Preset& s = activeState;
  if (s.oscMode == 0 || s.oscSpeed == 0) {
    writeOsc(0, 0);
    oscDriving = false;
    oscDeadtime = false;
    return;
  }
  if (s.oscMode != lastOscMode) {
    lastOscMode = s.oscMode;
    oscTargetValid = false;
  }

  if (oscDeadtime) {
    if (now - oscDeadStart < OSC_DEADTIME_MS) return;
    oscDeadtime = false;
  }

  // oscTime = waktu satu sapuan penuh, jadi batas tiap sisi = setengahnya dari tengah.
  const long half = s.oscTime / 2;
  long target;
  if (s.oscMode == 1) {
    target = (oscDir > 0) ? half : -half;
  } else {
    if (!oscTargetValid) pickRandomTarget(half);
    target = oscTarget;
  }

  bool reached = (oscDir > 0) ? (oscPos >= target) : (oscPos <= target);
  if (reached) {
    if (oscDriving) {
      writeOsc(0, 0);
      oscDriving = false;
      oscDeadtime = true;
      oscDeadStart = now;
    }
    if (s.oscMode == 1) {
      oscDir = -oscDir;
    } else {
      pickRandomTarget(half);
      oscDir = (oscTarget > oscPos) ? 1 : -1;
    }
    return;
  }

  if (oscDir > 0) writeOsc(s.oscSpeed, 0);
  else            writeOsc(0, s.oscSpeed);
  oscDriving = true;
}

static void updateMotors() {
  if (!isRunning) return;  // output sudah 0 oleh stopAllMotorsImmediate()
  unsigned long now = millis();

  // 1. Soft-start 3 motor MOSFET. Feeder baru boleh naik setelah roda mencapai target,
  //    agar bola tidak keluar sebelum roda berputar penuh dan arus start tidak menumpuk.
  if (now - lastRampMs >= RAMP_INTERVAL_MS) {
    lastRampMs = now;
    rampTo(curTop, activeState.top);
    rampTo(curBack, activeState.back);
    bool wheelsReady = curTop == activeState.top && curBack == activeState.back;
    if (wheelsReady || curFeed > activeState.feed) rampTo(curFeed, activeState.feed);

    writePwmPin(PIN_TOP, 0, (uint8_t)curTop);
    writePwmPin(PIN_BACK, 1, (uint8_t)curBack);
    writePwmPin(PIN_FEED, 2, (uint8_t)curFeed);
  }

  // 2. Motor osilasi (tengok kanan-kiri)
  updateOscillation(now);
}

// ================= ROLLBACK OTA =================
// Bootloader Arduino core dikompilasi dengan CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.
// Firmware baru hasil OTA baru ditandai "valid" setelah berjalan stabil 30 detik.
// Jika crash/restart sebelum itu, bootloader otomatis kembali ke firmware lama.
extern "C" bool verifyRollbackLater() { return true; }

static void markAppValidWhenStable() {
  static bool done = false;
  if (done || millis() < 30000) return;
  done = true;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("Firmware baru ditandai valid (rollback dibatalkan).");
  }
}

// ================= SETUP & LOOP =================
void setup() {
  Serial.begin(115200);

  setupPwmPin(PIN_TOP, 0);
  setupPwmPin(PIN_BACK, 1);
  setupPwmPin(PIN_FEED, 2);
  setupPwmPin(PIN_OSC_L, 3);
  setupPwmPin(PIN_OSC_R, 4);
  stopAllMotorsImmediate();

  cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(Command));

  WiFi.persistent(false);       // kredensial hanya disimpan di namespace "ipong", bukan di NVS WiFi SDK
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);         // latensi WebSocket rendah
  WiFi.setAutoReconnect(false); // jangan scan terus (mengganggu AP) jika router tidak ada

  loadStorage();

  if (!WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.println("softAP gagal dengan password tersimpan!");
  }
  Serial.printf("\n=== iPong V300 v%s ===\nWiFi robot : %s\nPassword   : %s\nAlamat     : http://%s\n",
                FW_VERSION, AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());

  // Captive portal: semua domain diarahkan ke robot, agar HP langsung membuka halaman kontrol.
  dnsServer.start(53, "*", WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", INDEX_HTML);
  });
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  processCommands();
  updateMotors();
  pollWifiTest();
  markAppValidWhenStable();

  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup >= 1000) {
    lastCleanup = millis();
    ws.cleanupClients();
  }
  delay(1);
}
