/*
  ============================================================
   ESP32 PATIENT MONITORING SYSTEM  (Final Version)
  ============================================================
   Hardware:
     - ESP32 DevKit (30 pin)
     - LM35DT temperature sensor -> D35 (GPIO35)
         (LM35 +Vs ko VIN/5V se do, GND common, OUT -> D35)
     - Analog PulseSensor (S, +, -) -> D34 (GPIO34)
         (+ ko 3V3 se do, - GND, S -> D34)
     - D34/D35 dono ADC1 pins hain (ADC2 WiFi ke saath kaam nahi karta)

   Open in browser:  http://<ESP-IP>/   ya   http://patientmonitor.local/

   NOTE: Ye sirf educational / demo project hai, medical device nahi.
         Skin par LM35 se jo reading aati hai wo core body temp se kam hoti hai.
  ============================================================
*/

#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  #include <HTTPClient.h>
#else   // ESP8266 (NodeMCU ESP-12E)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #include <ESP8266HTTPClient.h>
#endif
#include <WiFiClientSecure.h>
#include <Ticker.h>

// =====================================================
// SETTINGS  (yahan sirf apna data badlo)
// =====================================================

// Firebase Realtime Database (cloud dashboard ke liye)
const char* FIREBASE_URL     = "https://patient-76db1-default-rtdb.firebaseio.com/live.json";
const unsigned long FIREBASE_PUSH_MS = 2000;

const char* WIFI_SSID     = "Meet";
const char* WIFI_PASSWORD = "meettroj";
const char* HOST_NAME     = "patientmonitor";   // http://patientmonitor.local

#if defined(ESP32)
  #define PULSE_PIN  34      // D34 (ADC1)
  #define LM35_PIN   35      // D35 (ADC1)
  #define HAS_TEMP   1
  #define ADC_MAX    4095
#else
  // ESP8266 me sirf ek analog pin (A0) hai -> pulse sensor yahan, temperature band
  // (LM35 ke liye ADS1115 / DS18B20 / ESP32 chahiye)
  #define PULSE_PIN  A0
  #define HAS_TEMP   0
  #define ADC_MAX    1023
#endif

// Calibration: Serial Monitor me "LM35 mV" dekho, real thermometer se compare karo.
// Do points lo (T1,shown1) (T2,shown2): GAIN = (T2-T1)/(shown2-shown1), phir OFFSET se baseline match karo.
const float TEMP_GAIN     = 1.0;
const float TEMP_OFFSET_C = 0.0;

// Real LM35 nahi hai to temperature simulated dikhao (dashboard pe "SIMULATED" tag lagta hai)
const bool TEMP_SIMULATED = true;
#define TEMP_IS_SIM  (!HAS_TEMP && TEMP_SIMULATED)

// Pulse settings (analog PulseSensor)
const float         PULSE_MIN_AMPLITUDE = 8;     // beat wave ka peak (10-bit counts). Is se kam = finger nahi. Chhota = zyada sensitive
const unsigned long PULSE_SAMPLE_MS     = 20;    // 50 Hz
const unsigned long MIN_BEAT_GAP_MS   = 400;    // 150 BPM se tez beat ignore (dicrotic notch / noise)
const unsigned long MAX_BEAT_GAP_MS   = 2000;   // 30 BPM se slow beat = invalid
const unsigned long NO_PULSE_TIMEOUT  = 3000;   // itne ms beat nahi mili -> finger hata hai
const int           BPM_AVG_COUNT     = 9;      // last 9 beats ka median (miss hui beat se BPM nahi girta)
const int           BPM_MIN_BEATS     = 3;      // itni beats ke baad hi BPM dikhao (touch ke jhatke ignore)

// Temperature settings
const int           TEMP_SAMPLES      = 64;     // ADC noisy hai, zyada samples = stable
const unsigned long TEMP_INTERVAL_MS  = 250;
const float         TEMP_SMOOTHING    = 0.2;    // 0-1, chhota = zyada smooth

// WiFi reconnect check
const unsigned long WIFI_CHECK_MS     = 10000;

// =====================================================
// GLOBALS
// =====================================================

#if defined(ESP32)
WebServer server(80);
#else
ESP8266WebServer server(80);
#endif

