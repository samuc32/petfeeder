#include "../config.h"
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <RTClib.h>
#include <WiFi.h>

// ---------- GLOBAL OBJECTS --------------------------------------------------
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Servo feederServo;
RTC_DS3231 rtc;
Preferences prefs;

// ---------- STATE ----------------------------------------------------------
unsigned long lastStatusMillis = 0;
unsigned long lastSensorMillis = 0;
unsigned long pumpStartMillis = 0;
bool wifiConnected = false;
bool mqttConnected = false;
bool pumpActive = false;

// Schedule (defaults – can be overwritten by MQTT config)
uint8_t h1 = 8, m1 = 0;  // first feeding
uint8_t h2 = 18, m2 = 0; // second feeding
uint8_t portion = 5;     // 0‑100 %

// ---------- FUNCTION PROTOTYPES --------------------------------------------
void connectWiFi();
void mqttReconnect();
void mqttCallback(char *topic, byte *payload, unsigned int length);
void publishStatus();
void handleCommands(const String &cmd);
void handleConfig(const String &json);
void handleRTC(const String &json);
int readUltrasonic(uint8_t trigPin, uint8_t echoPin);
float levelFromDistance(int distance, int maxDist);
void updateLCD();
void feedPortion();
void controlPump(bool on, bool isManual = false);
String clientStateString();

// ---------- SETUP ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  Serial.println("\n=== PetFeeder Firmware Starting ===");

  // LCD (I2C0)
  Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PetFeeder Init");

  // RTC (I2C1)
  Wire1.begin(PIN_RTC_SDA, PIN_RTC_SCL);
  if (!rtc.begin(&Wire1)) {
    Serial.println("RTC not found! Check wiring.");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Servo
  feederServo.setPeriodHertz(50);
  feederServo.attach(PIN_SERVO);
  feederServo.write(0); // closed

  // Relay
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW); // OFF inicial

  // Ultrasonic pins (1: Reserva Água, 2: Silo Ração, 3: Tigela)
  pinMode(TRIG_WATER_RES, OUTPUT);  pinMode(ECHO_WATER_RES, INPUT);
  pinMode(TRIG_FOOD_RES, OUTPUT);   pinMode(ECHO_FOOD_RES, INPUT);
  pinMode(TRIG_BOWL_WATER, OUTPUT); pinMode(ECHO_BOWL_WATER, INPUT);

  // Load persisted schedule/portion
  prefs.begin("petfeeder", false);
  h1 = prefs.getUChar("h1", h1);
  m1 = prefs.getUChar("m1", m1);
  h2 = prefs.getUChar("h2", h2);
  m2 = prefs.getUChar("m2", m2);
  portion = prefs.getUChar("porc", portion);

  // Wi‑Fi & MQTT setup
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  connectWiFi();
}

