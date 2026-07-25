/**
 * ============================================================================
 * AQUAPULSE SMART FISH FEEDER - ESP32 FIRMWARE (v3.2)
 * ============================================================================
 * Built on the leaner v3.1 base. Fixes in this version:
 *   1. Firestore command processing now only acts on ONE pending command per
 *      poll cycle (the oldest), and every action is logged to Serial before
 *      it runs. This stops a stray leftover "reboot" command from silently
 *      firing alongside a "feed" command in the same pass.
 *   2. "reboot" only actually restarts the board if marking it done on
 *      Firestore actually succeeded (checked via HTTP response code). If the
 *      network call fails, it does NOT restart, it tries again next cycle
 *      instead, so a flaky connection can't cause a reboot loop.
 *   3. Manual button uses a proper edge-triggered debounce (tracks the last
 *      raw reading and only accepts a new stable state after it holds for
 *      the debounce window), instead of a simple time-lockout check. This
 *      is more tolerant of a noisy/bouncy physical button connection, which
 *      is the most likely cause of "needs 3-4 presses to register."
 *   4. No watchdog timer. It was removed after causing an unexplained
 *      restart loop that reproduced even with zero peripherals attached.
 *      Not worth the risk it re-introduces that same bug.
 *
 * IMPORTANT FIRST STEP BEFORE FLASHING THIS:
 *   Go to Firebase console -> devices/feeder_01/commands and DELETE any
 *   document with action "reboot". That stray document is almost certainly
 *   why "Feed now" was rebooting the board instead of dispensing.
 *
 * STEPPER CURRENT NOTE (re: your observation about the stepper needing a
 * lot of current): the 28BYJ-48 draws roughly 240-300mA when moving. This
 * is well within a healthy 5V supply's capability, but it is enough to
 * brown out a supply that's already marginal, shared with other loads, or
 * connected through thin/loose wiring. If dispensing is still unreliable
 * after these fixes, the stepper's power path (not this code) is the next
 * thing to check, dedicated wiring straight to the external 5V supply,
 * short thick wires, and a solid direct connection, no shared rail with
 * the ESP32's own power.
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
#include <RTClib.h>
#include <EEPROM.h>

// ============================================================================
// --- Wi-Fi / Firebase Configuration ---
// ============================================================================
const char* WIFI_SSID     = "Alex";
const char* WIFI_PASSWORD = "Angelic2020";

const char* FIREBASE_PROJECT_ID = "smartfishfeeder-25c86";
const char* DEVICE_ID           = "feeder_01";

const String firestoreBase         = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/databases/(default)/documents/devices/" + String(DEVICE_ID);
const String firestoreDeviceUrl    = firestoreBase;
const String firestoreCommandsUrl  = firestoreBase + "/commands";
const String firestoreHistoryUrl   = firestoreBase + "/history";
const String firestoreSchedulesUrl = firestoreBase + "/schedules";

// ============================================================================
// --- Pin Configuration ---
// ============================================================================
const int BUTTON_PIN = 4;
const int BUZZER_PIN = 5;
const int TRIG_PIN   = 18;
const int ECHO_PIN   = 19;
const int LED_PIN    = 13;

const int SDA_PIN = 21;
const int SCL_PIN = 22;

// Stepper (28BYJ-48 + ULN2003) - library wiring order: IN1, IN3, IN2, IN4
const int IN1 = 25;
const int IN2 = 27;
const int IN3 = 26;
const int IN4 = 14;
const int STEPS_PER_REV = 2048;

Stepper feedStepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;

// ============================================================================
// --- System State & Constants ---
// ============================================================================
const unsigned long FEED_COOLDOWN_MS = 30000UL;
const float HOPPER_EMPTY_DIST_CM = 20.0;
const float HOPPER_FULL_DIST_CM = 2.0;

int stepperSpeedSetting = 5;
unsigned long lastFeedTime = 0;
String lastFeedSource = "none";
int lastFeedQuantity = 0;
bool isCooldownActive = false;
int currentCapacity = 100;
int lastKnownCapacity = 100;
bool hasFaultCondition = false;
String faultReason = "";
int lastCheckedDay = -1;

bool rtcWorking = false;
bool lcdWorking = false;
bool sensorWorking = false;
String lastButtonPressedTime = "never";

// Non-blocking timers
unsigned long lastTelemetryUpdate = 0;
const unsigned long TELEMETRY_INTERVAL = 10000;
unsigned long lastCommandCheck = 0;
const unsigned long COMMAND_CHECK_INTERVAL = 3000;
unsigned long lastScheduleCheck = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL = 15000;
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
unsigned long lastLcdUpdate = 0;
String lastSchedulesRaw = "";

// EEPROM Configuration
const int EEPROM_SIZE = 512;
const int EEPROM_MAGIC_ADDR = 0;
const byte EEPROM_MAGIC_VAL = 0x5A;
const int EEPROM_SPEED_ADDR = 1;
const int EEPROM_SCHED_COUNT_ADDR = 2;
const int EEPROM_SCHED_START_ADDR = 10;
const int MAX_SCHEDULES = 10;

struct FeedingSchedule {
  bool enabled;
  byte hour;
  byte minute;
  byte portion;
  byte daysMask;
  bool triggeredToday;
};

// Declarations
void stepperCoilsOff();
void dispenseFood(int portions, String source);
void updateTelemetry();
void checkManualButton();
void checkScheduledFeedings();
void updateLcdDisplay();
void handleLedStatus(unsigned long currentMillis);
void handleFaultBuzzer(unsigned long currentMillis);
void pushTelemetryToFirestore();
void checkFirestoreCommands();
void checkFirestoreSchedules();
void uploadFeedHistoryItem(int qty, String source);
bool markCommandDone(String docResourceName);
void initAndLoadEEPROM();
bool addSchedule(int hour, int minute, int portion, int daysMask);
bool toggleSchedule(int index, bool enabled);
String getFormattedTime();
String getIsoTimestamp();

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n--- AQUAPULSE BOOTING ---"));

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);
  stepperCoilsOff();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(100);

  lcd.init();
  lcd.backlight();
  lcdWorking = true;
  lcd.setCursor(0, 0);
  lcd.print(F("AquaPulse feed"));
  lcd.setCursor(0, 1);
  lcd.print(F("Initializing..."));

  if (rtc.begin()) {
    rtcWorking = true;
    Serial.println(F("RTC Initialized successfully."));
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  } else {
    rtcWorking = false;
    Serial.println(F("RTC not detected (check wiring/power)."));
  }

  EEPROM.begin(EEPROM_SIZE);
  initAndLoadEEPROM();

  feedStepper.setSpeed(5); // keep modest, too fast under load skips steps

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (lcdWorking) {
    lcd.setCursor(0, 1);
    lcd.print(F("Connecting WiFi"));
  }

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi Connected! IP: "));
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    if (lcdWorking) {
      lcd.setCursor(0, 1);
      lcd.print(F("WiFi Connected "));
    }
  } else {
    Serial.println(F("WiFi offline. Operating in standalone mode."));
    if (lcdWorking) {
      lcd.setCursor(0, 1);
      lcd.print(F("WiFi Offline   "));
    }
  }

  tone(BUZZER_PIN, 2000, 100);
  delay(1000);

  if (lcdWorking) lcd.clear();

  Serial.println(F("Setup complete, entering loop()."));
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  unsigned long currentMillis = millis();

  // Local hardware functions always run independently of WiFi status
  checkManualButton();
  checkScheduledFeedings();
  updateTelemetry();

  if (currentMillis - lastLcdUpdate >= 500) {
    lastLcdUpdate = currentMillis;
    updateLcdDisplay();
  }

  handleLedStatus(currentMillis);
  handleFaultBuzzer(currentMillis);

  // Non-blocking WiFi reconnect handling
  if (currentMillis - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // Network tasks run only when connected
  if (WiFi.status() == WL_CONNECTED) {
    if (currentMillis - lastCommandCheck >= COMMAND_CHECK_INTERVAL) {
      lastCommandCheck = currentMillis;
      checkFirestoreCommands();
    }

    if (currentMillis - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
      lastScheduleCheck = currentMillis;
      checkFirestoreSchedules();
    }

    if (currentMillis - lastTelemetryUpdate >= TELEMETRY_INTERVAL) {
      lastTelemetryUpdate = currentMillis;
      pushTelemetryToFirestore();
    }
  }
}

// ============================================================================
// STEPPER & SENSORS
// ============================================================================
void stepperCoilsOff() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void dispenseFood(int portions, String source) {
  if (portions <= 0 || portions > 5) portions = 1;

  digitalWrite(LED_PIN, HIGH);
  if (lcdWorking) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Dispensing Food"));
  }

  for (int p = 0; p < portions; p++) {
    feedStepper.step(STEPS_PER_REV);
    delay(100);
  }

  stepperCoilsOff();
  tone(BUZZER_PIN, 2500, 150);

  lastFeedTime = millis();
  isCooldownActive = true;
  lastFeedSource = source;
  lastFeedQuantity = portions;

  Serial.print(F("Dispensed "));
  Serial.print(portions);
  Serial.print(F(" portion(s), source: "));
  Serial.println(source);

  if (WiFi.status() == WL_CONNECTED) {
    uploadFeedHistoryItem(portions, source);
  }

  if (lcdWorking) lcd.clear();
}

/**
 * Edge-triggered debounce: only accepts a new button state once the raw
 * reading has held steady for the full debounce window. This is more
 * tolerant of a noisy or marginal physical connection than a simple
 * "ignore repeats for 300ms" lockout, which can miss short/bouncy presses
 * entirely if the contact is flaky (worth checking the wiring too, but
 * this makes the code itself as forgiving as reasonably possible).
 */
