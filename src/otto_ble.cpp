#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Otto.h>
#include <cctype>
#include <esp_system.h>
#include <cstdlib>
#include <cstring>
#ifdef OTTO_SERVO_BACKEND_D031_UART
#include "D031ServoBus.h"
#endif

Otto Otto;

// --- Servos (PWM): 左腿 / 右腿 / 左脚 / 右脚 ---
constexpr int PIN_LEFT_LEG = 6;
constexpr int PIN_RIGHT_LEG = 7;
constexpr int PIN_LEFT_FOOT = 8;
constexpr int PIN_RIGHT_FOOT = 9;

// --- TFT eye reserved (single display, not initialized yet) ---
constexpr int PIN_TFT_MOSI = 3;
constexpr int PIN_TFT_DC = 4;
constexpr int PIN_TFT_CS = 5;

// --- Command UART (no BLE fallback) ---
constexpr int PIN_CMD_TX = 10;
constexpr int PIN_CMD_RX = 11;
constexpr uint32_t CMD_UART_BAUD = 115200;

constexpr int PIN_BUZZER_DISABLED = -1;

#ifdef OTTO_SERVO_BACKEND_D031_UART
constexpr int PIN_SERVO_BUS_IO = 8;
constexpr uint32_t SERVO_BUS_BAUD = 115200;
constexpr uint8_t SERVO_BUS_DEFAULT_SPEED = 50;
constexpr bool D031_TEST_MODE = true;
HardwareSerial ServoBusSerial(2);
D031ServoBus ServoBus;
bool servoBusReady = false;
#endif

constexpr char BLE_DEVICE_NAME[] = "OttoDIY-BLE";
constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr size_t COMMAND_BUFFER_SIZE = 128;

HardwareSerial CmdSerial(1);

BLECharacteristic *txCharacteristic = nullptr;
bool bleConnected = false;
char rxBuffer[COMMAND_BUFFER_SIZE] = {};
char pendingCommand[COMMAND_BUFFER_SIZE] = {};
size_t rxLength = 0;
unsigned long lastRxByteAtMs = 0;
volatile bool commandReady = false;
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

int T = 1000;
int moveId = 0;
int moveSize = 15;

void sendAck();
void sendFinalAck();
void handleCommand(char *line);
void moveRobot(int selectedMoveId);
void appendCommandChar(char c);
void configurePins();
void initLog(const char *message);
void printStartupCode();
void finalizePendingCommand();
#ifdef OTTO_SERVO_BACKEND_D031_UART
void receiveUartServo(char **context);
void initServoBus();
bool isD031TestMode();
bool isOttoCommandDisabledInTestMode(char cmd);
#endif

constexpr int kUsedPins[] = {
    PIN_TFT_MOSI, PIN_TFT_DC, PIN_TFT_CS,
    PIN_LEFT_LEG, PIN_RIGHT_LEG, PIN_LEFT_FOOT, PIN_RIGHT_FOOT,
    PIN_CMD_TX, PIN_CMD_RX,
#ifdef OTTO_SERVO_BACKEND_D031_UART
    PIN_SERVO_BUS_IO,
#endif
};

bool isUsedPin(int pin) {
  for (int used : kUsedPins) {
    if (pin == used) {
      return true;
    }
  }
  return false;
}

void configurePins() {
  // TFT 单眼预留脚：高阻输入，避免与后续 SPI 屏冲突
  pinMode(PIN_TFT_MOSI, INPUT);
  pinMode(PIN_TFT_DC, INPUT);
  pinMode(PIN_TFT_CS, INPUT);

  // 其余未使用 GPIO：高阻输入
  for (int pin = 0; pin <= 21; ++pin) {
    if (!isUsedPin(pin)) {
      pinMode(pin, INPUT);
    }
  }
}

void appendCommandChar(char c) {
  if (c == '\r' || c == '\n') {
    finalizePendingCommand();
    return;
  }

  if (rxLength < COMMAND_BUFFER_SIZE - 1) {
    rxBuffer[rxLength++] = c;
    rxBuffer[rxLength] = '\0';
    lastRxByteAtMs = millis();
  } else {
    rxLength = 0;
    rxBuffer[0] = '\0';
  }
}

