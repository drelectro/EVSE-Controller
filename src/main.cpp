#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>


#include <ArduinoJson.h>   

#include <TFT_eSPI.h> 
#include <SPI.h>
#include <Wire.h>

#include <driver/adc.h>
#include <esp_adc_cal.h>


/*
 * credentials.h contains static WiFi crededtials, like so:-
 *
 * const char* ssid = "your_SSID";
 * const char* password = "your_PASSWORD";
 * 
 * It is not included in the Git repository, you'll need to make your own
 * 
 */
#include "credentials.h"

#define TFT_GREY 0x5AEB // New colour

// GPIOs
#define BACKLIGHT 1
#define CP_IRQ   10
#define CP_DRIVE 11
#define CP_SENSE 9
#define PWR_ENABLE 13
#define AUX_ENABLE 14

TFT_eSPI tft = TFT_eSPI();  // Display library
uint16_t backgroundColor = TFT_BLACK;
uint16_t textColor = TFT_WHITE;


enum class _wifi_state {IDLE, CONNECTING, CONNECTED, RUN_WEBSERVER} wifi_state;
enum class _system_state {WAIT_VEHICLE, CHARGE, FAULT} system_state;
enum class _cp_state {STANDBY, VEHICLE_DETECTED, CHARGE, VENT_CHARGE, NO_POWER, FAULT, UNKNOWN} cp_state;
enum class _charge_rate {OFF = 255, ON_6A = 26, ON_10A = 41, ON_15A = 64, ON_18A = 77, ON_24A = 102, ON_30A = 128} cp_dutyCycle;

const int cp_pwmChannel = 0;
const int cp_pwmFrequency = 1000;
const int cp_pwmResolution = 8;

#define CP_AVERAGE_COUNT 10

uint32_t cp_activeVoltageInt;
bool cp_activeVoltageReady = false;
uint32_t cp_voltageAccumulator;
uint32_t cp_voltageCount;
float cp_activeVoltage;
float cp_Voltage;
float cp_statVoltage;

const float v_scale = 253;

//const float cp_filterAlpha = 0.2;

bool adc_calibrated;

int unknown_count;



static esp_adc_cal_characteristics_t adc1_chars;
static esp_adc_cal_characteristics_t adc2_chars;
static const char *TAG = "ADC INIT";
static bool adc_calibration_init(void)
{
    esp_err_t ret;
    bool cali_enable = false;

    ret = esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP_FIT);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Calibration scheme not supported, skip software calibration");
    } else if (ret == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else if (ret == ESP_OK) {
        cali_enable = true;
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, &adc1_chars);
        esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, &adc2_chars);
    } else {
        ESP_LOGE(TAG, "Invalid arg");
    }

    return cali_enable;
}

/* We can't use floats in ISR context https://esp32.com/viewtopic.php?t=831
 * 
 * Arduino's analogReadMilliVolts() takes forever and is non-deterministic.
 * So we use the ESP API, which is still a bit jittery but always finishes well
 * within 10% duty cycle on time
 * 
 */
void IRAM_ATTR handlePwmStartInterrupt() {
  //digitalWrite(AUX_ENABLE, HIGH); 

  //int adc_raw = adc1_get_raw(ADC1_CHANNEL_8);
  //cp_activeVoltageInt = esp_adc_cal_raw_to_voltage(adc_raw, &adc1_chars);
  cp_activeVoltageInt = adc1_get_raw(ADC1_CHANNEL_8);
  cp_activeVoltageReady = true;

  //digitalWrite(AUX_ENABLE, LOW);
}

