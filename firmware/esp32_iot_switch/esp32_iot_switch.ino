#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TouchSensorLib.h>
#include <GDEY037T03_AdafruitGFX.h>
#include "SinricPro.h"
#include "SinricProSwitch.h"
#include "icons.h"
#include "esp_task_wdt.h"

#define TOUCH_BUTTON 9
#define RELAY_OUTPUT 5
#define WIFI_LED 8
#define SW_LEDB 11
#define SW_LEDR 12
#define EPD_SCK 6
#define EPD_MOSI 7
#define EPD_CS 1
#define EPD_BUSY 4
#define EPD_RST 23
#define EPD_DC 24
#define AP_NAME "IoTSwitch_21098cf54"
#define DEVICE_LANGUAGE 1 // English: 0     Español: 1

TouchSensor touch(TOUCH_BUTTON);

WiFiManager wm;
AsyncWebServer server(80);
Preferences prefs;

GDEY037T03 epaper;  //< Global driver instance. Use driver methods to show images to screen
GDcanvas canvasOffline;   //< No Wifi Canvas
GDcanvas canvasOnline;   //< Wifi Canvas   

const uint32_t WDT_TIMEOUT_SEC = 10;

const long WIFI_SEARCH_TIMEOUT = 5L * 60L * 1000L; // 5 minutes in milliseconds
bool wifiSearching = false;
bool wifiLedState = LOW;
unsigned long lastBlink = 0;
unsigned long lastWifiSearch = 0;
bool switchStatus = false;
volatile bool touchFlag = false;
bool connectionAttempt = false;
bool updateFlag = false;
uint8_t updateCounts = 0;
bool newDataFlag = false;
bool weatherUpdate = false;
bool sinricState = false;

const char* PREF_NAMESPACE = "cfgdata";

String p_location_name = "";
double p_lat = 0.0;
double p_lon = 0.0;
bool   p_loc_set = false;

bool p_key_set    = false;
bool p_secret_set = false;
bool p_id_set     = false;

float  g_temperature  = NAN;
int    g_weathercode  = -1;
String g_localTime    = "";   // format: "2026-07-06T14:30"
String g_localDay     = "";   // format: "2026-07-06"
bool   g_weatherValid = false;

const long WEATHER_INTERVAL_SEC = 30L * 60L; // 30 minutes in seconds
time_t nextWeatherEpoch = 0;
bool   g_timeSynced = false;

void ARDUINO_ISR_ATTR touchISR() {
  touch.update();
  touchFlag = true;
}

void setupWatchdog() {
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };

  esp_err_t err = esp_task_wdt_reconfigure(&twdt_config);
  if (err != ESP_OK) {
    esp_task_wdt_init(&twdt_config);
  }

  esp_task_wdt_add(NULL);
  Serial.println("Watchdog configurado: 10s");
}

struct charReplace {
  const char* utf8;
  const char* ascii;
};

const charReplace REPLACE_TABLE[] = {
  {"á", "a"}, {"à", "a"}, {"ã", "a"}, {"â", "a"}, {"ä", "a"},
  {"é", "e"}, {"è", "e"}, {"ê", "e"}, {"ë", "e"},
  {"í", "i"}, {"ì", "i"}, {"î", "i"}, {"ï", "i"},
  {"ó", "o"}, {"ò", "o"}, {"õ", "o"}, {"ô", "o"}, {"ö", "o"},
  {"ú", "u"}, {"ù", "u"}, {"û", "u"}, {"ü", "u"},

  {"Á", "A"}, {"À", "A"}, {"Ã", "A"}, {"Â", "A"}, {"Ä", "A"},
  {"É", "E"}, {"È", "E"}, {"Ê", "E"}, {"Ë", "E"},
  {"Í", "I"}, {"Ì", "I"}, {"Î", "I"}, {"Ï", "I"},
  {"Ó", "O"}, {"Ò", "O"}, {"Õ", "O"}, {"Ô", "O"}, {"Ö", "O"},
  {"Ú", "U"}, {"Ù", "U"}, {"Û", "U"}, {"Ü", "U"},

  // Ñ > 'nt
  {"ñ", "n"}, {"Ñ", "N"},
  {"ç", "c"}, {"Ç", "C"},

  {"'", "'"},
  {"'", "'"},
  {"–", "-"},
  {"—", "-"},
};

