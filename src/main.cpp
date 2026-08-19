#include <Arduino.h>
#include <WiFiMulti.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID "<Your_wifi_name>"
#define WIFI_PASSWORD "<Your_password>"

// Laptop LAN IP (where server.js runs)
#define WS_HOST "192.168.0.198" // you can find it on terminal or in network settings
#define WS_PORT 3000
#define WS_URL  "/"

#define MSG_SIZE 256

WiFiMulti wifiMulti;
WebSocketsClient wsClient;

// ==== Hardware config ====
// Relay output (active-low relay module handled below)
const int relayPin = 23;           // safe GPIO
const bool RELAY_ACTIVE_LOW = true;

// Rocker input on GPIO18 with internal pull-up.
// Wiring: COM -> GND, NO -> GPIO18, NC unused.
const int switchPin = 18;
const bool SWITCH_ACTIVE_LOW = true; // LOW = ON (pressed/toggled to NO)

// Logical relay state we expose over WS (0=OFF,1=ON)
volatile int relayState = LOW;

// --- Helpers ---
uint8_t toMode(const char* val) {
  if (strcmp(val, "output") == 0) return OUTPUT;
  if (strcmp(val, "input")  == 0) return INPUT;
  return INPUT;
}

void sendErrorMessage(const char* error) {
  char msg[MSG_SIZE];
  snprintf(msg, MSG_SIZE, "{\"error\":\"%s\"}", error);
  wsClient.sendTXT(msg);
}

void sendOkMessage() { wsClient.sendTXT("{\"type\":\"cmd\",\"body\":{\"type\":\"acknowledgement\"}}"); }

int toPhysicalLevel(int logical) {
  return RELAY_ACTIVE_LOW ? (logical ? LOW : HIGH) : (logical ? HIGH : LOW);
}

void applyRelayState() {
  digitalWrite(relayPin, toPhysicalLevel(relayState));
}

void sendRelayState() {
  char msg[MSG_SIZE];
  snprintf(msg, MSG_SIZE, "{\"from\":\"esp32\",\"type\":\"relayState\",\"pin\":%d,\"value\":%d}", relayPin, relayState);
  wsClient.sendTXT(msg);
}

void setRelayState(int logical) {
  logical = logical ? 1 : 0;
  if (relayState != logical) {
    relayState = logical;
    applyRelayState();
    sendRelayState();
    Serial.printf("[relay] state -> %d\n", relayState);
  }
}

// --- Switch debounce state ---
static int sw_lastStable = HIGH;     // because of INPUT_PULLUP
static int sw_lastRead   = HIGH;
static unsigned long sw_lastChangeMs = 0;
const unsigned long SW_DEBOUNCE_MS = 30;

// Return early if the message doesn't look like JSON
static inline bool looksLikeJson(const uint8_t* p, size_t n) {
  if (!p || n < 2) return false;
  size_t i = 0; while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
  return i < n && p[i] == '{';
}

void handleMessage(uint8_t * payload, size_t length) {
  if (!looksLikeJson(payload, length)) return;

  #if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
  #else
    StaticJsonDocument<256> doc;
  #endif

  Serial.printf("[handleMessage] raw (%u): %.*s\n", (unsigned)length, (int)length, payload);

  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("[handleMessage] parse error: %s\n", error.c_str());
    sendErrorMessage(error.c_str());
    return;
  }

  // Ignore our own messages rebroadcast by server
  if (doc["from"].is<const char*>() && strcmp(doc["from"].as<const char*>(), "esp32") == 0) {
    return;
  }

  if (!doc["type"].is<const char*>()) {
    Serial.println("[handleMessage] missing type");
    sendErrorMessage("missing or invalid 'type'");
    return;
  }
  const char* t = doc["type"];
  Serial.printf("[handleMessage] type=%s\n", t);

  if (strcmp(t, "cmd") != 0) {
    Serial.println("[handleMessage] not a cmd");
    sendErrorMessage("type not supported");
    return;
  }

  if (!doc["body"].is<JsonObject>() || !doc["body"]["type"].is<const char*>()) {
    Serial.println("[handleMessage] bad body/type");
    sendErrorMessage("command body not object or missing type");
    return;
  }

  JsonObject body = doc["body"];
  const char* btype = body["type"];
  Serial.printf("[cmd] body.type=%s\n", btype);

  if (strcmp(btype, "pinMode") == 0) {
    if (body["pin"].is<int>() && body["mode"].is<const char*>()) {
      pinMode(body["pin"].as<int>(), toMode(body["mode"].as<const char*>()));
      sendOkMessage();
    } else {
      sendErrorMessage("Invalid pinMode parameters");
    }
    return;
  }

  if (strcmp(btype, "digitalWrite") == 0) {
    if (body["pin"].is<int>() && body["value"].is<int>()) {
      int pin = body["pin"].as<int>();
      int value = body["value"].as<int>();
      Serial.printf("[digitalWrite] pin=%d value=%d relayPin=%d\n", pin, value, relayPin);

      if (value == 0 || value == 1) {
        if (pin == relayPin) {
          Serial.printf("x1 Setting relay pin %d to %d\n", pin, value);
          setRelayState(value);
        } else {
          Serial.printf("x2 Digital write %d to %d\n", pin, value);
          digitalWrite(pin, value ? HIGH : LOW);
          sendOkMessage();
        }
      } else {
        sendErrorMessage("digitalWrite value must be 0 or 1");
      }
    } else {
      sendErrorMessage("Invalid digitalWrite parameters");
    }
    return;
  }

  if (strcmp(btype, "acknowledgement") == 0) {
    sendRelayState();
    Serial.println("Acknowledged");
    return;
  }

  sendErrorMessage("command not supported");
}

void onWSEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WSc] Connected: %s\n", payload ? (const char*)payload : "");
      // wsClient.sendTXT("{\"type\":\"hello\",\"from\":\"esp32\"}");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WSc] Disconnected");
      break;
    case WStype_TEXT:
      Serial.printf("[WSc] text: %.*s\n", (int)length, payload);
      handleMessage(payload, length);
      break;
    case WStype_ERROR:
      Serial.println("[WSc] Error");
      break;
    default:
      Serial.println("[WSc] Error unknown event type");
      break;
  }
}

void setup() {
  Serial.begin(921600);
  pinMode(relayPin, OUTPUT);
  setRelayState(0); // initialize OFF

  // Rocker input
  pinMode(switchPin, INPUT_PULLUP);  // COM->GND, NO->GPIO18
  sw_lastStable = digitalRead(switchPin);
  sw_lastRead   = sw_lastStable;

  // Wi-Fi + WS
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(200);
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  wsClient.begin(WS_HOST, WS_PORT, WS_URL);
  wsClient.setReconnectInterval(5000);
  wsClient.onEvent(onWSEvent);
}

void loop() {
  wsClient.loop();
  wifiMulti.run();

  // --- Debounce the rocker switch ---
  int level = digitalRead(switchPin);
  if (level != sw_lastRead) {
    sw_lastRead = level;
    sw_lastChangeMs = millis();
  }
  if ((millis() - sw_lastChangeMs) > SW_DEBOUNCE_MS) {
    if (level != sw_lastStable) {
      sw_lastStable = level;

      // Map input level to logical ON/OFF
      int logicalOn = SWITCH_ACTIVE_LOW ? (sw_lastStable == LOW) : (sw_lastStable == HIGH);
      Serial.printf("[switch] level=%d -> logicalOn=%d\n", sw_lastStable, logicalOn);
      setRelayState(logicalOn);
    }
  }
}