float temperatureC = 0.0;
bool  tempValid    = false;
float lastRawMv    = 0.0;   // LM35 pin ka average millivolt (debug)

// Pulse waveform state
float pulseBase = 0;        // slow baseline (DC)
float pulseSmooth = 0;      // baseline-removed, smoothed wave
float pulseAmp = 0;         // decaying peak of pulseSmooth
Ticker pulseTicker;               // pulse sampling timer pe: Firebase push (~3 s network wait) ke dauran bhi chalta hai
unsigned long lastBeatSeen = 0;   // koi bhi beat (timeout ke liye); lastBeatTime sirf interval ke liye

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
unsigned long lastFirebasePush = 0;

// GTS Root R1 (Google Trust Services) - Firebase ka TLS root. Google rotate kare to naya PEM yahan daalo.
static const char GTS_ROOT_R1[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFYjCCBEqgAwIBAgIQd70NbNs2+RrqIQ/E8FjTDTANBgkqhkiG9w0BAQsFADBX
MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE
CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIwMDYx
OTAwMDA0MloXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT
GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFIx
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAthECix7joXebO9y/lD63
ladAPKH9gvl9MgaCcfb2jH/76Nu8ai6Xl6OMS/kr9rH5zoQdsfnFl97vufKj6bwS
iV6nqlKr+CMny6SxnGPb15l+8Ape62im9MZaRw1NEDPjTrETo8gYbEvs/AmQ351k
KSUjB6G00j0uYODP0gmHu81I8E3CwnqIiru6z1kZ1q+PsAewnjHxgsHA3y6mbWwZ
DrXYfiYaRQM9sHmklCitD38m5agI/pboPGiUU+6DOogrFZYJsuB6jC511pzrp1Zk
j5ZPaK49l8KEj8C8QMALXL32h7M1bKwYUH+E4EzNktMg6TO8UpmvMrUpsyUqtEj5
cuHKZPfmghCN6J3Cioj6OGaK/GP5Afl4/Xtcd/p2h/rs37EOeZVXtL0m79YB0esW
CruOC7XFxYpVq9Os6pFLKcwZpDIlTirxZUTQAs6qzkm06p98g7BAe+dDq6dso499
iYH6TKX/1Y7DzkvgtdizjkXPdsDtQCv9Uw+wp9U7DbGKogPeMa3Md+pvez7W35Ei
Eua++tgy/BBjFFFy3l3WFpO9KWgz7zpm7AeKJt8T11dleCfeXkkUAKIAf5qoIbap
sZWwpbkNFhHax2xIPEDgfg1azVY80ZcFuctL7TlLnMQ/0lUTbiSw1nH69MG6zO0b
9f6BQdgAmD06yK56mDcYBZUCAwEAAaOCATgwggE0MA4GA1UdDwEB/wQEAwIBhjAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTkrysmcRorSCeFL1JmLO/wiRNxPjAf
BgNVHSMEGDAWgBRge2YaRQ2XyolQL30EzTSo//z9SzBgBggrBgEFBQcBAQRUMFIw
JQYIKwYBBQUHMAGGGWh0dHA6Ly9vY3NwLnBraS5nb29nL2dzcjEwKQYIKwYBBQUH
MAKGHWh0dHA6Ly9wa2kuZ29vZy9nc3IxL2dzcjEuY3J0MDIGA1UdHwQrMCkwJ6Al
oCOGIWh0dHA6Ly9jcmwucGtpLmdvb2cvZ3NyMS9nc3IxLmNybDA7BgNVHSAENDAy
MAgGBmeBDAECATAIBgZngQwBAgIwDQYLKwYBBAHWeQIFAwIwDQYLKwYBBAHWeQIF
AwMwDQYJKoZIhvcNAQELBQADggEBADSkHrEoo9C0dhemMXoh6dFSPsjbdBZBiLg9
NR3t5P+T4Vxfq7vqfM/b5A3Ri1fyJm9bvhdGaJQ3b2t6yMAYN/olUazsaL+yyEn9
WprKASOshIArAoyZl+tJaox118fessmXn1hIVw41oeQa1v1vg4Fv74zPl6/AhSrw
9U5pCZEt4Wi4wStz6dTZ/CLANx8LZh1J7QJVj2fhMtfTJr9w4z30Z209fOU0iOMy
+qduBmpvvYuR7hZL6Dupszfnw0Skfths18dG9ZKb59UhvmaSGZRVbNQpsg3BZlvi
d0lIKO2d1xozclOzgjXPYovJJIultzkMu34qQb9Sz/yilrbCgj8=
-----END CERTIFICATE-----
)EOF";
#if !defined(ESP32)
BearSSL::X509List fbTrust(GTS_ROOT_R1);
BearSSL::Session  fbSession;   // TLS session resume: handshake (3+ s, pulse sampling rukti thi) skip hota hai
#endif

