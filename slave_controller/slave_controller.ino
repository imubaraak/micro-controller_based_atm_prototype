#include <Wire.h> 
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>

const byte I2C_SLAVE_ADDR = 0x08;

// --- I2C Command Codes ---
const byte CMD_FP_VERIFY_START   = 0x01;  
const byte CMD_FP_ENROLL_START   = 0x02;  
const byte CMD_FP_RESET          = 0x03;         
const byte CMD_SEND_SMS          = 0x04;         
const byte CMD_DISPENSE          = 0x05;         
const byte CMD_SET_ID            = 0x07;           
const byte CMD_GET_VERIFIED_ID   = 0x08;  
const byte CMD_FP_DELETE         = 0x09;
const byte CMD_UPDATE_BAL_NOTIFY = 0x0A;
const byte CMD_GET_SENSOR_STATUS = 0x0B; // Checks sensor hardware connection
const byte CMD_GET_GSM_STATUS = 0x0C;

// --- Status Codes ---
const byte STATUS_FP_IN_PROGRESS   = 0x00; 
const byte STATUS_FP_SUCCESS       = 0x10;
const byte STATUS_FP_FAILED        = 0x11;
const byte STATUS_FP_ENROLL_FAILED  = 0x14;
const byte STATUS_FP_REMOVE_FINGER = 0x15;
const byte STATUS_FP_PLACE_AGAIN   = 0x16;
const byte STATUS_FP_SENSOR_OK     = 0x17;
const byte STATUS_FP_SENSOR_ERROR  = 0x18;
const byte STATUS_ACK              = 0x50;

SoftwareSerial fingerprintSerial(2, 3); // RX, TX
Adafruit_Fingerprint fingerprintSensor = Adafruit_Fingerprint(&fingerprintSerial);
SoftwareSerial gsmSerial(6, 7);        // RX, TX
const int DISPENSER_RELAY_PIN = 12;

// --- Shared ISR/loop state variables ---
volatile byte i2c_status_code = STATUS_ACK;
volatile byte targetEnrollId = 0;
volatile byte verifiedId = 0;
volatile bool isVerifying = false;
volatile bool isEnrolling = false;
volatile bool isDeleting = false;
volatile bool isCheckingSensor = false; 
volatile bool idRequested = false; // Solves timing mismatch during fetch requests
volatile byte deleteId = 0;
volatile bool gsmStatusRequested = false;
unsigned long dispenseStartTime = 0;
bool isDispensing = false;
const int COUNT_TIME = 200;
byte gsmStatus = 0;

char gsmNumber[25];
char gsmMsg[100];
volatile bool pendingSms = false;

// Safe sensor initialization at multiple baud speeds
bool initFingerprintSensor() {
  fingerprintSerial.listen();
  
  fingerprintSensor.begin(57600);
  if (fingerprintSensor.verifyPassword()) {
    Serial.println(F("[FINGER] Sensor verified at 57600."));
    return true;
  }
  
  delay(100);
  
  fingerprintSensor.begin(9600);
  if (fingerprintSensor.verifyPassword()) {
    Serial.println(F("[FINGER] Sensor verified fallback 9600."));
    return true;
  }
  
  return false;
}

