#include <Wire.h>
#include <EEPROM.h>
#include <LiquidCrystal.h>
#include "RTClib.h"
#include <Keypad.h>

// --- RTC Setup ---
RTC_DS1307 rtc; 

// --- I2C Constants ---
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

// --- I2C Status Codes ---
const byte STATUS_FP_IN_PROGRESS   = 0x00; 
const byte STATUS_FP_SUCCESS       = 0x10;
const byte STATUS_FP_FAILED        = 0x11;
const byte STATUS_FP_ENROLL_FAILED  = 0x14;
const byte STATUS_FP_REMOVE_FINGER = 0x15; // Feedback: Lift finger
const byte STATUS_FP_PLACE_AGAIN   = 0x16; // Feedback: Place again
const byte STATUS_FP_SENSOR_OK     = 0x17; // Feedback: Sensor is online and responsive
const byte STATUS_FP_SENSOR_ERROR  = 0x18; // Feedback: Sensor is missing or errored
const byte STATUS_ACK              = 0x50;

const int MAX_CUSTOMERS = 20; 
const unsigned long INACTIVITY_TIMEOUT = 75000; //75 seconds

// --- Keypad Definitions ---
const byte ROWS = 5;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'A', 'B', '#', '*'},
  {'1', '2', '3', 'U'},
  {'4', '5', '6', 'D'},
  {'7', '8', '9', 'X'},
  {'L', '0', 'R', 'E'}
};
byte rowPins[ROWS] = {10, 9, 8, 7, 6}; 
byte colPins[COLS] = {17, 16, 15, 14}; //A3, A2, A1, A0

const int rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

struct CustomerData {
  char customerName[16];
  char customerNumber[16];
  long customerBalance;
  char pin[5]; 
  unsigned long lockoutUntil;
  byte pinAttempts;
  byte isUsed; // Byte-based allocation bypasses AVR compiler bool logic error (255 reads)
};

enum class AppState {
  MAIN_MENU, AWAITING_MENU_CHOICE, ENROLL_WIZARD_PHONE, ENROLL_WIZARD_BALANCE,
  ENROLL_WIZARD_PIN, ENROLL_WAITING_SLAVE, BANKING_FP_VERIFY, BANKING_PIN_ENTRY,
  BANKING_OTP_ENTRY, BANKING_MENU, WITHDRAWAL_TYPE, QUICK_CASH_MENU,
  WITHDRAWAL_AMOUNT, DISPENSING_CASH, ADMIN_LOGIN, ADMIN_MENU,
  VIEW_BALANCES, MODIFY_BALANCE_ID, MODIFY_BALANCE_VAL, DELETE_USER, CONFIRM_WIPE,
  TRANSFER_PHONE, TRANSFER_AMOUNT
};

AppState currentAppState = AppState::MAIN_MENU;
CustomerData tempCustomer;
CustomerData recipientCustomer;
byte targetId;
byte currentCustomerId;
byte recipientId;
char inputBuffer[16];
char generatedOtp[7];
uint8_t fpRetryCount = 0;         // Tracks fingerprint retry attempts
uint8_t pinRetryCount = 0;        // Tracks PIN retry attempts
uint8_t otpRetryCount = 0;        // Tracks invalid OTP input attempts
uint32_t otpCreationUnixTime = 0; // Timestamp for expiry check
bool isInputReady = false;
bool refreshMenu = true;          // Prevents Admin/Main LCD flickering
bool isSendingSms = false;
unsigned long stateTimer = 0;
unsigned long lastActivityTime = 0;
unsigned long lastGsmCheck = 0;
const unsigned long GSM_CHECK_INTERVAL = 30000; // 30 seconds
int viewIndex = 0;
byte inputIndex = 0;
byte currentGsmStatus = 0;        // 0 = Down, 1 = Ready

// --- Polling System Variables ---
byte cachedSlaveStatus = STATUS_FP_IN_PROGRESS;
unsigned long lastPollTime = 0;

void updateLcd(const char* l1, const char* l2 = "", const char* l3 = "", const char* l4 = "") {
  // Overwrite rows line-by-line using 20-character formatted strings instead of lcd.clear()
  char lineBuf[21];

  lcd.setCursor(0, 0);
  snprintf(lineBuf, sizeof(lineBuf), "%-20s", l1);
  lcd.print(lineBuf);

  lcd.setCursor(0, 1);
  snprintf(lineBuf, sizeof(lineBuf), "%-20s", l2);
  lcd.print(lineBuf);

  lcd.setCursor(0, 2);
  snprintf(lineBuf, sizeof(lineBuf), "%-20s", l3);
  lcd.print(lineBuf);

  lcd.setCursor(0, 3);
  snprintf(lineBuf, sizeof(lineBuf), "%-20s", l4);
  lcd.print(lineBuf);
}
void updateLcd(const __FlashStringHelper* l1, const __FlashStringHelper* l2, const __FlashStringHelper* l3, const __FlashStringHelper* l4) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
  lcd.setCursor(0, 2); lcd.print(l3);
  lcd.setCursor(0, 3); lcd.print(l4);
}

