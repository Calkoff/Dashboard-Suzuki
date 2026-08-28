/*
  MotoStealth 3.0 — Bluetooth + Настройки через приложение
  - Bluetooth Classic (SPP)
  - Команды: B=, VOLT_CALIB=, TEMP_F=, VOLT_F=, GET_SETTINGS, RESET
  - Передачи, температура, напряжение, VC
*/

#include <BluetoothSerial.h>
#include <math.h>
#include <Preferences.h>

// ===== ПИНЫ =====
#define GEAR_N   16
#define GEAR_1   17
#define GEAR_2   18
#define GEAR_3   19
#define GEAR_4   21
#define GEAR_5   22
#define GEAR_6   23
#define GEAR_VC  25
#define NTC_PIN  32
#define VOLT_PIN 33

// ===== ПАРАМЕТРЫ NTC (по умолчанию) =====
#define NTC_NOMINAL         50000.0
#define SERIES_RESISTOR     50000.0
#define DEFAULT_B_COEFFICIENT   5100.0
#define TEMP_NOMINAL        25.0

// ===== ПАРАМЕТРЫ НАПРЯЖЕНИЯ (по умолчанию) =====
#define VOLT_DIVIDER        11.0
#define DEFAULT_VOLT_CALIB      1.148
#define VOLT_REF            3.3
#define ADC_MAX             4095.0

// ===== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ =====
int currentGear = -1;
float temperature = 20.0;
float voltage = 12.5;
bool vcEnabled = false;
String inputBuffer = "";

// ===== НАСТРОЙКИ (хранятся в Preferences) =====
float B_COEFFICIENT = DEFAULT_B_COEFFICIENT;
float VOLT_CALIB = DEFAULT_VOLT_CALIB;
uint8_t tempFilterDepth = 8;
uint8_t voltFilterDepth = 8;

// ===== ФИЛЬТРЫ =====
float *tempBuffer = nullptr;
float *voltBuffer = nullptr;
uint8_t tempIdx = 0, voltIdx = 0;
bool tempFilled = false, voltFilled = false;

// ===== BLUETOOTH =====
BluetoothSerial SerialBT;
Preferences preferences;

// ===== ПРОТОТИПЫ ФУНКЦИЙ =====
void processSettingsCommand(String cmd);
void processOBDCommand(String cmd);
void sendMotoData();
float readNTC();
float readVoltage();
int readGear();
bool readVC();
void loadSettings();
void saveSettings();
void showSettings();

// ===== ЧТЕНИЕ ПЕРЕДАЧ =====
int readGear() {
  if (digitalRead(GEAR_N) == LOW) return 0;
  if (digitalRead(GEAR_1) == LOW) return 1;
  if (digitalRead(GEAR_2) == LOW) return 2;
  if (digitalRead(GEAR_3) == LOW) return 3;
  if (digitalRead(GEAR_4) == LOW) return 4;
  if (digitalRead(GEAR_5) == LOW) return 5;
  if (digitalRead(GEAR_6) == LOW) return 6;
  return -1;
}

// ===== ТЕМПЕРАТУРА (NTC) =====
float readNTC() {
  int raw = analogRead(NTC_PIN);
  if (raw <= 0 || raw >= ADC_MAX) return NAN;
  float ratio = (float)raw / ADC_MAX;
  float resistance = SERIES_RESISTOR * ratio / (1.0 - ratio);
  float steinhart = log(resistance / NTC_NOMINAL) / B_COEFFICIENT;
  steinhart += 1.0 / (TEMP_NOMINAL + 273.15);
  float tC = (1.0 / steinhart) - 273.15;
  return (tC > -40 && tC < 150) ? tC : NAN;
}

float getFilteredTemp() {
  float t = readNTC();
  if (isnan(t)) return temperature;
  if (!tempBuffer) return t;
  tempBuffer[tempIdx] = t;
  if (++tempIdx >= tempFilterDepth) { tempIdx = 0; tempFilled = true; }
  int count = tempFilled ? tempFilterDepth : tempIdx;
  float sum = 0; for (int i = 0; i < count; i++) sum += tempBuffer[i];
  return sum / count;
}

// ===== НАПРЯЖЕНИЕ =====
float readVoltage() {
  int raw = analogRead(VOLT_PIN);
  if (raw <= 0 || raw >= ADC_MAX) return NAN;
  float vADC = (raw / ADC_MAX) * VOLT_REF;
  return vADC * VOLT_DIVIDER * VOLT_CALIB;
}

float getFilteredVoltage() {
  float v = readVoltage();
  if (isnan(v)) return voltage;
  if (!voltBuffer) return v;
  voltBuffer[voltIdx] = v;
  if (++voltIdx >= voltFilterDepth) { voltIdx = 0; voltFilled = true; }
  int count = voltFilled ? voltFilterDepth : voltIdx;
  float sum = 0; for (int i = 0; i < count; i++) sum += voltBuffer[i];
  return sum / count;
}

