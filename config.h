// config.h – usuário deve preencher as credenciais e parâmetros
#ifndef CONFIG_H
#define CONFIG_H

// ==== Wi‑Fi ==== 
#define WIFI_SSID        "DBUG_SAM.NET-2.4G"
#define WIFI_PASSWORD    "Sophia06maio!"

// ==== MQTT (Broker público aberto, sem necessidade de senha) ==== 
#define MQTT_BROKER      "broker.emqx.io"
#define MQTT_PORT        1883
#define MQTT_CLIENT_ID   "PetFeederESP32S3_Sam"

// MQTT topics – keep same naming as original firmware
#define TOPIC_STATUS     "petfeeder/status"
#define TOPIC_COMMAND    "petfeeder/comando"
#define TOPIC_CONFIG     "petfeeder/config"
#define TOPIC_RTC        "petfeeder/relogio"

// ==== Pin assignments (same as original) ==== 
#define PIN_LCD_SDA      4
#define PIN_LCD_SCL      5
#define LCD_I2C_ADDR     0x27

#define PIN_RTC_SDA      6
#define PIN_RTC_SCL      7

#define PIN_SERVO        13
#define PIN_RELAY        12   // Pino do relé da bomba
#define RELAY_ACTIVE_LOW true // true = relé ativa com LOW (comprovado com seu módulo)

// ==== Sensores Ultrassônicos (Conforme sua ligação física) ====
// 1 = Nível de água do reservatório (galão)
#define TRIG_WATER_RES   10
#define ECHO_WATER_RES   11
#define MAX_DIST_WATER   30   // Altura do galão de água em cm

// 2 = Nível do reservatório de ração (silo)
#define TRIG_FOOD_RES    15
#define ECHO_FOOD_RES    16
#define MAX_DIST_FOOD    30   // Altura do silo de ração em cm

// 3 = Nível da tigela de água
#define TRIG_BOWL_WATER  17
#define ECHO_BOWL_WATER  18
#define MAX_DIST_BOWL    15   // Profundidade da tigela em cm

// ==== Limits ==== 
#define PUMP_MAX_MS      (10UL * 1000)   // 10 s max pump runtime
#define STATUS_INTERVAL  2000            // ms between status publishes

#endif // CONFIG_H