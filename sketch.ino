#include <Arduino.h>
#include <HardwareSerial.h>
#include <SD.h>
#include <SPI.h>

// ── Pin Definitions ──────────────────────────────────────────────────────────
#define GPS_RX_PIN   18   // ESP RX ← GPS TX
#define GPS_TX_PIN   17   // ESP TX → GPS RX
#define SD_CS_PIN    10
#define SD_MOSI_PIN  11
#define SD_SCK_PIN   12
#define SD_MISO_PIN  13
#define LED_PIN       4
#define BTN_PIN       5

// ── Configuration ────────────────────────────────────────────────────────────
#define GPS_BAUD      9600
#define SERIAL_BAUD   115200
#define LOG_FILENAME  "/gpsdata.csv"
#define DEBOUNCE_MS   10
#define LED_BLINK_MS  30   // Duration of write-blink (LED goes LOW briefly)

// ── GPS Serial ───────────────────────────────────────────────────────────────
HardwareSerial gpsSerial(1);

// ── State ────────────────────────────────────────────────────────────────────
bool   sdReady         = false;
bool   isRecording     = false;
File   logFile;

// LED blink timing (non-blocking)
uint32_t ledRestoreTime = 0;   // When to restore LED HIGH after a write-blink

// Button debounce
bool     lastBtnPhysical  = HIGH;
bool     lastBtnDebounced = HIGH;
uint32_t lastDebounceTime = 0;

// ── Forward Declarations ─────────────────────────────────────────────────────
bool isValidNMEA(const String &sentence);
bool isGPRMC(const String &sentence);
void dumpCSV();
void startRecording();
void stopRecording();
String parseNMEAtoCSV(const String &sentence);

// ── Helpers ──────────────────────────────────────────────────────────────────

bool isValidNMEA(const String &sentence) {
  if (sentence.length() < 10)        return false;
  if (sentence.charAt(0) != '$')     return false;
  int starIdx = sentence.lastIndexOf('*');
  if (starIdx < 0 || starIdx + 2 >= (int)sentence.length()) return false;

  uint8_t computed = 0;
  for (int i = 1; i < starIdx; i++) computed ^= (uint8_t)sentence.charAt(i);

  String hexStr = sentence.substring(starIdx + 1, starIdx + 3);
  hexStr.toUpperCase();
  uint8_t provided = (uint8_t)strtol(hexStr.c_str(), nullptr, 16);
  return computed == provided;
}

// Only log GPRMC sentences so columns stay consistent
bool isGPRMC(const String &sentence) {
  return sentence.startsWith("$GPRMC") || sentence.startsWith("$GNRMC");
}

// Strip '$' prefix and '*XX' checksum from body; return as CSV row with
// checksum appended as a final column.
// GPRMC field order: Message_Type,Time,Status,Lat,NS,Lon,EW,Speed,Angle,Date,MagVar,MagDir
String parseNMEAtoCSV(const String &sentence) {
  int starIdx = sentence.lastIndexOf('*');
  String checksum = (starIdx >= 0) ? sentence.substring(starIdx + 1) : "";
  checksum.trim();
  String body = sentence.substring(1, starIdx >= 0 ? starIdx : sentence.length());
  return body + "," + checksum;
}

void dumpCSV() {
  Serial.println("\n===== BEGIN CSV EXPORT =====");
  File dumpFile = SD.open(LOG_FILENAME, FILE_READ);
  if (!dumpFile) {
    Serial.println("[ERROR] Could not open file for reading.");
    return;
  }
  while (dumpFile.available()) Serial.write(dumpFile.read());
  dumpFile.close();
  Serial.println("\n====== END CSV EXPORT ======\n");
}

void startRecording() {
  if (!sdReady) {
    Serial.println("[WARN] SD not ready – cannot start recording.");
    return;
  }
  bool fileExists = SD.exists(LOG_FILENAME);
  logFile = SD.open(LOG_FILENAME, FILE_APPEND);
  if (!logFile) {
    Serial.println("[ERROR] Failed to open CSV file.");
    return;
  }
  if (!fileExists) {
    logFile.println("Message_Type,Time,Status,Latitude,NS,Longitude,EW,Speed,Angle,Date,MagVar,MagDir,Checksum");
    logFile.flush();
    Serial.println("[INFO] New CSV created. Headers written.");
  }
  isRecording = true;
  digitalWrite(LED_PIN, HIGH);   // Solid ON = recording active
  Serial.println("[INFO] Recording STARTED → " LOG_FILENAME);
}

void stopRecording() {
  if (logFile) { logFile.flush(); logFile.close(); }
  isRecording = false;
  digitalWrite(LED_PIN, LOW);
  ledRestoreTime = 0;
  Serial.println("[INFO] Recording STOPPED.");
  dumpCSV();
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);
  Serial.println("\n=== GPS SD Logger ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BTN_PIN, INPUT_PULLUP);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[INFO] GPS UART initialized (RX=18, TX=17).");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[ERROR] SD card mount failed! Check wiring/power.");
  } else {
    Serial.println("[INFO] SD card mounted.");
    sdReady = true;
  }

  Serial.println("[INFO] Press button to start recording.");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

  digitalWrite(LED_PIN, !digitalRead(BTN_PIN));
  
  static bool lastRaw = HIGH;
  bool currentRaw = digitalRead(BTN_PIN);
  if (currentRaw != lastRaw) {
    Serial.print("!!! BUTTON RAW SIGNAL CHANGED TO: ");
    Serial.println(currentRaw == LOW ? "0 (PRESSED)" : "1 (RELEASED)");
    lastRaw = currentRaw;
  }
  // ── 1. Non-blocking write-blink restore ───────────────────────────────────
  // After a write the LED dips LOW for LED_BLINK_MS ms, then comes back HIGH.
  if (isRecording && ledRestoreTime != 0 && millis() >= ledRestoreTime) {
    digitalWrite(LED_PIN, HIGH);
    ledRestoreTime = 0;
  }

  // ── 2. Button debounce & toggle ───────────────────────────────────────────
  bool rawBtn = digitalRead(BTN_PIN);
  if (rawBtn != lastBtnPhysical) {
    lastDebounceTime = millis();
    lastBtnPhysical  = rawBtn;
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_MS && rawBtn != lastBtnDebounced) {
    lastBtnDebounced = rawBtn;
    if (rawBtn == LOW) {   // Falling edge = button pressed
      isRecording ? stopRecording() : startRecording();
    }
  }

  // ── 3. GPS ingestion ──────────────────────────────────────────────────────
  static String nmeaBuffer = "";

  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();

    if (c == '\r') continue;   // Ignore carriage returns

    if (c == '\n') {
      nmeaBuffer.trim();

      if (nmeaBuffer.length() > 0) {
        if (isRecording && isGPRMC(nmeaBuffer)) {
          if (isValidNMEA(nmeaBuffer)) {
            String csvRow = parseNMEAtoCSV(nmeaBuffer);
            logFile.println(csvRow);
            logFile.flush();

            // Brief LOW blink to indicate a successful write
            digitalWrite(LED_PIN, LOW);
            ledRestoreTime = millis() + LED_BLINK_MS;

            Serial.print("[LOG] ");
            Serial.println(csvRow);
          } else {
            Serial.println("[WARN] Bad checksum – skipped: " + nmeaBuffer);
          }
        } else if (!isRecording) {
          // Echo non-logged sentences to serial for debugging
          Serial.print("[GPS] ");
          Serial.println(nmeaBuffer);
        }
      }

      nmeaBuffer = "";
    } else {
      nmeaBuffer += c;
    }
  }
}
