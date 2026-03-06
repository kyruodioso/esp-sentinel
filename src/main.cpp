/**
 * Sentinel Node Firmware v3.0 - Actuator Support
 * Architecture: ESP8266 | Board: NodeMCU v2
 *
 * Changelog v3.0:
 *  - Actuator support (electrovalves, relays) with safety auto-shutoff timer
 *  - MQTT command subscriber: sentinel/v1/cmd/<token>
 *    Commands: {"action":"irrigate","duration":15} | {"action":"stop"} |
 * {"action":"toggle","actuator_id":"VALVE_01"}
 *  - Actuator status reports published to sentinel/v1/status/<token>
 *  - Web UI for actuator management
 *  - LittleFS persistence for actuator config
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <DNSServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <vector>

void reportDiscoveryHTTP();

// --- CONFIGURATION ---
#define MQTT_BROKER "broker.hivemq.com"
#define MQTT_PORT 1883
#define DATA_TOPIC_PREFIX "sentinel/v1/data/"
#define CMD_TOPIC_PREFIX "sentinel/v1/cmd/"
#define STATUS_TOPIC_PREFIX "sentinel/v1/status/"
#define CONFIG_FILE "/config.json"
#define SENSORS_FILE "/sensors.json"
#define ACTUATORS_FILE "/actuators.json"
#define MEASUREMENT_INTERVAL 30000
#define STATUS_INTERVAL 15000 // Publicar estado de actuadores cada 15s

// --- ACTUATOR TYPES ---
enum ActuatorType {
  ACT_RELAY_NO, // Relay Normally Open  (HIGH = activado)
  ACT_RELAY_NC, // Relay Normally Closed (LOW = activado)
  ACT_DIGITAL   // Digital output genérico (HIGH = activado)
};

// --- SENSOR TYPES ---
enum SensorType {
  TYPE_DHT11_TEMP,
  TYPE_DHT11_HUM,
  TYPE_DHT22_TEMP,
  TYPE_DHT22_HUM,
  TYPE_ANALOG
};

// --- DATA STRUCTURES ---
struct Sensor {
  String id;
  int pin;
  String unit;
  SensorType type;
};

struct Actuator {
  String id; // Ej: "VALVE_01", "PUMP_01"
  int pin;   // GPIO físico
  ActuatorType type;
  bool state; // true = activo/encendido
  unsigned long
      timer_ms; // 0 = sin timer. >0 = apagar en timer_ms ms desde activación
  unsigned long activated_at; // millis() cuando se activó
};

// --- GLOBAL VARIABLES ---
std::vector<Sensor> sensors;
std::vector<Actuator> actuators;

char sentinel_token[40] = "";
char device_name[40] = "Nodo_Sentinel";
char mqtt_host[64] = "broker.hivemq.com";
char mqtt_port_str[6] = "1883";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";
char api_url[128] = "http://200.58.98.50/factory/api/v1/iot/ingest/";
char www_user[32] = "admin";
char www_pass[32] = "sentinel123";

// --- Tópicos Discovery ---
#define DISCOVERY_TOPIC_PREFIX "sentinel/v1/discovery/"

bool shouldSaveConfig =
    false; // Flag para indicar que la configuración debe guardarse

unsigned long lastMsg = 0;
unsigned long lastStatus = 0;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer server(80);
DHTesp dht;

// --- PIN MAPPING ---
int getGpio(String pinStr) {
  if (pinStr == "A0")
    return A0;
  if (pinStr == "D0")
    return 16;
  if (pinStr == "D1")
    return 5;
  if (pinStr == "D2")
    return 4;
  if (pinStr == "D3")
    return 0;
  if (pinStr == "D4")
    return 2;
  if (pinStr == "D5")
    return 14;
  if (pinStr == "D6")
    return 12;
  if (pinStr == "D7")
    return 13;
  if (pinStr == "D8")
    return 15;
  return pinStr.toInt();
}

// ============================================================
// ACTUATOR LOGIC
// ============================================================

/** Activa un actuador. Si duration_ms > 0 configura apagado automático. */
void activateActuator(Actuator &a, unsigned long duration_ms = 0) {
  a.state = true;
  a.timer_ms = duration_ms;
  a.activated_at = millis();

  // Para relay NC, LOW activa; para NO y DIGITAL, HIGH activa
  int writeVal = (a.type == ACT_RELAY_NC) ? LOW : HIGH;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, writeVal);

  Serial.print("✅ Actuator ON: ");
  Serial.print(a.id);
  if (duration_ms > 0) {
    Serial.print(" | Auto-OFF in ");
    Serial.print(duration_ms / 1000);
    Serial.println("s");
  } else {
    Serial.println(" (indefinite)");
  }
}

/** Desactiva un actuador inmediatamente. */
void deactivateActuator(Actuator &a) {
  a.state = false;
  a.timer_ms = 0;

  int writeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, writeVal);

  Serial.print("⛔ Actuator OFF: ");
  Serial.println(a.id);
}

/** Inicializa todos los pines de actuadores en estado seguro (apagado). */
void initActuators() {
  for (auto &a : actuators) {
    a.state = false;
    a.timer_ms = 0;
    // Relay NC → HIGH = apagado. Relay NO / DIGITAL → LOW = apagado
    int safeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
    pinMode(a.pin, OUTPUT);
    digitalWrite(a.pin, safeVal);
  }
}