void checkManualButton() {
  static int lastRawReading = HIGH;
  static int stableState = HIGH;
  static unsigned long lastChangeTime = 0;
  const unsigned long DEBOUNCE_MS = 50;

  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastRawReading) {
    lastChangeTime = millis();
  }

  if ((millis() - lastChangeTime) > DEBOUNCE_MS) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {
        if (!isCooldownActive) {
          Serial.println(F("Button press detected, dispensing."));
          lastButtonPressedTime = getFormattedTime();
          dispenseFood(1, "manual_button");
        } else {
          Serial.println(F("Manual feed ignored, cooldown active."));
        }
      }
    }
  }

  lastRawReading = reading;
}

void updateTelemetry() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) {
    currentCapacity = 0;
    sensorWorking = false;
  } else {
    sensorWorking = true;
    float distanceCm = (duration * 0.0343) / 2.0;
    float rawPercent = ((HOPPER_EMPTY_DIST_CM - distanceCm) / (HOPPER_EMPTY_DIST_CM - HOPPER_FULL_DIST_CM)) * 100.0;
    currentCapacity = constrain(round(rawPercent), 0, 100);
  }

  // Fault flags are informational only in this build - nothing here blocks
  // dispensing (see checkManualButton() and checkFirestoreCommands()), they
  // only drive the LCD/LED/buzzer alert and what gets reported to Firestore.
  if (!sensorWorking) {
    hasFaultCondition = true;
    faultReason = "SENSOR FAIL";
  } else if (!rtcWorking) {
    hasFaultCondition = true;
    faultReason = "RTC FAIL";
  } else if (currentCapacity <= 10) {
    hasFaultCondition = true;
    faultReason = "FOOD EMPTY";
  } else {
    hasFaultCondition = false;
    faultReason = "";
  }

  if (isCooldownActive && (millis() - lastFeedTime >= FEED_COOLDOWN_MS)) {
    isCooldownActive = false;
  }
}

