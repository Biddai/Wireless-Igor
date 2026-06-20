#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>
#include <nrf.h>
#include <nrf_gpio.h>

// Set to 0 for the battery-focused release build.
#define DEBUG_SERIAL 0

using namespace Adafruit_LittleFS_Namespace;

#if DEBUG_SERIAL
  #define DEBUG_PRINT(value) Serial.print(value)
  #define DEBUG_PRINTLN(value) Serial.println(value)
#else
  #define DEBUG_PRINT(value) do {} while (0)
  #define DEBUG_PRINTLN(value) do {} while (0)
#endif

// Use the nice!nano v2 board package's native aliases. Its Arduino pin
// numbering is not raw nRF GPIO numbering (for example P1.04 is D8, not 36).
// #ifndef TARGET_NICE_NANO_V2
//   #error "Select Tools > Board > nRFMicro-like Boards > nice!nano v2."
// #endif

constexpr uint8_t OLED_SDA = P0_11;  // D7
constexpr uint8_t OLED_SCL = P1_04;  // D8
constexpr uint8_t CLK = P1_11;       // D12
constexpr uint8_t SW = P1_13;        // D13
constexpr uint8_t DT = P0_10;        // D11

// This OLED has no reset line connected to the controller.
Adafruit_SSD1306 display(128, 64, &Wire, -1);

int flowMinutes = 0;
constexpr char FLOW_FILE[] = "/flow.bin";
constexpr uint32_t FLOW_FILE_MAGIC = 0x464C4F57;  // "FLOW"
struct FlowRecord {
  uint32_t magic;
  uint32_t minutes;
  uint32_t check;
};
bool flowStorageReady = false;

int menuIndex = 0;
const char* const menuOptions[] = {"POMO", "UP", "DOWN", "Reset"};
constexpr uint8_t MENU_OPTION_COUNT = 4;

constexpr unsigned long inactivityLimit = 1UL * 60000UL;
constexpr unsigned long buttonDebounceDelay = 800;
constexpr int8_t ROTARY_TRANSITIONS_PER_TICK = 4;
constexpr unsigned long BATTERY_SAMPLE_INTERVAL_MS = 30000UL;
constexpr uint8_t LOW_BATTERY_PERCENT = 20;

// Version-1 Flow Status service. The characteristic is read-only and notifies
// connected clients with a fixed 12-byte status packet.
constexpr char FLOW_STATUS_SERVICE_UUID[] = "7d4f0001-9f4b-4f4a-8e9b-0a6c1e4f1000";
constexpr char FLOW_STATUS_CHARACTERISTIC_UUID[] = "7d4f0002-9f4b-4f4a-8e9b-0a6c1e4f1000";
constexpr uint8_t FLOW_STATUS_PACKET_SIZE = 12;
BLEService flowStatusService(FLOW_STATUS_SERVICE_UUID);
BLECharacteristic flowStatusCharacteristic(FLOW_STATUS_CHARACTERISTIC_UUID);

enum State {
  MENU,
  IDLE,
  COUNTING_UP,
  COUNTING_DOWN,
  SELECTING_DOWN_DURATION,
  SELECTING_POMO_FOCUS,
  SELECTING_POMO_REST,
  POMODORO_FOCUS,
  POMODORO_REST
};
State currentState = MENU;

int countdownValue = 20;
int initialCountdownValue = 20;
// ponytail: session-only defaults; persist them only if users ask for saved settings.
int pomodoroFocusMinutes = 40;
int pomodoroRestMinutes = 5;
int elapsedMinutes = 0;
unsigned long lastActivityTime = 0;
unsigned long previousMillis = 0;
unsigned long buttonDebounceTime = 0;
uint8_t previousRotaryState = 0;
int8_t rotaryTransitionAccumulator = 0;
unsigned long lastBatterySampleTime = 0;
uint16_t bleStatusSequence = 0;
uint16_t batteryMillivolts = 0;
uint8_t batteryPercentage = 0;
bool lastUsbVbusPresent = false;
bool bleInitialized = false;

