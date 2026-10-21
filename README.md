# Dual-Controller Biometric ATM System with GSM & RTC

A robust, standalone, microcontroller-based ATM banking terminal powered by a Master-Slave dual Arduino architecture. It features secure biometric fingerprint verification, 4-digit PIN authentication, 2-factor OTP transaction verification via GSM, real-time clock (RTC) security lockouts, and persistent EEPROM account storage.

---

## Key Features

* **Dual-Controller Architecture:** 
  * **Master Controller:** Manages the 20x4 LCD, 20-button custom keypad, RTC DS3231/DS1307 time tracking, EEPROM local database, and state machine logic.
  * **Slave Controller:** Interfaces with the Adafruit optical fingerprint sensor, SIM800 GSM/SMS module, buzzer alerts, and cash dispenser relay motor.
* **Biometric Security:** Fast-search optical fingerprint matching with automated 3-attempt failure counters and permanent per-user EEPROM penalty lockouts.
* **2-Factor Authentication (2FA):** Dynamic 6-digit OTP generation sent via SMS for secure banking session authorization.
* **Banking Operations:** 
  * Cash withdrawals (Quick Cash presets & custom amounts with $>= 500$ NGN validation).
  * Account balance inquiries and live deposit updates.
  * Account-to-account fund transfers via 10-digit phone numbers with automatic zero-padding.
* **Administrative Controls:** Admin login portal (`1234`), user account creation wizard, balance management, account deletion, and full system factory resets.

---

## Hardware Components

* **Microcontrollers:** 2x Arduino (Uno/Nano/Mega) configured for Master and Slave roles.
* **Display:** 20x4 Character LCD (HD44780 controller).
* **Input:** Custom 5x4 Matrix Keypad.
* **Biometric Sensor:** Adafruit Optical Fingerprint Sensor.
* **Clock Module:** RTC DS3231 / DS1307 Real-Time Clock.
* **Communication:** I2C Protocol (Master-to-Slave command and data packets).
* **Connectivity:** SIM800 / SIM900 GSM Module.
* **Actuator:** Relay module connected to a cash dispenser motor.

---

## Repository Structure

* `master_controller/`: Contains the main state machine, UI handling, I2C transmission routines, and financial transaction logic.
* `slave_controller/`: Contains peripheral control handlers for sensor verification, SMS dispatching, and hardware dispensing.

---

## Required Arduino Libraries

Make sure you have the following libraries installed in your Arduino IDE before compiling:
* [`Wire.h`](https://www.arduino.cc/en/reference/wire) (Built-in)
* [`EEPROM.h`](https://www.arduino.cc/en/reference/eeprom) (Built-in)
* [`LiquidCrystal.h`](https://www.arduino.cc/en/library/liquidcrystal) (Built-in)
* [`Keypad.h`](https://www.arduinolibraries.info/libraries/keypad)
* [`RTClib.h`](https://www.arduinolibraries.info/libraries/rtclib)
* [`Adafruit_Fingerprint_Sensor_Library`](https://www.arduinolibraries.info/libraries/adafruit-fingerprint-sensor-library)
* [`SoftwareSerial.h`](https://www.arduino.cc/en/reference/software-serial) (Built-in)

---

## Installation & Setup

1. Clone or download this repository:
   ```bash
   git clone [https://github.com/imubaraak/micro-controller_based_atm_prototype.git](https://github.com/imubaraak/micro-controller_based_atm_prototype.git)