void setup(void) {

  bool res;

  Serial0.begin(115200);
  Serial0.println("Initialising...!");

  pinMode(BACKLIGHT, OUTPUT);     // Display backlight
  
  /* Setup CP PWM output*/
  pinMode(CP_DRIVE, OUTPUT);      // CP drive output
  ledcSetup(cp_pwmChannel, cp_pwmFrequency, cp_pwmResolution);
  ledcAttachPin(CP_DRIVE, cp_pwmChannel);
  cp_dutyCycle = _charge_rate::OFF; 
  ledcWrite(cp_pwmChannel, (uint32_t)cp_dutyCycle);

  /* 
   * We need to measure the CP voltage when the CP PWM output is high
   * The ESP32 can't generate an interrupt for each PWM cycle
   * So we look the PWM output back to another GPIO in hardware and configure that as an interrupt
   */
  cp_activeVoltage = 0;
  pinMode(CP_IRQ, INPUT);
  attachInterrupt(CP_IRQ, handlePwmStartInterrupt, RISING);

  adc_calibrated = adc_calibration_init();

  pinMode(CP_SENSE, INPUT);       // CP voltage monitor input
  cp_Voltage = 0;
  cp_voltageAccumulator = 0;
  cp_voltageCount = 0;
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_8, ADC_ATTEN_DB_11);
  
  pinMode(PWR_ENABLE, OUTPUT);    // Charge power contactor drive
  digitalWrite(PWR_ENABLE, LOW);  

  pinMode(AUX_ENABLE, OUTPUT);    // Auxiliarly contactor drive
  digitalWrite(AUX_ENABLE, LOW);  

  //analogSetClockDiv(10);

  tft.init();

  /* Setup display */
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0, 4);
  tft.println("Initialising...");

  /* Set default state */
  cp_state = _cp_state::NO_POWER;
  system_state = _system_state::WAIT_VEHICLE;
  wifi_state = _wifi_state::IDLE;

  unknown_count = 0;

  
  Serial0.println("Initiaslised");
  Serial0.printf("Display = %d x %d\r\n", tft.width(), tft.height());
  Serial0.printf("Font height= %d\r\n", tft.fontHeight());

}

WebServer webserver(80);

void handleRoot() {
  if (SPIFFS.exists("/index.html")) {
    File file = SPIFFS.open("/index.html", "r");
    webserver.streamFile(file, "text/html");
    file.close();
  } else {
    webserver.send(500, "text/plain", "Error: /index.html not found in SPIFFS.");
  }
}


void handleApiGetStatus() {


  DynamicJsonDocument doc(256);

  float cpv = cp_statVoltage - 1656;  // 1656 = 0V
  cpv *= 7.7;  // Multiplier
  cpv /= 1000; // mV to V

  doc["cpVoltage"] = String(cpv, 0);
  doc["cpPWM"] = String(((uint32_t)cp_dutyCycle / 256.0) * 100, 0);; 

  //enum class _cp_state {STANDBY, VEHICLE_DETECTED, CHARGE, VENT_CHARGE, NO_POWER, FAULT, UNKNOWN} cp_state;
  switch (cp_state){
    case _cp_state::STANDBY:
      doc["system_state"] = String("STANDBY");
      break;
    case _cp_state::VEHICLE_DETECTED:
      doc["system_state"] = String("VEHICLE_DETECTED");
      break;
    case _cp_state::CHARGE:
      doc["system_state"] = String("CHARGE");
      break;
    case _cp_state::VENT_CHARGE:
      doc["system_state"] = String("VENT_CHARGE");
      break;
    case _cp_state::NO_POWER:
      doc["system_state"] = String("NO_POWER");
      break;
    case _cp_state::FAULT:
      doc["system_state"] = String("FAULT");
      break;
    default:
      doc["system_state"] = String("UNKNOWN");
      break;
  }

  //enum class _charge_rate {OFF = 255, ON_6A = 26, ON_10A = 41, ON_15A = 64, ON_18A = 77, ON_24A = 102, ON_30A = 128} cp_dutyCycle;
  switch (cp_dutyCycle){
    case _charge_rate::OFF:
      doc["chargeCurrent"] = "0";
      break;
    case _charge_rate::ON_6A:
      doc["chargeCurrent"] = "6";
      break;
    case _charge_rate::ON_10A:
      doc["chargeCurrent"] = "10";
      break;
    case _charge_rate::ON_15A:
      doc["chargeCurrent"] = "15";
      break;
    case _charge_rate::ON_18A:
      doc["chargeCurrent"] = "18";
      break;
    case _charge_rate::ON_24A:
      doc["chargeCurrent"] = "24";
      break;
    case _charge_rate::ON_30A:
      doc["chargeCurrent"] = "30";
      break;

    
  }


  String jsonResponse;
  serializeJson(doc, jsonResponse);

  webserver.send(200, "application/json", jsonResponse);
}