void setup() {
  Serial.begin(9600);
  Wire.begin(I2C_SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  
  gsmSerial.begin(9600);
  
  pinMode(10, OUTPUT); // Buzzer
  pinMode(12, OUTPUT); // Motor Pin
  
  if (initFingerprintSensor()) {
    digitalWrite(10, HIGH); delay(100); digitalWrite(10, LOW);
  } else {
    Serial.println(F("[ERROR] Fingerprint Sensor wiring / baud fault!"));
  }
}

void loop() {
  // --- Automatic GSM Status Refresh ---
  static unsigned long lastGsmCheck = 0;
  // Check every 30 seconds to keep the variable updated
  if (millis() - lastGsmCheck >= 30000 || lastGsmCheck == 0) {
    gsmStatus = getGsmStatus(); 
    lastGsmCheck = millis();
    Serial.print(F("[GSM] Status Updated: ")); 
    Serial.println(gsmStatus);
  }
  
  // --- Safe Sensor Handshake execution (OUTSIDE Interrupt ISR Context) ---
  if (isCheckingSensor) {
    Serial.println(F("[CHECK] Received checking request. Activating Serial sensor check..."));
    fingerprintSerial.listen();
    if (fingerprintSensor.verifyPassword()) {
      i2c_status_code = STATUS_FP_SENSOR_OK;
      Serial.println(F("[CHECK] SUCCESS: Sensor verified connected and online."));
    } else {
      i2c_status_code = STATUS_FP_SENSOR_ERROR;
      Serial.println(F("[CHECK] ERROR: Sensor password handshake failed."));
    }
    isCheckingSensor = false; // Reset task flag
  }

  // --- Fingerprint Verification ---
  if (isVerifying) {
    fingerprintSerial.listen();
    Serial.println(F("[VERIFY] Waiting for valid finger..."));
    
    unsigned long startTime = millis();
    bool matchFinished = false;

    // Wait up to 10 seconds for user to put their finger on the sensor
    while (millis() - startTime < 10000) { 
      int p = fingerprintSensor.getImage();
      if (p == FINGERPRINT_OK) {
        p = fingerprintSensor.image2Tz();
        if (p == FINGERPRINT_OK) {
          p = fingerprintSensor.fingerFastSearch();
          if (p == FINGERPRINT_OK) { 
            verifiedId = fingerprintSensor.fingerID; 
            i2c_status_code = STATUS_FP_SUCCESS; 
            digitalWrite(10, HIGH); delay(200); digitalWrite(10, LOW);
            Serial.print(F("[VERIFY] MATCH FOUND! ID: ")); Serial.println(verifiedId);
            matchFinished = true;
            break;
          } else if (p == FINGERPRINT_NOTFOUND) {
            i2c_status_code = STATUS_FP_FAILED;
            Serial.println(F("[VERIFY] No match found in database."));
            matchFinished = true;
            break;
          }
        }
      }
      delay(100); // Prevent CPU thrashing
    }

    if (!matchFinished) {
      i2c_status_code = STATUS_FP_FAILED;
      Serial.println(F("[VERIFY] Timeout. No finger placed."));
    }
    isVerifying = false;
  }

  // --- Fingerprint Enrollment ---
  if (isEnrolling) {
    fingerprintSerial.listen();
    if (enrollFingerprint(targetEnrollId)) {
      i2c_status_code = STATUS_FP_SUCCESS;
    } else {
      i2c_status_code = STATUS_FP_ENROLL_FAILED;
    }
    isEnrolling = false;
  }

  // --- Fingerprint Deletion ---
  if (isDeleting) {
    fingerprintSerial.listen();
    if (fingerprintSensor.deleteModel(deleteId) == FINGERPRINT_OK) {
      Serial.print(F("Successfully deleted ID: ")); Serial.println(deleteId);
    } else {
      Serial.println(F("Failed to delete fingerprint ID from sensor memory."));
    }
    isDeleting = false;
  }

//--- DISPENSE ---
  if (isDispensing) {
      if (millis() - dispenseStartTime >= COUNT_TIME) {
        digitalWrite(DISPENSER_RELAY_PIN, HIGH);
        delay(1500);
        digitalWrite(DISPENSER_RELAY_PIN, LOW);
        
        isDispensing = false;
        Serial.println(F("Dispense cycle complete."));
      }
    }

  // --- GSM SMS Handler ---
  if (pendingSms) {
    Serial.println(F("[SMS] Starting transmission..."));

    gsmSerial.listen(); 
    gsmSerial.println("AT+CMGF=1"); 
    delay(500);
    
    gsmSerial.print("AT+CMGS=\""); 
    gsmSerial.print(gsmNumber); 
    gsmSerial.println("\"");
    delay(500); 
    
    gsmSerial.print(gsmMsg); 
    delay(500);
    
    gsmSerial.write(26); 
    
    // NEW: Capture response while waiting for the network to send
    unsigned long sendTimeout = millis();
    while (millis() - sendTimeout < 5000) {
      while (gsmSerial.available()) {
        Serial.write(gsmSerial.read()); 
      }
    }
    
    pendingSms = false;
    Serial.println(F("\n[SMS] Task complete."));
  }
}

byte getGsmStatus() {
  gsmSerial.listen(); // Ensure the GSM serial port is active
  gsmSerial.println(F("AT+CREG?"));
  
  unsigned long timeout = millis();
  while (millis() - timeout < 1500) { // Increased timeout to 1.5 seconds for stability
    if (gsmSerial.available()) {
      // Look for registration codes ,1 (home) or ,5 (roaming) anywhere in the response stream
      if (gsmSerial.find((char*)",1") || gsmSerial.find((char*)",5")) {
        return 1; // Registered
      }
    }
  }
  return 0; // Failed to register within timeout
}

void receiveEvent(int howMany) {
  if (howMany < 1) return;
  byte cmd = Wire.read();

  if (cmd == CMD_SEND_SMS) {
    int i = 0;
    while(Wire.available()) {
      char c = Wire.read();
      if(c == ':') break;
      if(i < 24) gsmNumber[i++] = c;
    }
    gsmNumber[i] = '\0';
  } 
  else if (cmd == 0xFE) { // Message Body detected
    int i = 0;
    while(Wire.available()) {
      char c = Wire.read();
      if(c == '\0') break;
      if(i < 99) gsmMsg[i++] = c;
    }
    gsmMsg[i] = '\0';
    pendingSms = true; // Now trigger the SMS
  }
  else if (cmd == 0xFD) { 
    // Capture and print OTP on Slave Serial Monitor
    char slaveOtp[7];
    int i = 0;
    while (Wire.available() && i < 6) {
      slaveOtp[i++] = Wire.read();
    }
    slaveOtp[i] = '\0';
    Serial.print(F("[SLAVE DEBUG] Received OTP: "));
    Serial.println(slaveOtp);
  }
  
  else if (cmd == CMD_FP_VERIFY_START) { 
    i2c_status_code = STATUS_FP_IN_PROGRESS; 
    isVerifying = true; 
  }
  else if (cmd == CMD_FP_ENROLL_START) { 
    i2c_status_code = STATUS_FP_IN_PROGRESS; 
    isEnrolling = true; 
  }
  else if (cmd == CMD_FP_DELETE) { 
    if (Wire.available()) { 
      deleteId = Wire.read(); 
      isDeleting = true; 
    } 
  }
  else if (cmd == CMD_FP_RESET) {
    fingerprintSensor.emptyDatabase();
    Serial.println(F("Database completely cleared."));
  }
  else if (cmd == CMD_SET_ID) { 
    if (Wire.available()) targetEnrollId = Wire.read(); 
  }
  else if (cmd == CMD_GET_VERIFIED_ID) {
    idRequested = true; 
  }
  else if (cmd == CMD_DISPENSE) {
    dispenseStartTime = millis();
    isDispensing = true;
    Serial.println(F("Dispense started..."));
  }
  else if (cmd == CMD_GET_SENSOR_STATUS) {
    // Flag the main loop to run check securely without interrupting I2C wire ISR context
    i2c_status_code = STATUS_FP_IN_PROGRESS;
    isCheckingSensor = true; 
  }
  else if (cmd == CMD_GET_GSM_STATUS) {
    gsmStatusRequested = true;
  }
  else if (cmd == CMD_UPDATE_BAL_NOTIFY) {
    int i = 0; while (Wire.available()) { char c = Wire.read(); if (c == ':') break; if (i < 13) gsmNumber[i++] = c; }
    gsmNumber[i] = '\0';
    char balance[16]; i = 0; while (Wire.available()) { char c = Wire.read(); if (c == '\0') break; if (i < 15) balance[i++] = c; }
    balance[i] = '\0';
    sprintf(gsmMsg, "Admin Update: New Bal NGN %s", balance);
    pendingSms = true;
  }
}

void requestEvent() {
  if (gsmStatusRequested) {
    Wire.write(gsmStatus);
    gsmStatusRequested = false;
  }
  else if (idRequested) { 
    Wire.write(verifiedId); 
    idRequested = false; 
    i2c_status_code = STATUS_ACK; 
  }
  else {
    Wire.write(i2c_status_code);
  }
}

bool enrollFingerprint(int id) {
  int p = -1;
  
  // --- Capture 1 ---
  while (p != FINGERPRINT_OK) {
    p = fingerprintSensor.getImage();
    if (p == FINGERPRINT_PACKETRECIEVEERR) return false;
  }
  if (fingerprintSensor.image2Tz(1) != FINGERPRINT_OK) return false;
  
  i2c_status_code = STATUS_FP_REMOVE_FINGER;
  digitalWrite(10, HIGH); delay(150); digitalWrite(10, LOW);
  
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = fingerprintSensor.getImage();
    delay(50);
  }
  
  i2c_status_code = STATUS_FP_PLACE_AGAIN;
  
  // --- Capture 2 ---
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = fingerprintSensor.getImage();
  }
  
  if (fingerprintSensor.image2Tz(2) != FINGERPRINT_OK) return false;
  
  if (fingerprintSensor.createModel() != FINGERPRINT_OK) return false;
  if (fingerprintSensor.storeModel(id) != FINGERPRINT_OK) return false;
  
  digitalWrite(10, HIGH); delay(500); digitalWrite(10, LOW);
  return true;
}