void checkScheduledFeedings() {
  if (!rtcWorking) return;
  DateTime now = rtc.now();

  if (now.day() != lastCheckedDay) {
    int schedCount = EEPROM.read(EEPROM_SCHED_COUNT_ADDR);
    for (int i = 0; i < schedCount; i++) {
      int addr = EEPROM_SCHED_START_ADDR + (i * sizeof(FeedingSchedule));
      FeedingSchedule temp;
      EEPROM.get(addr, temp);
      if (temp.triggeredToday) {
        temp.triggeredToday = false;
        EEPROM.put(addr, temp);
      }
    }
    EEPROM.commit();
    lastCheckedDay = now.day();
  }

  int count = EEPROM.read(EEPROM_SCHED_COUNT_ADDR);
  if (count <= 0) return;

  int dayOfWeek = now.dayOfTheWeek();
  for (int i = 0; i < count; i++) {
    int addr = EEPROM_SCHED_START_ADDR + (i * sizeof(FeedingSchedule));
    FeedingSchedule sched;
    EEPROM.get(addr, sched);

    if (sched.enabled && !sched.triggeredToday) {
      if (sched.hour == now.hour() && sched.minute == now.minute()) {
        if (sched.daysMask & (1 << dayOfWeek)) {
          dispenseFood(sched.portion, "scheduled");
          sched.triggeredToday = true;
          EEPROM.put(addr, sched);
          EEPROM.commit();
        }
      }
    }
  }
}

