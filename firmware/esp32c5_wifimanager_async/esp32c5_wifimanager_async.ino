
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TouchSensorLib.h>

#define TOUCH_BUTTON 9
#define WIFI_LED 8
#define SW_LEDB 11
#define SW_LEDR 12
#define AP_NAME "IoTSwitch_21098cf54"

TouchSensor touch(TOUCH_BUTTON);
WiFiManager wm;
AsyncWebServer server(80);
Preferences prefs;

bool wifiSearching = false;
bool switchStatus = false;
unsigned long lastBlink = 0;
unsigned long lastWifiSearch = 0;
bool wifiLedState = LOW;
volatile bool touchFlag = false;
bool connectionAttempt = false;

const char* PREF_NAMESPACE = "cfgdata";

// ---------- Ubicacion (persistida en NVS, SI se muestra en la pagina) ----------
String p_location_name = "";
double p_lat = 0.0;
double p_lon = 0.0;
bool   p_loc_set = false;

// ---------- Secrets (persistidos en NVS, NUNCA se muestran, solo su existencia) ----------
bool p_key_set    = false;
bool p_secret_set = false;
bool p_id_set     = false;

// ---------- Clima (SOLO en RAM, nunca en NVS, nunca en la pagina) ----------
float  g_temperature  = NAN;
int    g_weathercode  = -1;
String g_localTime    = "";   // ej "2026-07-06T14:30"
String g_localDay     = "";   // ej "2026-07-06"
bool   g_weatherValid = false;

const long WEATHER_INTERVAL_SEC = 5L * 60L; // 30 minutos, en segundos de reloj real
time_t nextWeatherEpoch = 0;   // proximo instante (epoch UTC) en que toca refrescar clima
bool   g_timeSynced = false;   // true una vez que NTP entrego una hora valida

void ARDUINO_ISR_ATTR touchISR() {
  touch.update();
  touchFlag = true;
}

// ================= Persistencia =================

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

// Solo sobrescribe el secreto si el usuario mando un valor no vacio.
// Dejar el campo en blanco en el formulario = mantener el valor anterior.
void saveSecretIfProvided(const char* nvsKey, const String& value) {
  if (value.length() == 0) return;
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString(nvsKey, value);
  prefs.end();
}

// Lectura real de un secreto (uso interno del programa, ej. para
// autenticarse contra un servicio; NUNCA se expone via HTTP).
String getSecret(const char* nvsKey) {
  prefs.begin(PREF_NAMESPACE, true);
  String v = prefs.getString(nvsKey, "");
  prefs.end();
  return v;
}

// ================= Utilidades HTML =================

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// ================= Open-Meteo (clima) =================

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