const size_t TOTAL_REPL = sizeof(REPLACE_TABLE) / sizeof(REPLACE_TABLE[0]);

String simplifyName(String textCity) {
  for (size_t i = 0; i < TOTAL_REPL; i++) {
    textCity.replace(REPLACE_TABLE[i].utf8, REPLACE_TABLE[i].ascii);
  }
  return textCity;
}

void drawLight(bool state) {
  if (state) {
    canvasOffline.drawBitmap(LIGHT_X, LIGHT_Y, lightBulbOn, LIGHT_W, LIGHT_H, EPD_BLACK, EPD_WHITE);
    canvasOffline.drawBitmap(SWITCH_X, SWITCH_Y, switchOn, SWITCH_W, SWITCH_H, EPD_BLACK, EPD_WHITE);
  } else{
    canvasOffline.drawBitmap(LIGHT_X, LIGHT_Y, lightBulbOff, LIGHT_W, LIGHT_H, EPD_BLACK, EPD_WHITE);
    canvasOffline.drawBitmap(SWITCH_X, SWITCH_Y, switchOff, SWITCH_W, SWITCH_H, EPD_BLACK, EPD_WHITE);
  }
}

void drawOfflineScreen(){
  canvasOffline.clearBuffer(EPD_WHITE);
  canvasOffline.setTextColor(EPD_BLACK, EPD_WHITE);
  canvasOffline.drawRect(0, 0, 240, 416, EPD_BLACK);
  canvasOffline.drawRect(1, 1, 238, 414, EPD_BLACK);
  canvasOffline.drawRect(2, 2, 236, 412, EPD_BLACK);
  canvasOffline.drawRect(3, 3, 234, 410, EPD_BLACK);
  // canvasOffline.drawLine(4, 22, 25, 22, EPD_BLACK);
  // canvasOffline.drawLine(4, 23, 23, 23, EPD_BLACK);
  // canvasOffline.drawBitmap(SLEEP_X, SLEEP_Y, sleepMode, SLEEP_W, SLEEP_H, EPD_BLACK, EPD_WHITE);
  canvasOffline.drawLine(208, 22, 235, 22, EPD_BLACK);
  canvasOffline.drawLine(210, 23, 235, 23, EPD_BLACK);
  canvasOffline.drawBitmap(WIFI_X, WIFI_Y, wifiOff, WIFI_W, WIFI_H, EPD_BLACK, EPD_WHITE);
}

void drawOnlineScreen(){
  canvasOnline.clearBuffer(EPD_WHITE);
  canvasOnline.setTextColor(EPD_BLACK, EPD_WHITE);
  canvasOnline.drawRect(0, 0, 240, 416, EPD_BLACK);
  canvasOnline.drawRect(1, 1, 238, 414, EPD_BLACK);
  canvasOnline.drawRect(2, 2, 236, 412, EPD_BLACK);
  canvasOnline.drawRect(3, 3, 234, 410, EPD_BLACK);
  canvasOnline.drawLine(208, 22, 235, 22, EPD_BLACK);
  canvasOnline.drawLine(210, 23, 235, 23, EPD_BLACK);
  canvasOnline.drawBitmap(WIFI_X, WIFI_Y, wifiOn, WIFI_W, WIFI_H, EPD_BLACK, EPD_WHITE);

  canvasOnline.drawLine(44, 91, 44, 180, EPD_BLACK);
  canvasOnline.drawLine(45, 91, 45, 180, EPD_BLACK);
  canvasOnline.drawLine(46, 91, 46, 180, EPD_BLACK);
  canvasOnline.drawLine(193, 91, 193, 180, EPD_BLACK);
  canvasOnline.drawLine(194, 91, 194, 180, EPD_BLACK);
  canvasOnline.drawLine(195, 91, 195, 180, EPD_BLACK);
}