// ============================================================================
// FIRESTORE & NETWORK
// ============================================================================
String getIsoTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void pushTelemetryToFirestore() {
  lastKnownCapacity = currentCapacity;

  JsonDocument payload;
  JsonObject fields = payload["fields"].to<JsonObject>();

  fields["connectivity"]["stringValue"] = "online";
  fields["lastCommunication"]["timestampValue"] = getIsoTimestamp();
  fields["capacity"]["integerValue"] = String(currentCapacity);
  fields["healthSummary"]["stringValue"] = hasFaultCondition ? ("Alert: " + faultReason) : "All systems operational.";

  String payloadStr;
  serializeJson(payload, payloadStr);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);

  String url = firestoreDeviceUrl +
    "?updateMask.fieldPaths=connectivity"
    "&updateMask.fieldPaths=lastCommunication"
    "&updateMask.fieldPaths=capacity"
    "&updateMask.fieldPaths=healthSummary";

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.PATCH(payloadStr);
    http.end();
  }
}

/**
 * Processes at most ONE pending command per call, the oldest one returned
 * by Firestore. This is the key fix: previously, if multiple pending
 * commands existed at once (e.g. a stray old "reboot" alongside a new
 * "feed"), all of them got processed in the same pass, so a leftover
 * reboot command could fire the instant a feed command was also present.
 * Processing one at a time, in order, and logging each one to Serial,
 * makes this fully visible and prevents that kind of cross-talk.
 */
void checkFirestoreCommands() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);

  if (!http.begin(client, firestoreCommandsUrl)) return;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, response)) return;

  JsonArray documents = doc["documents"].as<JsonArray>();

  for (JsonObject document : documents) {
    JsonObject fields = document["fields"];
    String status = fields["status"]["stringValue"] | "";
    if (status != "pending") continue;

    String action = fields["action"]["stringValue"] | "";
    String docName = document["name"].as<String>();

    Serial.print(F("Firestore command found: "));
    Serial.print(action);
    Serial.print(F(" ("));
    Serial.print(docName);
    Serial.println(F(")"));

    if (action == "feed") {
      int portion = fields["portion"]["integerValue"] | 1;
      markCommandDone(docName);
      if (!isCooldownActive) {
        dispenseFood(portion, "manual_app");
      } else {
        Serial.println(F("Feed command ignored, cooldown active."));
      }
    } else if (action == "reboot") {
      Serial.println(F("Reboot command received."));
      bool marked = markCommandDone(docName);
      if (marked) {
        Serial.println(F("Marked done, rebooting now."));
        delay(200);
        ESP.restart();
      } else {
        Serial.println(F("Could not confirm mark-done over network, NOT rebooting this cycle, will retry."));
      }
    } else if (action == "selftest") {
      markCommandDone(docName);
      Serial.println(F("Self test command received (no-op in this build)."));
    }

    // Only process one command per call, on purpose - see function comment.
    break;
  }
}

/**
 * Returns true only if the PATCH request actually succeeded. Callers that
 * gate a risky action (like rebooting) on this should NOT proceed if it
 * returns false, that means Firestore might still show this command as
 * pending, and the safe behavior is to retry next cycle, not act blindly.
 */
bool markCommandDone(String docResourceName) {
  String url = "https://firestore.googleapis.com/v1/" + docResourceName + "?updateMask.fieldPaths=status";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);

  bool success = false;
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.PATCH("{\"fields\": {\"status\": {\"stringValue\": \"done\"}}}");
    success = (code == 200);
    http.end();
  }
  return success;
}

void checkFirestoreSchedules() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);

  if (!http.begin(client, firestoreSchedulesUrl)) return;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  if (response == lastSchedulesRaw) return;
  lastSchedulesRaw = response;

  JsonDocument doc;
  if (deserializeJson(doc, response)) return;

  JsonArray documents = doc["documents"].as<JsonArray>();
  EEPROM.write(EEPROM_SCHED_COUNT_ADDR, 0);
  EEPROM.commit();

  const char* dayNames[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  int index = 0;

  for (JsonObject document : documents) {
    if (index >= MAX_SCHEDULES) break;

    JsonObject fields = document["fields"];
    String timeStr = fields["time"]["stringValue"] | "00:00";
    bool enabled = fields["enabled"]["booleanValue"] | true;
    int portion = fields["portion"]["integerValue"] | 1;

    int hour = timeStr.substring(0, 2).toInt();
    int minute = timeStr.substring(3, 5).toInt();

    int daysMask = 0;
    JsonArray days = fields["days"]["arrayValue"]["values"].as<JsonArray>();
    for (JsonObject dayEntry : days) {
      String dayStr = dayEntry["stringValue"] | "";
      for (int i = 0; i < 7; i++) {
        if (dayStr == dayNames[i]) {
          daysMask |= (1 << i);
          break;
        }
      }
    }

    addSchedule(hour, minute, portion, daysMask);
    if (!enabled) toggleSchedule(index, false);
    index++;
  }
}