/**
 * Safety Watchdog: apaga automáticamente actuadores con timer vencido.
 * Se llama en cada iteración de loop().
 */
void checkActuatorTimers() {
  for (auto &a : actuators) {
    if (a.state && a.timer_ms > 0) {
      if (millis() - a.activated_at >= a.timer_ms) {
        Serial.print("⏰ Auto-shutoff: ");
        Serial.println(a.id);
        deactivateActuator(a);
      }
    }
  }
}

// ============================================================
// PERSISTENCE
// ============================================================

void loadConfig() {
  if (!LittleFS.begin())
    return;

  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    strcpy(sentinel_token, doc["token"] | "");
    strcpy(device_name, doc["name"] | "Nodo_Sentinel"); // Load device_name
    strcpy(mqtt_host, doc["mqtt_host"] | "broker.hivemq.com");
    strcpy(mqtt_port_str, doc["mqtt_port"] | "1883");
    strcpy(mqtt_user, doc["mqtt_user"] | "");
    strcpy(mqtt_pass, doc["mqtt_pass"] | "");
    strcpy(www_user, doc["www_user"] | "admin");
    strcpy(www_pass, doc["www_pass"] | "sentinel123");
    f.close();
  }

  if (LittleFS.exists(SENSORS_FILE)) {
    File f = LittleFS.open(SENSORS_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    JsonArray arr = doc.as<JsonArray>();
    sensors.clear();
    for (JsonObject obj : arr) {
      sensors.push_back({obj["id"].as<String>(), obj["pin"].as<int>(),
                         obj["unit"].as<String>(),
                         (SensorType)obj["type"].as<int>()});
    }
    f.close();
  }

  if (LittleFS.exists(ACTUATORS_FILE)) {
    File f = LittleFS.open(ACTUATORS_FILE, "r");
    JsonDocument doc;
    deserializeJson(doc, f);
    JsonArray arr = doc.as<JsonArray>();
    actuators.clear();
    for (JsonObject obj : arr) {
      Actuator a;
      a.id = obj["id"].as<String>();
      a.pin = obj["pin"].as<int>();
      a.type = (ActuatorType)obj["type"].as<int>();
      a.state = false;
      a.timer_ms = 0;
      a.activated_at = 0;
      actuators.push_back(a);
    }
    f.close();
  }
}

void saveActuators() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = a.id;
    obj["pin"] = a.pin;
    obj["type"] = (int)a.type;
    obj["state"] = a.state; // Guardar estado actual
  }
  File f = LittleFS.open(ACTUATORS_FILE, "w");
  serializeJson(doc, f);
  f.close();
}

void saveSensors() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &s : sensors) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = s.id;
    obj["pin"] = s.pin;
    obj["unit"] = s.unit;
    obj["type"] = (int)s.type;
  }
  File f = LittleFS.open(SENSORS_FILE, "w");
  serializeJson(doc, f);
  f.close();
}

void saveConfig() {
  JsonDocument doc;
  doc["token"] = sentinel_token;
  doc["name"] = device_name; // Save device_name
  doc["mqtt_host"] = mqtt_host;
  doc["mqtt_port"] = mqtt_port_str;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_pass"] = mqtt_pass;
  doc["www_user"] = www_user;
  doc["www_pass"] = www_pass;
  File f = LittleFS.open(CONFIG_FILE, "w");
  serializeJson(doc, f);
  f.close();
}

// Callback para WiFiManager para indicar que la configuración debe guardarse
void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// ============================================================
// SENSOR READING
// ============================================================

float readSensorValue(const Sensor &s) {
  static int lastPin = -1;
  static unsigned long lastReadTime = 0;
  static float lastTemp = NAN;
  static float lastHum = NAN;

  if (s.pin != A0 && s.pin == lastPin && (millis() - lastReadTime < 2000)) {
    if (s.type == TYPE_DHT11_TEMP || s.type == TYPE_DHT22_TEMP)
      return lastTemp;
    if (s.type == TYPE_DHT11_HUM || s.type == TYPE_DHT22_HUM)
      return lastHum;
  }

  float val = NAN;
  if (s.type == TYPE_DHT11_TEMP || s.type == TYPE_DHT11_HUM) {
    dht.setup(s.pin, DHTesp::DHT11);
    delay(100);
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE) {
      lastTemp = th.temperature;
      lastHum = th.humidity;
      lastPin = s.pin;
      lastReadTime = millis();
    }
    val = (s.type == TYPE_DHT11_TEMP) ? lastTemp : lastHum;
  } else if (s.type == TYPE_DHT22_TEMP || s.type == TYPE_DHT22_HUM) {
    dht.setup(s.pin, DHTesp::DHT22);
    delay(100);
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE) {
      lastTemp = th.temperature;
      lastHum = th.humidity;
      lastPin = s.pin;
      lastReadTime = millis();
    }
    val = (s.type == TYPE_DHT22_TEMP) ? lastTemp : lastHum;
  } else if (s.type == TYPE_ANALOG) {
    val = analogRead(s.pin);
  }
  return val;
}

// ============================================================
// MQTT — COMMAND HANDLER
// ============================================================

