/*
  ============================================================
   ESP8266 PATIENT MONITORING SYSTEM  (Final Version)
  ============================================================
   Hardware:
     - NodeMCU ESP8266
     - LM35 temperature sensor  -> A0
         (LM35 VCC ko VIN/5V se do, GND common, OUT -> A0)
     - Digital pulse sensor     -> D0 (GPIO16)

   Open in browser:  http://<ESP-IP>/   ya   http://patientmonitor.local/

   NOTE: Ye sirf educational / demo project hai, medical device nahi.
         Skin par LM35 se jo reading aati hai wo core body temp se kam hoti hai.
  ============================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// =====================================================
// SETTINGS  (yahan sirf apna data badlo)
// =====================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOST_NAME     = "patientmonitor";   // http://patientmonitor.local

#define LM35_PIN   A0
#define PULSE_PIN  D0

const float TEMP_OFFSET_C = 0.0;     // calibration: thermometer se compare karke adjust karo

// Pulse settings
const unsigned long MIN_BEAT_GAP_MS   = 300;    // 200 BPM se tez beat ignore
const unsigned long MAX_BEAT_GAP_MS   = 2000;   // 30 BPM se slow beat = invalid
const unsigned long NO_PULSE_TIMEOUT  = 3000;   // itne ms beat nahi mili -> finger hata hai
const int           BPM_AVG_COUNT     = 5;      // last 5 beats ka average

// Temperature settings
const int           TEMP_SAMPLES      = 16;     // ek reading = 16 samples ka average
const unsigned long TEMP_INTERVAL_MS  = 250;
const float         TEMP_SMOOTHING    = 0.2;    // 0-1, chhota = zyada smooth

// WiFi reconnect check
const unsigned long WIFI_CHECK_MS     = 10000;

// =====================================================
// GLOBALS
// =====================================================

ESP8266WebServer server(80);

float temperatureC = 0.0;
bool  tempValid    = false;

int  bpm            = 0;
bool fingerPresent  = false;

bool          pulseState   = false;
unsigned long lastBeatTime = 0;
int           bpmHistory[BPM_AVG_COUNT];
int           bpmCount     = 0;
int           bpmIndex     = 0;

unsigned long lastTempRead   = 0;
unsigned long lastWifiCheck  = 0;
unsigned long lastSerialLog  = 0;

// =====================================================
// TEMPERATURE (LM35)
// =====================================================

float readTemperatureOnce()
{
  long sum = 0;
  for (int i = 0; i < TEMP_SAMPLES; i++)
  {
    sum += analogRead(LM35_PIN);
    delayMicroseconds(200);
  }
  float raw     = sum / (float)TEMP_SAMPLES;

  // NodeMCU A0 = 0-3.3V -> 0-1023
  float voltage = (raw / 1023.0) * 3.3;

  // LM35 = 10 mV per degree C
  return (voltage * 100.0) + TEMP_OFFSET_C;
}

void updateTemperature()
{
  unsigned long now = millis();
  if (now - lastTempRead < TEMP_INTERVAL_MS) return;
  lastTempRead = now;

  float t = readTemperatureOnce();

  // Sensor disconnected / garbage reading
  if (t < 5.0 || t > 60.0)
  {
    tempValid = false;
    return;
  }

  if (!tempValid) temperatureC = t;                       // first valid reading
  else temperatureC += TEMP_SMOOTHING * (t - temperatureC); // exponential smoothing

  tempValid = true;
}

// =====================================================
// PULSE / HEART RATE
// =====================================================

void addBpmSample(int value)
{
  bpmHistory[bpmIndex] = value;
  bpmIndex = (bpmIndex + 1) % BPM_AVG_COUNT;
  if (bpmCount < BPM_AVG_COUNT) bpmCount++;

  long sum = 0;
  for (int i = 0; i < bpmCount; i++) sum += bpmHistory[i];
  bpm = sum / bpmCount;
}

void resetBpm()
{
  bpm = 0;
  bpmCount = 0;
  bpmIndex = 0;
  lastBeatTime = 0;
  fingerPresent = false;
}

void readPulse()
{
  bool signal = digitalRead(PULSE_PIN);
  unsigned long now = millis();

  // Rising edge = new beat
  if (signal == HIGH && !pulseState)
  {
    pulseState = true;

    if (lastBeatTime == 0)
    {
      lastBeatTime = now;
    }
    else
    {
      unsigned long interval = now - lastBeatTime;

      if (interval >= MIN_BEAT_GAP_MS && interval <= MAX_BEAT_GAP_MS)
      {
        addBpmSample(60000UL / interval);
        fingerPresent = true;
        lastBeatTime = now;
      }
      else if (interval > MAX_BEAT_GAP_MS)
      {
        // Bahut der baad beat aayi - dobara start karo
        resetBpm();
        lastBeatTime = now;
      }
      // interval < MIN_BEAT_GAP_MS => noise, ignore
    }
  }

  if (signal == LOW) pulseState = false;

  // Timeout: kaafi der se beat nahi mili
  if (lastBeatTime > 0 && (now - lastBeatTime) > NO_PULSE_TIMEOUT)
  {
    resetBpm();
  }
}

// =====================================================
// STATUS TEXT
// =====================================================

const char* bpmStatus()
{
  if (!fingerPresent || bpm == 0) return "NO SIGNAL";
  if (bpm < 50)  return "LOW";
  if (bpm > 100) return "HIGH";
  return "NORMAL";
}

const char* tempStatus()
{
  if (!tempValid)          return "NO SENSOR";
  if (temperatureC < 35.0) return "LOW";
  if (temperatureC >= 38.0) return "FEVER";
  if (temperatureC >= 37.5) return "ELEVATED";
  return "NORMAL";
}

// =====================================================
// WEB PAGE
// =====================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Patient Monitoring System</title>

<style>
*{box-sizing:border-box}
body{margin:0;font-family:Arial,Helvetica,sans-serif;background:#f1f5f9;color:#0f172a}
.header{background:#0f172a;color:#fff;padding:22px}
.header h1{margin:0;font-size:25px}
.header p{margin:6px 0 0;color:#cbd5e1}
.container{max-width:1050px;margin:auto;padding:20px}

.status{background:#fff;border-radius:15px;padding:16px 20px;margin-bottom:20px;
display:flex;justify-content:space-between;align-items:center;box-shadow:0 4px 15px rgba(0,0,0,.07)}
.online{color:#16a34a;font-weight:bold}
.offline{color:#dc2626;font-weight:bold}

.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:18px}
.card{background:#fff;border-radius:18px;padding:24px;box-shadow:0 4px 15px rgba(0,0,0,.07)}
.title{color:#64748b;font-size:14px;margin-bottom:15px;letter-spacing:.5px}
.value{font-size:42px;font-weight:bold}
.unit{font-size:16px;color:#64748b}
.heart{color:#e11d48}
.temp{color:#ea580c}

.pill{display:inline-block;margin-top:14px;padding:5px 12px;border-radius:999px;
font-size:12px;font-weight:bold;background:#e2e8f0;color:#475569}
.pill.ok{background:#dcfce7;color:#15803d}
.pill.warn{background:#fef3c7;color:#b45309}
.pill.bad{background:#fee2e2;color:#b91c1c}

.beat{display:inline-block;animation:none}
.beat.on{animation:pulse .8s infinite}
@keyframes pulse{0%,100%{transform:scale(1)}50%{transform:scale(1.25)}}

.panel{margin-top:20px;background:#fff;border-radius:18px;padding:22px;box-shadow:0 4px 15px rgba(0,0,0,.07)}
.panel h3{margin:0 0 8px}
.row{display:flex;justify-content:space-between;padding:13px 0;border-bottom:1px solid #e2e8f0}
.row:last-child{border-bottom:none}

.alert{display:none;margin-bottom:20px;padding:16px 20px;border-radius:15px;
background:#fee2e2;color:#b91c1c;font-weight:bold;text-align:center}

.footer{text-align:center;margin-top:30px;color:#64748b;font-size:13px}
</style>
</head>

<body>

<div class="header">
  <h1>Patient Monitoring System</h1>
  <p>ESP8266 Local Health Dashboard</p>
</div>

<div class="container">

  <div class="alert" id="alert"></div>

  <div class="status">
    <strong>Device Status</strong>
    <span id="conn" class="offline">&#9679; CONNECTING...</span>
  </div>

  <div class="grid">

    <div class="card">
      <div class="title">HEART RATE</div>
      <div class="value heart">
        <span class="beat" id="heartIcon">&#10084;</span>
        <span id="bpm">--</span>
        <span class="unit">BPM</span>
      </div>
      <span class="pill" id="bpmStatus">--</span>
    </div>

    <div class="card">
      <div class="title">BODY TEMPERATURE</div>
      <div class="value temp">
        <span id="temperature">--</span>
        <span class="unit">&deg;C</span>
      </div>
      <span class="pill" id="tempStatus">--</span>
    </div>

  </div>

  <div class="panel">
    <h3>Patient Information</h3>
    <div class="row"><span>Heart Rate</span><strong><span id="bpm2">--</span> BPM</strong></div>
    <div class="row"><span>Temperature</span><strong><span id="temperature2">--</span> &deg;C</strong></div>
    <div class="row"><span>Temperature (&deg;F)</span><strong><span id="tempF">--</span> &deg;F</strong></div>
    <div class="row"><span>Wi-Fi Signal</span><strong id="rssi">--</strong></div>
    <div class="row"><span>ESP8266 IP</span><strong id="ip">--</strong></div>
    <div class="row"><span>Uptime</span><strong id="uptime">--</strong></div>
    <div class="row"><span>Last Update</span><strong id="updated">--</strong></div>
  </div>

  <div class="footer">
    ESP8266 Patient Monitoring System &bull; For demonstration only, not a medical device
  </div>

</div>

<script>
var failCount = 0;

function pillClass(s){
  if(s==="NORMAL") return "pill ok";
  if(s==="NO SIGNAL"||s==="NO SENSOR") return "pill";
  if(s==="ELEVATED"||s==="LOW") return "pill warn";
  return "pill bad";
}

function fmtUptime(sec){
  var h=Math.floor(sec/3600), m=Math.floor((sec%3600)/60), s=sec%60;
  return h+"h "+m+"m "+s+"s";
}

function setText(id, v){ document.getElementById(id).innerText = v; }

function updateData(){
  fetch('/data')
  .then(function(r){ return r.json(); })
  .then(function(d){
    failCount = 0;

    var conn = document.getElementById("conn");
    conn.className = "online";
    conn.innerHTML = "&#9679; ONLINE";

    var bpmTxt  = d.bpm > 0 ? d.bpm : "--";
    var tempTxt = d.tempValid ? d.temperature.toFixed(1) : "--";
    var tempF   = d.tempValid ? (d.temperature*9/5+32).toFixed(1) : "--";

    setText("bpm", bpmTxt);
    setText("bpm2", bpmTxt);
    setText("temperature", tempTxt);
    setText("temperature2", tempTxt);
    setText("tempF", tempF);
    setText("rssi", d.rssi + " dBm");
    setText("ip", d.ip);
    setText("uptime", fmtUptime(d.uptime));
    setText("updated", new Date().toLocaleTimeString());

    var bs = document.getElementById("bpmStatus");
    bs.innerText = d.bpmStatus; bs.className = pillClass(d.bpmStatus);

    var ts = document.getElementById("tempStatus");
    ts.innerText = d.tempStatus; ts.className = pillClass(d.tempStatus);

    document.getElementById("heartIcon").className = d.bpm > 0 ? "beat on" : "beat";

    // Alert banner
    var msgs = [];
    if(d.tempStatus==="FEVER") msgs.push("High temperature detected");
    if(d.bpmStatus==="HIGH")   msgs.push("High heart rate detected");
    if(d.bpmStatus==="LOW")    msgs.push("Low heart rate detected");
    var al = document.getElementById("alert");
    if(msgs.length){ al.style.display="block"; al.innerText="\u26A0 " + msgs.join(" | "); }
    else { al.style.display="none"; }
  })
  .catch(function(){
    failCount++;
    if(failCount >= 3){
      var conn = document.getElementById("conn");
      conn.className = "offline";
      conn.innerHTML = "&#9679; OFFLINE";
    }
  });
}

setInterval(updateData, 1000);
updateData();
</script>

</body>
</html>
)rawliteral";

// =====================================================
// HTTP HANDLERS
// =====================================================

void handleRoot()
{
  server.send_P(200, "text/html", MAIN_PAGE);
}

void handleData()
{
  char json[400];

  snprintf(json, sizeof(json),
    "{\"bpm\":%d,\"temperature\":%.2f,\"tempValid\":%s,"
    "\"bpmStatus\":\"%s\",\"tempStatus\":\"%s\","
    "\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu}",
    bpm,
    temperatureC,
    tempValid ? "true" : "false",
    bpmStatus(),
    tempStatus(),
    WiFi.RSSI(),
    WiFi.localIP().toString().c_str(),
    millis() / 1000UL
  );

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleNotFound()
{
  server.send(404, "text/plain", "Not found");
}

// =====================================================
// WIFI
// =====================================================

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOST_NAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("WiFi Connected!");
    Serial.print("ESP8266 IP Address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("WiFi connect failed - will keep retrying in background.");
  }
}

void checkWiFi()
{
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_MS) return;
  lastWifiCheck = now;

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi lost. Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==============================");
  Serial.println(" PATIENT MONITORING SYSTEM");
  Serial.println("==============================");

  pinMode(PULSE_PIN, INPUT);

  connectWiFi();

  if (MDNS.begin(HOST_NAME))
  {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS: http://");
    Serial.print(HOST_NAME);
    Serial.println(".local");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Web Server Started");
  Serial.println("Open the IP address in browser");
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  updateTemperature();
  readPulse();

  server.handleClient();
  MDNS.update();
  checkWiFi();

  // Serial monitor log har 2 second
  if (millis() - lastSerialLog >= 2000)
  {
    lastSerialLog = millis();
    Serial.printf("BPM: %d (%s) | Temp: %.2f C (%s)\n",
                  bpm, bpmStatus(), temperatureC, tempStatus());
  }

  delay(2);
}