void drawQR(int8_t language = -1) {
  canvasOffline.clearBuffer(EPD_WHITE);
  canvasOffline.setTextColor(EPD_BLACK, EPD_WHITE);
  canvasOffline.drawRect(0, 0, 240, 416, EPD_BLACK);
  canvasOffline.drawRect(1, 1, 238, 414, EPD_BLACK);
  canvasOffline.drawRect(2, 2, 236, 412, EPD_BLACK);
  canvasOffline.drawRect(3, 3, 234, 410, EPD_BLACK);

  canvasOffline.setTextSize(2);
  canvasOffline.setCursor(20, 330);
  switch (language) {
    default:
    case 0: canvasOffline.print("Scan to configure"); break;
    case 1: canvasOffline.print("  Escanear para"); break;
  }
  canvasOffline.setCursor(42, 360);
  switch (language) {
    default:
    case 0: canvasOffline.print("WiFi network"); break;
    case 1: canvasOffline.print("configurar WiFi"); break;
  }
  canvasOffline.drawBitmap(QR_X, QR_Y, wifiNetQR, QR_W, QR_H, EPD_BLACK, EPD_WHITE);
}

void printCentered(int16_t middleX, int16_t y, const String& text, uint8_t size) {
  int16_t startX = middleX - ((6 * (text.length()) - 1) * size / 2);
  canvasOnline.setTextSize(size);
  canvasOnline.setCursor(startX, y);
  canvasOnline.print(text);
}