void updateDisplay();
void successAnimation();
void debugI2cScan();
String formatFlowLabel();
int centeredTextX(const String& text, int characterWidth, int availableWidth);
uint8_t readRotaryState();
void initBle();
void publishBleStatus();
void stopBle();
void updateBatteryReading(unsigned long now, bool force = false);
uint8_t batteryPercentageFromMillivolts(uint16_t millivolts);
bool usbVbusPresent();
void initFlowStorage();
void saveFlow();
void addFlowMinutes(int minutes);
void resetFlowMinutes();

void setup() {
  pinMode(CLK, INPUT_PULLUP);
  pinMode(DT, INPUT_PULLUP);
  pinMode(SW, INPUT_PULLUP);
  previousRotaryState = readRotaryState();

  // Route I2C to the OLED wiring rather than the package's default Wire pins.
  Wire.setPins(OLED_SDA, OLED_SCL);
  Wire.begin();
  analogReadResolution(10);

#if DEBUG_SERIAL
  Serial.begin(115200);
  // Give a freshly opened USB serial monitor a chance to attach at boot.
  const unsigned long serialWaitStarted = millis();
  while (!Serial && millis() - serialWaitStarted < 2500UL) {
    delay(10);
  }
  DEBUG_PRINTLN(F("\nFlow timer booting"));
  DEBUG_PRINT(F("OLED SDA Arduino/raw pin: "));
  DEBUG_PRINTLN(OLED_SDA);
  DEBUG_PRINT(F("  / "));
  DEBUG_PRINTLN(digitalPinToPinName(OLED_SDA));
  DEBUG_PRINT(F("OLED SCL Arduino/raw pin: "));
  DEBUG_PRINTLN(OLED_SCL);
  DEBUG_PRINT(F("  / "));
  DEBUG_PRINTLN(digitalPinToPinName(OLED_SCL));
  DEBUG_PRINT(F("Encoder CLK/SW/DT Arduino pins: "));
  DEBUG_PRINT(CLK);
  DEBUG_PRINT(F(" / "));
  DEBUG_PRINT(SW);
  DEBUG_PRINT(F(" / "));
  DEBUG_PRINTLN(DT);
  DEBUG_PRINT(F("Initial encoder levels CLK/SW/DT: "));
  DEBUG_PRINT(digitalRead(CLK));
  DEBUG_PRINT(F(" / "));
  DEBUG_PRINT(digitalRead(SW));
  DEBUG_PRINT(F(" / "));
  DEBUG_PRINTLN(digitalRead(DT));
  debugI2cScan();
#endif

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    DEBUG_PRINTLN(F("ERROR: SSD1306 not found at I2C address 0x3C."));
    DEBUG_PRINTLN(F("Check OLED power, GND, SDA/SCL wiring, and its I2C address."));
    while (true) { delay(1); }
  }

  updateBatteryReading(millis(), true);
  initFlowStorage();
  initBle();
  DEBUG_PRINTLN(F("SSD1306 initialized at 0x3C."));
  lastActivityTime = millis();
  updateDisplay();
  DEBUG_PRINTLN(F("Menu rendered."));
}

void debugI2cScan() {
#if DEBUG_SERIAL
  DEBUG_PRINTLN(F("Scanning I2C bus..."));
  uint8_t deviceCount = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      DEBUG_PRINT(F("  I2C device found at 0x"));
      if (address < 16) DEBUG_PRINT('0');
      Serial.println(address, HEX);
      ++deviceCount;
    }
  }
  if (deviceCount == 0) {
    DEBUG_PRINTLN(F("  No I2C devices responded."));
  }
#endif
}

void loop() {
  const unsigned long now = millis();

  const int rotation = getRotation();
  if (rotation != 0) {
    handleRotaryInput(rotation);
  }

  handleButtonPresses(now);
  handleCounting(now);
  updateBatteryReading(now);
  handleInactivity(now);
}

void initBle() {
  Bluefruit.begin(1, 0);
  Bluefruit.setName("Project IGOR Wireless");

  flowStatusService.begin();
  flowStatusCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  flowStatusCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  flowStatusCharacteristic.setFixedLen(FLOW_STATUS_PACKET_SIZE);
  flowStatusCharacteristic.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(flowStatusService);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);

  bleInitialized = true;
  publishBleStatus();
  DEBUG_PRINTLN(F("BLE advertising as Project IGOR Wireless."));
}

