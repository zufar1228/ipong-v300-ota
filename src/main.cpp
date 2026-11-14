#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// ================= KONFIGURASI VERSI & GITHUB =================
#define FW_VERSION "1.0.0"
const char* VERSION_URL = "https://raw.githubusercontent.com/zufar1228/ipong-v300-ota/main/firmware/version.json";
const char* AP_SSID = "iPong-V300-Pro";
const char* AP_PASS = "12345678";

// ================= PIN HARDWARE =================
#define PIN_TOP   25
#define PIN_BACK  26
#define PIN_FEED  27
#define PIN_OSC_L 14
#define PIN_OSC_R 33

#define PWM_FREQ 18000 // 18 kHz (senyap, tanpa bunyi berdenging)
#define PWM_RES  8     // 0 - 255

// ================= STRUKTUR DATA =================
struct Preset {
  char name[20];
  uint8_t top;
  uint8_t back;
  uint8_t feed;
  uint8_t oscSpeed;
  uint8_t oscMode;     // 0: Off, 1: Sweep, 2: Random
  uint16_t oscTime;    // ms (400 - 3000)
};

Preset presets[5];
Preset activeState = {"Manual", 0, 0, 0, 0, 0, 1000};
bool isRunning = false;

// Variabel Soft-Start & Osilasi
float curTop = 0, curBack = 0, curFeed = 0;
unsigned long lastRampMs = 0;
unsigned long lastOscMs = 0;
unsigned long currentOscDuration = 1000;
int8_t oscDir = 1; // 1: Kiri, -1: Kanan, 0: Deadtime jeda balik arah
bool oscDeadtime = false;

// Variabel OTA & WiFi
String staSsid = "";
String staPass = "";
bool triggerOta = false;
String otaStatus = "Idle";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

// ================= HELPER PWM (Kompatibel Core 2.x & 3.x) =================
void setupPwmPin(uint8_t pin, uint8_t ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(pin, PWM_FREQ, PWM_RES);
#else
  ledcSetup(ch, PWM_FREQ, PWM_RES);
  ledcAttachPin(pin, ch);
#endif
}

void writePwmPin(uint8_t pin, uint8_t ch, uint8_t val) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, val);
#else
  ledcWrite(ch, val);
#endif
}

void stopAllMotorsImmediate() {
  isRunning = false;
  curTop = curBack = curFeed = 0;
  writePwmPin(PIN_TOP, 0, 0);
  writePwmPin(PIN_BACK, 1, 0);
  writePwmPin(PIN_FEED, 2, 0);
  writePwmPin(PIN_OSC_L, 3, 0);
  writePwmPin(PIN_OSC_R, 4, 0);
}

