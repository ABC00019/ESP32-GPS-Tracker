#include <Arduino.h>
#include <HardwareSerial.h>
#include <SD.h>
#include <SPI.h>

// ── Pin Definitions ──────────────────────────────────────────────────────────
#define GPS_RX_PIN   18
#define GPS_TX_PIN   17
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
#define DEBOUNCE_MS   50

// ── GPS Serial ───────────────────────────────────────────────────────────────
HardwareSerial gpsSerial(1);

// ── State ────────────────────────────────────────────────────────────────────
bool   sdReady         = false;
bool   isRecording     = false;
File   logFile;

// LED blink timing (non-blocking)
bool     ledOn          = false;
uint32_t ledOffTime     = 0;

// Button debounce
bool     lastBtnPhysical  = HIGH;
bool     lastBtnDebounced = HIGH;
uint32_t lastDebounceTime = 0;

// ── Forward Declarations ─────────────────────────────────────────────────────
void setLED(bool on);
bool isValidNMEA(const String &sentence);
void dumpCSV();
void startRecording();
void stopRecording();
String parseNMEAtoCSV(const String &sentence);

// ── Helpers ──────────────────────────────────────────────────────────────────

void setLED(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

bool isValidNMEA(const String &sentence) {
  if (sentence.length() < 10)          return false;
  if (sentence.charAt(0) != '$')       return false;
  int starIdx = sentence.lastIndexOf('*');
  if (starIdx < 0 || starIdx + 2 >= (int)sentence.length()) return false;

  uint8_t computed = 0;
  for (int i = 1; i < starIdx; i++) computed ^= (uint8_t)sentence.charAt(i);

  String hexStr = sentence.substring(starIdx + 1, starIdx + 3);
  hexStr.toUpperCase();
  uint8_t provided = (uint8_t)strtol(hexStr.c_str(), nullptr, 16);
  return computed == provided;
}

// Strips the leading '$' and writes the raw NMEA fields as CSV.
// The NMEA sentence is already comma-delimited; we just clean it up.
// Format: $GPRMC,time,status,lat,N/S,lon,E/W,speed,angle,date,magvar,magdir*checksum
String parseNMEAtoCSV(const String &sentence) {
  // Remove leading '$' and everything from '*' onward, keep checksum separately
  int starIdx = sentence.lastIndexOf('*');
  String checksum = (starIdx >= 0) ? sentence.substring(starIdx + 1) : "";
  String body     = sentence.substring(1, starIdx >= 0 ? starIdx : sentence.length());

  // body is already comma-separated: GPRMC,field1,field2,...
  // Append checksum as final column
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
  Serial.println("====== END CSV EXPORT ======\n");
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
    // Column order matches NMEA field order with checksum appended
    logFile.println("Message_Type,Time,Status,Latitude,NS,Longitude,EW,Speed,Angle,Date,MagVar,MagDir,Checksum");
    logFile.flush();
    Serial.println("[INFO] New CSV created. Headers written.");
  }
  isRecording = true;
  setLED(true);   // solid ON while recording, blinks on each write
  Serial.println("[INFO] Recording STARTED → " LOG_FILENAME);
}

void stopRecording() {
  if (logFile) { logFile.flush(); logFile.close(); }
  isRecording = false;
  setLED(false);
  Serial.println("[INFO] Recording STOPPED.");
  dumpCSV();
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);
  Serial.println("\n=== GPS SD Logger ===");

  pinMode(LED_PIN, OUTPUT);  setLED(false);
  pinMode(BTN_PIN, INPUT_PULLUP);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[INFO] GPS UART initialized.");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[ERROR] SD card mount failed! Check wiring.");
  } else {
    Serial.println("[INFO] SD card mounted.");
    sdReady = true;
  }
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

  // ── 1. Non-blocking LED off timer ─────────────────────────────────────────
  if (ledOn && ledOffTime != 0 && millis() >= ledOffTime) {
    digitalWrite(LED_PIN, HIGH);
    ledOffTime = 0;
  }

  // ── 2. Button debounce & toggle ───────────────────────────────────────────
  bool rawBtn = digitalRead(BTN_PIN);
  if (rawBtn != lastBtnPhysical) {
    lastDebounceTime = millis();
    lastBtnPhysical  = rawBtn;
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_MS && rawBtn != lastBtnDebounced) {
    lastBtnDebounced = rawBtn;
    if (rawBtn == LOW) {
      isRecording ? stopRecording() : startRecording();
    }
  }

  // ── 3. GPS ingestion ──────────────────────────────────────────────────────
  static String nmeaBuffer = "";

  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();
    if (c == '\n') {
      nmeaBuffer.trim();
      if (nmeaBuffer.length() > 0 && isRecording) {
        if (isValidNMEA(nmeaBuffer)) {
          String csvRow = parseNMEAtoCSV(nmeaBuffer);
          logFile.println(csvRow);
          logFile.flush();
          digitalWrite(LED_PIN, LOW);
          ledOffTime = millis() + 30;
          Serial.print("[LOG] "); Serial.println(csvRow);
        } else {
          Serial.println("[WARN] Bad checksum – skipped: " + nmeaBuffer);
        }
      }
      nmeaBuffer = "";
    } else {
      nmeaBuffer += c;
    }
  }
}