// ---------- LOOP -----------------------------------------------------------
void loop() {
  // ---- Wi‑Fi keep‑alive (non‑blocking) ----
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected)
      Serial.println("Wi‑Fi lost – reconnecting");
    wifiConnected = false;
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt > 5000) {
      lastAttempt = millis();
      connectWiFi();
    }
  } else if (!wifiConnected) {
    Serial.println("Wi‑Fi connected");
    wifiConnected = true;
  }

  // ---- MQTT handling (non‑blocking) ----
  if (wifiConnected) {
    if (!mqttClient.connected()) {
      if (mqttConnected)
        Serial.println("MQTT disconnected – will reconnect");
      mqttConnected = false;
      mqttReconnect();
    } else {
      if (!mqttConnected) {
        Serial.println("MQTT connected");
        mqttClient.subscribe(TOPIC_COMMAND);
        mqttClient.subscribe(TOPIC_CONFIG);
        mqttClient.subscribe(TOPIC_RTC);
      }
      mqttConnected = true;
      mqttClient.loop();
    }
  }

  // ---- Sensor reading & automation (run every 1 s) ----
  if (millis() - lastSensorMillis > 1000) {
    lastSensorMillis = millis();

    // Leitura dos 3 sensores ultrassônicos
    int dWaterRes  = readUltrasonic(TRIG_WATER_RES, ECHO_WATER_RES);   // 1: Galão de água
    int dFoodRes   = readUltrasonic(TRIG_FOOD_RES, ECHO_FOOD_RES);     // 2: Silo de ração
    int dBowlWater = readUltrasonic(TRIG_BOWL_WATER, ECHO_BOWL_WATER); // 3: Tigela de água

    float lvlWaterRes  = levelFromDistance(dWaterRes, MAX_DIST_WATER);
    float lvlFoodRes   = levelFromDistance(dFoodRes, MAX_DIST_FOOD);
    float lvlBowlWater = levelFromDistance(dBowlWater, MAX_DIST_BOWL);

    // Pump timeout safety (desliga após 10s se ficar ligada)
    if (pumpActive && (millis() - pumpStartMillis > PUMP_MAX_MS)) {
      Serial.println("Pump timeout – turning off automatically");
      controlPump(false);
    }

    // Auto‑refill da tigela (somente se a tigela estiver baixa < 20% e tiver água na reserva > 5%)
    if (lvlBowlWater < 20.0 && lvlWaterRes >= 5.0 && !pumpActive) {
      Serial.println("Bowl low & water available – starting auto pump");
      controlPump(true);
    }

    updateLCD();

    // Scheduled feedings
    DateTime now = rtc.now();
    if (now.hour() == h1 && now.minute() == m1 && now.second() == 0)
      feedPortion();
    if (now.hour() == h2 && now.minute() == m2 && now.second() == 0)
      feedPortion();
  }

  // ---- Periodic status publish (every 2 s) ----
  if (millis() - lastStatusMillis >= STATUS_INTERVAL) {
    lastStatusMillis = millis();
    publishStatus();
  }
}

// ---------- Wi‑Fi ---------------------------------------------------------
void connectWiFi() {
  if (wifiConnected)
    return;
  Serial.print("Connecting to Wi‑Fi ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Loop handled in main loop
}

// ---------- MQTT ----------------------------------------------------------
void mqttReconnect() {
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000)
    return; // retry every 5 s
  lastAttempt = millis();
  Serial.print("Attempting MQTT connection… ");
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("connected");
    // Subscriptions will be (re)sent in loop after connection
  } else {
    Serial.print("failed, state=");
    Serial.println(clientStateString());
  }
}

String clientStateString() {
  switch (mqttClient.state()) {
  case MQTT_CONNECTION_TIMEOUT:
    return "Connection Timeout";
  case MQTT_CONNECTION_LOST:
    return "Connection Lost";
  case MQTT_CONNECT_FAILED:
    return "Connect Failed";
  case MQTT_DISCONNECTED:
    return "Disconnected";
  case MQTT_CONNECTED:
    return "Connected";
  case MQTT_CONNECT_BAD_PROTOCOL:
    return "Bad Protocol";
  case MQTT_CONNECT_BAD_CLIENT_ID:
    return "Bad Client ID";
  case MQTT_CONNECT_UNAVAILABLE:
    return "Broker Unavailable";
  case MQTT_CONNECT_BAD_CREDENTIALS:
    return "Bad Credentials";
  case MQTT_CONNECT_UNAUTHORIZED:
    return "Unauthorized";
  default:
    return "Unknown";
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; ++i)
    msg += (char)payload[i];
  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if (strcmp(topic, TOPIC_COMMAND) == 0)
    handleCommands(msg);
  else if (strcmp(topic, TOPIC_CONFIG) == 0)
    handleConfig(msg);
  else if (strcmp(topic, TOPIC_RTC) == 0)
    handleRTC(msg);
}