void writeLittleEndian16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void writeLittleEndian32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint16_t currentTimerMinutes() {
  if (currentState == COUNTING_UP) return static_cast<uint16_t>(elapsedMinutes);
  if (currentState != MENU && currentState != IDLE) {
    return static_cast<uint16_t>(countdownValue);
  }
  return 0;
}

bool usbVbusPresent() {
  // VBUS detection works for a wall adapter too; Serial only detects a host.
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void initFlowStorage() {
  flowStorageReady = InternalFS.begin();
  if (!flowStorageReady) {
    InternalFS.format();  // First boot: create the otherwise empty filesystem.
    flowStorageReady = InternalFS.begin();
  }
  if (!flowStorageReady) {
    DEBUG_PRINTLN(F("Flow storage unavailable."));
    return;
  }

  File file(InternalFS);
  FlowRecord record = {};
  if (file.open(FLOW_FILE, FILE_O_READ) &&
      file.read(&record, sizeof(record)) == sizeof(record) &&
      record.magic == FLOW_FILE_MAGIC && record.check == ~record.minutes) {
    flowMinutes = static_cast<int>(min(record.minutes, 2147483647UL));
  }
  file.close();
}

void saveFlow() {
  if (!flowStorageReady) return;

  File file(InternalFS);
  if (!file.open(FLOW_FILE, FILE_O_WRITE)) return;

  const FlowRecord record = {FLOW_FILE_MAGIC, static_cast<uint32_t>(flowMinutes),
                             ~static_cast<uint32_t>(flowMinutes)};
  file.truncate(0);
  file.seek(0);
  file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  file.close();
}

void addFlowMinutes(int minutes) {
  if (minutes <= 0) return;
  flowMinutes += minutes;
  saveFlow();
}

void resetFlowMinutes() {
  flowMinutes = 0;
  saveFlow();
}

bool isTimerRunning() {
  return currentState == COUNTING_UP || currentState == COUNTING_DOWN ||
         currentState == POMODORO_FOCUS || currentState == POMODORO_REST;
}

void startPomodoroPhase(State phase, unsigned long now) {
  currentState = phase;
  countdownValue = phase == POMODORO_FOCUS ? pomodoroFocusMinutes : pomodoroRestMinutes;
  previousMillis = now;
}

void stopTimer() {
  successAnimation();
  currentState = MENU;
}

void publishBleStatus() {
  if (!bleInitialized) return;

  uint8_t packet[FLOW_STATUS_PACKET_SIZE] = {};
  packet[0] = 1;  // Protocol version.
  packet[1] = static_cast<uint8_t>(currentState);
  writeLittleEndian16(packet + 2, currentTimerMinutes());
  writeLittleEndian32(packet + 4, static_cast<uint32_t>(flowMinutes));
  writeLittleEndian16(packet + 8, batteryMillivolts);
  writeLittleEndian16(packet + 10, ++bleStatusSequence);

  flowStatusCharacteristic.write(packet, sizeof(packet));
  flowStatusCharacteristic.notify(packet, sizeof(packet));
}

void stopBle() {
  if (bleInitialized) {
    Bluefruit.Advertising.stop();
  }
}

uint8_t batteryPercentageFromMillivolts(uint16_t millivolts) {
  // Approximate single-cell LiPo open-circuit discharge curve. Under load the
  // reported percentage can temporarily read lower, which is safer than
  // overstating the remaining battery.
  constexpr uint16_t voltagePoints[] = {3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
  constexpr uint8_t percentagePoints[] = {0, 5, 15, 35, 55, 75, 90, 100};
  constexpr uint8_t pointCount = sizeof(voltagePoints) / sizeof(voltagePoints[0]);

  if (millivolts <= voltagePoints[0]) return percentagePoints[0];
  if (millivolts >= voltagePoints[pointCount - 1]) return percentagePoints[pointCount - 1];

  for (uint8_t i = 1; i < pointCount; ++i) {
    if (millivolts <= voltagePoints[i]) {
      const uint16_t span = voltagePoints[i] - voltagePoints[i - 1];
      const uint16_t position = millivolts - voltagePoints[i - 1];
      const int16_t percentageSpan = percentagePoints[i] - percentagePoints[i - 1];
      return percentagePoints[i - 1] + (position * percentageSpan + span / 2) / span;
    }
  }
  return 0;
}

void updateBatteryReading(unsigned long now, bool force) {
  const bool usbPowered = usbVbusPresent();
  if (!force && usbPowered == lastUsbVbusPresent &&
      now - lastBatterySampleTime < BATTERY_SAMPLE_INTERVAL_MS) return;

  lastUsbVbusPresent = usbPowered;
  lastBatterySampleTime = now;

  // This board routes USB VBUS through VDDH/5 while plugged in, so it cannot
  // report a real LiPo voltage or percentage in that state.
  if (usbPowered) {
    batteryMillivolts = 0;  // BLE convention: battery voltage unavailable.
    batteryPercentage = 0;
    DEBUG_PRINTLN(F("USB power: battery voltage unavailable."));
    if (bleInitialized) publishBleStatus();
    return;
  }

  // The nice!nano routes the LiPo through VDDH/5. With the board package's
  // 10-bit ADC, 1/6 gain, and 0.6 V reference, full scale is 18,000 mV.
  const uint32_t rawValue = analogReadVDDHDIV5();
  const uint32_t millivolts = (rawValue * 18000UL + 511UL) / 1023UL;
  batteryMillivolts = static_cast<uint16_t>(min(millivolts, 65535UL));
  batteryPercentage = batteryPercentageFromMillivolts(batteryMillivolts);

  DEBUG_PRINT(F("Battery: "));
  DEBUG_PRINT(batteryMillivolts);
  DEBUG_PRINT(F(" mV ("));
  DEBUG_PRINT(batteryPercentage);
  DEBUG_PRINTLN(F("%)."));

  if (bleInitialized) publishBleStatus();
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  String topRowText;
  if (currentState == COUNTING_UP) {
    topRowText = "Focus! \x18";
  } else if (currentState == COUNTING_DOWN) {
    topRowText = "Focus! \x19";
  } else if (currentState == SELECTING_POMO_FOCUS) {
    topRowText = "Set F";
  } else if (currentState == SELECTING_POMO_REST) {
    topRowText = "Set R";
  } else if (currentState == POMODORO_FOCUS) {
    topRowText = "Focus! \x19";
  } else if (currentState == POMODORO_REST) {
    topRowText = "Rest";
  } else {
    topRowText = formatFlowLabel();
  }

  display.setTextSize(2);
  display.setCursor(centeredTextX(topRowText, 12, 128), 0);
  display.print(topRowText);
  if (currentState == MENU) {
    display.setTextSize(1);
    if (usbVbusPresent()) {
      display.setCursor(110, 18);
      display.print(F("USB"));
    } else {
      display.setCursor(104, 18);
      display.print(batteryPercentage);
      display.print('%');
    }
  } else if (!usbVbusPresent() && batteryPercentage <= LOW_BATTERY_PERCENT) {
    display.setTextSize(1);
    display.setCursor(120, 0);
    display.print('!');
  }

  String mainRowText;
  if (currentState == MENU) {
    mainRowText = menuOptions[menuIndex];
  } else if (currentState == IDLE) {
    mainRowText = "IDLE";
  } else if (currentState == COUNTING_UP) {
    mainRowText = String(elapsedMinutes);
  } else {
    mainRowText = String(countdownValue);
  }

  display.setTextSize(4);
  display.setCursor((128 - mainRowText.length() * 24) / 2, 30);
  display.print(mainRowText);
  display.display();
  publishBleStatus();
}

String formatFlowLabel() {
  if (flowMinutes < 100) return "Flow: " + String(flowMinutes);
  if (flowMinutes < 1000) return "F: " + String(flowMinutes);
  if (flowMinutes < 1000000) return "F: " + String(flowMinutes / 1000) + "k";
  return "F: " + String(flowMinutes / 1000000) + "M";
}

int centeredTextX(const String& text, int characterWidth, int availableWidth) {
  const int textWidth = text.length() * characterWidth;
  return max(0, (availableWidth - textWidth) / 2);
}

bool buttonPressed() {
  if (digitalRead(SW) != LOW || millis() - buttonDebounceTime <= buttonDebounceDelay) {
    return false;
  }
  buttonDebounceTime = millis();
  lastActivityTime = buttonDebounceTime;
  DEBUG_PRINTLN(F("Encoder button press."));
  return true;
}

void handleButtonPresses(unsigned long now) {
  if (!buttonPressed()) return;

  switch (currentState) {
    case MENU:
      if (menuIndex == 0) {
        currentState = SELECTING_POMO_FOCUS;
        pomodoroFocusMinutes = 40;
        pomodoroRestMinutes = 5;
        countdownValue = pomodoroFocusMinutes;
        DEBUG_PRINTLN(F("Selecting Pomodoro focus duration."));
      } else if (menuIndex == 1) {
        currentState = COUNTING_UP;
        elapsedMinutes = 0;
        previousMillis = now;
        DEBUG_PRINTLN(F("Started counting up."));
      } else if (menuIndex == 2) {
        currentState = SELECTING_DOWN_DURATION;
        countdownValue = 20;
        DEBUG_PRINTLN(F("Selecting countdown duration."));
      } else {
        resetFlowMinutes();
        DEBUG_PRINTLN(F("Flow total reset."));
      }
      break;

    case IDLE:
      currentState = MENU;
      break;

    case SELECTING_DOWN_DURATION:
      initialCountdownValue = countdownValue;
      currentState = COUNTING_DOWN;
      previousMillis = now;
      DEBUG_PRINT(F("Started countdown for "));
      DEBUG_PRINT(countdownValue);
      DEBUG_PRINTLN(F(" minutes."));
      break;

    case SELECTING_POMO_FOCUS:
      pomodoroFocusMinutes = countdownValue;
      currentState = SELECTING_POMO_REST;
      countdownValue = pomodoroRestMinutes;
      DEBUG_PRINTLN(F("Selecting Pomodoro rest duration."));
      break;

    case SELECTING_POMO_REST:
      pomodoroRestMinutes = countdownValue;
      startPomodoroPhase(POMODORO_FOCUS, now);
      DEBUG_PRINTLN(F("Started Pomodoro focus."));
      break;

    case COUNTING_UP:
      addFlowMinutes(elapsedMinutes);
      stopTimer();
      DEBUG_PRINTLN(F("Stopped count-up timer."));
      break;

    case COUNTING_DOWN:
      addFlowMinutes(initialCountdownValue - countdownValue);
      stopTimer();
      DEBUG_PRINTLN(F("Stopped countdown timer."));
      break;

    case POMODORO_FOCUS:
      addFlowMinutes(pomodoroFocusMinutes - countdownValue);
      stopTimer();
      DEBUG_PRINTLN(F("Stopped Pomodoro during focus."));
      break;

    case POMODORO_REST:
      stopTimer();
      DEBUG_PRINTLN(F("Stopped Pomodoro during rest."));
      break;
  }
  updateDisplay();
}

void handleCounting(unsigned long now) {
  if (!isTimerRunning() || now - previousMillis < 60000UL) return;
  previousMillis = now;

  if (currentState == COUNTING_UP) {
    ++elapsedMinutes;
    DEBUG_PRINT(F("Count-up minutes: "));
    DEBUG_PRINTLN(elapsedMinutes);
  } else if (currentState == COUNTING_DOWN) {
    --countdownValue;
    DEBUG_PRINT(F("Countdown minutes remaining: "));
    DEBUG_PRINTLN(countdownValue);
    if (countdownValue <= 0) {
      addFlowMinutes(initialCountdownValue);
      stopTimer();
    }
  } else if (currentState == POMODORO_FOCUS) {
    --countdownValue;
    if (countdownValue <= 0) {
      addFlowMinutes(pomodoroFocusMinutes);
      successAnimation();
      startPomodoroPhase(POMODORO_REST, millis());
      DEBUG_PRINTLN(F("Pomodoro rest started."));
    }
  } else if (currentState == POMODORO_REST) {
    --countdownValue;
    if (countdownValue <= 0) {
      successAnimation();
      startPomodoroPhase(POMODORO_FOCUS, millis());
      DEBUG_PRINTLN(F("Pomodoro focus started."));
    }
  }
  updateDisplay();
}

void successAnimation() {
  constexpr int centerX = 64;
  constexpr int centerY = 32;
  display.clearDisplay();
  for (int radius = 2; radius <= 30; radius += 2) {
    display.drawCircle(centerX, centerY, radius, SSD1306_WHITE);
    display.display();
    delay(100);
    if (radius % 4 == 0) {
      display.clearDisplay();
      display.display();
      delay(2);
    }
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 20);
  display.print("SUCCESS!");
  display.display();
  delay(1000);
}

int getRotation() {
  // Standard Gray-code quadrature transition table. Each adjacent state change
  // is a quarter-step; unchanged and impossible two-bit jumps are zero.
  static constexpr int8_t transitionDelta[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };

  const uint8_t currentState = readRotaryState();
  const int8_t delta = transitionDelta[(previousRotaryState << 2) | currentState];
  previousRotaryState = currentState;

  if (delta == 0) return 0;

  rotaryTransitionAccumulator += delta;
  if (rotaryTransitionAccumulator >= ROTARY_TRANSITIONS_PER_TICK) {
    rotaryTransitionAccumulator -= ROTARY_TRANSITIONS_PER_TICK;
    return 1;
  }
  if (rotaryTransitionAccumulator <= -ROTARY_TRANSITIONS_PER_TICK) {
    rotaryTransitionAccumulator += ROTARY_TRANSITIONS_PER_TICK;
    return -1;
  }
  return 0;
}

uint8_t readRotaryState() {
  return static_cast<uint8_t>((digitalRead(CLK) << 1) | digitalRead(DT));
}

void handleRotaryInput(int rotation) {
  lastActivityTime = millis();
  DEBUG_PRINT(F("Encoder rotation: "));
  DEBUG_PRINTLN(rotation);
  if (currentState == IDLE) {
    currentState = MENU;
    updateDisplay();
  } else if (currentState == MENU) {
    menuIndex = (menuIndex + rotation + MENU_OPTION_COUNT) % MENU_OPTION_COUNT;
    DEBUG_PRINT(F("Menu option: "));
    DEBUG_PRINTLN(menuOptions[menuIndex]);
    updateDisplay();
  } else if (currentState == SELECTING_DOWN_DURATION ||
             currentState == SELECTING_POMO_FOCUS ||
             currentState == SELECTING_POMO_REST) {
    countdownValue = max(1, countdownValue + rotation);
    DEBUG_PRINT(F("Countdown selection: "));
    DEBUG_PRINTLN(countdownValue);
    updateDisplay();
  }
}

bool encoderIsReleased() {
  return digitalRead(CLK) == HIGH && digitalRead(DT) == HIGH && digitalRead(SW) == HIGH;
}

void configureWakePin(uint8_t pin) {
  // System OFF wakes by GPIO SENSE; INPUT_PULLUP makes a contact-to-GND wake on LOW.
  nrf_gpio_cfg_sense_input(digitalPinToPinName(pin), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
}

[[noreturn]] void enterSystemOff() {
  DEBUG_PRINTLN(F("Entering System OFF. Rotate or press encoder to wake."));
  Serial.flush();
  stopBle();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  display.display();
  delay(10);
  Wire.end();
  pinMode(OLED_SDA, INPUT);
  pinMode(OLED_SCL, INPUT);

  // Keep EXT_VCC on: this encoder's wake path may depend on the external rail.
  // A separate OLED load switch is needed to cut display power without
  // depowering the encoder.

  configureWakePin(CLK);
  configureWakePin(DT);
  configureWakePin(SW);
  NRF_POWER->SYSTEMOFF = 1;
  while (true) { __WFE(); }
}

void handleInactivity(unsigned long now) {
  const bool mayBecomeIdle = currentState == MENU ||
                             currentState == SELECTING_DOWN_DURATION ||
                             currentState == SELECTING_POMO_FOCUS ||
                             currentState == SELECTING_POMO_REST;

  if (currentState == IDLE) {
    if (!usbVbusPresent()) enterSystemOff();
  } else if (mayBecomeIdle && now - lastActivityTime >= inactivityLimit) {
    if (usbVbusPresent()) {
      currentState = IDLE;
      updateDisplay();
    } else if (encoderIsReleased()) {
      enterSystemOff();
    }
  }
}
