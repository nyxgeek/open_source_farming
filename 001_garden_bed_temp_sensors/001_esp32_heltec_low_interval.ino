// Heltec WiFi LoRa 32 V3 (ESP32-S3) + DS18B20 x3 (normal 3-wire)
// Data pin: GPIO 6  (use 4.7k pull-up to 3.3V)

#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>

// we are using pin 6 for onewire
#define ONE_WIRE_BUS 6

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

DeviceAddress addrs[3];
uint8_t N = 0;

WiFiMulti wifiMulti;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// local network name
const char* HOSTNAME = "my-garden-node-001";

// wifi setup - only made to handle two APs currently, will try one then the other
const char* SSID_1 = "mySSID1";
const char* PASS_1 = "mypassword1";
const char* SSID_2 = "mySSID2";
const char* PASS_2 = "mypassword2";

// MQTT for HomeAssistant - hA stuff isn't tested, MQTT should work though
const char* MQTT_HOST  = "myserver.local";
const int   MQTT_PORT  = 1883;
const char* MQTT_USER  = "mqttuser";
const char* MQTT_PASS  = "secretpassword";

const char* DEVICE_ID  = "my-garden-beds-node-01";
const char* BASE_DISC  = "homeassistant";
const char* BASE_STATE = "sensors/garden_beds/telemetry";
const char* AVAIL_T    = "sensors/garden_beds/status";

unsigned long lastPublish = 0;
const unsigned long PUBLISH_MS = 30000;

void printAddr(const DeviceAddress a){
  for (uint8_t i=0;i<8;i++){ if(a[i]<16) Serial.print('0'); Serial.print(a[i],HEX); }
}


bool same(const DeviceAddress a,const DeviceAddress b){
  for(uint8_t i=0;i<8;i++) if(a[i]!=b[i]) return false; return true;
}

String romToKey(const DeviceAddress a){
  char buf[17];
  for (int i=0;i<8;i++) sprintf(buf+2*i, "%02X", a[i]);
  return String(buf);
}

bool connectWiFi(uint32_t timeout_ms = 15000) {
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    uint8_t status = wifiMulti.run();
    if (status == WL_CONNECTED) {
      Serial.println("Connected, SSID: " + WiFi.SSID() + "  IP: " + WiFi.localIP().toString());
      return true;
    }
    delay(250);
  }
  Serial.println("ERROR: No wifi - timeout has occured.");
  return false;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("WARNING: Wifi lost - reconnecting");
  connectWiFi();
}

void ensureMqtt() {
  if (mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // Set LWT (retain "offline"); upon connect we'll publish "online" retained.
  String clientId = String("beds-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  while (!mqtt.connected()) {
    if (mqtt.connect(
          clientId.c_str(),
          MQTT_USER, MQTT_PASS,
          AVAIL_T, 0, true, "offline"  // LWT retained
        )) {
      mqtt.publish(AVAIL_T, "online", true); // retained online
      Serial.println("MQTT connected");
    } else {
      Serial.println("ERROR: MQTT connection failed, rc=" + mqtt.state());
      delay(1000);
    }
  }
}

// Publish HA discovery for each detected sensor (object_id = "temp_<ROM>")
void publishDiscoveryForAllProbes() {
  for (uint8_t i=0;i<N;i++) {
    String key = romToKey(addrs[i]);
    String obj = "temp_" + key;
    String topic = String(BASE_DISC) + "/sensor/" + DEVICE_ID + "/" + obj + "/config";

    String payload;
    payload.reserve(400);
    payload += "{";
      payload += "\"name\":\""; payload += key; payload += "\",";
      payload += "\"uniq_id\":\""; payload += DEVICE_ID; payload += "_"; payload += obj; payload += "\",";
      payload += "\"stat_t\":\""; payload += BASE_STATE; payload += "\",";
      payload += "\"val_tpl\":\"{{ value_json."; payload += key; payload += " }}\",";
      payload += "\"dev_cla\":\"temperature\",";
      payload += "\"stat_cla\":\"measurement\",";
      payload += "\"unit_of_meas\":\"°C\",";
      payload += "\"avty_t\":\""; payload += AVAIL_T; payload += "\",";
      payload += "\"device\":{";
        payload += "\"ids\":[\""; payload += DEVICE_ID; payload += "\"],";
        payload += "\"name\":\"Garden Beds Node\",";
        payload += "\"mf\":\"DIY\",";
        payload += "\"mdl\":\"ESP32-DS18B20\"";
      payload += "}";
    payload += "}";

    mqtt.publish(topic.c_str(), payload.c_str(), true);
  }
}

// ----- Arduino -----
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\nDS18B20 discovery on GPIO 6...");
  sensors.begin();
  sensors.setResolution(12);

  oneWire.reset_search();
  DeviceAddress a;
  while (oneWire.search(a)) {
    if (OneWire::crc8(a,7) != a[7]) continue;
    if (a[0] != 0x28) continue; // DS18B20 family
    bool dup=false; for(uint8_t i=0;i<N;i++) if(same(a,addrs[i])) {dup=true;break;}
    if (dup) continue;
    memcpy(addrs[N], a, 8);
    sensors.setResolution(addrs[N], 12);
    Serial.printf("Found #%u addr=", N);
  }

  if (N==0) Serial.println("No DS18B20 sensors found. Check wiring & pull-up.");
  else { Serial.print("Total sensors: "); Serial.println(N); }

  // Wi-Fi (two APs)
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  wifiMulti.addAP(SSID_1, PASS_1);
  wifiMulti.addAP(SSID_2, PASS_2);
  connectWiFi();

  // MQTT
  ensureMqtt();

  // Discovery (after MQTT up)
  publishDiscoveryForAllProbes();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  if (millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();

    sensors.requestTemperatures();

    // JSON payload
    String json = "{";
    for (uint8_t i=0;i<N;i++) {
      if (i) json += ",";
      String key = romToKey(addrs[i]);
      float c = sensors.getTempC(addrs[i]);
      Serial.print("Sensor "); Serial.print(i); Serial.print(" ("); printAddr(addrs[i]); Serial.print("): ");

      json += "\""; json += key; json += "\":";
      if (c == DEVICE_DISCONNECTED_C) json += "null";
      else { 
        char buf[16]; dtostrf(c, 0, 2, buf); json += buf; 
        Serial.printf("%.2f °C  |  %.2f °F\n", c, DallasTemperature::toFahrenheit(c));

        }
    }
    json += "}";

    mqtt.publish(BASE_STATE, json.c_str(), false);
  }
}