// ---------- MQTT message handlers ----------------------------------------
void handleCommands(const String &cmd) {
  String lc = cmd;
  lc.toLowerCase();
  if (lc == "racao") {
    Serial.println("Manual command – dispensing food");
    feedPortion();
  } else if (lc == "agua") {
    Serial.println("Manual command – toggling water pump");
    controlPump(!pumpActive, true); // true = comando manual (não bloqueia por falta de sensor)
    publishStatus();                // atualiza o app imediatamente
  } else if (lc == "agua_high") {
    digitalWrite(PIN_RELAY, HIGH);
    pumpActive = true;
    Serial.println("PIN_RELAY forçado em HIGH (3.3V)");
    publishStatus();
  } else if (lc == "agua_low") {
    digitalWrite(PIN_RELAY, LOW);
    pumpActive = false;
    Serial.println("PIN_RELAY forçado em LOW (0V)");
    publishStatus();
  } else {
    Serial.println("Unknown manual command");
  }
}

void handleConfig(const String &json) {
  // Expected: {"h1":int,"m1":int,"h2":int,"m2":int,"porcao":int}
  int nh1, nm1, nh2, nm2, nporc;
  if (sscanf(json.c_str(),
             "{\"h1\":%d,\"m1\":%d,\"h2\":%d,\"m2\":%d,\"porcao\":%d}", &nh1,
             &nm1, &nh2, &nm2, &nporc) == 5) {
    h1 = (uint8_t)nh1;
    m1 = (uint8_t)nm1;
    h2 = (uint8_t)nh2;
    m2 = (uint8_t)nm2;
    portion = (uint8_t)nporc;
    // persist
    prefs.putUChar("h1", h1);
    prefs.putUChar("m1", m1);
    prefs.putUChar("h2", h2);
    prefs.putUChar("m2", m2);
    prefs.putUChar("porc", portion);
    Serial.println("Schedule & portion updated from MQTT config");
  } else {
    Serial.println("Failed to parse config JSON");
  }
}