void finalizePendingCommand() {
  if (rxLength == 0) {
    return;
  }

  portENTER_CRITICAL(&commandMux);
  if (!commandReady) {
    memcpy(pendingCommand, rxBuffer, rxLength);
    pendingCommand[rxLength] = '\0';
    commandReady = true;
  }
  portEXIT_CRITICAL(&commandMux);

  rxLength = 0;
  rxBuffer[0] = '\0';
}

void pollCommandUart() {
  while (CmdSerial.available() > 0) {
    appendCommandChar(static_cast<char>(CmdSerial.read()));
  }

  // 兼容部分串口工具未发送 CR/LF：超时后自动提交
  if (rxLength > 0 && (millis() - lastRxByteAtMs) > 40) {
    finalizePendingCommand();
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    Serial.println(F("BLE connected"));
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    Serial.println(F("BLE disconnected"));
    server->startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    auto value = characteristic->getValue();
    for (size_t i = 0; i < value.length(); ++i) {
      appendCommandChar(value[i]);
    }
  }
};

char *nextArg(char **context) {
  return strtok_r(nullptr, " ", context);
}

bool parseIntArg(char **context, int &value) {
  char *arg = nextArg(context);
  if (arg == nullptr) {
    return false;
  }
  value = atoi(arg);
  return true;
}

void replyLine(const char *message) {
  Serial.println(message);
  CmdSerial.println(message);

  if (bleConnected && txCharacteristic != nullptr) {
    char line[32];
    snprintf(line, sizeof(line), "%s\r\n", message);
    txCharacteristic->setValue(reinterpret_cast<uint8_t *>(line), strlen(line));
    txCharacteristic->notify();
  }
}

void initLog(const char *message) {
  Serial.println(message);
  CmdSerial.println(message);
}

void printStartupCode() {
  const uint32_t startupCode = esp_random();
  char line[48];
  snprintf(line, sizeof(line), "STARTUP-CODE:%08lX", static_cast<unsigned long>(startupCode));
  initLog(line);
}

void sendAck() {
  delay(30);
  replyLine("&&A%%");
}

void sendFinalAck() {
  delay(30);
  replyLine("&&F%%");
}

void receiveStop() {
  sendAck();
  Otto.home();
  moveId = 0;
  sendFinalAck();
}

void receiveLED(char **context) {
  sendAck();
  Otto.home();
  (void)context;
  Serial.println(F("L: matrix disabled; TFT eyes not implemented yet"));
  sendFinalAck();
}

void receiveBuzzer(char **context) {
  sendAck();
  Otto.home();
  (void)context;
  Serial.println(F("T: buzzer disabled; use Xiaozhi speaker later"));
  sendFinalAck();
}

void receiveTrims(char **context) {
  sendAck();
  Otto.home();

  int trimYL = 0;
  int trimYR = 0;
  int trimRL = 0;
  int trimRR = 0;
  if (parseIntArg(context, trimYL) && parseIntArg(context, trimYR) &&
      parseIntArg(context, trimRL) && parseIntArg(context, trimRR)) {
    Otto.setTrims(trimYL, trimYR, trimRL, trimRR);
    Serial.println(F("Trim storage disabled; trims are RAM-only."));
  } else {
    Serial.println(F("C: invalid trim arguments"));
  }

  sendFinalAck();
}

void receiveServo(char **context) {
  sendAck();
  moveId = 30;

  int servoYL = 0;
  int servoYR = 0;
  int servoRL = 0;
  int servoRR = 0;
  if (parseIntArg(context, servoYL) && parseIntArg(context, servoYR) &&
      parseIntArg(context, servoRL) && parseIntArg(context, servoRR)) {
    int servoPos[4] = {servoYL, servoYR, servoRL, servoRR};
    Otto._moveServos(200, servoPos);
  } else {
    Serial.println(F("G: invalid servo arguments"));
  }

  sendFinalAck();
}

