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
#define SW_PIN        5   // Changed from BTN_PIN to SW_PIN

// ── Configuration ────────────────────────────────────────────────────────────
#define GPS_BAUD      9600
#define SERIAL_BAUD   115200
#define LOG_FILENAME  "/gpsdata.csv"
#define LED_BLINK_MS  30   // Duration of write-blink

// ── GPS Serial ───────────────────────────────────────────────────────────────
HardwareSerial gpsSerial(1);

// Create a dedicated, low-level hardware SPI bus for the SD card
SPIClass sdSPI(FSPI);

// ── State ────────────────────────────────────────────────────────────────────
bool   sdReady         = false;
bool   isRecording     = false;
File   logFile;

// LED blink timing
uint32_t ledRestoreTime = 0;   

// Switch state tracking
bool   lastSwitchState = HIGH; 

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

bool isGPRMC(const String &sentence) {
  return sentence.startsWith("$GPRMC") || sentence.startsWith("$GNRMC");
}

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
  pinMode(SW_PIN, INPUT_PULLUP);

  // Initialize Switch State
  lastSwitchState = digitalRead(SW_PIN);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[INFO] GPS UART initialized.");

  // 1. Give the virtual SD card time to wake up
  delay(100);

  // 2. Initialize our dedicated hardware bus using your exact pins
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  
  // 3. Force the SD library to use our custom bus instead of the buggy default one
  if (!SD.begin(SD_CS_PIN, sdSPI, 4000000)) {
    Serial.println("[ERROR] SD card mount failed! Check wiring/power.");
  } else {
    Serial.println("[INFO] SD card mounted successfully.");
    sdReady = true;
  }

  Serial.println("[INFO] Slide switch to start recording.");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

  // ── 1. Non-blocking write-blink restore ───────────────────────────────────
  if (isRecording && ledRestoreTime != 0 && millis() >= ledRestoreTime) {
    digitalWrite(LED_PIN, HIGH);
    ledRestoreTime = 0;
  }

  // ── 2. Switch State Reader ────────────────────────────────────────────────
  bool currentSwitchState = digitalRead(SW_PIN);
  
  if (currentSwitchState != lastSwitchState) {
    delay(50); // Small mechanical debounce just in case
    currentSwitchState = digitalRead(SW_PIN);
    
    if (currentSwitchState != lastSwitchState) {
      lastSwitchState = currentSwitchState;
      
      if (currentSwitchState == LOW) {
        startRecording(); // Switch slid to the left (Grounded)
      } else {
        stopRecording();  // Switch slid to the right (Floating/High)
      }
    }
  }

  // ── 3. GPS ingestion ──────────────────────────────────────────────────────
  static String nmeaBuffer = "";

  while (gpsSerial.available()) {
    char c = (char)gpsSerial.read();

    if (c == '\r') continue;   

    if (c == '\n') {
      nmeaBuffer.trim();

      if (nmeaBuffer.length() > 0) {
        if (isRecording && isGPRMC(nmeaBuffer)) {
          if (isValidNMEA(nmeaBuffer)) {
            String csvRow = parseNMEAtoCSV(nmeaBuffer);
            logFile.println(csvRow);
            logFile.flush();

            digitalWrite(LED_PIN, LOW);
            ledRestoreTime = millis() + LED_BLINK_MS;

            Serial.print("[LOG] ");
            Serial.println(csvRow);
          } else {
            Serial.println("[WARN] Bad checksum – skipped.");
          }
        } else if (!isRecording) {
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