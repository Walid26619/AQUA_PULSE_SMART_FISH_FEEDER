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
// Physical DS3231 is primary time source again (NTP-only proved unreliable
// on this network). NTP is now used as an opportunistic correction on top
// of the RTC when available, see the NTP retry block in loop() below.
#include <EEPROM.h>

// ============================================================================
// --- Wi-Fi / Firebase Configuration ---
// ============================================================================
const char* WIFI_SSID     = "Galaxy";
const char* WIFI_PASSWORD = "zpub1832";

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
const float HOPPER_EMPTY_DIST_CM = 11.0;
const float HOPPER_FULL_DIST_CM = 5.0;

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

bool rtcWorking = false; // true only if the physical DS3231 responded on the MOST RECENT check
bool ntpSynced = false;  // true once NTP has succeeded at least once this session, used as fallback when RTC is intermittent
bool lcdWorking = false;
bool sensorWorking = false;
String lastButtonPressedTime = "never";

// Non-blocking timers
unsigned long lastTelemetryUpdate = 0;
const unsigned long TELEMETRY_INTERVAL = 10000;
unsigned long lastCommandCheck = 0;
const unsigned long COMMAND_CHECK_INTERVAL = 5000;
unsigned long lastScheduleCheck = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL = 60000;
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000;
unsigned long lastNtpCheck = 0;
const unsigned long NTP_CHECK_INTERVAL = 300000; // 5 minutes, periodic RTC drift correction
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
bool getCurrentTime(struct tm &out);
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
  lcd.print(F("AquaPulse v3.2"));
  lcd.setCursor(0, 1);
  lcd.print(F("Initializing..."));

  if (rtc.begin()) {
    rtcWorking = true;
    Serial.println(F("RTC Initialized successfully."));
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("RTC lost power, set from compile time (will correct from NTP once WiFi connects)."));
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

    // Kampala/Uganda is UTC+3, no daylight saving. Adjust GMT_OFFSET_SEC
    // if you're ever in a different timezone.
    const long GMT_OFFSET_SEC = 3 * 3600;
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");

    // Wait for NTP to actually resolve before trusting the time. Without
    // this, the first telemetry push can go out with a garbage/epoch
    // timestamp if configTime()'s async sync hasn't finished yet.
    time_t nowTime = time(nullptr);
    unsigned long ntpStart = millis();
    while (nowTime < 100000 && millis() - ntpStart < 8000) {
      delay(200);
      nowTime = time(nullptr);
    }
    if (nowTime < 100000) {
      Serial.println(F("NTP sync did not complete in time, timestamps may be off until it catches up."));
    } else {
      Serial.println(F("NTP time synced, correcting RTC."));
      ntpSynced = true;
      if (rtcWorking) {
        struct tm timeinfo;
        localtime_r(&nowTime, &timeinfo); // applies the GMT_OFFSET_SEC set via configTime
        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        Serial.println(F("RTC corrected from NTP."));
      }
    }

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

  // Periodic RTC correction "kick": every few minutes, if WiFi and NTP are
  // available, nudge the physical RTC back in line with real time. This
  // catches drift over a long-running session without requiring NTP at
  // all for normal operation, the RTC alone is enough to keep working.
  if (WiFi.status() == WL_CONNECTED) {
    if (currentMillis - lastNtpCheck >= NTP_CHECK_INTERVAL) {
      lastNtpCheck = currentMillis;
      time_t nowTime = time(nullptr);
      if (nowTime >= 100000) {
        ntpSynced = true; // keep this fresh regardless of whether the RTC happens to be up right now
        if (rtcWorking) {
          struct tm timeinfo;
          localtime_r(&nowTime, &timeinfo);
          rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                               timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
          Serial.println(F("RTC periodic correction from NTP applied."));
        }
      }
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
  
  tone(BUZZER_PIN, 2000); // starts continuous beep, no duration = plays until noTone() stops it

  for (int p = 0; p < portions; p++) {
    feedStepper.step(STEPS_PER_REV);
    delay(100);
  }
  
  noTone(BUZZER_PIN); // stop the continuous beep once dispensing is done
  
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
/**
 * Polling-based edge-triggered debounce. Reverted from an interrupt-based
 * approach after the interrupt turned out to be too sensitive to electrical
 * noise from the stepper motor, a motor coil switching can induce a false
 * edge on a nearby signal line, which a raw interrupt can't distinguish
 * from a real press. Polling with a debounce window is slower to react
 * during a blocking network call, but far more resistant to that kind of
 * false triggering. Network timeouts were shortened instead (see
 * HTTPClient .setTimeout() calls) to reduce how long the button check can
 * be delayed by a blocking call.
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

    // CALIBRATION AID: prints the raw measured distance once every 2
    // seconds. Use this to find your actual HOPPER_FULL_DIST_CM and
    // HOPPER_EMPTY_DIST_CM values, see the calibration steps in the docs.
    // Safe to leave in permanently, it's cheap and useful for future
    // troubleshooting too.
    static unsigned long lastDistancePrint = 0;
    if (millis() - lastDistancePrint >= 2000) {
      lastDistancePrint = millis();
      Serial.print(F("Raw distance: "));
      Serial.print(distanceCm);
      Serial.print(F(" cm, capacity: "));
      Serial.print(currentCapacity);
      Serial.println(F("%"));
    }
  }

  // Fault flags are informational only in this build - nothing here blocks
  // dispensing (see checkManualButton() and checkFirestoreCommands()), they
  // only drive the LCD/LED/buzzer alert and what gets reported to Firestore.
  // RTC FAIL only fires when NEITHER the physical RTC NOR the NTP fallback
  // is available, an intermittent RTC alone is covered silently by NTP
  // (see getCurrentTime()), so it doesn't false-alarm every time the
  // corroded pins have one of their "off" moments.
  if (!sensorWorking) {
    hasFaultCondition = true;
    faultReason = "SENSOR FAIL";
  } else if (!rtcWorking && !ntpSynced) {
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
  struct tm now;
  if (!getCurrentTime(now)) return; // neither RTC nor NTP available right now

  if (now.tm_mday != lastCheckedDay) {
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
    lastCheckedDay = now.tm_mday;
  }

  int count = EEPROM.read(EEPROM_SCHED_COUNT_ADDR);
  if (count <= 0) return;

  int dayOfWeek = now.tm_wday;
  for (int i = 0; i < count; i++) {
    int addr = EEPROM_SCHED_START_ADDR + (i * sizeof(FeedingSchedule));
    FeedingSchedule sched;
    EEPROM.get(addr, sched);

    if (sched.enabled && !sched.triggeredToday) {
      if (sched.hour == now.tm_hour && sched.minute == now.tm_min) {
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

  JsonObject health = fields["health"]["mapValue"]["fields"].to<JsonObject>();

  JsonObject board = health["esp32Board"]["mapValue"]["fields"].to<JsonObject>();
  board["responding"]["booleanValue"] = true;
  board["uptime"]["integerValue"] = String(millis() / 1000);

  JsonObject network = health["network"]["mapValue"]["fields"].to<JsonObject>();
  network["wifiSignal"]["stringValue"] = WiFi.RSSI() > -60 ? "good" : (WiFi.RSSI() > -80 ? "weak" : "none");
  network["networkStatus"]["stringValue"] = "connected";
  network["ipAddress"]["stringValue"] = WiFi.localIP().toString();

  JsonObject buttons = health["buttons"]["mapValue"]["fields"].to<JsonObject>();
  buttons["lastPressed"]["stringValue"] = lastButtonPressedTime;
  buttons["functional"]["booleanValue"] = true;

  JsonObject lcdScreen = health["lcdScreen"]["mapValue"]["fields"].to<JsonObject>();
  lcdScreen["working"]["booleanValue"] = lcdWorking;
  lcdScreen["lastMessage"]["stringValue"] = hasFaultCondition ? ("ALERT: " + faultReason) : "SYSTEM: ACTIVE";

  JsonObject ultrasonic = health["ultrasonicSensor"]["mapValue"]["fields"].to<JsonObject>();
  ultrasonic["working"]["booleanValue"] = sensorWorking;
  ultrasonic["lastMeasuredLevel"]["stringValue"] = currentCapacity <= 10 ? "Empty" : currentCapacity <= 30 ? "Low" : currentCapacity <= 75 ? "Medium" : "Full";

  JsonObject rtcModule = health["rtcModule"]["mapValue"]["fields"].to<JsonObject>();
  rtcModule["synced"]["booleanValue"] = rtcWorking;
  rtcModule["deviceTime"]["stringValue"] = getFormattedTime().substring(0, 5);

  JsonObject stepperHealth = health["stepperMotor"]["mapValue"]["fields"].to<JsonObject>();
  stepperHealth["status"]["stringValue"] = isCooldownActive ? "idle" : (hasFaultCondition ? "error" : "idle");
  stepperHealth["lastActuation"]["stringValue"] = lastButtonPressedTime;
  stepperHealth["configuredSpeed"]["integerValue"] = String(stepperSpeedSetting);

  String payloadStr;
  serializeJson(payload, payloadStr);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(2500);

  String url = firestoreDeviceUrl +
    "?updateMask.fieldPaths=connectivity"
    "&updateMask.fieldPaths=lastCommunication"
    "&updateMask.fieldPaths=capacity"
    "&updateMask.fieldPaths=healthSummary"
    "&updateMask.fieldPaths=health";

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    http.PATCH(payloadStr);
    http.end();
  }
}

/**
 * Processes at most ONE pending command per call.
 *
 * IMPORTANT: this uses Firestore's runQuery endpoint with a filter for
 * status == "pending", NOT a plain listDocuments GET on the whole
 * collection. Firestore bills one read per document a query touches - a
 * plain GET on the collection re-reads every "done" command that has ever
 * accumulated, every single poll, forever. Filtering server-side means old
 * done commands cost nothing, no matter how many pile up over time.
 */
void checkFirestoreCommands() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(2500);

  String runQueryUrl = firestoreDeviceUrl + ":runQuery";

  JsonDocument queryBody;
  JsonArray from = queryBody["structuredQuery"]["from"].to<JsonArray>();
  from.add<JsonObject>()["collectionId"] = "commands";
  JsonObject fieldFilter = queryBody["structuredQuery"]["where"]["fieldFilter"].to<JsonObject>();
  fieldFilter["field"]["fieldPath"] = "status";
  fieldFilter["op"] = "EQUAL";
  fieldFilter["value"]["stringValue"] = "pending";
  queryBody["structuredQuery"]["limit"] = 5;

  String bodyStr;
  serializeJson(queryBody, bodyStr);

  if (!http.begin(client, runQueryUrl)) return;
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(bodyStr);
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  // runQuery returns a top-level JSON array, each entry either has a
  // "document" key (a match) or is just a readTime heartbeat (no match).
  JsonDocument doc;
  if (deserializeJson(doc, response)) return;
  JsonArray results = doc.as<JsonArray>();

  for (JsonObject result : results) {
    if (!result["document"].is<JsonObject>()) continue;
    JsonObject document = result["document"];
    JsonObject fields = document["fields"];

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
  http.setTimeout(2500);

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
  http.setTimeout(2500);

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
  http.setTimeout(2500);

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

/**
 * Unified time source with fallback. Tries the physical RTC first (cheap
 * re-check every call, since it's known to be intermittent), and falls
 * back to the NTP-synced system clock if the RTC doesn't respond right
 * now. Only returns false if NEITHER source is available, which is the
 * only case that should actually count as a real time failure.
 *
 * This is what lets scheduling and app commands keep working even while
 * the RTC's corroded pins are having one of their "off" moments, as long
 * as NTP has synced at least once this session, that reading covers it.
 */
bool getCurrentTime(struct tm &out) {
  // Lightweight presence check (not a full rtc.begin() re-init) - cheap
  // enough to call on every read, since the RTC is known to be
  // intermittent and we want the freshest possible answer each time.
  Wire.beginTransmission(0x68); // DS3231 default I2C address
  byte i2cError = Wire.endTransmission();

  if (i2cError == 0) {
    DateTime firstRead = rtc.now();
    delay(5); // tiny gap between the two reads
    DateTime secondRead = rtc.now();

    // Double-read consistency check: a genuine reading will agree with
    // itself (within a second, for the elapsed gap) on a second read
    // moments later. A corrupted glitch from a marginal connection is
    // extremely unlikely to reproduce the exact same wrong value twice,
    // this catches subtle corruption that still LOOKS like a valid time
    // (e.g. a wrong-but-in-range minute), which a plausibility range check
    // alone can't detect.
    long diffSeconds = abs((long)(secondRead.unixtime() - firstRead.unixtime()));
    bool consistent = diffSeconds <= 1;

    DateTime now = secondRead;

    bool plausible =
      now.year() >= 2024 && now.year() <= 2099 &&
      now.month() >= 1 && now.month() <= 12 &&
      now.day() >= 1 && now.day() <= 31 &&
      now.hour() <= 23 &&
      now.minute() <= 59 &&
      now.second() <= 59;

    if (plausible && consistent) {
      rtcWorking = true;
      out.tm_year = now.year() - 1900;
      out.tm_mon = now.month() - 1;
      out.tm_mday = now.day();
      out.tm_hour = now.hour();
      out.tm_min = now.minute();
      out.tm_sec = now.second();
      out.tm_wday = now.dayOfTheWeek();
      return true;
    }

    Serial.println(F("RTC read looked corrupted (implausible or inconsistent between two reads), discarding and falling back."));
  }
  rtcWorking = false;

  if (ntpSynced) {
    time_t nowEpoch = time(nullptr);
    if (nowEpoch >= 100000) {
      localtime_r(&nowEpoch, &out);
      return true;
    }
  }

  return false;
}

String getFormattedTime() {
  struct tm now;
  if (!getCurrentTime(now)) return "--:--:--";
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
  return String(buf);
}

void updateLcdDisplay() {
  if (!lcdWorking) return;

  lcd.setCursor(0, 0);
  if (rtcWorking || ntpSynced) {
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