void uploadFeedHistoryItem(int qty, String source) {
  String nowIso = getIsoTimestamp();
  int newCapacity = max(0, lastKnownCapacity - qty * 4);
  lastKnownCapacity = newCapacity;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);

  if (http.begin(client, firestoreHistoryUrl)) {
    http.addHeader("Content-Type", "application/json");
    JsonDocument payload;
    JsonObject fields = payload["fields"].to<JsonObject>();
    fields["quantity"]["integerValue"] = String(qty);
    fields["source"]["stringValue"] = source;
    fields["timestamp"]["timestampValue"] = nowIso;
    fields["status"]["stringValue"] = "success";

    String payloadStr;
    serializeJson(payload, payloadStr);
    http.POST(payloadStr);
    http.end();
  }
}

// ============================================================================
// LCD & HELPERS
// ============================================================================
void initAndLoadEEPROM() {
  byte magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC_VAL) {
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    EEPROM.write(EEPROM_SPEED_ADDR, 5);
    EEPROM.write(EEPROM_SCHED_COUNT_ADDR, 0);
    EEPROM.commit();
    stepperSpeedSetting = 5;
  } else {
    stepperSpeedSetting = EEPROM.read(EEPROM_SPEED_ADDR);
    stepperSpeedSetting = constrain(stepperSpeedSetting, 1, 10);
  }
}

bool addSchedule(int hour, int minute, int portion, int daysMask) {
  int count = EEPROM.read(EEPROM_SCHED_COUNT_ADDR);
  if (count >= MAX_SCHEDULES) return false;
  FeedingSchedule newSched = { true, (byte)hour, (byte)minute, (byte)portion, (byte)daysMask, false };
  int addr = EEPROM_SCHED_START_ADDR + (count * sizeof(FeedingSchedule));
  EEPROM.put(addr, newSched);
  EEPROM.write(EEPROM_SCHED_COUNT_ADDR, count + 1);
  EEPROM.commit();
  return true;
}

bool toggleSchedule(int index, bool enabled) {
  int count = EEPROM.read(EEPROM_SCHED_COUNT_ADDR);
  if (index < 0 || index >= count) return false;
  int addr = EEPROM_SCHED_START_ADDR + (index * sizeof(FeedingSchedule));
  FeedingSchedule temp;
  EEPROM.get(addr, temp);
  temp.enabled = enabled;
  EEPROM.put(addr, temp);
  EEPROM.commit();
  return true;
}

String getFormattedTime() {
  if (!rtcWorking) return "--:--:--";
  DateTime now = rtc.now();
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(buf);
}

void updateLcdDisplay() {
  if (!lcdWorking) return;

  lcd.setCursor(0, 0);
  if (rtcWorking) {
    lcd.print(getFormattedTime().substring(0, 5));
  } else {
    lcd.print(F("NO RTC"));
  }

  lcd.print(F(" Cap:"));
  if (sensorWorking) {
    lcd.print(currentCapacity);
    lcd.print(F("% "));
  } else {
    lcd.print(F("ERR "));
  }

  lcd.setCursor(0, 1);
  if (isCooldownActive) {
    lcd.print(F("Cooldown...     "));
  } else if (hasFaultCondition) {
    lcd.print(F("ALT: "));
    lcd.print(faultReason);
    lcd.print(F("        "));
  } else if (WiFi.status() == WL_CONNECTED) {
    lcd.print(F("Ready (Online)  "));
  } else {
    lcd.print(F("Ready (Offline) "));
  }
}

void handleLedStatus(unsigned long currentMillis) {
  static unsigned long lastBlink = 0;
  static bool state = false;
  if (hasFaultCondition) {
    if (currentMillis - lastBlink >= 200) {
      lastBlink = currentMillis;
      state = !state;
      digitalWrite(LED_PIN, state ? HIGH : LOW);
    }
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}

void handleFaultBuzzer(unsigned long currentMillis) {
  static unsigned long lastBeep = 0;
  if (hasFaultCondition) {
    if (currentMillis - lastBeep >= 3000) {
      lastBeep = currentMillis;
      tone(BUZZER_PIN, 1500, 100);
    }
  }
}