// Language   English:0  Spanish:1
void drawDateAndWeather(float f_temp, int w_code, const String& yyyy_mm_dd, const String& locationName, int8_t language = -1){
  if (yyyy_mm_dd.length() < 10) return;

  int year = yyyy_mm_dd.substring(0, 4).toInt();
  int month = yyyy_mm_dd.substring(5, 7).toInt();
  int day = yyyy_mm_dd.substring(8, 10).toInt();

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12;

  mktime(&t);

  static const char* dayNamesEs[] = {"Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado"};
  static const char* monthNamesEs[] = {"Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio","Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"};
  static const char* dayNamesEn[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  static const char* monthNamesEn[] = {"January", "February", "March", "April", "May", "June","July", "August", "September", "October", "November", "December"};

  const char** dayNames;
  const char** monthNames;
  switch (language) {
    default:
    case 0:
      dayNames = dayNamesEn;
      monthNames = monthNamesEn;
      break;
    case 1:
      dayNames = dayNamesEs;
      monthNames = monthNamesEs;
      break;
  }

  String dayName = dayNames[t.tm_wday];
  String monthName = monthNames[month - 1];
  
  canvasOnline.fillRect(4, 60, 232, 14, EPD_WHITE);
  printCentered(EPD_WIDTH / 2, 60, dayName, 2);

  canvasOnline.setTextSize(10);
  canvasOnline.setCursor(65,101);
  canvasOnline.print(yyyy_mm_dd.substring(8, 10));

  canvasOnline.fillRect(4, 196, 232, 21, EPD_WHITE);
  printCentered(EPD_WIDTH / 2, 196, monthName, 3);

  canvasOnline.fillRect(113, 317, 123, 21, EPD_WHITE);
  canvasOnline.setTextSize(3);
  canvasOnline.setCursor(125,317);
  canvasOnline.print((int)round(f_temp)); canvasOnline.print(" "); canvasOnline.write(0xF7); canvasOnline.print("C");

  canvasOnline.fillRect(113, 349, 123, 14, EPD_WHITE);
  canvasOnline.setTextSize(1);
  canvasOnline.setCursor(125,349);
  canvasOnline.print(locationName);

  switch (w_code) {
    case 0: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, clear, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 1: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, mostlyClear, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 2: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, partCloudy, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 3: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, cloudy, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 45: case 48: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, foggy, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 51: case 53: case 55: case 56: case 57: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, drizzle, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 61: case 63: case 65: case 66: case 67: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, rain, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 71: case 73: case 75: case 85: case 86: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, snowFall, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 77: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, hail, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 80: case 81: case 82: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, rainShower, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 95: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, thunderstorm, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    case 96: case 99: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, thunderstormHail, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
    default: canvasOnline.drawBitmap(WEATHER_X, WEATHER_Y, defaultWeather, WEATHER_W, WEATHER_H, EPD_BLACK, EPD_WHITE); break;
  }
}

void loadParams() {
  prefs.begin(PREF_NAMESPACE, true); // solo lectura
  p_location_name = prefs.getString("loc_name", "");
  p_lat     = prefs.getDouble("loc_lat", 0.0);
  p_lon     = prefs.getDouble("loc_lon", 0.0);
  p_loc_set = prefs.isKey("loc_lat");

  p_key_set    = prefs.isKey("sec_key");
  p_secret_set = prefs.isKey("sec_secret");
  p_id_set     = prefs.isKey("sec_id");
  prefs.end();
}

void saveLocation(const String& name, double lat, double lon) {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString("loc_name", name);
  prefs.putDouble("loc_lat", lat);
  prefs.putDouble("loc_lon", lon);
  prefs.end();
}

void saveSecretIfProvided(const char* nvsKey, const String& value) {
  if (value.length() == 0) return;
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString(nvsKey, value);
  prefs.end();
}

String getSecret(const char* nvsKey) {
  prefs.begin(PREF_NAMESPACE, true);
  String v = prefs.getString(nvsKey, "");
  prefs.end();
  return v;
}

void clearAllParams() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

String weatherCodeToText(int code) {
  switch (code) {
    case 0: return "Despejado";
    case 1: return "Mayormente despejado";
    case 2: return "Parcialmente nublado";
    case 3: return "Nublado";
    case 45: case 48: return "Niebla";
    case 51: case 53: case 55: return "Llovizna";
    case 56: case 57: return "Llovizna helada";
    case 61: case 63: case 65: return "Lluvia";
    case 66: case 67: return "Lluvia helada";
    case 71: case 73: case 75: return "Nieve";
    case 77: return "Granizo fino";
    case 80: case 81: case 82: return "Chubascos";
    case 85: case 86: return "Chubascos de nieve";
    case 95: return "Tormenta electrica";
    case 96: case 99: return "Tormenta con granizo";
    default: return "Desconocido";
  }
}

bool fetchWeather(double lat, double lon) {
  if (WiFi.status() != WL_CONNECTED) return false;

  const int maxAttempts = 4;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    esp_task_wdt_reset();
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    char url[220];
    snprintf(url, sizeof(url),
      "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true&timezone=auto",
      lat, lon);

    if (!http.begin(client, url)) return false;
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      http.end();

      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, payload)) return false;

      g_temperature = doc["current_weather"]["temperature"] | NAN;
      g_weathercode = doc["current_weather"]["weathercode"] | -1;
      String fullTime = doc["current_weather"]["time"].as<String>();
      g_localTime = fullTime;
      int tPos = fullTime.indexOf('T');
      g_localDay = (tPos > 0) ? fullTime.substring(0, tPos) : "";
      g_weatherValid = true;

      int comaPos = p_location_name.indexOf(',');
      String p_city = (comaPos >= 0) ? p_location_name.substring(0, comaPos) : "Unknown";
      p_city = (p_city.length() <= 17) ? p_city : p_city.substring(0,17);

      drawDateAndWeather(g_temperature, g_weathercode, g_localDay, simplifyName(p_city), DEVICE_LANGUAGE);

      // Serial.printf("[clima] %.1f C, codigo %d (%s), hora local %s\n",
      //               g_temperature, g_weathercode,
      //               weatherCodeToText(g_weathercode).c_str(), g_localTime.c_str());
      // Serial.println();
      return true;
    }
  }
  return false;
}