// ================= NVS PRESETS & WIFI =================
void loadStorage() {
  prefs.begin("ipong", false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");

  Preset defaults[5] = {
    {"1. Warmup Top",  140,   0, 100, 130, 1, 1200},
    {"2. Heavy Back",    0, 185,  90, 120, 1, 1400},
    {"3. Fast Drill",  180,  40, 150, 170, 2, 1000},
    {"4. Forehand",    160,   0, 120,   0, 0, 1000},
    {"5. Custom",      120, 120, 110, 140, 1, 1100}
  };

  for (int i = 0; i < 5; i++) {
    String key = "p" + String(i);
    if (prefs.getBytesLength(key.c_str()) == sizeof(Preset)) {
      prefs.getBytes(key.c_str(), &presets[i], sizeof(Preset));
    } else {
      presets[i] = defaults[i];
      prefs.putBytes(key.c_str(), &presets[i], sizeof(Preset));
    }
  }
  activeState = presets[0];
}

void savePreset(uint8_t idx) {
  if (idx >= 5) return;
  presets[idx] = activeState;
  String key = "p" + String(idx);
  prefs.putBytes(key.c_str(), &presets[idx], sizeof(Preset));
}

// ================= EMBEDDED WEB UI (MOBILE-FIRST) =================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>iPong V300 Controller</title>
<style>
  :root{--bg:#0f172a;--card:#1e293b;--pri:#3b82f6;--ok:#10b981;--err:#ef4444;--txt:#f8fafc;--mut:#94a3b8}
  *{box-sizing:border-box;font-family:system-ui,-apple-system,sans-serif}
  body{margin:0;padding:12px;background:var(--bg);color:var(--txt);max-width:480px;margin:auto}
  .card{background:var(--card);border-radius:12px;padding:14px;margin-bottom:12px;box-shadow:0 4px 6px rgba(0,0,0,.3)}
  h2{margin:0 0 10px;font-size:16px;display:flex;justify-content:space-between;align-items:center}
  .badge{font-size:11px;background:#334155;padding:3px 8px;border-radius:99px;color:var(--mut)}
  .btn-main{width:100%;padding:16px;font-size:18px;font-weight:700;border:none;border-radius:12px;color:#fff;cursor:pointer;margin-bottom:12px}
  .start{background:var(--ok)} .stop{background:var(--err)}
  .row{margin-bottom:12px}
  .lbl{display:flex;justify-content:space-between;font-size:13px;margin-bottom:4px;color:var(--mut)}
  .lbl b{color:var(--txt)}
  input[type=range]{width:100%;height:8px;border-radius:5px;background:#334155;accent-color:var(--pri)}
  select,input[type=text],input[type=password]{width:100%;padding:10px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;margin-top:4px}
  .grid-p{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-bottom:8px}
  .btn-p{padding:10px 4px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:12px;cursor:pointer}
  .btn-p.act{background:var(--pri);border-color:var(--pri);font-weight:700}
  .btn-sm{padding:10px;border-radius:8px;border:none;background:#334155;color:#fff;font-size:13px;cursor:pointer;width:100%;margin-top:6px}
  .btn-pri{background:var(--pri)}
</style>
</head>
<body>
  <button id="btnPower" class="btn-main start" onclick="toggleRun()">START ROBOT</button>

  <div class="card">
    <h2>Preset Latihan <span id="pTitle" class="badge">Slot 1</span></h2>
    <div class="grid-p" id="pGrid"></div>
    <div style="display:flex;gap:6px">
      <input type="text" id="pName" placeholder="Nama Preset" maxlength="18">
      <button class="btn-sm btn-pri" style="width:90px;margin-top:4px" onclick="saveP()">Save</button>
    </div>
  </div>

  <div class="card">
    <h2>Tuning Motor (Real-Time)</h2>
    <div class="row"><div class="lbl"><span>Topspin (Roda Atas)</span><b id="vTop">0</b></div>
      <input type="range" id="sTop" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Backspin (Roda Bawah)</span><b id="vBack">0</b></div>
      <input type="range" id="sBack" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Ball Feeder (Jatuh Bola)</span><b id="vFeed">0</b></div>
      <input type="range" id="sFeed" min="0" max="255" oninput="sendVal()"></div>
    <div class="row"><div class="lbl"><span>Kecepatan Tengok</span><b id="vOscS">0</b></div>
      <input type="range" id="sOscS" min="0" max="255" oninput="sendVal()"></div>
    <div class="row">
      <div class="lbl"><span>Mode Tengok Kanan-Kiri</span></div>
      <select id="sOscM" onchange="sendVal()">
        <option value="0">Off (Diam)</option>
        <option value="1">Sweep (Bolak-Balik Teratur)</option>
        <option value="2">Random (Acak)</option>
      </select>
    </div>
    <div class="row" style="margin:0"><div class="lbl"><span>Durasi Ayun (ms)</span><b id="vOscT">1000</b></div>
      <input type="range" id="sOscT" min="400" max="3000" step="100" oninput="sendVal()"></div>
  </div>

  <div class="card">
    <h2>WiFi & GitHub OTA <span id="fwVer" class="badge">v...</span></h2>
    <input type="text" id="wSsid" placeholder="Nama WiFi Internet (untuk OTA)">
    <input type="password" id="wPass" placeholder="Password WiFi">
    <button class="btn-sm" onclick="saveWifi()">Simpan WiFi & Koneksikan</button>
    <button class="btn-sm btn-pri" onclick="startOta()">Cek & Update Firmware (GitHub)</button>
    <div id="otaMsg" style="font-size:12px;color:var(--mut);margin-top:8px;text-align:center"></div>
  </div>

<script>
let ws, selSlot = 0, run = false, lastTx = 0;
function initWs(){
  ws = new WebSocket(`ws://${location.hostname}/ws`);
  ws.onmessage = (e)=>{
    const d = JSON.parse(e.data);
    if(d.type === 'init'){
      document.getElementById('fwVer').innerText = 'v' + d.ver;
      document.getElementById('wSsid').value = d.ssid || '';
      window.presets = d.presets;
      renderPresets();
      applyUi(d.active, d.run);
    } else if(d.type === 'ota'){
      document.getElementById('otaMsg').innerText = d.msg;
    }
  };
  ws.onclose = ()=> setTimeout(initWs, 1500);
}
function renderPresets(){
  const g = document.getElementById('pGrid'); g.innerHTML = '';
  window.presets.forEach((p,i)=>{
    const b = document.createElement('button');
    b.className = 'btn-p' + (i===selSlot?' act':'');
    b.innerText = 'P' + (i+1);
    b.onclick = ()=>{ selSlot = i; renderPresets(); applyUi(window.presets[i], run); sendVal(true); };
    g.appendChild(b);
  });
}
function applyUi(p, r){
  run = r;
  const bp = document.getElementById('btnPower');
  bp.innerText = run ? 'STOP ROBOT' : 'START ROBOT';
  bp.className = 'btn-main ' + (run ? 'stop' : 'start');
  document.getElementById('pTitle').innerText = p.name;
  document.getElementById('pName').value = p.name;
  document.getElementById('sTop').value = p.top;
  document.getElementById('sBack').value = p.back;
  document.getElementById('sFeed').value = p.feed;
  document.getElementById('sOscS').value = p.oscSpeed;
  document.getElementById('sOscM').value = p.oscMode;
  document.getElementById('sOscT').value = p.oscTime;
  updLabels();
}
function updLabels(){
  ['Top','Back','Feed','OscS','OscT'].forEach(k=>{
    document.getElementById('v'+k).innerText = document.getElementById('s'+k).value;
  });
}
function toggleRun(){ run = !run; applyUi(getForm(), run); sendVal(true); }
function getForm(){
  return {
    name: document.getElementById('pName').value || ('Preset '+(selSlot+1)),
    top: +document.getElementById('sTop').value,
    back: +document.getElementById('sBack').value,
    feed: +document.getElementById('sFeed').value,
    oscSpeed: +document.getElementById('sOscS').value,
    oscMode: +document.getElementById('sOscM').value,
    oscTime: +document.getElementById('sOscT').value
  };
}
function sendVal(force=false){
  updLabels();
  const now = Date.now();
  if(!force && now - lastTx < 50) return;
  lastTx = now;
  if(ws && ws.readyState === 1){
    ws.send(JSON.stringify({cmd:'set', run:run, data:getForm()}));
  }
}
function saveP(){
  const d = getForm();
  window.presets[selSlot] = d;
  renderPresets();
  ws.send(JSON.stringify({cmd:'save', slot:selSlot, data:d}));
  document.getElementById('otaMsg').innerText = 'Preset tersimpan ke memori ESP32!';
}
function saveWifi(){
  ws.send(JSON.stringify({cmd:'wifi', ssid:document.getElementById('wSsid').value, pass:document.getElementById('wPass').value}));
  document.getElementById('otaMsg').innerText = 'Menyimpan & menghubungkan WiFi...';
}
function startOta(){
  ws.send(JSON.stringify({cmd:'ota'}));
  document.getElementById('otaMsg').innerText = 'Mengecek versi di GitHub...';
}
initWs();
</script>
</body>
</html>
)rawliteral";

// ================= WEBSOCKET & HTTP HANDLER =================
void sendStateToClient(AsyncWebSocketClient* client = nullptr) {
  JsonDocument doc;
  doc["type"] = "init";
  doc["ver"] = FW_VERSION;
  doc["ssid"] = staSsid;
  doc["run"] = isRunning;

  JsonObject act = doc["active"].to<JsonObject>();
  act["name"] = activeState.name;
  act["top"] = activeState.top;
  act["back"] = activeState.back;
  act["feed"] = activeState.feed;
  act["oscSpeed"] = activeState.oscSpeed;
  act["oscMode"] = activeState.oscMode;
  act["oscTime"] = activeState.oscTime;

  JsonArray arr = doc["presets"].to<JsonArray>();
  for (int i = 0; i < 5; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = presets[i].name;
    o["top"] = presets[i].top;
    o["back"] = presets[i].back;
    o["feed"] = presets[i].feed;
    o["oscSpeed"] = presets[i].oscSpeed;
    o["oscMode"] = presets[i].oscMode;
    o["oscTime"] = presets[i].oscTime;
  }

  String out;
  serializeJson(doc, out);
  if (client) client->text(out);
  else ws.textAll(out);
}

void broadcastOtaMsg(const String& msg) {
  JsonDocument doc;
  doc["type"] = "ota";
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    sendStateToClient(client);
  } else if (type == WS_EVT_DATA) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;
    const char* cmd = doc["cmd"];

    if (strcmp(cmd, "set") == 0) {
      isRunning = doc["run"];
      JsonObject d = doc["data"];
      strlcpy(activeState.name, d["name"] | "Manual", sizeof(activeState.name));
      activeState.top = d["top"];
      activeState.back = d["back"];
      activeState.feed = d["feed"];
      activeState.oscSpeed = d["oscSpeed"];
      activeState.oscMode = d["oscMode"];
      activeState.oscTime = d["oscTime"];
      if (!isRunning) stopAllMotorsImmediate();
    } else if (strcmp(cmd, "save") == 0) {
      uint8_t slot = doc["slot"];
      JsonObject d = doc["data"];
      strlcpy(activeState.name, d["name"] | "Preset", sizeof(activeState.name));
      activeState.top = d["top"];
      activeState.back = d["back"];
      activeState.feed = d["feed"];
      activeState.oscSpeed = d["oscSpeed"];
      activeState.oscMode = d["oscMode"];
      activeState.oscTime = d["oscTime"];
      savePreset(slot);
    } else if (strcmp(cmd, "wifi") == 0) {
      staSsid = doc["ssid"].as<String>();
      staPass = doc["pass"].as<String>();
      prefs.putString("ssid", staSsid);
      prefs.putString("pass", staPass);
      WiFi.begin(staSsid.c_str(), staPass.c_str());
    } else if (strcmp(cmd, "ota") == 0) {
      triggerOta = true;
    }
  }
}

// ================= PROSES AUTO-PULL GITHUB OTA =================
void performGitHubOTA() {
  stopAllMotorsImmediate(); // Wajib matikan semua motor sebelum flashing

  if (WiFi.status() != WL_CONNECTED) {
    broadcastOtaMsg("Gagal: ESP32 belum terhubung ke WiFi internet!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Bypass verifikasi sertifikat SSL agar tahan jangka panjang

  HTTPClient http;
  broadcastOtaMsg("Mengunduh metadata version.json...");
  if (!http.begin(client, VERSION_URL)) {
    broadcastOtaMsg("Gagal membuka URL GitHub.");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    broadcastOtaMsg("HTTP Error version.json: " + String(httpCode));
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    broadcastOtaMsg("Format version.json tidak valid.");
    return;
  }

  String remoteVer = doc["version"].as<String>();
  String binUrl = doc["url"].as<String>();

  if (remoteVer == FW_VERSION) {
    broadcastOtaMsg("Firmware sudah versi terbaru (v" + String(FW_VERSION) + ").");
    return;
  }

  broadcastOtaMsg("Menemukan v" + remoteVer + ". Mengunduh firmware...");
  delay(300);

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(client, binUrl);

  if (ret == HTTP_UPDATE_FAILED) {
    broadcastOtaMsg("Update Gagal: " + httpUpdate.getLastErrorString());
  }
  // Jika sukses, ESP32 otomatis reboot
}

// ================= KONTROL MOTOR NON-BLOCKING =================
void updateMotors() {
  unsigned long now = millis();

  if (!isRunning) {
    stopAllMotorsImmediate();
    return;
  }

  // 1. Soft-start ramping untuk 3 motor MOSFET (update tiap 15ms)
  if (now - lastRampMs >= 15) {
    lastRampMs = now;
    auto ramp = [](float &cur, uint8_t target) {
      if (cur < target) cur = min((float)target, cur + 4.0f);
      else if (cur > target) cur = max((float)target, cur - 8.0f);
    };
    ramp(curTop, activeState.top);
    ramp(curBack, activeState.back);
    ramp(curFeed, activeState.feed);

    writePwmPin(PIN_TOP, 0, (uint8_t)curTop);
    writePwmPin(PIN_BACK, 1, (uint8_t)curBack);
    writePwmPin(PIN_FEED, 2, (uint8_t)curFeed);
  }

  // 2. Kontrol H-Bridge Motor Osilasi (Tengok Kanan-Kiri)
  if (activeState.oscMode == 0 || activeState.oscSpeed == 0) {
    writePwmPin(PIN_OSC_L, 3, 0);
    writePwmPin(PIN_OSC_R, 4, 0);
    return;
  }

  if (oscDeadtime) {
    // Jeda 80ms sebelum membalik arah putaran agar gir tidak hentak
    if (now - lastOscMs >= 80) {
      oscDeadtime = false;
      lastOscMs = now;
      oscDir = -oscDir;
      currentOscDuration = (activeState.oscMode == 2)
                           ? random(400, activeState.oscTime + 1)
                           : activeState.oscTime;
    }
    return;
  }

  if (now - lastOscMs >= currentOscDuration) {
    lastOscMs = now;
    oscDeadtime = true;
    writePwmPin(PIN_OSC_L, 3, 0);
    writePwmPin(PIN_OSC_R, 4, 0);
  } else {
    if (oscDir > 0) {
      writePwmPin(PIN_OSC_L, 3, activeState.oscSpeed);
      writePwmPin(PIN_OSC_R, 4, 0);
    } else {
      writePwmPin(PIN_OSC_L, 3, 0);
      writePwmPin(PIN_OSC_R, 4, activeState.oscSpeed);
    }
  }
}

void setup() {
  Serial.begin(115200);

  setupPwmPin(PIN_TOP, 0);
  setupPwmPin(PIN_BACK, 1);
  setupPwmPin(PIN_FEED, 2);
  setupPwmPin(PIN_OSC_L, 3);
  setupPwmPin(PIN_OSC_R, 4);
  stopAllMotorsImmediate();

  loadStorage();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  if (staSsid.length() > 0) {
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });
  server.begin();
}

void loop() {
  ws.cleanupClients();
  updateMotors();

  if (triggerOta) {
    triggerOta = false;
    performGitHubOTA();
  }
}