WiFiClientSecure fbClient;   // global + setReuse = TLS connection reused, push ~100ms (pulse sampling block nahi hota)

// =====================================================
// TEMPERATURE (LM35)
// =====================================================

#if HAS_TEMP
float readTemperatureOnce()
{
  long sum = 0;
  for (int i = 0; i < TEMP_SAMPLES; i++)
  {
    sum += analogReadMilliVolts(LM35_PIN);   // ESP32 factory-calibrated mV
    delayMicroseconds(200);
  }
  lastRawMv = sum / (float)TEMP_SAMPLES;

  // LM35 = 10 mV per degree C
  return (lastRawMv / 10.0) * TEMP_GAIN + TEMP_OFFSET_C;
}
#endif

void updateTemperature()
{
#if !HAS_TEMP
  // ESP8266: A0 pulse sensor ke paas hai, LM35 nahi -> demo ke liye random walk 36.2-37.0 C
  if (!TEMP_SIMULATED) { tempValid = false; return; }
  unsigned long now = millis();
  if (now - lastTempRead < 2000) return;
  lastTempRead = now;
  if (!tempValid) temperatureC = 36.6;
  temperatureC += random(-10, 11) / 100.0;                // +-0.10 C step
  temperatureC = constrain(temperatureC, 36.2, 37.0);
  tempValid = true;
#else
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
#endif
}

// =====================================================
// PULSE / HEART RATE
// =====================================================