void initTimeSync() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  // Serial.print("Sincronizando hora via NTP");
  time_t now = time(nullptr);
  uint8_t attempts = 0;
  while (now < 1700000000 && attempts < 20) {
    esp_task_wdt_reset();
    delay(250);
    // Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  // Serial.println();

  g_timeSynced = (now >= 1700000000);
  // if (g_timeSynced) {
  //   Serial.printf("Hora sincronizada (epoch UTC: %ld)\n", (long)now);
  // } else {
  //   Serial.println("No se pudo sincronizar la hora por NTP");
  // }
}

time_t nextHalfHourBoundary(time_t now) {
  return ((now / WEATHER_INTERVAL_SEC) + 1) * WEATHER_INTERVAL_SEC;
}

void updateWeatherIfDue() {
  if (!p_loc_set) return;

  if (!g_timeSynced) {
    initTimeSync();
    if (!g_timeSynced) return;
  }

  time_t now = time(nullptr);

  if (nextWeatherEpoch == 0) {
    weatherUpdate = fetchWeather(p_lat, p_lon);
    nextWeatherEpoch = nextHalfHourBoundary(now);
    return;
  }

  if (now >= nextWeatherEpoch) {
    weatherUpdate = fetchWeather(p_lat, p_lon);
    nextWeatherEpoch = nextHalfHourBoundary(now);
  }
}