/**
 * Procesa comandos recibidos en sentinel/v1/cmd/<token>
 *
 * Formatos soportados:
 *  {"action":"irrigate","duration":15}          → todos los actuadores (15 min)
 *  {"action":"irrigate","duration":10,"actuator_id":"VALVE_01"} → solo VALVE_01
 *  {"action":"stop"}                             → detiene todos
 *  {"action":"stop","actuator_id":"VALVE_01"}   → detiene solo VALVE_01
 *  {"action":"toggle","actuator_id":"PUMP_01"}  → invierte estado de PUMP_01
 *  {"action":"status"}                           → publica estado actual
 */
void handleCommand(const char *payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("❌ Invalid command JSON");
    return;
  }

  String action = doc["action"].as<String>();
  String actuator_id = doc["actuator_id"] | "";
  int duration = doc["duration"] | 0; // minutos
  unsigned long duration_ms = (unsigned long)duration * 60UL * 1000UL;

  Serial.print("🎮 CMD: action=");
  Serial.print(action);
  if (actuator_id.length()) {
    Serial.print(" actuator=");
    Serial.print(actuator_id);
  }
  if (duration) {
    Serial.print(" duration=");
    Serial.print(duration);
    Serial.print("min");
  }
  Serial.println();

  if (action == "irrigate" || action == "on") {
    for (auto &a : actuators) {
      if (actuator_id.length() == 0 || a.id == actuator_id) {
        activateActuator(a, duration_ms);
      }
    }

  } else if (action == "stop" || action == "off") {
    for (auto &a : actuators) {
      if (actuator_id.length() == 0 || a.id == actuator_id) {
        deactivateActuator(a);
      }
    }

  } else if (action == "toggle") {
    for (auto &a : actuators) {
      if (a.id == actuator_id) {
        if (a.state)
          deactivateActuator(a);
        else
          activateActuator(a, duration_ms);
      }
    }

  } else if (action == "status") {
    // Responder con un publish de estado (se hará en publishStatus)
    lastStatus = 0; // Forzar publicación inmediata

  } else {
    Serial.print("⚠️ Unknown action: ");
    Serial.println(action);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  payload[length] = '\0';
  String topicStr = String(topic);
  Serial.print("📥 MQTT ← [");
  Serial.print(topicStr);
  Serial.print("] ");
  Serial.println((char *)payload);

  // Solo procesar si el tópico es de comandos para este token
  String cmdTopic = String(CMD_TOPIC_PREFIX) + String(sentinel_token);
  String discTopic = String(DISCOVERY_TOPIC_PREFIX) + String(device_name);

  if (topicStr == cmdTopic) {
    handleCommand((char *)payload);
  } else if (topicStr == discTopic) {
    // Autoprovisionamiento: {"token": "sentinel_xxxx"}
    JsonDocument doc;
    deserializeJson(doc, payload);
    if (doc.containsKey("token")) {
      const char *newToken = doc["token"];
      if (strlen(newToken) > 5) {
        strncpy(sentinel_token, newToken, 39);
        Serial.printf("✨ Token provisioned via MQTT: %s\n", sentinel_token);
        saveConfig();
        mqttClient.disconnect(); // Reconectar con el nuevo token
      }
    }
  }
}

// ============================================================
// MQTT — PUBLISH
// ============================================================

/** Publica estado de todos los actuadores en sentinel/v1/status/<token> */
void publishStatus() {
  if (strlen(sentinel_token) < 5)
    return;

  JsonDocument doc;
  doc["device_id"] = device_name;
  doc["token"] = sentinel_token;
  JsonArray acts = doc["actuators"].to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = acts.add<JsonObject>();
    obj["id"] = a.id;
    obj["state"] = a.state;
    if (a.state && a.timer_ms > 0) {
      unsigned long elapsed = millis() - a.activated_at;
      obj["remaining_s"] =
          (a.timer_ms > elapsed) ? (a.timer_ms - elapsed) / 1000 : 0;
    }
  }

  char buffer[512];
  serializeJson(doc, buffer);
  char topic[100];
  snprintf(topic, sizeof(topic), "%s%s", STATUS_TOPIC_PREFIX, sentinel_token);
  mqttClient.publish(topic, buffer);
}

/** Publica lecturas de sensores y estado de actuadores */
void collectAndPublish() {
  // Para autodescubrimiento: si no hay token, enviamos igual para que el
  // backend nos vea pero el topic será especial.

  JsonDocument doc;
  doc["token"] = sentinel_token;
  doc["token"] = sentinel_token;
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String unique_id = String(device_name) + "_" + mac.substring(8);
  doc["device_id"] = unique_id;
  doc["ip"] = WiFi.localIP().toString();

  // 1. Lecturas de Sensores
  JsonArray readings = doc["readings"].to<JsonArray>();
  for (const auto &s : sensors) {
    float val = readSensorValue(s);
    if (!isnan(val)) {
      JsonObject r = readings.add<JsonObject>();
      r["sensor_id"] = s.id;
      r["value"] = val;
      r["unit"] = s.unit;
    }
  }

  // 2. Estado de Actuadores (para autodescubrimiento en backend)
  JsonArray acts = doc["actuators"].to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = acts.add<JsonObject>();
    obj["id"] = a.id;
    obj["state"] = a.state;
  }

  char buffer[1536]; // Aumentado para soportar múltiples sensores y actuadores
  serializeJson(doc, buffer);

  char topic[120];
  if (strlen(sentinel_token) >= 5) {
    snprintf(topic, sizeof(topic), "%s%s", DATA_TOPIC_PREFIX, sentinel_token);
  } else {
    // Usar MAC para evitar colisiones en el descubrimiento
    String mac_suffix = WiFi.macAddress();
    mac_suffix.replace(":", "");
    snprintf(topic, sizeof(topic), "%ssynergy_discovery/%s_%s",
             DATA_TOPIC_PREFIX, device_name, mac_suffix.substring(8).c_str());
  }
  mqttClient.publish(topic, buffer);
  Serial.println("📤 Sensor & Actuator data published.");

  // Si no hay token, enviar reporte por HTTP para descubrimiento
  if (strlen(sentinel_token) < 5) {
    reportDiscoveryHTTP();
  }
}