void addBpmSample(int value)
{
  bpmHistory[bpmIndex] = value;
  bpmIndex = (bpmIndex + 1) % BPM_AVG_COUNT;
  if (bpmCount < BPM_AVG_COUNT) bpmCount++;

  // Median of last beats (mean nahi): ek galat beat se BPM nahi uchhalta
  int tmp[BPM_AVG_COUNT];
  for (int i = 0; i < bpmCount; i++) tmp[i] = bpmHistory[i];
  for (int i = 1; i < bpmCount; i++)
    for (int j = i; j > 0 && tmp[j] < tmp[j - 1]; j--) { int t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t; }
  bpm = bpmCount >= BPM_MIN_BEATS ? tmp[bpmCount / 2] : 0;
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
  unsigned long now = millis();

  bool signal = pulseState;

  // 50 Hz (Ticker se call hota hai). Slow baseline hataya (finger ka DC drift), phir bacha hua beat wave detect hota hai
  {
    int raw = analogRead(PULSE_PIN);
    if (pulseBase == 0) pulseBase = raw;
    pulseBase += (fabs(raw - pulseBase) > 60 ? 0.3 : 0.04) * (raw - pulseBase);   // touch/spike pe baseline jaldi sambhalta hai
    float hp = constrain(raw - pulseBase, -100.0, 100.0) * (1023.0 / ADC_MAX);   // 10-bit units, touch spikes clamp
    pulseSmooth += 0.25 * (hp - pulseSmooth);
    pulseAmp = max(pulseSmooth, pulseAmp * 0.99f);                                // decaying peak = signal size

    float thr = max(5.0f, 0.3f * pulseAmp);
    if (pulseAmp < PULSE_MIN_AMPLITUDE)  signal = false;       // flat line = finger nahi
    else if (pulseSmooth > thr)          signal = true;
    else if (pulseSmooth < thr * 0.5f)   signal = false;       // hysteresis: ek beat = ek hi crossing
  }

  // Rising crossing = new beat
  if (signal && !pulseState)
  {
    pulseState = true;
    lastBeatSeen = now;

    if (lastBeatTime == 0)
    {
      lastBeatTime = now;
    }
    else
    {
      unsigned long interval = now - lastBeatTime;

      if (interval >= MIN_BEAT_GAP_MS && interval <= MAX_BEAT_GAP_MS)
      {
        addBpmSample(60000UL / interval);   // median lena outlier beats ko khud hata deta hai
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

  if (!signal) pulseState = false;

  // Timeout: kaafi der se beat nahi mili
  if (bpm > 0 && (now - lastBeatSeen) > NO_PULSE_TIMEOUT)
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
  if (!tempValid)          return TEMP_IS_SIM ? "NO SIGNAL" : "NO SENSOR";
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
      <div class="title" id="tempTitle">BODY TEMPERATURE</div>
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
    <div class="row"><span>Device IP</span><strong id="ip">--</strong></div>
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
    setText("tempTitle", d.tempSim ? "BODY TEMPERATURE (SIMULATED)" : "BODY TEMPERATURE");
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
    "\"bpmStatus\":\"%s\",\"tempStatus\":\"%s\",\"tempSim\":%s,"
    "\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu}",
    bpm,
    temperatureC,
    tempValid ? "true" : "false",
    bpmStatus(),
    tempStatus(),
    TEMP_IS_SIM ? "true" : "false",
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
#if defined(ESP32)
  WiFi.setHostname(HOST_NAME);   // ESP32: mode() se pehle
  WiFi.mode(WIFI_STA);
#else
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOST_NAME);
#endif
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
    Serial.print("IP Address: ");
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
// FIREBASE PUSH
// =====================================================

void pushToFirebase()
{
  unsigned long now = millis();
  if (now - lastFirebasePush < FIREBASE_PUSH_MS) return;
  lastFirebasePush = now;
  if (WiFi.status() != WL_CONNECTED) return;
  if (time(nullptr) < 1700000000) return;   // NTP sync nahi hua = cert date check fail hoga

  char json[400];
  snprintf(json, sizeof(json),
    "{\"bpm\":%d,\"temperature\":%.2f,\"tempValid\":%s,"
    "\"bpmStatus\":\"%s\",\"tempStatus\":\"%s\",\"tempSim\":%s,"
    "\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"ts\":{\".sv\":\"timestamp\"}}",
    bpm, temperatureC, tempValid ? "true" : "false",
    bpmStatus(), tempStatus(), TEMP_IS_SIM ? "true" : "false",
    WiFi.RSSI(), WiFi.localIP().toString().c_str(), millis() / 1000UL
  );

  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(3000);
  if (http.begin(fbClient, FIREBASE_URL))
  {
    int code = http.PUT(json);   // ~3 s (network); pulse Ticker is dauran bhi sample karta hai
    if (code != 200) Serial.printf("Firebase push failed: %d\n", code);
    http.end();
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
#if defined(ESP32)
  pinMode(LM35_PIN, INPUT);
  fbClient.setCACert(GTS_ROOT_R1);   // Firebase cert validate hota hai (insecure mode nahi)
#else
  system_update_cpu_freq(SYS_CPU_160MHZ);   // TLS/crypto ~2x tez
  fbClient.setTrustAnchors(&fbTrust);
  fbClient.setSession(&fbSession);
#endif

  connectWiFi();
  randomSeed(micros() + WiFi.RSSI());

  // TLS cert validity ke liye sahi time chahiye
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 20 && time(nullptr) < 1700000000; i++) delay(500);
  Serial.println(time(nullptr) < 1700000000 ? "NTP sync failed - Firebase push paused" : "Time synced (NTP)");

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

  pulseTicker.attach_ms(PULSE_SAMPLE_MS, readPulse);
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  updateTemperature();

  server.handleClient();
#if !defined(ESP32)
  MDNS.update();
#endif
  checkWiFi();
  pushToFirebase();

  // Serial monitor log har 2 second
  if (millis() - lastSerialLog >= 2000)
  {
    lastSerialLog = millis();
    Serial.printf("BPM: %d (%s) | Temp: %.2f C (%s) | pulse peak: %d (min %d)\n",
                  bpm, bpmStatus(), temperatureC, tempStatus(),
                  (int)pulseAmp, (int)PULSE_MIN_AMPLITUDE);
  }

  delay(2);
}