void receiveMovement(char **context) {
  sendAck();

  if (Otto.getRestState()) {
    Otto.setRestState(false);
  }

  if (!parseIntArg(context, moveId)) {
    Serial.println(F("M: invalid move id"));
    moveId = 0;
  }

  if (!parseIntArg(context, T)) {
    T = 1000;
  }

  if (!parseIntArg(context, moveSize)) {
    moveSize = 15;
  }
}

void receiveGesture(char **context) {
  sendAck();
  Otto.home();

  int gesture = 0;
  parseIntArg(context, gesture);

  switch (gesture) {
    case 1: Otto.playGesture(OttoHappy); break;
    case 2: Otto.playGesture(OttoSuperHappy); break;
    case 3: Otto.playGesture(OttoSad); break;
    case 4: Otto.playGesture(OttoSleeping); break;
    case 5: Otto.playGesture(OttoFart); break;
    case 6: Otto.playGesture(OttoConfused); break;
    case 7: Otto.playGesture(OttoLove); break;
    case 8: Otto.playGesture(OttoAngry); break;
    case 9: Otto.playGesture(OttoFretful); break;
    case 10: Otto.playGesture(OttoMagic); break;
    case 11: Otto.playGesture(OttoWave); break;
    case 12: Otto.playGesture(OttoVictory); break;
    case 13: Otto.playGesture(OttoFail); break;
    default: break;
  }

  sendFinalAck();
}

void receiveSing(char **context) {
  sendAck();
  Otto.home();

  int song = 0;
  parseIntArg(context, song);

  switch (song) {
    case 1: Otto.sing(S_connection); break;
    case 2: Otto.sing(S_disconnection); break;
    case 3: Otto.sing(S_surprise); break;
    case 4: Otto.sing(S_OhOoh); break;
    case 5: Otto.sing(S_OhOoh2); break;
    case 6: Otto.sing(S_cuddly); break;
    case 7: Otto.sing(S_sleeping); break;
    case 8: Otto.sing(S_happy); break;
    case 9: Otto.sing(S_superHappy); break;
    case 10: Otto.sing(S_happy_short); break;
    case 11: Otto.sing(S_sad); break;
    case 12: Otto.sing(S_confused); break;
    case 13: Otto.sing(S_fart1); break;
    case 14: Otto.sing(S_fart2); break;
    case 15: Otto.sing(S_fart3); break;
    case 16: Otto.sing(S_mode1); break;
    case 17: Otto.sing(S_mode2); break;
    case 18: Otto.sing(S_mode3); break;
    case 19: Otto.sing(S_buttonPushed); break;
    default: break;
  }

  sendFinalAck();
}

#ifdef OTTO_SERVO_BACKEND_D031_UART
void initServoBus() {
  servoBusReady = ServoBus.begin(ServoBusSerial, PIN_SERVO_BUS_IO, SERVO_BUS_BAUD);
  if (servoBusReady) {
    initLog("D031 bus init: OK");
    initLog("  Bus: UART2 single-wire GPIO8 @ 115200");
  } else {
    initLog("D031 bus init: FAIL");
  }
}

bool isD031TestMode() {
  return D031_TEST_MODE;
}

bool isOttoCommandDisabledInTestMode(char cmd) {
  return cmd != 'U';
}