void handleRTC(const String &json) {
  // Expected: {"ano":int,"mes":int,"dia":int,"hora":int,"min":int,"seg":int}
  int yr, mo, dy, hr, mn, sc;
  if (sscanf(json.c_str(),
             "{\"ano\":%d,\"mes\":%d,\"dia\":%d,\"hora\":%d,\"min\":%d,\"seg\":"
             "%d}",
             &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
    DateTime dt(yr, mo, dy, hr, mn, sc);
    rtc.adjust(dt);
    Serial.println("RTC synchronized via MQTT");
  } else {
    Serial.println("Failed to parse RTC sync JSON");
  }
}

// ---------- Status publishing ---------------------------------------------
void publishStatus() {
  DateTime now = rtc.now();

  // Leitura em tempo real dos 3 sensores
  int dWaterRes  = readUltrasonic(TRIG_WATER_RES, ECHO_WATER_RES);   // 1: Galão de Água
  int dFoodRes   = readUltrasonic(TRIG_FOOD_RES, ECHO_FOOD_RES);     // 2: Silo de Ração
  int dBowlWater = readUltrasonic(TRIG_BOWL_WATER, ECHO_BOWL_WATER); // 3: Tigela de Água

  float lvlWaterRes  = levelFromDistance(dWaterRes, MAX_DIST_WATER);
  float lvlFoodRes   = levelFromDistance(dFoodRes, MAX_DIST_FOOD);
  float lvlBowlWater = levelFromDistance(dBowlWater, MAX_DIST_BOWL);

  // Debug no Serial Monitor para conferência de montagem
  Serial.printf("[ULTRASSOM] S1(Água): %d cm | S2(Ração): %d cm | S3(Tigela): %d cm\n", dWaterRes, dFoodRes, dBowlWater);

  char payload[360];
  snprintf(payload, sizeof(payload),
           "{\"hora\":\"%02d:%02d:%02d\","
           "\"agua_reserva\":%.1f,"
           "\"racao_reserva\":%.1f,"
           "\"tigela\":%.1f,"
           "\"d1\":%d,\"d2\":%d,\"d3\":%d,"
           "\"agua\":%s,"
           "\"porcao\":%d,"
           "\"h1\":%d,\"m1\":%d,"
           "\"h2\":%d,\"m2\":%d}",
           now.hour(), now.minute(), now.second(),
           lvlWaterRes,
           lvlFoodRes,
           lvlBowlWater,
           dWaterRes, dFoodRes, dBowlWater,
           pumpActive ? "true" : "false",
           portion,
           h1, m1, h2, m2);

  if (mqttConnected) {
    bool ok = mqttClient.publish(TOPIC_STATUS, payload, true);
    Serial.print("Status publish ");
    Serial.println(ok ? "OK" : "FAIL");
  } else {
    Serial.println("Skipping status publish – MQTT offline");
  }
}

// ---------- Ultrasonic helpers --------------------------------------------
int readUltrasonic(uint8_t trigPin, uint8_t echoPin) {
  // Garante pino limpo em LOW antes do disparo
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  
  // Pulso de 10 microssegundos para o sensor disparar os 8 ciclos de 40kHz
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Leitura do tempo de eco (timeout de 25000us ≈ 4 metros)
  unsigned long duration = pulseIn(echoPin, HIGH, 25000);
  if (duration == 0) {
    return -1; // Sem eco (desconectado ou obstáculo longe demais)
  }
  int cm = (int)(duration * 0.0343 / 2.0);
  return cm;
}

float levelFromDistance(int distance, int maxDist) {
  // Se não leu nada (-1) ou se a distância for maior que a altura do reservatório
  if (distance < 0 || distance > maxDist) return 0.0f;
  // Se estiver a menos de 3cm do topo, considera 100% cheio
  if (distance <= 3) return 100.0f;
  
  float perc = 100.0f * (float)(maxDist - distance) / (float)(maxDist - 3);
  if (perc < 0.0f)   perc = 0.0f;
  if (perc > 100.0f) perc = 100.0f;
  return perc;
}

// ---------- LCD ------------------------------------------------------------
void updateLCD() {
  static uint8_t lastHour = 255, lastMinute = 255;
  DateTime now = rtc.now();
  if (now.hour() != lastHour || now.minute() != lastMinute) {
    lastHour = now.hour();
    lastMinute = now.minute();
    lcd.clear();
    char line1[17];
    snprintf(line1, sizeof(line1), "%02d:%02d %s", now.hour(), now.minute(),
             pumpActive ? "Água" : "");
    lcd.setCursor(0, 0);
    lcd.print(line1);
    char line2[17];
    snprintf(line2, sizeof(line2), "F:%02d:%02d P:%d", h1, m1, portion);
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }
}

// ---------- Feeding --------------------------------------------------------
void feedPortion() {
  Serial.println("Dispensing food portion");
  int angle = map(portion, 0, 100, 0, 90); // linear mapping to servo angle
  feederServo.write(angle);
  delay(1500);          // gate open
  feederServo.write(0); // close
  Serial.println("Food dispensed");
}

// ---------- Water pump -----------------------------------------------------
void controlPump(bool on, bool isManual) {
  if (on) {
    // Se for acionamento automático pelo sensor, checa o nível de reserva
    if (!isManual) {
      int dWaterR = readUltrasonic(TRIG_WATER_RES, ECHO_WATER_RES);
      float lvlWaterReserve = levelFromDistance(dWaterR, 30);
      if (lvlWaterReserve < 5.0) {
        Serial.println("Pump not started – reserve water too low (<5%)");
        return;
      }
    } else {
      Serial.println("Manual pump override accepted");
    }

    // Ativa o pino do relé
    digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? LOW : HIGH);
    pumpStartMillis = millis();
    pumpActive = true;
    Serial.println("Pump turned ON (Relay pin triggered)");
  } else {
    // Desativa o pino do relé
    digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pumpActive = false;
    Serial.println("Pump turned OFF");
  }
}