void handleApiSetParameter(){

  for (uint8_t i = 0; i < webserver.args(); i++) {
    Serial0.printf("Set param: %s = %s \r\n", webserver.argName(i), webserver.arg(i));
    //printf("Set param: %s = %s \r\n", webserver.argName(i), webserver.arg(i));

    
    if (webserver.argName(i) == "supply"){

      if (webserver.arg(i) == "MAIN")
        digitalWrite(AUX_ENABLE, LOW); 
      if (webserver.arg(i) == "AUX")
        digitalWrite(AUX_ENABLE, HIGH); 
    }

    if (webserver.argName(i) == "setChargeCurrent"){

      if ((cp_state == _cp_state::CHARGE) || (cp_state == _cp_state::VENT_CHARGE)){

        if (webserver.arg(i) == "0") {
          cp_dutyCycle = _charge_rate::OFF; 
          digitalWrite(PWR_ENABLE, LOW); 
        }
        
        else if (webserver.arg(i) == "6") cp_dutyCycle = _charge_rate::ON_6A;
        else if (webserver.arg(i) == "10") cp_dutyCycle = _charge_rate::ON_10A;
        else if (webserver.arg(i) == "15") cp_dutyCycle = _charge_rate::ON_15A;

        ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);

        if (cp_dutyCycle != _charge_rate::OFF){
          digitalWrite(PWR_ENABLE, HIGH); 
        }
      }

    }

    /*
    if (webserver.argName(i) == "cutoff"){
      runtimeState.cut_out_voltage = webserver.arg(i).toFloat();
      lv_label_set_text_fmt(label_Lower_Status, "Low voltage cutoff = %.2f V", runtimeState.cut_out_voltage);
    }
    */

  }

  webserver.send(200, "application/json", "{\"status\": \"success\"}");
}



void do_WiFi()
{
  switch (wifi_state){

    case _wifi_state::IDLE:
      tft.setCursor(0, 0, 4);
      tft.println("Connecting...");
      WiFi.begin(ssid, password);
      wifi_state = _wifi_state::CONNECTING;
      break;

    case _wifi_state::CONNECTING:
      if (WiFi.status() == WL_CONNECTED){
        tft.setCursor(0, 0, 4);
        tft.printf("%s", WiFi.localIP().toString());
        wifi_state = _wifi_state::CONNECTED;
      }
      break;

    case _wifi_state::CONNECTED:
      webserver.on("/", handleRoot); // Set the root request handler
      webserver.on("/api/getStatus", handleApiGetStatus); // Set the API request handler
      webserver.on("/api/setParameter", handleApiSetParameter);
      webserver.begin(); // Start the server

      if (!SPIFFS.begin(true)) {
        Serial0.println("Error mounting SPIFFS");
        //lv_label_set_text(label_Status, "Warning SPIFFS not mounted !");
      }

      wifi_state = _wifi_state::RUN_WEBSERVER;
      break;

  case _wifi_state::RUN_WEBSERVER:
    webserver.handleClient();
    break;

    default:
      break;
  }

}


/*
 * https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/peripherals/adc.html
 */