void receiveUartServo(char **context) {
  sendAck();
  char *op = nextArg(context);
  if (op == nullptr) {
    initLog("U usage: U I | U P <id> | U E <id> <0|1> | U W <id> <target> <speed> | U R <id>");
    sendFinalAck();
    return;
  }

  const char action = static_cast<char>(toupper(static_cast<unsigned char>(op[0])));
  if (action == 'I') {
    initServoBus();
    sendFinalAck();
    return;
  }

  if (!servoBusReady) {
    initLog("U: bus not ready, run 'U I' first");
    sendFinalAck();
    return;
  }

  int id = 0;
  if (!parseIntArg(context, id) || id < 1 || id > 252) {
    initLog("U: invalid id (1..252)");
    sendFinalAck();
    return;
  }

  char line[96];
  if (action == 'P') {
    uint8_t status = 0;
    const bool ok = ServoBus.ping(static_cast<uint8_t>(id), &status);
    snprintf(line, sizeof(line), "U P id=%d %s status=0x%02X", id, ok ? "OK" : "FAIL", status);
    initLog(line);
    sendFinalAck();
    return;
  }

  if (action == 'E') {
    int enable = 0;
    if (!parseIntArg(context, enable)) {
      initLog("U E: missing enable (0/1)");
      sendFinalAck();
      return;
    }
    uint8_t status = 0;
    const bool ok = ServoBus.enableTorque(static_cast<uint8_t>(id), enable != 0, &status);
    snprintf(line, sizeof(line), "U E id=%d en=%d %s status=0x%02X", id, enable != 0 ? 1 : 0, ok ? "OK" : "FAIL", status);
    initLog(line);
    sendFinalAck();
    return;
  }

  if (action == 'W') {
    int target = 0;
    int speed = SERVO_BUS_DEFAULT_SPEED;
    if (!parseIntArg(context, target)) {
      initLog("U W: missing target (-700..700)");
      sendFinalAck();
      return;
    }
    if (target < -700) target = -700;
    if (target > 700) target = 700;
    if (!parseIntArg(context, speed)) {
      speed = SERVO_BUS_DEFAULT_SPEED;
    }
    if (speed < 0) speed = 0;
    if (speed > 100) speed = 100;

    uint8_t status = 0;
    const bool ok = ServoBus.writeTarget(static_cast<uint8_t>(id), static_cast<int16_t>(target), static_cast<uint8_t>(speed), &status);
    snprintf(line, sizeof(line), "U W id=%d target=%d speed=%d %s status=0x%02X", id, target, speed, ok ? "OK" : "FAIL", status);
    initLog(line);
    sendFinalAck();
    return;
  }

  if (action == 'R') {
    int16_t pos = 0;
    uint8_t status = 0;
    const bool ok = ServoBus.readPosition(static_cast<uint8_t>(id), pos, &status);
    snprintf(line, sizeof(line), "U R id=%d pos=%d %s status=0x%02X", id, static_cast<int>(pos), ok ? "OK" : "FAIL", status);
    initLog(line);
    sendFinalAck();
    return;
  }

  initLog("U: unknown action");
  sendFinalAck();
}
#endif

void handleCommand(char *line) {
  char *context = nullptr;
  char *token = strtok_r(line, " ", &context);
  if (token == nullptr || token[0] == '\0') {
    return;
  }

  Serial.print(F("Command: "));
  Serial.println(token);

  const char cmd = static_cast<char>(toupper(static_cast<unsigned char>(token[0])));
#ifdef OTTO_SERVO_BACKEND_D031_UART
  if (isD031TestMode() && isOttoCommandDisabledInTestMode(cmd)) {
    sendAck();
    initLog("D031 test mode: Otto PWM motion path disabled; use U commands only");
    sendFinalAck();
    return;
  }
#endif
  switch (cmd) {
    case 'S': receiveStop(); break;
    case 'L': receiveLED(&context); break;
    case 'T': receiveBuzzer(&context); break;
    case 'M': receiveMovement(&context); break;
    case 'H': receiveGesture(&context); break;
    case 'K': receiveSing(&context); break;
    case 'C': receiveTrims(&context); break;
    case 'G': receiveServo(&context); break;
#ifdef OTTO_SERVO_BACKEND_D031_UART
    case 'U': receiveUartServo(&context); break;
#endif
    default: receiveStop(); break;
  }
}