void resetInput() {
  inputIndex = 0;
  memset(inputBuffer, 0, sizeof(inputBuffer));
  isInputReady = false;
  lastActivityTime = millis();
}

void saveCustomer(byte id, CustomerData &data) {
  data.isUsed = 1; 
  EEPROM.put((id - 1) * sizeof(CustomerData), data);
}

void checkGsmStatus() {
  if (isSendingSms) return; // Don't check GSM if we are currently sending an SMS
  if (millis() - lastGsmCheck >= GSM_CHECK_INTERVAL || lastGsmCheck == 0) {
    Wire.beginTransmission(I2C_SLAVE_ADDR);
    Wire.write(CMD_GET_GSM_STATUS);
    byte err = Wire.endTransmission();
    
    if (err == 0) {
      Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)1);
      if (Wire.available()) {
        byte newStatus = Wire.read();
        // Trigger a refresh ONLY if the status actually changed
        if (newStatus != currentGsmStatus) {
          currentGsmStatus = newStatus;
          refreshMenu = true; // <--- ADD THIS LINE
          Serial.println(F("[SYSTEM] GSM Status changed. Refreshing Menu..."));
        }
      }
    }
    lastGsmCheck = millis();
  }
}

void sendSms(const char* number, const char* msg) {

  if (currentGsmStatus == 0) {
    Serial.println(F("[WARNING] Attempted to send SMS, but GSM Network is Down!"));
    updateLcd(F("GSM Network is Down!"), F(""), F(""), F(""));
    delay(1000);
  }

  isSendingSms = true;
  char fullNumber[16];
  
  if (number[0] == '0') sprintf(fullNumber, "+234%s", &number[1]);
  else strcpy(fullNumber, number);

  // Chunk 1: Send the Command and the Number
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  Wire.write(CMD_SEND_SMS);
  for(int i=0; i<(int)strlen(fullNumber); i++) Wire.write(fullNumber[i]);
  Wire.write(':');
  Wire.endTransmission(); // Flushes the first 16-20 bytes
  
  delay(50); // Important: Give the Slave time to process the first chunk

  // Chunk 2: Send the Message Body
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  Wire.write(0xFE); // Use a secondary flag to tell Slave "this is the message body"
  for(int i=0; i<(int)strlen(msg); i++) Wire.write(msg[i]);
  Wire.write('\0');
  Wire.endTransmission(); // Flushes the remaining bytes
  
  isSendingSms = false;
}

// Resets the state of the I2C poller for fresh verify or enroll cycles
void resetSlavePoll() {
  cachedSlaveStatus = STATUS_FP_IN_PROGRESS;
  lastPollTime = millis();
}

// Poll function prevents I2C traffic from corrupting Slave SoftwareSerial bits
byte pollSlave() {
  if (millis() - lastPollTime >= 300) { // Limit polling to every 300ms
    lastPollTime = millis();
    Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)1);
    if (Wire.available()) {
      cachedSlaveStatus = Wire.read();
    }
  }
  return cachedSlaveStatus;
}

// Queries slave device with an interrupt-free to verify fingerprint module communication health
bool checkSensorStatus() {
  Serial.println(F("[I2C] Sending CMD_GET_SENSOR_STATUS to Slave..."));
  
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  Wire.write(CMD_GET_SENSOR_STATUS);
  byte error = Wire.endTransmission();
  
  if (error != 0) {
    Serial.print(F("[I2C] Transmission failed! Error code: "));
    Serial.println(error);
    Serial.println(F("[I2C] Error 2 = NACK (Slave not physically wired or powered)"));
    Serial.println(F("[I2C] Error 4 = General Bus fault (Check SDA/SCL pull-ups)"));
    return false;
  }
  
  Serial.println(F("[I2C] Write Successful. Pausing 400ms for Slave check..."));
  delay(400); 

  Serial.println(F("[I2C] Requesting verification status byte..."));
  Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)1);
  if (Wire.available()) {
    byte status = Wire.read();
    Serial.print(F("[I2C] Status code received: 0x"));
    Serial.println(status, HEX);
    
    if (status == STATUS_FP_SENSOR_OK) {
      Serial.println(F("[I2C] SUCCESS: Sensor is confirmed ONLINE."));
      return true;
    } else {
      Serial.println(F("[I2C] ERROR: Slave reported offline status or generic ack."));
    }
  } else {
    Serial.println(F("[I2C] Read failed! Slave failed to return any I2C bytes."));
  }
  return false;
}

int findNextFreeSlot() {
  for (int i = 0; i < MAX_CUSTOMERS; i++) {
    CustomerData check;
    EEPROM.get(i * sizeof(CustomerData), check);
    if (check.isUsed != 1) {
      return i + 1; 
    }
  }
  return -1;
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  lcd.begin(20, 4);
  if (!rtc.begin()) { Serial.println("RTC not found!"); }
  randomSeed(analogRead(0)); 
  updateLcd(F("ATBU ATM"), F("Booting..."), F(""), F(""));
  delay(1500);
  updateLcd("ENHANCED ATM", "BIOMETRIC USING", "FINGERPRINT", "AUTHENTICATION");
  delay(4000);
  updateLcd(F("AHMAD, MUHAMMAD"), F("KHAMISU"), F("PGS/18-19/5/M/10611"), F(""));
  delay(4000);
  updateLcd(F("SUPERVISED BY:"), F("PROF. A. Y. GITAL"), F("PROF. A. Y. DUTSE"), F("Loading System..."));
  delay(4000);
  Serial.println(F("[SYSTEM] Master Controller initialized and ready."));
}