bool get_cp_state()
{

  float v;
  bool state_changed = false;

  //cp_Voltage = cp_filterAlpha * (float)analogReadMilliVolts(CP_SENSE) +
  //                    (1 - cp_filterAlpha) * cp_Voltage;

  if (cp_activeVoltageReady){
    cp_activeVoltageReady = false;

    //cp_activeVoltage = cp_filterAlpha * (float)cp_activeVoltageInt +
    //                   (1 - cp_filterAlpha) * cp_activeVoltage;

      cp_voltageAccumulator += cp_activeVoltageInt;
      cp_voltageCount ++;

      if (cp_voltageCount == CP_AVERAGE_COUNT){
        cp_activeVoltage = (float)esp_adc_cal_raw_to_voltage((cp_voltageAccumulator / cp_voltageCount), &adc1_chars);
        cp_voltageCount = 0;
        cp_voltageAccumulator = 0;
      }

    
  }
  else{
    cp_activeVoltage = 0;
  }

  //Serial0.printf("CP V = %.0f act = %.0f %d\r\n", cp_Voltage, cp_activeVoltage, cp_state);
 

  if ((cp_state == _cp_state::VEHICLE_DETECTED) || (cp_state == _cp_state::CHARGE) || (cp_state == _cp_state::VENT_CHARGE)){
    v = cp_activeVoltage;

    if (v == 0){
      return false;
    }
  }
  else{
    cp_voltageAccumulator += adc1_get_raw(ADC1_CHANNEL_8);
    cp_voltageCount ++;

      if (cp_voltageCount == CP_AVERAGE_COUNT){
        cp_Voltage = (float)esp_adc_cal_raw_to_voltage((cp_voltageAccumulator / CP_AVERAGE_COUNT), &adc1_chars);
        cp_voltageCount = 0;
        cp_voltageAccumulator = 0;
      }

    
    v = cp_Voltage;

  }    

  cp_statVoltage = v;

  tft.setCursor(0, 40, 4);
  tft.printf("CP = %.0f   \r\n", v);

  /* CP Voltage to state map
   * 
   * 3011mV +      = Status A - Standby
   * 2760 - 3010mV = Status B - Vehicle Detected 
   * 2365 - 2620mV = Status C - Ready (charging)
   * 2000 - 2250mV = Status D - Charging with ventilation
   * 0V            = Status E - No power 
   *               = Status F - Error             
   * 
   */

  // STANDBY
  if (v >= 3011.0){
    if (cp_state != _cp_state::STANDBY){
      state_changed = true;
      cp_state = _cp_state::STANDBY;
      tft.printf("Standby                  ");
      unknown_count = 0;
    }
  }

  // VEHICLE_DETECTED
  else if ((v >= 2760.0) && (v <= 3010.0)){
    if (cp_state != _cp_state::VEHICLE_DETECTED){
      state_changed = true;
      cp_state = _cp_state::VEHICLE_DETECTED;
      tft.printf("Vehicle detected");
      unknown_count = 0;
    }
  }

  // CHARGE
  else if ((v >= 2365.0) && (v <= 2620.0)){
    if (cp_state != _cp_state::CHARGE){
      state_changed = true;
      cp_state = _cp_state::CHARGE;
      tft.printf("Ready           ");
      unknown_count = 0;
    }
  }

  // VENT_CHARGE
  else if ((v >= 2000.0) && (v <= 2250.0)){
    if (cp_state != _cp_state::VENT_CHARGE){
      state_changed = true;
      cp_state = _cp_state::VENT_CHARGE;
      tft.printf("Ready-v         ");
      unknown_count = 0;
    }
  }

  // FAULT
  //else if ((v >= 2000) && (v <= 22250)){
  //  if (cp_state != _cp_state::FAULT){
  //    state_changed = true;
  //    cp_state == _cp_state::FAULT;
  //    tft.printf("Fault");
  //  }
  //}

  // Undefined 
  else{
    if (cp_state != _cp_state::UNKNOWN){

      if (unknown_count >= 50)
      {
        state_changed = true;
        cp_state = _cp_state::UNKNOWN;
        tft.printf("CP Invalid");
      }
      else
        unknown_count ++;
    }
  }
  
  if (state_changed)
    Serial0.printf("CP V = %.0f act = %.0f %d\r\n", cp_Voltage, cp_activeVoltage, cp_state);

  return state_changed;
}

void loop() 
{
  long now = millis();

  if (get_cp_state()){
    Serial0.printf("CP State changed to %d\r\n", cp_state);

    switch (cp_state)
    {

    // Vehicle Detected  
    // Set CP pulse width to desired charge rate
    // But don't apply power yet
    case _cp_state::VEHICLE_DETECTED:
      tft.setCursor(0, 40, 4);
      tft.printf("\r\nDetected                      ");
      cp_dutyCycle = _charge_rate::ON_10A;  
      digitalWrite(PWR_ENABLE, LOW);        
      ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);
      break;

    // Vehicle ready, apply power
    case _cp_state::CHARGE:
      tft.setCursor(0, 40, 4);
      tft.printf("\r\nCharging                      ");
      digitalWrite(PWR_ENABLE, HIGH);       
      cp_dutyCycle = _charge_rate::ON_10A; 
      ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);
      break;

    // Vehicle ready, apply power
    case _cp_state::VENT_CHARGE:
      tft.setCursor(0, 40, 4);
      tft.printf("\r\nVent Charging       ");
      digitalWrite(PWR_ENABLE, HIGH); 
      cp_dutyCycle = _charge_rate::ON_10A; 
      ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);
      break;

    case _cp_state::STANDBY:
    case _cp_state::FAULT:
    case _cp_state::NO_POWER:
    case _cp_state::UNKNOWN:
      cp_dutyCycle = _charge_rate::OFF; 
      digitalWrite(PWR_ENABLE, LOW); 
      digitalWrite(AUX_ENABLE, LOW); 
      ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);
      break;

    default:
      Serial0.printf("CP Unknown State\r\n");
      cp_dutyCycle = _charge_rate::OFF; 
      ledcWrite(cp_pwmChannel, (uint32_t) cp_dutyCycle);
      break;
    }
  }

  do_WiFi();
  
  delay(10);
}