// ===== VC =====
bool readVC() {
  return digitalRead(GEAR_VC) == LOW;
}

// ===== ЗАГРУЗКА / СОХРАНЕНИЕ НАСТРОЕК =====
void loadSettings() {
  preferences.begin("MotoStealth", false);
  B_COEFFICIENT = preferences.getFloat("B_COEF", DEFAULT_B_COEFFICIENT);
  VOLT_CALIB = preferences.getFloat("VOLT_CAL", DEFAULT_VOLT_CALIB);
  tempFilterDepth = preferences.getUChar("TEMP_F", 8);
  voltFilterDepth = preferences.getUChar("VOLT_F", 8);
  preferences.end();

  if (tempBuffer) delete[] tempBuffer;
  if (voltBuffer) delete[] voltBuffer;
  
  tempBuffer = new float[tempFilterDepth]();
  voltBuffer = new float[voltFilterDepth]();
}

void saveSettings() {
  preferences.begin("MotoStealth", false);
  preferences.putFloat("B_COEF", B_COEFFICIENT);
  preferences.putFloat("VOLT_CAL", VOLT_CALIB);
  preferences.putUChar("TEMP_F", tempFilterDepth);
  preferences.putUChar("VOLT_F", voltFilterDepth);
  preferences.end();
}

void showSettings() {
  Serial.println("=== MotoStealth 3.0 ===");
  Serial.print("B_COEFFICIENT: "); Serial.println(B_COEFFICIENT);
  Serial.print("VOLT_CALIB: "); Serial.println(VOLT_CALIB);
  Serial.print("TEMP_FILTER: "); Serial.println(tempFilterDepth);
  Serial.print("VOLT_FILTER: "); Serial.println(voltFilterDepth);
  Serial.println("========================");
}

// ===== ОТПРАВКА ДАННЫХ В ПРИЛОЖЕНИЕ =====
void sendMotoData() {
  String data = "GEAR:" + String(currentGear) + 
                " TEMP:" + String(temperature, 1) + 
                " VOLT:" + String(voltage, 2) + 
                " VC:" + String(vcEnabled ? 1 : 0) + "\r\n";
  SerialBT.print(data);
}

// ===== ОБРАБОТЧИК КОМАНД НАСТРОЕК =====
void processSettingsCommand(String cmd) {
  String response = "";
  bool handled = true;

  if (cmd.startsWith("B=")) {
    B_COEFFICIENT = cmd.substring(2).toFloat();
    response = "OK: B=" + String(B_COEFFICIENT);
  }
  else if (cmd.startsWith("VOLT_CALIB=")) {
    VOLT_CALIB = cmd.substring(11).toFloat();
    response = "OK: VOLT_CALIB=" + String(VOLT_CALIB);
  }
  else if (cmd.startsWith("TEMP_F=")) {
    tempFilterDepth = cmd.substring(7).toInt();
    loadSettings(); // пересоздаст буферы
    response = "OK: TEMP_F=" + String(tempFilterDepth);
  }
  else if (cmd.startsWith("VOLT_F=")) {
    voltFilterDepth = cmd.substring(7).toInt();
    loadSettings();
    response = "OK: VOLT_F=" + String(voltFilterDepth);
  }
  else if (cmd == "GET_SETTINGS") {
    response = "SETTINGS: B=" + String(B_COEFFICIENT) +
               ", VOLT_CALIB=" + String(VOLT_CALIB) +
               ", TEMP_F=" + String(tempFilterDepth) +
               ", VOLT_F=" + String(voltFilterDepth);
  }
  else if (cmd == "RESET") {
    B_COEFFICIENT = DEFAULT_B_COEFFICIENT;
    VOLT_CALIB = DEFAULT_VOLT_CALIB;
    tempFilterDepth = 8;
    voltFilterDepth = 8;
    saveSettings();
    loadSettings();
    response = "OK: RESET";
  }
  else {
    handled = false;
    response = "ERROR: Unknown command";
  }

  if (handled) {
    SerialBT.println(response);
    Serial.println(response);
  }
}