void loop() {
  checkGsmStatus();
  char key = customKeypad.getKey();
  if (key) {
    lastActivityTime = millis();
    refreshMenu = true; 
    if (key >= '0' && key <= '9') {
      if (inputIndex < 15) {
        inputBuffer[inputIndex++] = key;
        inputBuffer[inputIndex] = '\0';
        lcd.setCursor(inputIndex - 1, 2);
        if (currentAppState == AppState::BANKING_PIN_ENTRY || currentAppState == AppState::ADMIN_LOGIN || 
            currentAppState == AppState::ENROLL_WIZARD_PIN || currentAppState == AppState::BANKING_OTP_ENTRY) 
          lcd.print('*');
        else lcd.print(key);
      }
    } else if (key == '*') {
      resetInput();
      updateLcd(F("Cleared"), F("Re-enter:"), F(""), F(""));
    }
  }

  if (currentAppState != AppState::MAIN_MENU && (millis() - lastActivityTime > INACTIVITY_TIMEOUT)) {
      currentAppState = AppState::MAIN_MENU;
      refreshMenu = true;
      return;
  }

  switch (currentAppState) {
    case AppState::MAIN_MENU:
      if (refreshMenu) {
        if (currentGsmStatus == 0) {
          updateLcd(F("F1. Admin"), F("F2. Banking"), F("[GSM Net: Offline]"), F("Choice:"));
        } else {
          updateLcd(F("F1. Admin"), F("F2. Banking"), F("[GSM Net: Online]"), F("Choice:"));
        }
        refreshMenu = false;
      }
      currentAppState = AppState::AWAITING_MENU_CHOICE;
      break;

    case AppState::AWAITING_MENU_CHOICE:
      if (key == 'A') {
        updateLcd(F("ADMIN LOGIN"), F("Enter Admin PIN:"), F(""), F("Press # to confirm")); resetInput(); currentAppState = AppState::ADMIN_LOGIN;

      } else if (key == 'B') {
        updateLcd(F("Checking Sensor..."), F("Please wait"), F(""), F(""));
        if (checkSensorStatus()) {
          updateLcd(F("Banking"), F("Place Finger"), F("Sensor: ONLINE"), F(""));
          Wire.beginTransmission(I2C_SLAVE_ADDR); 
          Wire.write(CMD_FP_VERIFY_START); 
          Wire.endTransmission();
          resetSlavePoll();
          currentAppState = AppState::BANKING_FP_VERIFY;
        } else {
          updateLcd(F("Sensor Offline!"), F("Check hardware conn"), F("Returning to home"), F(""));
          delay(3000);
          currentAppState = AppState::MAIN_MENU;
          refreshMenu = true;
        }
      }
      break;

    case AppState::ADMIN_LOGIN:
      if (key == '#') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Aborted"), F("Returning to Menu"), F(""), F(""));
        delay(1000);
        currentAppState = AppState::MAIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        if (strcmp(inputBuffer, "1234") == 0) {
            currentAppState = AppState::ADMIN_MENU;
            refreshMenu = true;
        }
        else { 
          updateLcd(F("Access Denied"), F(""), F(""), F("")); 
          delay(2000); 
          currentAppState = AppState::MAIN_MENU; 
          refreshMenu = true; 
        }
        resetInput();
      }
      break;

    case AppState::ADMIN_MENU:
      if (refreshMenu) {
        updateLcd(F("1.Users 2.AddUser"), F("3.Deposit/Withdraw"), F("4.Delete 5.Reset "), F("(*:Exit)"));
        refreshMenu = false;
      }
      
      if (key == '1') { viewIndex = 0; currentAppState = AppState::VIEW_BALANCES; refreshMenu = true; }
      else if (key == '2') { 
        updateLcd(F("Checking Sensor..."), F("Please wait"), F(""), F(""));
        if (checkSensorStatus()) {
          int slot = findNextFreeSlot();
          if (slot == -1) { 
            updateLcd(F("Full"), F("Max 20 Users"), F(""), F("")); 
            delay(2000); 
            currentAppState = AppState::ADMIN_MENU; 
            refreshMenu = true; 
          }
          else { 
            targetId = (byte)slot; 
            updateLcd(F("Enrollment"), F("Enter Phone:"), F(""), F("Press # to Submit")); 
            resetInput(); 
            currentAppState = AppState::ENROLL_WIZARD_PHONE; 
          }
        } else {
          updateLcd(F("Sensor Offline!"), F("Enrollment aborted."), F("Check hardware conn."), F(""));
          delay(3000);
          currentAppState = AppState::ADMIN_MENU;
          refreshMenu = true;
        }
      }
      else if (key == '3') { updateLcd(F("Deposit/Withdraw Bal"), F("Enter Acct. ID:"), F(""), F("Press # to Submit")); resetInput(); currentAppState = AppState::MODIFY_BALANCE_ID; }
      else if (key == '4') { updateLcd(F("Delete User"), F("Enter Acct. ID:"), F(""), F("")); resetInput(); currentAppState = AppState::DELETE_USER; }
      else if (key == '5') { updateLcd(F(""), F("Confirm Wipe?"), F(""), F("#: Wipe  *: Cancel")); currentAppState = AppState::CONFIRM_WIPE; }
      else if (key == '*') { currentAppState = AppState::MAIN_MENU; refreshMenu = true; }
      break;

    case AppState::ENROLL_WIZARD_PHONE:
      if (key == '#') { isInputReady = true; }
      else if (key == 'X') { currentAppState = AppState::ADMIN_MENU; refreshMenu = true; resetInput(); break; }

      if (isInputReady) { 
        strncpy(tempCustomer.customerNumber, inputBuffer, 12); 
        updateLcd(F("Initial Deposit:"), F("NGN"), F(""), F("Press # to Submit")); 
        resetInput(); 
        currentAppState = AppState::ENROLL_WIZARD_BALANCE; 
      }
      break;

    case AppState::ENROLL_WIZARD_BALANCE:
      if (key == '#') { isInputReady = true; }
      else if (key == 'X') { currentAppState = AppState::ADMIN_MENU; refreshMenu = true; resetInput(); break; }
      
      if (isInputReady) { 
        tempCustomer.customerBalance = atol(inputBuffer); 
        updateLcd(F("Set 4-Digit PIN:"), F(""), F(""), F("Press # to Submit")); 
        resetInput(); 
        currentAppState = AppState::ENROLL_WIZARD_PIN; 
      }
      break;

    case AppState::ENROLL_WIZARD_PIN:
      if (key == '#') { isInputReady = true; }
      else if (key == 'X') { currentAppState = AppState::ADMIN_MENU; refreshMenu = true; resetInput(); break; }

      if (isInputReady) {
        strncpy(tempCustomer.pin, inputBuffer, 4);
        sprintf(tempCustomer.customerName, "User %d", targetId);
        
        updateLcd(F("Syncing ID..."), F("Please wait"), F(""), F(""));
        Wire.beginTransmission(I2C_SLAVE_ADDR);
        Wire.write(CMD_SET_ID);
        Wire.write(targetId);
        Wire.endTransmission();
        delay(300);

        Wire.beginTransmission(I2C_SLAVE_ADDR);
        Wire.write(CMD_FP_ENROLL_START);
        Wire.endTransmission();

        updateLcd(F("Place your Finger"), F("On Sensor Now"), F(""), F(""));
        resetInput(); 
        resetSlavePoll();
        currentAppState = AppState::ENROLL_WAITING_SLAVE;
      }
      break;

    case AppState::MODIFY_BALANCE_ID:
      if (key == '#') {
        isInputReady = true;
      }
      else if (key == 'X') {
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        targetId = atoi(inputBuffer);
        if (targetId > 0 && targetId <= MAX_CUSTOMERS) {
          EEPROM.get((targetId - 1) * sizeof(CustomerData), tempCustomer);
          updateLcd(F("New Balance:"), F("Enter Amount"), F(""), F("U:Deposit D:Withdraw"));
          resetInput();
          currentAppState = AppState::MODIFY_BALANCE_VAL;
        } else {
          updateLcd(F("Invalid CustomerID"), F(""), F(""), F("")); delay(2000); currentAppState = AppState::ADMIN_MENU; refreshMenu = true;
        }
      }
      break;

    case AppState::MODIFY_BALANCE_VAL:
      if (key == 'X') {
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }
      
      if (key == 'U' || key == 'D') {
        long amount = atol(inputBuffer);
        char alertMsg[65];
        if (key == 'U') { // Logic for Deposit (Credit)
          tempCustomer.customerBalance += amount;
          sprintf(alertMsg, "CR: NGN %ld, New Bal: NGN %ld.", amount, tempCustomer.customerBalance);
          sendSms(tempCustomer.customerNumber, alertMsg);
        } else if (key == 'D') { // Logic for Withdrawal (Debit)
          tempCustomer.customerBalance -= amount;
          sprintf(alertMsg, "DR: NGN %ld, New Bal: NGN %ld.", amount, tempCustomer.customerBalance);
          sendSms(tempCustomer.customerNumber, alertMsg);
        }
        
        saveCustomer(targetId, tempCustomer);
        updateLcd(F("Success!"), F("Balance Updated"), F(""), F(""));
        delay(2000);
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
      }
      break;

    case AppState::ENROLL_WAITING_SLAVE: {
      byte s = pollSlave();
      if (s == STATUS_FP_REMOVE_FINGER) {
        updateLcd(F("Captured!"), F("Remove Finger"), F(""), F(""));
      } 
      else if (s == STATUS_FP_PLACE_AGAIN) {
        updateLcd(F("Place same finger"), F("Again now"), F(""), F(""));
      }
      else if (s == STATUS_FP_SUCCESS) {
        saveCustomer(targetId, tempCustomer);
        String msg = "Customer ID: " + String(targetId);
        updateLcd("Enroll Success!", msg.c_str(), "", "");
        delay(3000);
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
      } 
      else if (s == STATUS_FP_ENROLL_FAILED) {
        updateLcd(F("Enroll Failed"), F("Bad scan or timeout"), F("Try Again"), F(""));
        delay(3000);
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
      }
      break;
    }
    case AppState::VIEW_BALANCES: {
      if (refreshMenu) {
        CustomerData c; EEPROM.get(viewIndex * sizeof(CustomerData), c);
        if (c.isUsed == 1) { 
          String balStr = "Bal: " + String(c.customerBalance);
          updateLcd(c.customerName, balStr.c_str(), "<- Prev.   Next ->", "*:Exit"); 
        }
        else {
          String idStr = "Customer ID: " + String(viewIndex+1);
          updateLcd("Empty Slot", idStr.c_str(), "<- Prev.   Next ->", "*:Exit");
        }
        refreshMenu = false;
      }
      
      if (key == 'R' && viewIndex < MAX_CUSTOMERS-1) { viewIndex++; refreshMenu = true; }
      else if (key == 'L' && viewIndex > 0) { viewIndex--; refreshMenu = true; }
      else if (key == '*') { currentAppState = AppState::ADMIN_MENU; refreshMenu = true; }
      break;
    }

    case AppState::BANKING_FP_VERIFY: {
      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::MAIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      byte s = pollSlave();
      if (s == STATUS_FP_SUCCESS) {
        // Step 1: Notify Slave to set verification index fetch mode
        Wire.beginTransmission(I2C_SLAVE_ADDR); 
        Wire.write(CMD_GET_VERIFIED_ID); 
        Wire.endTransmission();
        delay(50); // Safety pause for Slave variable latching
        
        // Step 2: Request the matched customer ID register
        Wire.requestFrom((uint8_t)I2C_SLAVE_ADDR, (uint8_t)1); 
        currentCustomerId = Wire.read();
        
        // Safeguard to make sure read is safe before fetching
        if (currentCustomerId > 0 && currentCustomerId <= MAX_CUSTOMERS) {
          EEPROM.get((currentCustomerId - 1) * sizeof(CustomerData), tempCustomer);
          
          uint32_t currentUnixTime = rtc.now().unixtime();
          
          // Check if this specific user account is currently locked out
          if (tempCustomer.lockoutUntil > 0 && currentUnixTime < tempCustomer.lockoutUntil) {
            uint32_t remaining = tempCustomer.lockoutUntil - currentUnixTime;
            char timerLine[21];
            snprintf(timerLine, sizeof(timerLine), "Try in %02lu:%02lu mins", 
                    (unsigned long)(remaining / 60), (unsigned long)(remaining % 60));
            
            updateLcd("Account Locked!", timerLine, "Access Denied", "");
            delay(3000);
            currentAppState = AppState::MAIN_MENU;
            refreshMenu = true;
            break;
          }
          // If lockout time has expired, clear it in EEPROM
          if (tempCustomer.lockoutUntil > 0 && currentUnixTime >= tempCustomer.lockoutUntil) {
            tempCustomer.lockoutUntil = 0;
            saveCustomer(currentCustomerId, tempCustomer);
          }

          // Account is active - proceed to PIN entry
          char greeting[21];
          snprintf(greeting, sizeof(greeting), "Welcome %s", tempCustomer.customerName);
          updateLcd(greeting, "Enter your PIN:", "", "Press Ent to Submit"); 
          
          resetInput();
          fpRetryCount = 0;  // Reset fingerprint attempt counter on success
          pinRetryCount = 0; // Reset PIN attempt counter on success
          currentAppState = AppState::BANKING_PIN_ENTRY;
        } else {
          // Matched ID from sensor was out of range
          fpRetryCount++;
          if (fpRetryCount >= 3) {
            updateLcd(F("Invalid Match ID"), F("Max Retries Exceeded"), F("Access Denied"), F(""));
            delay(2000);
            fpRetryCount = 0;
            currentAppState = AppState::MAIN_MENU;
            refreshMenu = true;
          } else {
            char retryP[21];
            snprintf(retryP, sizeof(retryP), "Try Again (%d/3)", fpRetryCount);
            updateLcd("Invalid Match ID", retryP, "", "");
            delay(1500);
            
            // Re-trigger verification on Slave for the next scan attempt
            Wire.beginTransmission(I2C_SLAVE_ADDR); 
            Wire.write(CMD_FP_VERIFY_START); 
            Wire.endTransmission();
            resetSlavePoll();
            updateLcd(F("Place your Finger"), F("On Sensor..."), F(""), F(""));
          }
        }
      } else if (s == STATUS_FP_FAILED) { 
        fpRetryCount++;
        if (fpRetryCount >= 3) {
          updateLcd(F("Finger Not Found"), F("Max Retries Exceeded"), F("Access Denied"), F("")); 
          delay(2000); 
          fpRetryCount = 0;
          currentAppState = AppState::MAIN_MENU; 
          refreshMenu = true; 
        } else {
          char retryP[21];
          snprintf(retryP, sizeof(retryP), "Try Again (%d/3)", fpRetryCount);
          updateLcd("Finger Not Found", retryP, "", "");
          delay(1500);
          
          // Re-trigger verification on Slave for the next scan attempt
          Wire.beginTransmission(I2C_SLAVE_ADDR); 
          Wire.write(CMD_FP_VERIFY_START); 
          Wire.endTransmission();
          resetSlavePoll();
          updateLcd(F("Place Finger"), F("On Sensor..."), F(""), F(""));
        }
      }  
      break;
    }
    case AppState::BANKING_PIN_ENTRY: {
      DateTime now = rtc.now();

      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::MAIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      // Process Keypad Confirmation (E Enter/Confirm action)
      if (isInputReady) {
        if (strcmp(inputBuffer, tempCustomer.pin) == 0) {
          // Success: Reset retry counter
          pinRetryCount = 0; 
          
          long otp = random(100000, 999999); 
          sprintf(generatedOtp, "%06ld", otp);
          Serial.print(F("[DEBUG] Generated OTP: ")); Serial.println(generatedOtp);
          otpCreationUnixTime = rtc.now().unixtime(); 

          Wire.beginTransmission(I2C_SLAVE_ADDR);
          Wire.write(0xFD); // Custom command for OTP transmission to slave
          for(int i = 0; i < 6; i++) {
            Wire.write(generatedOtp[i]);
          }
          Wire.endTransmission();
          delay(20);

          char m[64]; 
          sprintf(m, "Your OTP is %s. This code expires in 5 minutes. DO NOT share this code with anyone.", generatedOtp);
          sendSms(tempCustomer.customerNumber, m);
          
          updateLcd(F("OTP Sent"), F("Enter 6 digits:"), F(""), F("Press Ent to Submit")); 
          resetInput(); 
          currentAppState = AppState::BANKING_OTP_ENTRY;
        } 
        else {
          // Failed PIN Attempt: Increment retry counter
          pinRetryCount++;
          
          if (pinRetryCount >= 3) {
            // Set 2-minute (120-second) lockout on user's EEPROM record
            tempCustomer.lockoutUntil = now.unixtime() + 120;
            saveCustomer(currentCustomerId, tempCustomer);
            
            updateLcd(F("3 Wrong PIN Entries!"), F("Acct Locked for 2min"), F("Access Denied"), F(""));
            delay(2500);
            
            pinRetryCount = 0; 
            currentAppState = AppState::MAIN_MENU;
            refreshMenu = true;
          } else {
            char retryMsg[21];
            snprintf(retryMsg, sizeof(retryMsg), "Try Again (%d/3)", pinRetryCount);
            
            updateLcd("Wrong PIN!", retryMsg, "", "");
            delay(1500);
            
            char greeting[21];
            snprintf(greeting, sizeof(greeting), "Welcome %s", tempCustomer.customerName);
            updateLcd(greeting, "Enter your PIN:", "", "Press Ent to Submit");
            resetInput();
          }
        }
      }
      break;
    }
    case AppState::BANKING_OTP_ENTRY: {
      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::MAIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        // 1. Check if the 5-minute (300 seconds) OTP window has expired
        if ((rtc.now().unixtime() - otpCreationUnixTime) > 60) {
          updateLcd(F("OTP Expired!"), F("Request New Code"), F("Try again later"), F(""));
          delay(2500);
          otpRetryCount = 0;
          currentAppState = AppState::MAIN_MENU;
          refreshMenu = true;
        } 
        // 2. Check if OTP matches
        else if (strcmp(inputBuffer, generatedOtp) == 0) {
          updateLcd(F("OTP Verified!"), F("Access Granted"), F(""), F(""));
          delay(1500);
          otpRetryCount = 0; // Reset counter on success
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        } 
        // 3. Invalid OTP Entered -> Track Retries
        else {
          otpRetryCount++;
          
          if (otpRetryCount >= 3) {
            updateLcd(F("3 Invalid OTPs!"), F("Session Terminated"), F("Access Denied"), F(""));
            delay(2500);
            otpRetryCount = 0; // Reset counter
            currentAppState = AppState::MAIN_MENU;
            refreshMenu = true;
          } else {
            char retryMsg[21];
            snprintf(retryMsg, sizeof(retryMsg), "Try Again (%d/3)", otpRetryCount);
            
            updateLcd("Invalid OTP!", retryMsg, "", "");
            delay(1500);
            
            // Re-prompt user
            updateLcd(F("OTP Sent"), F("Enter 6 digits:"), F(""), F("Press Ent to Submit"));
            resetInput();
          }
        }
      }
      break;
    }

    case AppState::BANKING_MENU:
      if (refreshMenu) {
        updateLcd(F("1.Balance Inquiry"), F("2.Withdraw"), F("3.Fund Transfer"), F("Esc: Exit"));
        refreshMenu = false;
      }
      if (key == '1') { 
        String balStr = "NGN " + String(tempCustomer.customerBalance);
        updateLcd("Balance:", balStr.c_str(), "Press Esc to go back", ""); 
        while(customKeypad.getKey() != 'X'); 
        refreshMenu = true; 
      }
      else if (key == '2') { currentAppState = AppState::WITHDRAWAL_TYPE; refreshMenu = true; }
      else if (key == '3') { 
        updateLcd(F("Fund Transfer"), F("Enter Account No:"), F(""), F("Press Ent to submit")); 
        resetInput(); 
        currentAppState = AppState::TRANSFER_PHONE; 
      }
      else if (key == 'X') { currentAppState = AppState::MAIN_MENU; refreshMenu = true; }
      break;

    case AppState::WITHDRAWAL_TYPE:
      if (refreshMenu) {
        updateLcd(F("1.Quick Cash(1000)"), F("2.2000     3.5000"), F("4.Other Amount"), F("X:Back"));
        refreshMenu = false;
      }
      
      if (key == 'X' || key == 'L') { 
        currentAppState = AppState::BANKING_MENU; 
        refreshMenu = true; 
      }
      else if (key == '1' || key == '2' || key == '3') { 
        long quickAmt = 0;
        if (key == '1') quickAmt = 1000;
        else if (key == '2') quickAmt = 2000;
        else if (key == '3') quickAmt = 5000;

        if (tempCustomer.customerBalance >= quickAmt) { 
          tempCustomer.customerBalance -= quickAmt;
          saveCustomer(currentCustomerId, tempCustomer);
          
          // Trigger hardware dispense via I2C slave
          Wire.beginTransmission(I2C_SLAVE_ADDR);
          Wire.write(CMD_DISPENSE);
          Wire.endTransmission();
          
          // Send SMS debit alert (matching Option 4 behavior)
          char alertMsg[64];
          sprintf(alertMsg, "Debit: NGN %ld, New Bal: NGN %ld.", quickAmt, tempCustomer.customerBalance);
          sendSms(tempCustomer.customerNumber, alertMsg);
          
          currentAppState = AppState::DISPENSING_CASH;
          stateTimer = millis();
        }
        else { 
          updateLcd(F("Insufficient funds"), F(""), F(""), F("")); 
          delay(2000); 
          updateLcd(F("1.Quick Cash(1000)"), F("2.2000     3.5000"), F("4.Other Amount"), F("<-:Back"));
        }
      }
      else if (key == '4') { 
        updateLcd(F("Enter Amount:"), F(""), F(""), F("Esc:Back  Ent:Submit")); 
        resetInput(); 
        currentAppState = AppState::WITHDRAWAL_AMOUNT; 
      }
      break;

    case AppState::WITHDRAWAL_AMOUNT:
      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::BANKING_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        long a = atol(inputBuffer);
        if (a <= 499) {
         updateLcd(F("Invalid Amount"), F("Please enter"), F("value 500 and above"), F(""));
          delay(2000);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true; 
        }
        else if ( a > tempCustomer.customerBalance){
          updateLcd(F("Insufficient Funds"), F("Available Bal:"), F(""), F(""));
          char balBuf[20];
          sprintf(balBuf, "NGN %ld", tempCustomer.customerBalance);
          lcd.setCursor(0, 2);
          lcd.print(balBuf);
          delay(3000);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        }
        else { 
          tempCustomer.customerBalance -= a; 
          saveCustomer(currentCustomerId, tempCustomer); 
          Wire.beginTransmission(I2C_SLAVE_ADDR); 
          Wire.write(CMD_DISPENSE);
          Wire.endTransmission(); 
          char alertMsg[64];
          sprintf(alertMsg, "Debit of: NGN %ld debited, New Bal: NGN %ld.", a, tempCustomer.customerBalance);
          sendSms(tempCustomer.customerNumber, alertMsg);
          currentAppState = AppState::DISPENSING_CASH; 
          stateTimer = millis(); 
        }
      }
      break;

    case AppState::TRANSFER_PHONE:
      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::BANKING_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        // Enforce exactly 10-digit input rule (e.g., 8012345678)
        if (strlen(inputBuffer) != 10) {
          updateLcd(F("Invalid Input!"), F("Enter 10 Digits"), F("(e.g. 0123456789)"), F(""));
          delay(2500);
          updateLcd(F("Fund Transfer"), F("Enter 10-Digit Acct:"), F(""), F("Press Ent to Submit"));
          resetInput();
          break;
        }

        // Automatically prepend '0' to match the 11-digit stored EEPROM format
        char searchNumber[16];
        snprintf(searchNumber, sizeof(searchNumber), "0%s", inputBuffer);

        bool found = false;
        bool isSelf = false;
        byte foundSlot = 0;

        for (int i = 0; i < MAX_CUSTOMERS; i++) {
          CustomerData checkUser;
          EEPROM.get(i * sizeof(CustomerData), checkUser);

          if (checkUser.isUsed == 1 && strcmp(checkUser.customerNumber, searchNumber) == 0) {
            foundSlot = i + 1;
            if (foundSlot == currentCustomerId) {
              isSelf = true;
            } else {
              recipientId = foundSlot;
              recipientCustomer = checkUser;
              found = true;
              break;
            }
          }
        }

        if (found) {
          updateLcd(F("Transfer Amount:"), F("Enter NGN:"), F(""), F("Esc:Back Submit:Ent"));
          resetInput();
          currentAppState = AppState::TRANSFER_AMOUNT;
        } 
        else if (isSelf) {
          updateLcd(F("Invalid Transfer"), F("Cannot transfer to"), F("your own account!"), F(""));
          delay(2500);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        } 
        else {
          updateLcd(F("Account Not Found"), F("Check and try again"), F(""), F(""));
          delay(2500);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        }
      }
      break;
      
    case AppState::TRANSFER_AMOUNT:
      if (key == 'E') {
        isInputReady = true;
      }
      else if (key == 'X') {
        updateLcd(F("Cancelled"), F("Returning..."), F(""), F(""));
        delay(1000);
        currentAppState = AppState::BANKING_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }

      if (isInputReady) {
        long transferAmt = atol(inputBuffer);
        if (transferAmt < 500) {
          updateLcd(F("Invalid Amount"), F("Minimum is NGN 500"), F(""), F(""));
          delay(2000);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        } else if (transferAmt > tempCustomer.customerBalance) {
          updateLcd(F("Insufficient Funds"), F("Check balance"), F(""), F(""));
          delay(2000);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        } else {
          // Perform balances update
          tempCustomer.customerBalance -= transferAmt;
          saveCustomer(currentCustomerId, tempCustomer);
          
          recipientCustomer.customerBalance += transferAmt;
          saveCustomer(recipientId, recipientCustomer);
          
          // Send Debit Alert to Sender
          char senderMsg[80];
          snprintf(senderMsg, sizeof(senderMsg), "Trf DR: NGN %ld to %s. New Bal: NGN %ld", 
                   transferAmt, recipientCustomer.customerNumber, tempCustomer.customerBalance);
          sendSms(tempCustomer.customerNumber, senderMsg);
          delay(100);
          
          // Send Credit Alert to Recipient
          char recipientMsg[80];
          snprintf(recipientMsg, sizeof(recipientMsg), "Trf CR: NGN %ld from %s. New Bal: NGN %ld", 
                   transferAmt, tempCustomer.customerNumber, recipientCustomer.customerBalance);
          sendSms(recipientCustomer.customerNumber, recipientMsg);
          
          updateLcd(F("Transfer Success!"), F("Thanks For Banking"), F("with Us"), F(""));
          delay(3000);
          currentAppState = AppState::BANKING_MENU;
          refreshMenu = true;
        }
      }
      break;
    
    case AppState::DISPENSING_CASH:
      if (refreshMenu) {
        updateLcd(F("Please Wait..."), F("Dispensing Cash..."), F(""), F(""));
        refreshMenu = false;
      }
      
      // Wait for the dispense cycle duration (4 seconds)
      if (millis() - stateTimer > 4000) {
        updateLcd(F("Take your cash"), F("Thank you!"), F(""), F(""));
        delay(2000);
        currentAppState = AppState::MAIN_MENU;
        refreshMenu = true;
      }
      break;

    case AppState::DELETE_USER:
      if (key == '#') {
        isInputReady = true;
      }
      else if (key == 'X') {
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
        resetInput();
        break;
      }
      
      if (isInputReady) {
        targetId = atoi(inputBuffer);
        if (targetId > 0 && targetId <= MAX_CUSTOMERS) {
          Wire.beginTransmission(I2C_SLAVE_ADDR); Wire.write(CMD_FP_DELETE); Wire.write(targetId); Wire.endTransmission();
          CustomerData blank; 
          memset(&blank, 0, sizeof(CustomerData)); 
          blank.isUsed = 0; 
          EEPROM.put((targetId-1)*sizeof(CustomerData), blank);
          
          updateLcd(F("Customer Data"), F("Deleted"), F(""), F("")); 
          delay(2000); 
        }
        currentAppState = AppState::ADMIN_MENU;
        refreshMenu = true;
      }
      break;

    case AppState::CONFIRM_WIPE:
      if (key == '#') {
        for (int i=0; i<(int)EEPROM.length(); i++) {
          EEPROM.write(i, 0);
        }
        Wire.beginTransmission(I2C_SLAVE_ADDR); 
        Wire.write(CMD_FP_RESET); 
        Wire.endTransmission();
        
        updateLcd(F("System Reset"), F("All data wiped"), F(""), F("")); 
        delay(3000); 
        currentAppState = AppState::MAIN_MENU; 
        refreshMenu = true;
      } else if (key == '*') {
        updateLcd(F("System Reset"), F("Aborted"), F(""), F(""));
        delay(1000);
        currentAppState = AppState::ADMIN_MENU; 
        refreshMenu = true; 
      }
      break;
  }
}