// Llamada BLOQUEANTE (HTTPClient sincrono). Se ejecuta solo cada 30 min
// desde loop(), y no afecta al servidor web porque ESPAsyncWebServer
// atiende peticiones fuera del hilo/loop principal.
bool fetchWeatherBlocking(double lat, double lon) {
  if (WiFi.status() != WL_CONNECTED) return false;

  const int maxIntentos = 3;
  for (int intento = 0; intento < maxIntentos; intento++) {
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

      Serial.printf("[clima] %.1f C, codigo %d (%s), hora local %s\n",
                    g_temperature, g_weathercode,
                    weatherCodeToText(g_weathercode).c_str(), g_localTime.c_str());
      return true;
    }
    Serial.printf("httpCode=%d (%s) | heap libre=%u | heap contiguo max=%u\n",
                  httpCode, HTTPClient::errorToString(httpCode).c_str(),
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return false;
}

// ================= Sincronizacion de hora real (NTP) =================
//
// Por que NTP en vez de millis():
//  - millis() cuenta desde el arranque del ESP32. Si el chip se reinicia,
//    el refresco "cada 30 min" queda desfasado respecto al reloj de pared
//    (no cae en horas redondas como 10:00, 10:30, 11:00...).
//  - Con NTP obtenemos el epoch UTC real y alineamos el refresco a los
//    multiplos exactos de 30 minutos del reloj, sin importar cuando
//    arranco el dispositivo.
//  - Si el ESP32 se reinicia a las 10:57, con NTP el siguiente refresco
//    cae de inmediato en el 11:00 (el proximo limite real), en vez de
//    esperar 30 minutos completos desde el reinicio.

void initTimeSync() {
  // gmtOffset=0, daylightOffset=0: trabajamos en UTC puro. No hace falta
  // conocer la zona horaria del lugar para alinear a :00/:30 de reloj,
  // porque casi todas las zonas horarias tienen un offset multiplo de
  // 30 minutos respecto a UTC, asi que los limites de media hora en UTC
  // coinciden con los limites de media hora en hora local.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  Serial.print("Sincronizando hora via NTP");
  time_t now = time(nullptr);
  uint8_t intentos = 0;
  // Antes de sincronizar, time() devuelve una fecha cercana a 1970.
  // Esperamos (con limite) a que supere una fecha razonable (2023+).
  while (now < 1700000000 && intentos < 20) {
    delay(250);
    Serial.print(".");
    now = time(nullptr);
    intentos++;
  }
  Serial.println();

  g_timeSynced = (now >= 1700000000);
  if (g_timeSynced) {
    Serial.printf("Hora sincronizada (epoch UTC: %ld)\n", (long)now);
  } else {
    Serial.println("No se pudo sincronizar la hora por NTP todavia; se reintentara en loop().");
  }
}

// Calcula el proximo limite de media hora (:00 o :30) a partir de "now".
time_t nextHalfHourBoundary(time_t now) {
  return ((now / WEATHER_INTERVAL_SEC) + 1) * WEATHER_INTERVAL_SEC;
}

// Se llama en cada loop(); decide si toca refrescar segun la hora real.
void updateWeatherIfDue() {
  loadParams();
  if (!p_loc_set) return; // sin ubicacion configurada, no hay nada que consultar

  // Si NTP aun no sincronizo (ej. justo tras el boot), reintenta aqui
  // sin bloquear demasiado tiempo el loop.
  if (!g_timeSynced) {
    initTimeSync();
    if (!g_timeSynced) return; // lo intentara de nuevo en la siguiente vuelta de loop()
  }

  time_t now = time(nullptr);

  // Primera vez tras el boot (o tras cambiar de ubicacion): programa el
  // proximo limite y ademas dispara una lectura inmediata para no esperar
  // hasta el siguiente :00/:30.
  if (nextWeatherEpoch == 0) {
    fetchWeatherBlocking(p_lat, p_lon);
    nextWeatherEpoch = nextHalfHourBoundary(now);
    return;
  }

  if (now >= nextWeatherEpoch) {
    fetchWeatherBlocking(p_lat, p_lon);
    // Se reprograma a partir de "now" real, no sumando ciegamente 1800s,
    // para no acumular desfase si el fetch tardo varios segundos.
    nextWeatherEpoch = nextHalfHourBoundary(now);
  }
}

// ================= Pagina de configuracion =================

String buildConfigPage() {
  loadParams();

  String locNameEsc = htmlEscape(p_location_name);
  String locDisplay = p_loc_set
      ? (locNameEsc + " (" + String(p_lat, 4) + ", " + String(p_lon, 4) + ")")
      : "&lt;blank&gt;";

  String keyDisplay    = p_key_set    ? "guardado" : "&lt;blank&gt;";
  String secretDisplay = p_secret_set ? "guardado" : "&lt;blank&gt;";
  String idDisplay     = p_id_set     ? "guardado" : "&lt;blank&gt;";

  String keyPlaceholder    = p_key_set    ? "(ya configurado, dejar en blanco para no cambiar)" : "";
  String secretPlaceholder = p_secret_set ? "(ya configurado, dejar en blanco para no cambiar)" : "";
  String idPlaceholder     = p_id_set     ? "(ya configurado, dejar en blanco para no cambiar)" : "";

  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Configuracion del dispositivo</title>";
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

  html += "<h2>Configuracion del dispositivo</h2>";
  html += "<p>IP: " + WiFi.localIP().toString() + "</p>";

  // ---- Tabla resumen ----
  html += "<h3>Valores guardados actualmente</h3>";
  html += "<table><tr><th>Parametro</th><th>Valor</th></tr>";
  html += "<tr><td>Ubicacion</td><td>" + locDisplay + "</td></tr>";
  html += "<tr><td>Key</td><td>" + keyDisplay + "</td></tr>";
  html += "<tr><td>Secret</td><td>" + secretDisplay + "</td></tr>";
  html += "<tr><td>ID</td><td>" + idDisplay + "</td></tr>";
  html += "</table>";

  // ---- Formulario ----
  html += "<h3>Editar configuracion</h3>";
  html += "<form id='cfgForm' action='/save' method='POST'>";

  html += "<label for='loc_search'>Ubicacion (escribe para buscar)</label>";
  html += "<input type='text' id='loc_search' autocomplete='off' placeholder='Ej: Trujillo' value='" + locNameEsc + "'>";
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

  html += "<small>Deja Key/Secret/ID en blanco si no quieres cambiar el valor ya guardado.</small>";

  html += "<button type='submit'>Guardar</button>";
  html += "</form>";

  // ---- JS: autocompletado de ubicacion contra Open-Meteo Geocoding API ----
  html += "<script>";
  html += "let debounceTimer;";
  html += "const searchBox = document.getElementById('loc_search');";
  html += "const suggBox = document.getElementById('suggestions');";
  html += "searchBox.addEventListener('input', function() {";
  html += "  clearTimeout(debounceTimer);";
  html += "  const q = this.value.trim();";
  html += "  if (q.length < 2) { suggBox.innerHTML=''; return; }";
  html += "  debounceTimer = setTimeout(() => buscarLugar(q), 350);";
  html += "});";
  html += "async function buscarLugar(q) {";
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
  html += "      div.onclick = () => seleccionarLugar(r);";
  html += "      suggBox.appendChild(div);";
  html += "    });";
  html += "  } catch (e) { console.error('Error buscando lugar', e); }";
  html += "}";
  html += "function seleccionarLugar(r) {";
  html += "  const admin = r.admin1 ? (r.admin1 + ', ') : '';";
  html += "  const nombreCompleto = r.name + ', ' + admin + r.country;";
  html += "  searchBox.value = nombreCompleto;";
  html += "  document.getElementById('location_name').value = nombreCompleto;";
  html += "  document.getElementById('latitude').value = r.latitude;";
  html += "  document.getElementById('longitude').value = r.longitude;";
  html += "  suggBox.innerHTML = '';";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  return html;
}

// ================= Handlers (ESPAsyncWebServer) =================

void handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/html; charset=utf-8", buildConfigPage());
}