String buildConfigPage() {
  loadParams();

  String locNameEsc = htmlEscape(p_location_name);
  String locDisplay = p_loc_set
      ? (locNameEsc + " (" + String(p_lat, 4) + ", " + String(p_lon, 4) + ")")
      : "&lt;blank&gt;";

  String keyDisplay    = p_key_set    ? "saved" : "&lt;blank&gt;";
  String secretDisplay = p_secret_set ? "saved" : "&lt;blank&gt;";
  String idDisplay     = p_id_set     ? "saved" : "&lt;blank&gt;";

  String keyPlaceholder    = p_key_set    ? "(already set, leave blank to not change)" : "";
  String secretPlaceholder = p_secret_set ? "(already set, leave blank to not change)" : "";
  String idPlaceholder     = p_id_set     ? "(already set, leave blank to not change)" : "";

  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Device configuration</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;max-width:480px;margin:30px auto;padding:0 16px;color:#222;}";
  html += "label{display:block;margin-top:12px;font-weight:bold;}";
  html += "input{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;font-size:15px;}";
  html += "button{margin-top:20px;padding:10px 20px;background:#2e7d32;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:15px;}";
  html += "button:hover{background:#256428;}";
  html += "table{margin-top:24px;width:100%;border-collapse:collapse;}";
  html += "td,th{border:1px solid #ccc;padding:6px 8px;text-align:left;font-size:14px;}";
  html += "#suggestions{position:relative;border:1px solid #ccc;border-top:none;max-height:180px;overflow-y:auto;background:#fff;}";
  html += "#suggestions div{padding:8px;cursor:pointer;border-bottom:1px solid #eee;font-size:14px;}";
  html += "#suggestions div:hover{background:#f0f0f0;}";
  html += "small{color:#666;}";
  html += "</style></head><body>";

  html += "<h2>Device configuration</h2>";
  html += "<p>IP: " + WiFi.localIP().toString() + "</p>";

  // ---- Table ----
  html += "<h3>Saved configuration</h3>";
  html += "<table><tr><th>Parameter</th><th>Value</th></tr>";
  html += "<tr><td>Location</td><td>" + locDisplay + "</td></tr>";
  html += "<tr><td>Key</td><td>" + keyDisplay + "</td></tr>";
  html += "<tr><td>Secret</td><td>" + secretDisplay + "</td></tr>";
  html += "<tr><td>ID</td><td>" + idDisplay + "</td></tr>";
  html += "</table>";

  // ---- Configuration ----
  html += "<h3>Edit configuration</h3>";
  html += "<form id='cfgForm' action='/save' method='POST'>";

  html += "<label for='loc_search'>Location (write to search)</label>";
  html += "<input type='text' id='loc_search' autocomplete='off' placeholder='Ej: City, State, Country' value='" + locNameEsc + "'>";
  html += "<div id='suggestions'></div>";
  html += "<input type='hidden' id='location_name' name='location_name' value='" + locNameEsc + "'>";
  html += "<input type='hidden' id='latitude' name='latitude' value='" + String(p_lat, 6) + "'>";
  html += "<input type='hidden' id='longitude' name='longitude' value='" + String(p_lon, 6) + "'>";

  html += "<label for='key_param'>Key</label>";
  html += "<input type='text' id='key_param' name='key_param' placeholder=\"" + keyPlaceholder + "\" autocomplete='off'>";

  html += "<label for='secret_param'>Secret</label>";
  html += "<input type='text' id='secret_param' name='secret_param' placeholder=\"" + secretPlaceholder + "\" autocomplete='off'>";

  html += "<label for='id_param'>ID</label>";
  html += "<input type='text' id='id_param' name='id_param' placeholder=\"" + idPlaceholder + "\" autocomplete='off'>";

  html += "<small>Leave Key/Secret/ID blank if you don't want to change an already saved value.</small>";

  html += "<button type='submit'>Save</button>";
  html += "</form>";

  // ---- JS: Open-Meteo Geocoding API ----
  html += "<script>";
  html += "let debounceTimer;";
  html += "const searchBox = document.getElementById('loc_search');";
  html += "const suggBox = document.getElementById('suggestions');";
  html += "searchBox.addEventListener('input', function() {";
  html += "  clearTimeout(debounceTimer);";
  html += "  const q = this.value.trim();";
  html += "  if (q.length < 2) { suggBox.innerHTML=''; return; }";
  html += "  debounceTimer = setTimeout(() => searchLocation(q), 350);";
  html += "});";
  html += "async function searchLocation(q) {";
  html += "  try {";
  html += "    const url = 'https://geocoding-api.open-meteo.com/v1/search?name=' + encodeURIComponent(q) + '&count=5&language=es';";
  html += "    const res = await fetch(url);";
  html += "    const data = await res.json();";
  html += "    suggBox.innerHTML = '';";
  html += "    if (!data.results) return;";
  html += "    data.results.forEach(r => {";
  html += "      const div = document.createElement('div');";
  html += "      const admin = r.admin1 ? (r.admin1 + ', ') : '';";
  html += "      div.textContent = r.name + ', ' + admin + r.country;";
  html += "      div.onclick = () => selectLocation(r);";
  html += "      suggBox.appendChild(div);";
  html += "    });";
  html += "  } catch (e) { console.error('Error searching for location', e); }";
  html += "}";
  html += "function selectLocation(r) {";
  html += "  const admin = r.admin1 ? (r.admin1 + ', ') : '';";
  html += "  const fullName = r.name + ', ' + admin + r.country;";
  html += "  searchBox.value = fullName;";
  html += "  document.getElementById('location_name').value = fullName;";
  html += "  document.getElementById('latitude').value = r.latitude;";
  html += "  document.getElementById('longitude').value = r.longitude;";
  html += "  suggBox.innerHTML = '';";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  return html;
}

void handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/html; charset=utf-8", buildConfigPage());
}