// ===== OBD ОБРАБОТЧИК (ELM327) =====
void processOBDCommand(String cmd) {
  String response = "NO DATA\r\n";
  bool handled = false;

  if (cmd == "ATZ") {
    response = "ELM327 v1.5\r\n>";
    handled = true;
  }
  else if (cmd == "ATE0" || cmd == "ATE1" || cmd == "ATL0" || cmd == "ATL1" ||
           cmd == "ATH0" || cmd == "ATH1" || cmd == "ATS0" || cmd == "ATS1" ||
           cmd.startsWith("ATST") || cmd.startsWith("ATAT") || cmd.startsWith("AT ")) {
    response = "OK\r\n>";
    handled = true;
  }
  else if (cmd == "ATRV") {
    float v = readVoltage();
    if (!isnan(v)) {
      char buf[20];
      sprintf(buf, "%.2fV\r\n>", v);
      response = String(buf);
    } else {
      response = "12.00V\r\n>";
    }
    handled = true;
  }
  else if (cmd == "ATDP") {
    response = "ISO 9141-2\r\n>";
    handled = true;
  }
  else if (cmd == "ATI" || cmd == "AT@1") {
    response = "ELM327 v1.5\r\n>";
    handled = true;
  }
  else if (cmd == "ATSP0" || cmd.startsWith("ATSP")) {
    response = "OK\r\n>";
    handled = true;
  }
  else if (cmd.startsWith("AT")) {
    response = "OK\r\n>";
    handled = true;
  }
  else if (cmd == "0100") {
    response = "41 00 98 3F A0 04\r\n>";
    handled = true;
  }
  else if (cmd == "0104") {
    response = "41 04 00\r\n>";
    handled = true;
  }
  else if (cmd == "0105") {
    float temp = getFilteredTemp();
    if (!isnan(temp)) {
      int value = (int)(temp + 40.0);
      char buf[20];
      sprintf(buf, "41 05 %02X\r\n>", value);
      response = String(buf);
    } else {
      response = "41 05 40\r\n>";
    }
    handled = true;
  }
  else if (cmd == "010C") {
    response = "41 0C 00 00\r\n>";
    handled = true;
  }
  else if (cmd == "010D") {
    response = "41 0D 00\r\n>";
    handled = true;
  }
  else if (cmd == "010F") {
    int value = vcEnabled ? 140 : 40;
    char buf[20];
    sprintf(buf, "41 0F %02X\r\n>", value);
    response = String(buf);
    handled = true;
  }
  else if (cmd == "0142") {
    float v = getFilteredVoltage();
    if (!isnan(v)) {
      int value = (int)(v * 1000);
      char buf[20];
      sprintf(buf, "41 42 %02X %02X\r\n>", (value >> 8) & 0xFF, value & 0xFF);
      response = String(buf);
    } else {
      response = "41 42 0C 80\r\n>";
    }
    handled = true;
  }
  else if (cmd == "015E") {
    int gear = readGear();
    if (gear >= 0 && gear <= 6) {
      int value = gear * 10;
      char buf[20];
      sprintf(buf, "41 5E %02X\r\n>", value);
      response = String(buf);
    } else {
      response = "41 5E 00\r\n>";
    }
    handled = true;
  }
  else if (cmd.startsWith("01")) {
    String pid = cmd.substring(2, 4);
    response = "41 " + pid + " 00\r\n>";
    handled = true;
  }
  else if (cmd.startsWith("03")) {
    response = "43 00 00 00 00\r\n>";
    handled = true;
  }
  else if (cmd.startsWith("04")) {
    response = "44\r\n>";
    handled = true;
  }

  if (handled && response.length() > 0) {
    SerialBT.print(response);
  } else if (!handled) {
    SerialBT.print("NO DATA\r\n>");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== MotoStealth 3.0 ===");

  pinMode(GEAR_N, INPUT_PULLUP);
  pinMode(GEAR_1, INPUT_PULLUP);
  pinMode(GEAR_2, INPUT_PULLUP);
  pinMode(GEAR_3, INPUT_PULLUP);
  pinMode(GEAR_4, INPUT_PULLUP);
  pinMode(GEAR_5, INPUT_PULLUP);
  pinMode(GEAR_6, INPUT_PULLUP);
  pinMode(GEAR_VC, INPUT_PULLUP);
  pinMode(NTC_PIN, INPUT);
  pinMode(VOLT_PIN, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadSettings();

  SerialBT.begin("MotoDashOBD");
  Serial.println("Bluetooth ready: MotoDashOBD");
  Serial.println("Commands: B=, VOLT_CALIB=, TEMP_F=, VOLT_F=, GET_SETTINGS, RESET");
}

// ===== LOOP =====
void loop() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 100) {
    temperature = getFilteredTemp();
    voltage = getFilteredVoltage();
    currentGear = readGear();
    vcEnabled = readVC();
    lastUpdate = millis();
  }

  static unsigned long lastSend = 0;
  if (SerialBT.hasClient() && millis() - lastSend > 200) {
    sendMotoData();
    lastSend = millis();
  }

  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\r' || c == '\n') {
      if (inputBuffer.length() > 0) {
        inputBuffer.trim();
        if (inputBuffer.startsWith("B=") ||
            inputBuffer.startsWith("VOLT_CALIB=") ||
            inputBuffer.startsWith("TEMP_F=") ||
            inputBuffer.startsWith("VOLT_F=") ||
            inputBuffer == "GET_SETTINGS" ||
            inputBuffer == "RESET") {
          processSettingsCommand(inputBuffer);
        } else {
          processOBDCommand(inputBuffer);
        }
        inputBuffer = "";
      }
    } else {
      inputBuffer += c;
    }
  }

  delay(10);
}