/** Reporta al servidor vía HTTP para asegurar que se capture la IP pública */
void reportDiscoveryHTTP() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  HTTPClient http;

  http.begin(client, api_url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["token"] = "discovery";
  // Usar el nombre con MAC para asegurar que sea único en el descubrimiento
  doc["token"] = sentinel_token;
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String unique_id = String(device_name) + "_" + mac.substring(8);
  doc["device_id"] = unique_id;
  doc["ip"] = WiFi.localIP().toString();

  // Enviar sensores y actuadores para que el backend los conozca desde el
  // inicio
  JsonArray readings = doc["readings"].to<JsonArray>();
  for (const auto &s : sensors) {
    JsonObject r = readings.add<JsonObject>();
    r["sensor_id"] = s.id;
    r["value"] = 0; // Valor dummy para registro inicial
    r["unit"] = s.unit;
  }

  JsonArray acts = doc["actuators"].to<JsonArray>();
  for (const auto &a : actuators) {
    JsonObject obj = acts.add<JsonObject>();
    obj["id"] = a.id;
    obj["state"] = a.state;
  }

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("🌐 Discovery HTTP [%s] response: %d\n", api_url, httpCode);
  } else {
    Serial.printf("❌ Discovery HTTP failed: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ============================================================
// MQTT — CONNECTION
// ============================================================

void reconnectMQTT() {
  static unsigned long lastRetry = 0;
  if (!mqttClient.connected() && millis() - lastRetry > 10000) {
    lastRetry = millis();
    Serial.print("Connecting MQTT → ");
    Serial.print(mqtt_host);
    Serial.print("...");

    // Si no hay token, usamos el nombre del dispositivo para el ClientID
    bool connected = strlen(mqtt_user) > 0
                         ? mqttClient.connect(device_name, mqtt_user, mqtt_pass)
                         : mqttClient.connect(device_name);

    if (connected) {
      Serial.println(" OK");
      // Suscribirse al tópico de comandos (con token o con nombre si no hay
      // token)
      char cmdTopic[100];
      if (strlen(sentinel_token) >= 5) {
        snprintf(cmdTopic, sizeof(cmdTopic), "%s%s", CMD_TOPIC_PREFIX,
                 sentinel_token);
      } else {
        snprintf(cmdTopic, sizeof(cmdTopic), "%sdiscovery/%s", CMD_TOPIC_PREFIX,
                 device_name);
      }
      mqttClient.subscribe(cmdTopic);
      Serial.print("🎮 Subscribed to: ");
      Serial.println(cmdTopic);

      // Si no tiene token, suscribirse al tópico de autoprovisionamiento
      if (strlen(sentinel_token) < 5) {
        char discTopic[100];
        snprintf(discTopic, sizeof(discTopic), "%s%s", DISCOVERY_TOPIC_PREFIX,
                 device_name);
        mqttClient.subscribe(discTopic);
        Serial.print("🔍 Awaiting provision on: ");
        Serial.println(discTopic);
      }
    } else {
      Serial.print(" FAIL rc=");
      Serial.println(mqttClient.state());
    }
  }
}

// ============================================================
// WEB SERVER
// ============================================================

void handleRoot() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  String html =
      "<html><head><meta name='viewport' "
      "content='width=device-width,initial-scale=1'><meta charset='UTF-8'>";
  html += "<style>";
  html +=
      ":root{--bg:#0f172a;--card:rgba(30,41,59,0.7);--accent:#10b981;--hover:#"
      "059669;--text:#f8fafc;--dim:#94a3b8;--border:rgba(16,185,129,0.15);}";
  html += "body{font-family:'Inter',system-ui,sans-serif;background-color:var(-"
          "-bg);background-image:radial-gradient(at 0% "
          "0%,rgba(16,185,129,0.15) 0,transparent 50%),radial-gradient(at 100% "
          "0%,rgba(59,130,246,0.1) 0,transparent "
          "50%);min-height:100vh;margin:0;padding:20px;color:var(--text);"
          "display:flex;flex-direction:column;align-items:center;}";
  html += ".glass{background:var(--card);backdrop-filter:blur(16px);border:1px "
          "solid var(--border);border-radius:24px;box-shadow:0 10px 40px -10px "
          "rgba(0,0,0,0.5);padding:24px;margin-bottom:24px;width:100%;max-"
          "width:480px;box-sizing:border-box;}";
  html += "h1{font-weight:900;letter-spacing:-0.02em;text-transform:uppercase;"
          "margin:0;font-size:20px;display:flex;align-items:center;justify-"
          "content:center;gap:10px;color:var(--accent);}";
  html += ".dev-name{font-size:12px;color:var(--dim);margin:4px 0 32px "
          "0;letter-spacing:0.1em;text-transform:uppercase;font-weight:700;"
          "opacity:0.8;}";
  html += "h3{margin:0 0 16px "
          "0;font-weight:700;font-size:14px;color:var(--text);letter-spacing:0."
          "05em;text-transform:uppercase;display:flex;justify-content:space-"
          "between;align-items:center;opacity:0.9;}";
  html += ".live-val{background:rgba(16,185,129,0.1);color:var(--accent);"
          "padding:4px "
          "10px;border-radius:8px;font-weight:800;font-size:10px;text-"
          "transform:uppercase;border:1px solid var(--border);}";
  html += ".act-on{background:#065f46;color:#34d399;padding:4px "
          "10px;border-radius:8px;font-weight:800;font-size:10px;border:1px "
          "solid rgba(52,211,153,0.2);}";
  html += ".act-off{background:#451a1a;color:#f87171;padding:4px "
          "10px;border-radius:8px;font-weight:800;font-size:10px;border:1px "
          "solid rgba(248,113,113,0.2);}";
  html += "ul{list-style:none;padding:0;margin:0;}";
  html +=
      "li{background:rgba(15,23,42,0.4);padding:16px;border-radius:16px;margin-"
      "bottom:12px;display:flex;justify-content:space-between;align-items:"
      "center;border:1px solid rgba(255,255,255,0.03);transition:0.2s;}";
  html += "li:hover{border-color:var(--border);background:rgba(15,23,42,0.6);}";
  html += "input,select,textarea{width:100%;padding:14px;margin:8px 0 16px "
          "0;background:rgba(15,23,42,0.5);border:1px solid "
          "var(--border);border-radius:12px;color:var(--text);outline:none;"
          "font-size:14px;transition:0.2s;}";
  html += "input:focus,select:focus{border-color:var(--accent);background:rgba("
          "15,23,42,0.8);box-shadow:0 0 0 3px rgba(16,185,129,0.1);}";
  html += "button{width:100%;padding:14px;border:none;border-radius:12px;"
          "cursor:pointer;font-weight:700;font-size:14px;text-transform:"
          "uppercase;letter-spacing:0.05em;transition:0.2s;display:flex;align-"
          "items:center;justify-content:center;gap:8px;}";
  html += ".btn-add{background:var(--accent);color:var(--bg);box-shadow:0 10px "
          "15px -3px rgba(16,185,129,0.4);}";
  html += ".btn-add:hover{background:var(--hover);transform:translateY(-1px);}";
  html += ".btn-act-on{background:rgba(16,185,129,0.1);color:var(--accent);"
          "border:1px solid var(--border);}";
  html += ".btn-act-on:hover{background:rgba(16,185,129,0.2);}";
  html += ".btn-act-off{background:rgba(239,68,68,0.1);color:#ef4444;border:"
          "1px solid rgba(239,68,68,0.2);}";
  html += ".btn-act-off:hover{background:rgba(239,68,68,0.2);}";
  html += ".btn-token{background:transparent;color:var(--text);border:1px "
          "solid rgba(255,255,255,0.1);}";
  html += ".btn-token:hover{background:rgba(255,255,255,0.05);border-color:"
          "rgba(255,255,255,0.2);}";
  html += ".btn-reboot{background:rgba(59,130,246,0.1);border:1px solid "
          "rgba(59,130,246,0.2);color:#60a5fa;}";
  html += ".btn-wifi{background:rgba(168,85,247,0.1);border:1px solid "
          "rgba(168,85,247,0.2);color:#c084fc;}";
  html += ".btn-del{width:auto;padding:6px "
          "12px;margin:0;background:rgba(239,68,68,0.1);color:#ef4444;font-"
          "size:10px;border-radius:8px;}";
  html += "option{background:#1e293b;color:var(--text);}";
  html += "small{display:block;margin-bottom:6px;color:var(--dim);font-weight:"
          "700;font-size:10px;text-transform:uppercase;letter-spacing:0.05em;}";
  html += "</style>";
  html += "<script>function toggle(id){var "
          "x=document.getElementById(id);x.style.display=(x.style.display==='"
          "block')?'none':'block';}</script>";
  html += "</head><body>";
  html +=
      "<h1><svg width='32' height='32' viewBox='0 0 500 540' fill='none' "
      "xmlns='http://www.w3.org/2000/svg'><defs><linearGradient id='lg' "
      "x1='250' y1='50' x2='250' y2='450' gradientUnits='userSpaceOnUse'><stop "
      "stop-color='#4ADE80'/><stop offset='1' "
      "stop-color='#166534'/></linearGradient><linearGradient id='sg' x1='250' "
      "y1='50' x2='450' y2='250' gradientUnits='userSpaceOnUse'><stop "
      "stop-color='black' stop-opacity='0.2'/><stop offset='1' "
      "stop-color='black' stop-opacity='0'/></linearGradient></defs><path "
      "d='M250 40C180 140 80 240 80 340C80 430 156 503 250 503C344 503 420 430 "
      "420 340C420 240 320 140 250 40Z' fill='url(#lg)'/><path d='M250 40C320 "
      "140 420 240 420 340C420 430 344 503 250 503V40Z' fill='url(#sg)'/><text "
      "x='250' y='385' text-anchor='middle' fill='white' "
      "font-family='sans-serif' font-weight='900' "
      "font-size='240'>S</text></svg> SENTINEL Node</h1><div "
      "class='dev-name'>" +
      String(device_name) + "</div>";

  // ── LIVE SENSOR MONITOR ──
  html += "<div class='glass'><h3>Live Sensors <span "
          "class='live-val'>Real-time</span></h3><ul>";
  if (sensors.empty())
    html += "<p "
            "style='opacity:.4;font-size:12px;text-align:center;padding:20px;'>"
            "No sensors configured.</p>";
  for (size_t i = 0; i < sensors.size(); i++) {
    float val = readSensorValue(sensors[i]);
    String valStr = isnan(val) ? "--" : String(val, 1);
    html += "<li><div><b style='font-size:14px;'>" + sensors[i].id +
            "</b></div>"
            "<div style='display:flex;align-items:center;gap:12px;'><div "
            "style='color:var(--accent);font-weight:900;font-size:18px;'>" +
            valStr +
            " <span style='font-size:12px;color:var(--dim);font-weight:400;'>" +
            sensors[i].unit +
            "</span></div>"
            "<a href='/delete_s?index=" +
            String(i) + "'><button class='btn-del'>×</button></a></div></li>";
  }
  html += "</ul><p "
          "style='opacity:.3;font-size:10px;text-align:center;margin-top:16px;'"
          ">Refreshed automatically on page load</p></div>";

  // ── ACTUATOR STATUS & CONTROL ──
  html += "<div class='glass'><h3>Actuators <span "
          "class='live-val'>Control</span></h3><ul>";
  if (actuators.empty())
    html += "<p "
            "style='opacity:.4;font-size:12px;text-align:center;padding:20px;'>"
            "No actuators configured.</p>";
  for (size_t i = 0; i < actuators.size(); i++) {
    String statusBadge = actuators[i].state
                             ? "<span class='act-on'>ACTIVE</span>"
                             : "<span class='act-off'>INACTIVE</span>";
    String toggleBtn = actuators[i].state
                           ? "<a href='/act?index=" + String(i) +
                                 "&cmd=off'><button class='btn-act-off' "
                                 "style='width:auto;padding:8px "
                                 "16px;font-size:10px;'>OFF</button></a>"
                           : "<a href='/act?index=" + String(i) +
                                 "&cmd=on&dur=15'><button class='btn-act-on' "
                                 "style='width:auto;padding:8px "
                                 "16px;font-size:10px;'>ON 15m</button></a>";
    html += "<li><div><b style='font-size:14px;'>" + actuators[i].id +
            "</b><br><span style='font-size:10px;color:var(--dim);'>GPIO " +
            String(actuators[i].pin) +
            "</span></div>"
            "<div style='display:flex;gap:12px;align-items:center'>" +
            statusBadge + toggleBtn + "<a href='/delete_a?index=" + String(i) +
            "'><button class='btn-del'>×</button></a></div></li>";
  }
  html += "</ul>";

  // Add Actuator Form Toggle
  html += "<button onclick=\"toggle('add-act-form')\" class='btn-token' "
          "style='margin-top:20px;font-size:11px;'>+ Add Actuator</button>";
  html += "<div id='add-act-form' "
          "style='display:none;margin-top:20px;padding-top:20px;border-top:1px "
          "solid var(--border);'>";
  html += "<form action='/add_a' method='GET'>";
  html += "<small>Actuator Identifier</small><input name='id' "
          "placeholder='e.g. VALVE_01'>";
  html += "<small>Hardware Pin (GPIO)</small><select name='pin'>"
          "<option value='D1'>D1 (GPIO5)</option><option value='D2'>D2 "
          "(GPIO4)</option>"
          "<option value='D5'>D5 (GPIO14)</option><option value='D6'>D6 "
          "(GPIO12)</option>"
          "<option value='D7'>D7 (GPIO13)</option><option value='D8'>D8 "
          "(GPIO15)</option></select>";
  html += "<small>Operating Mode</small><select name='type'>"
          "<option value='0'>Normally Open (HIGH=ON)</option>"
          "<option value='1'>Normally Closed (LOW=ON)</option>"
          "<option value='2'>Generic Digital Output</option></select>";
  html += "<button type='submit' class='btn-add'>Register "
          "Actuator</button></form></div></div>";

  // ── ADD SENSOR FORM ──
  html += "<div class='glass'><h3>Add Sensor</h3>";
  html += "<form action='/add_s' method='GET'>";
  html += "<small>Sensor Name</small><input name='id' placeholder='e.g. "
          "TEMP_ENTRY'>";
  html += "<small>Connection Pin</small><select name='pin'><option "
          "value='A0'>A0 (Analog)</option><option value='D1'>D1</option>"
          "<option value='D2'>D2</option><option value='D5'>D5</option><option "
          "value='D6'>D6</option></select>";
  html += "<small>Sensor Model</small><select name='type'>"
          "<option value='0'>DHT11 Temperature</option><option value='1'>DHT11 "
          "Humidity</option>"
          "<option value='2'>DHT22 Temperature</option><option value='3'>DHT22 "
          "Humidity</option>"
          "<option value='4'>Analog (Raw Data)</option></select>";
  html += "<small>Measure Unit</small><input name='unit' placeholder='e.g. °C, "
          "%, V'>";
  html += "<button type='submit' class='btn-add'>Register "
          "Sensor</button></form></div>";

  // ── SYSTEM SETTINGS ──
  html += "<div class='glass'><h3>System Infrastructure</h3>";
  html += "<button onclick=\"toggle('set-form')\" class='btn-token'>Configure "
          "Connectivity & MQTT</button>";
  html +=
      "<div id='set-form' style='display:none;margin-top:15px;border-top:1px "
      "solid var(--border);padding-top:20px;'>";
  html += "<form action='/save_sys' method='GET'>";
  html += "<small>Friendly Name</small><input name='name' value='" +
          String(device_name) + "'>";
  html += "<small>Sentinel Security Token</small><input type='password' "
          "name='token' value='" +
          String(sentinel_token) + "'>";

  // Web Auth Section
  html +=
      "<div "
      "style='margin-top:20px;padding:16px;background:rgba(0,0,0,0.2);"
      "border-radius:16px;border:1px solid var(--border);margin-bottom:20px;'>";
  html += "<p style='font-size:10px;margin:0 0 12px "
          "0;color:var(--accent);font-weight:900;text-transform:uppercase;'>"
          "Web UI Access Security</p>";
  html += "<small>Admin Username</small><input name='wu' value='" +
          String(www_user) + "'>";
  html +=
      "<small>Admin Password</small><input type='password' name='wp' value=''> "
      "<small style='font-size:8px;'>Leave empty to keep current</small>";
  html += "</div>";

  html += "<div "
          "style='margin-top:20px;padding:16px;background:rgba(0,0,0,0.2);"
          "border-radius:16px;border:1px solid var(--border);'>";
  html += "<p style='font-size:10px;margin:0 0 12px "
          "0;color:var(--accent);font-weight:900;text-transform:uppercase;'>"
          "MQTT Node Link</p>";
  html += "<small>Broker Host</small><input name='mh' value='" +
          String(mqtt_host) + "'>";
  html += "<small>Port</small><input name='mp' value='" +
          String(mqtt_port_str) + "'>";
  html += "<small>Username</small><input name='mu' value='" +
          String(mqtt_user) + "'>";
  html += "<small>Password</small><input type='password' name='mpx' value='" +
          String(mqtt_pass) + "'>";
  html += "</div>";
  html += "<button type='submit' "
          "style='background:rgba(255,255,255,0.05);color:var(--accent);border:"
          "1px solid var(--border);margin-top:20px;'>Deploy Config</button>";
  html += "</form></div></div>";

  // ── FOOTER ──
  html += "<div "
          "style='width:100%;max-width:480px;display:grid;grid-template-"
          "columns:1fr 1fr;gap:12px;'>";
  html += "<a href='/reboot' style='text-decoration:none;'><button "
          "class='btn-reboot'>Reboot</button></a>";
  html += "<a href='/reset_wifi' style='text-decoration:none;'><button "
          "class='btn-wifi'>Reset WiFi</button></a></div>";
  html += "<p "
          "style='font-size:10px;color:var(--dim);margin-top:24px;text-align:"
          "center;letter-spacing:0.02em;'>Firmware v3.0 | Sentinel AI Core | "
          "Solsteinn Innovations</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleAddActuator() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  Actuator a;
  a.id = server.arg("id");
  a.pin = getGpio(server.arg("pin"));
  a.type = (ActuatorType)server.arg("type").toInt();
  a.state = false;
  a.timer_ms = 0;
  a.activated_at = 0;
  actuators.push_back(a);
  // Inicializar en estado seguro
  int safeVal = (a.type == ACT_RELAY_NC) ? HIGH : LOW;
  pinMode(a.pin, OUTPUT);
  digitalWrite(a.pin, safeVal);
  saveActuators();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleDeleteActuator() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  int i = server.arg("index").toInt();
  if (i >= 0 && i < (int)actuators.size()) {
    deactivateActuator(actuators[i]); // Apagar antes de eliminar
    actuators.erase(actuators.begin() + i);
    saveActuators();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

/** Control manual de actuador desde la UI web */
void handleActuatorControl() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  int i = server.arg("index").toInt();
  String cmd = server.arg("cmd");
  int dur = server.arg("dur").toInt(); // minutos

  if (i >= 0 && i < (int)actuators.size()) {
    if (cmd == "on")
      activateActuator(actuators[i], (unsigned long)dur * 60000UL);
    if (cmd == "off")
      deactivateActuator(actuators[i]);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSaveSys() {
  strncpy(device_name, server.arg("name").c_str(), 39);
  strncpy(sentinel_token, server.arg("token").c_str(), 39);
  strncpy(mqtt_host, server.arg("mh").c_str(), 63);
  strncpy(mqtt_port_str, server.arg("mp").c_str(), 5);
  strncpy(mqtt_user, server.arg("mu").c_str(), 31);
  strncpy(mqtt_pass, server.arg("mpx").c_str(), 31);
  strncpy(www_user, server.arg("wu").c_str(), 31);
  // Solo actualizar password si no está vacío
  if (server.arg("wp").length() > 0) {
    strncpy(www_pass, server.arg("wp").c_str(), 31);
  }
  saveConfig();
  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  mqttClient.disconnect();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleAddSensor() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  sensors.push_back({server.arg("id"), getGpio(server.arg("pin")),
                     server.arg("unit"),
                     (SensorType)server.arg("type").toInt()});
  saveSensors();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleDeleteSensor() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  int i = server.arg("index").toInt();
  if (i >= 0 && i < (int)sensors.size()) {
    sensors.erase(sensors.begin() + i);
    saveSensors();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleResetWiFi() {
  if (!server.authenticate(www_user, www_pass)) {
    return server.requestAuthentication();
  }
  server.send(200, "text/plain", "WiFi reset. Node will restart into AP mode.");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ============================================================
// SETUP & LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n🚀 Sentinel Node v3.0 booting...");
  loadConfig();

  WiFiManager wm;
  wm.setConnectTimeout(
      20); // 20s máximo para conectar con credenciales guardadas
  wm.setConnectRetries(3); // 3 intentos antes de rendirse
  wm.setConfigPortalTimeout(
      0); // Portal abierto indefinidamente (0 = sin timeout)

  // Configurar parámetros personalizados para WiFiManager
  WiFiManagerParameter custom_device_name(
      "name", "Nombre del Nodo (Ej: Bomba_Riego_1)", device_name, 40);
  wm.addParameter(&custom_device_name);

  // Configurar Callback para guardar configuración
  wm.setSaveConfigCallback(saveConfigCallback);

  String apName = "Sentinel_Node_" + String(ESP.getChipId(), HEX);
  // Si falla la conexión (contraseña incorrecta u otra razón):
  // borra las credenciales guardadas y reinicia → vuelve a abrir el portal de
  // configuración
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println("❌ WiFi falló (contraseña incorrecta o sin señal).");
    Serial.println("🔄 Borrando credenciales y reiniciando en modo portal...");
    delay(1000);
    wm.resetSettings(); // Limpia SSID/password guardados
    ESP.restart(); // Al reiniciar, no hay credenciales → abre portal de nuevo
  }

  // Si la configuración fue guardada desde el portal, actualizar device_name y
  // persistir
  if (shouldSaveConfig) {
    Serial.println("Saving config from WiFiManager portal...");
    strncpy(device_name, custom_device_name.getValue(), 39);
    saveConfig();
  }

  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  // mDNS: hostname único usando últimos 3 bytes de la MAC del chip
  // Garantiza que múltiples ESP8266 en la misma red no colisionen
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macSuffix[8];
  snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x", mac[3], mac[4],
           mac[5]);

  String mdnsBase = String(device_name);
  mdnsBase.toLowerCase();
  mdnsBase.replace(" ", "-");
  mdnsBase.replace("_", "-");

  // Si el nombre es el default, usar solo el sufijo MAC para que sea corto
  String mdnsName;
  if (mdnsBase == "nodo-sentinel") { // Check against the new default name
    mdnsName = "sentinel-" + String(macSuffix);
    // IMPORTANTE: actualizar device_name para que el backend lo reconozca por
    // este ID único
    snprintf(device_name, sizeof(device_name), "SENTINEL_%s", macSuffix);
  } else {
    // Nombre personalizado + sufijo MAC para evitar colisiones
    mdnsName = mdnsBase + "-" + String(macSuffix);
  }

  if (MDNS.begin(mdnsName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("\U0001f310 mDNS: http://");
    Serial.print(mdnsName);
    Serial.println(".local");
  }

  // Inicializar actuadores en estado seguro antes de conectar MQTT
  initActuators();

  // Web routes
  server.on("/", handleRoot);
  server.on("/add_s", handleAddSensor);
  server.on("/delete_s", handleDeleteSensor);
  server.on("/add_a", handleAddActuator);
  server.on("/delete_a", handleDeleteActuator);
  server.on("/act", handleActuatorControl);
  server.on("/save_sys", handleSaveSys);
  server.on("/reset_wifi", handleResetWiFi);
  server.on("/reboot", []() {
    if (!server.authenticate(www_user, www_pass)) {
      return server.requestAuthentication();
    }
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });
  server.begin();

  mqttClient.setServer(mqtt_host, atoi(mqtt_port_str));
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024); // Aumentar buffer para payloads de actuador

  Serial.println("✅ Sentinel Node Ready.");
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected())
      reconnectMQTT();
    mqttClient.loop(); // Procesar mensajes MQTT entrantes (comandos)

    unsigned long now = millis();

    // Publicar datos de sensores cada MEASUREMENT_INTERVAL
    if (now - lastMsg > MEASUREMENT_INTERVAL) {
      lastMsg = now;
      collectAndPublish();
    }

    // Publicar estado de actuadores cada STATUS_INTERVAL
    if (now - lastStatus > STATUS_INTERVAL) {
      lastStatus = now;
      publishStatus();
    }
  }

  // Safety watchdog: apagar actuadores con timer vencido
  // Se ejecuta siempre, incluso sin WiFi
  checkActuatorTimers();
}