void handleSave(AsyncWebServerRequest *request) {
  if (!request->hasParam("location_name", true) ||
      !request->hasParam("latitude", true) ||
      !request->hasParam("longitude", true)) {
    request->send(400, "text/plain", "Location parameters missing");
    return;
  }

  String locName = request->getParam("location_name", true)->value();
  double lat = request->getParam("latitude", true)->value().toDouble();
  double lon = request->getParam("longitude", true)->value().toDouble();
  if (locName.length() > 0) {
    saveLocation(locName, lat, lon);
  }

  String keyVal    = request->hasParam("key_param", true)    ? request->getParam("key_param", true)->value()    : "";
  String secretVal = request->hasParam("secret_param", true) ? request->getParam("secret_param", true)->value() : "";
  String idVal     = request->hasParam("id_param", true)     ? request->getParam("id_param", true)->value()     : "";

  if ((keyVal.length() != 0) ||
      (secretVal.length() != 0) ||
      (idVal.length() != 0)) { newDataFlag = true; }

  saveSecretIfProvided("sec_key", keyVal);
  saveSecretIfProvided("sec_secret", secretVal);
  saveSecretIfProvided("sec_id", idVal);

  // If a save is made, forces weather featch in next loop
  nextWeatherEpoch = 0;

  AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
  response->addHeader("Location", "/");
  request->send(response);

  loadParams();
}

void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
}

bool onPowerState(const String &deviceId, bool &state) {
  switchStatus = state;
  digitalWrite(RELAY_OUTPUT, switchStatus);
  digitalWrite(SW_LEDB, switchStatus ? HIGH : LOW);
  digitalWrite(SW_LEDR, switchStatus ? LOW  : HIGH);
  drawLight(switchStatus);
  return true; // request handled properly
}

void setupSinricPro() {
  SinricProSwitch& mySwitch = SinricPro[getSecret("sec_id")];

  mySwitch.onPowerState(onPowerState);

  SinricPro.onConnected([](){ sinricState = true; }); 
  SinricPro.onDisconnected([](){ sinricState = false; });
  SinricPro.restoreDeviceStates(true);
  SinricPro.begin(getSecret("sec_key"), getSecret("sec_secret"));
}

void setup() {
  delay(5000);
  loadParams();
  // Serial.begin(115200);
  // while(!Serial) delay(10);

  canvasOffline.setRotation(1);
  canvasOnline.setRotation(1);
  drawOfflineScreen();
  drawOnlineScreen();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalBlocking(false);
  wm.setDarkMode(true);
  wm.setDebugOutput(false);

  pinMode(TOUCH_BUTTON, INPUT);
  pinMode(RELAY_OUTPUT, OUTPUT);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(SW_LEDB, OUTPUT);
  pinMode(SW_LEDR, OUTPUT);
  digitalWrite(RELAY_OUTPUT, switchStatus);
  digitalWrite(SW_LEDB, switchStatus ? HIGH : LOW);
  digitalWrite(SW_LEDR, switchStatus ? LOW  : HIGH);
  digitalWrite(WIFI_LED, wifiLedState);
  touch.setDebounceTime(50);
  touch.setLongPressTime(5000);
  attachInterrupt(TOUCH_BUTTON, touchISR, RISING);

  epaper.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS, EPD_BUSY, EPD_RST, EPD_DC);
  epaper.screenWhite(1);

  if (wm.getWiFiIsSaved()) {
    bool connected = wm.autoConnect(AP_NAME);
    if (connected) {
      // Serial.print("WiFi conectado. IP: ");
      // Serial.println(WiFi.localIP());
      canvasOnline.setTextSize(1);
      canvasOnline.setCursor(10,10);
      canvasOnline.print(WiFi.localIP());
    } else {
      wm.stopWebPortal();
      // Serial.println("Connection timeout");
    }
    epaper.screenShow(canvasOnline.getBuffer(), 1);
  } else {
    drawLight(switchStatus);
    epaper.screenShow(canvasOffline.getBuffer(), 1);
  }

  if (WiFi.status() == WL_CONNECTED){
    setupWebServer();
    initTimeSync();
    updateWeatherIfDue();

    if (p_key_set) {
      setupSinricPro();
    }
  }
  setupWatchdog();
}