void moveRobot(int selectedMoveId) {
  bool manualMode = false;

  switch (selectedMoveId) {
    case 0: Otto.home(); break;
    case 1: Otto.walk(1, T, 1); break;
    case 2: Otto.walk(1, T, -1); break;
    case 3: Otto.turn(1, T, 1); break;
    case 4: Otto.turn(1, T, -1); break;
    case 5: Otto.updown(1, T, moveSize); break;
    case 6: Otto.moonwalker(1, T, moveSize, 1); break;
    case 7: Otto.moonwalker(1, T, moveSize, -1); break;
    case 8: Otto.swing(1, T, moveSize); break;
    case 9: Otto.crusaito(1, T, moveSize, 1); break;
    case 10: Otto.crusaito(1, T, moveSize, -1); break;
    case 11: Otto.jump(1, T); break;
    case 12: Otto.flapping(1, T, moveSize, 1); break;
    case 13: Otto.flapping(1, T, moveSize, -1); break;
    case 14: Otto.tiptoeSwing(1, T, moveSize); break;
    case 15: Otto.bend(1, T, 1); break;
    case 16: Otto.bend(1, T, -1); break;
    case 17: Otto.shakeLeg(1, T, 1); break;
    case 18: Otto.shakeLeg(1, T, -1); break;
    case 19: Otto.jitter(1, T, moveSize); break;
    case 20: Otto.ascendingTurn(1, T, moveSize); break;
    default: manualMode = true; break;
  }

  if (!manualMode) {
    sendFinalAck();
    // 单次执行：动作结束后回中位，避免 loop 反复 moveRobot(moveId)
    if (selectedMoveId != 0) {
      Otto.home();
      moveId = 0;
    }
  }
}

void setupBle() {
  BLEDevice::init(BLE_DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);

  txCharacteristic = service->createCharacteristic(
      BLE_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
      BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.print(F("BLE advertising as "));
  Serial.println(BLE_DEVICE_NAME);
}

void setup() {
  configurePins();

  // USB CDC 日志（见 otto_ble.ini: ARDUINO_USB_CDC_ON_BOOT=1）
  Serial.begin(115200);
  delay(300);

  CmdSerial.begin(CMD_UART_BAUD, SERIAL_8N1, PIN_CMD_RX, PIN_CMD_TX);
  initLog("UART1 init: OK");
  initLog("UART1 pins: TX=GPIO10 RX=GPIO11 BAUD=115200");
  printStartupCode();
#ifdef OTTO_SERVO_BACKEND_D031_UART
  initServoBus();
#endif

#ifdef OTTO_SERVO_BACKEND_D031_UART
  if (!isD031TestMode()) {
    Otto.init(PIN_LEFT_LEG, PIN_RIGHT_LEG, PIN_LEFT_FOOT, PIN_RIGHT_FOOT, false,
              PIN_BUZZER_DISABLED);
    Otto.enableServoLimit();
  }
#else
  Otto.init(PIN_LEFT_LEG, PIN_RIGHT_LEG, PIN_LEFT_FOOT, PIN_RIGHT_FOOT, false,
            PIN_BUZZER_DISABLED);
  Otto.enableServoLimit();
#endif

  setupBle();

#ifdef OTTO_SERVO_BACKEND_D031_UART
  if (!isD031TestMode()) {
    Otto.home();
  }
#else
  Otto.home();
#endif

  initLog("Otto ESP32-S3 ready");
  initLog("  Log: USB Serial @ 115200");
  initLog("  Cmd: UART1 GPIO10=TX GPIO11=RX @ 115200");
  initLog("  Matrix: disabled (TFT eye GPIO3/4/5 planned)");
  initLog("  Buzzer: disabled");
#ifdef OTTO_SERVO_BACKEND_D031_UART
  initLog("  ServoBus test cmd: U I|P|E|W|R");
  if (isD031TestMode()) {
    initLog("  D031 test mode: Otto PWM init disabled");
    initLog("  PWM channels GPIO6/7/8/9 are not attached in this mode");
  }
#endif
}

void loop() {
  pollCommandUart();

  char command[COMMAND_BUFFER_SIZE] = {};

  portENTER_CRITICAL(&commandMux);
  if (commandReady) {
    strncpy(command, pendingCommand, sizeof(command) - 1);
    commandReady = false;
    pendingCommand[0] = '\0';
  }
  portEXIT_CRITICAL(&commandMux);

  if (command[0] != '\0') {
    handleCommand(command);
  }

#ifdef OTTO_SERVO_BACKEND_D031_UART
  if (!isD031TestMode()) {
    if (!Otto.getRestState()) {
      moveRobot(moveId);
    }
  }
#else
  if (!Otto.getRestState()) {
    moveRobot(moveId);
  }
#endif
}