void handleSave(AsyncWebServerRequest *request) {
  if (!request->hasParam("location_name", true) ||
      !request->hasParam("latitude", true) ||
      !request->hasParam("longitude", true)) {
    request->send(400, "text/plain", "Faltan parametros de ubicacion");
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

  saveSecretIfProvided("sec_key", keyVal);
  saveSecretIfProvided("sec_secret", secretVal);
  saveSecretIfProvided("sec_id", idVal);

  // Fuerza un refresco de clima inmediato en el siguiente loop() si cambio
  // la ubicacion (updateWeatherIfDue vuelve a disparar fetch + reprograma
  // el proximo limite de media hora cuando nextWeatherEpoch queda en 0).
  nextWeatherEpoch = 0;

  AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
  response->addHeader("Location", "/");
  request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "No encontrado");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor web asincrono iniciado en el puerto 80");
}

// ================= Setup / Loop =================

void setup() {
  Serial.begin(115200);
  while(!Serial) delay(10);
  delay(300);

  pinMode(TOUCH_BUTTON, INPUT);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(SW_LEDB, OUTPUT);
  pinMode(SW_LEDR, OUTPUT);
  digitalWrite(SW_LEDB, switchStatus ? HIGH : LOW);
  digitalWrite(SW_LEDR, switchStatus ? LOW  : HIGH);
  digitalWrite(WIFI_LED, wifiLedState);
  touch.setDebounceTime(50);
  touch.setLongPressTime(5000);
  attachInterrupt(TOUCH_BUTTON, touchISR, RISING);

  WiFi.mode(WIFI_STA);
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(10);
  wm.setDarkMode(true);

  if (wm.getWiFiIsSaved()) {
    bool connected = wm.autoConnect(AP_NAME);
    if (connected) {
      Serial.print("WiFi conectado. IP: ");
      Serial.println(WiFi.localIP());
    } else {
      wm.stopWebPortal();
      Serial.println("Connection timeout");
    }
  }

  if (WiFi.status() == WL_CONNECTED){
    setupWebServer();
    // Sincroniza la hora real (NTP) antes de empezar a programar el clima.
    // Es una espera acotada (maximo ~5s) y solo ocurre una vez al arrancar.
    initTimeSync();
    // Primer intento de clima inmediato si ya hay ubicacion guardada
    updateWeatherIfDue();
  }
}

void loop() {
  if (connectionAttempt && (WiFi.status() == WL_CONNECTED) && !wifiSearching){
    ESP.restart();
  }

  wm.process();
  wifiSearching = wm.getConfigPortalActive();

  unsigned long nowMillis = millis();
  if (wifiSearching) {
    if (nowMillis - lastBlink >= 2000){
      lastBlink = nowMillis;
      wifiLedState = !wifiLedState;
    }
    if (nowMillis - lastWifiSearch >= 180000){
      lastWifiSearch = nowMillis;
      connectionAttempt = false;
      wm.stopWebPortal();
      wifiLedState = LOW;
    }
  } else {
    wifiLedState = LOW;
  }
  digitalWrite(WIFI_LED, wifiLedState);

  if(touchFlag) {
    touch.update();
  }

  if (touch.wasTapped()) {
    Serial.println(nextWeatherEpoch);
    switchStatus = !switchStatus;
    digitalWrite(SW_LEDB, switchStatus ? HIGH : LOW);
    digitalWrite(SW_LEDR, switchStatus ? LOW  : HIGH);
    touchFlag = false;
  }

  if (touch.wasLongPress()) {
    touchFlag = false;
    if (wm.getWiFiIsSaved()) {
      wm.resetSettings();
      ESP.restart();
    } else if (!wifiSearching){
      connectionAttempt = true;
      wm.autoConnect(AP_NAME);
      lastWifiSearch = millis();
      lastBlink = millis();
      wifiLedState = HIGH;
    } else {
      connectionAttempt = false;
      wm.stopWebPortal();
    }
  }

  if (WiFi.status() == WL_CONNECTED){
    // No hay server.handleClient(): ESPAsyncWebServer atiende peticiones
    // por su cuenta via AsyncTCP. loop() queda libre para tareas propias,
    // incluida esta consulta de clima cada 30 minutos.
    updateWeatherIfDue();
  }
}