void loop() {
  static bool lastWiFiState = (WiFi.status() == WL_CONNECTED);

  bool wiFiState = (WiFi.status() == WL_CONNECTED);
  if (wiFiState != lastWiFiState) {
    updateFlag = true;
    updateCounts = 4;
    lastWiFiState = wiFiState;
  }

  wm.process();
  wifiSearching = wm.getConfigPortalActive();

  unsigned long now = millis();
  if (wifiSearching) {
    if (now - lastBlink >= 2000){
      lastBlink = now;
      wifiLedState = !wifiLedState;
    }
    if (now - lastWifiSearch >= WIFI_SEARCH_TIMEOUT ){
      lastWifiSearch = now;
      connectionAttempt = false;
      wm.stopWebPortal();
      wifiLedState = LOW;
      drawOfflineScreen();
      drawLight(switchStatus);
      updateFlag = true;
      updateCounts = 4;
    }
    digitalWrite(WIFI_LED, wifiLedState);
  }

  if(touchFlag) {
    touch.update();
  }

  if (touch.wasTapped()) {
    switchStatus = !switchStatus;
    digitalWrite(RELAY_OUTPUT, switchStatus);
    digitalWrite(SW_LEDB, switchStatus ? HIGH : LOW);
    digitalWrite(SW_LEDR, switchStatus ? LOW  : HIGH);

    if (p_key_set) {
      SinricProSwitch& mySwitch = SinricPro[getSecret("sec_id")];

      const int sinricAttempts = 3;
      for (int attempt = 0; attempt < sinricAttempts; attempt++) {
        esp_task_wdt_reset();
        if (mySwitch.sendPowerStateEvent(switchStatus)) { break; }
      }
    }

    touchFlag = false;
    if (WiFi.status() != WL_CONNECTED) {
      updateFlag = true;
    }
    if (!wifiSearching) {
      drawLight(switchStatus);
    }
  }

  if (p_key_set) {
    SinricPro.handle();
  }

  if (touch.wasLongPress()) {
    touchFlag = false;
    if (wm.getWiFiIsSaved()) {
      wm.resetSettings();
      if (!switchStatus) {
        clearAllParams();
      }
      ESP.restart();
    } else if (!wifiSearching){
      connectionAttempt = true;
      wm.autoConnect(AP_NAME);
      lastWifiSearch = millis();
      lastBlink = millis();
      wifiLedState = HIGH;
      drawQR(DEVICE_LANGUAGE);
      updateFlag = true;
      updateCounts = 4;
    } else {
      connectionAttempt = false;
      wifiLedState = LOW;
      digitalWrite(WIFI_LED, wifiLedState);
      wm.stopWebPortal();
      drawOfflineScreen();
      drawLight(switchStatus);
      updateFlag = true;
      updateCounts = 4;
    }
  }

  esp_task_wdt_reset();

  if (WiFi.status() == WL_CONNECTED) {
    updateWeatherIfDue();
  }

  if (weatherUpdate){
    weatherUpdate = false;
    updateFlag = true;
  }

  if (connectionAttempt && (WiFi.status() == WL_CONNECTED) && !wifiSearching){
    ESP.restart();
  }

  if (newDataFlag) {
    ESP.restart();
  }

  if (updateFlag) {
    if (WiFi.status() == WL_CONNECTED) {
      if (updateCounts < 4) { 
        epaper.screenShowPart(canvasOnline.getBuffer());
        updateCounts++;
      } else {
        epaper.screenShow(canvasOnline.getBuffer(), 1);
        updateCounts = 0;
      }  
    } else {
      if (updateCounts < 4) {
        epaper.screenShowPart(canvasOffline.getBuffer());
        updateCounts++;
      } else {
        epaper.screenShow(canvasOffline.getBuffer(), 1);
        updateCounts = 0;
      }
    }
    updateFlag = false;
  }
}
