#pragma once

// DiaGnostix-ECG firmware configuration. Pin numbers refer to the
// ESP32-S3-DevKitC-1 N16R8 wired as in docs/wiring-diagram.svg; see
// docs/hardware.md.

#include <esp_arduino_version.h>
#include <sdkconfig.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "DiaGnostix-ECG needs the esp32 board package 3.x: Tools > Board > Boards Manager > esp32 by Espressif Systems"
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "DiaGnostix-ECG is written for the ESP32-S3: choose Tools > Board > esp32 > ESP32S3 Dev Module"
#endif

#define DIAGNOSTIX_FIRMWARE_VERSION "3.0.0"
#define DIAGNOSTIX_DEVICE_NAME "DiaGnostix-ECG"

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
// AD8232 ECG front end. The output must be on an ADC1 channel (GPIO1-10):
// ADC2 is not usable while Wi-Fi is active.
#define PIN_ECG_OUTPUT 4
#define PIN_ECG_LO_PLUS 5
#define PIN_ECG_LO_MINUS 6
#define PIN_ECG_SDN -1  // AD8232 shutdown pin; -1 if not connected (module default: enabled)

// 10 kOhm NTC thermistor divider (ADC1).
#define PIN_NTC 1

// Shared I2C bus: MAX30102 (0x57) and BME280 (0x76 or 0x77).
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define I2C_FREQUENCY_HZ 400000

// Alert LED (through a 220-330 Ohm resistor to GND) and optional buzzer.
#define PIN_ALERT_LED 15
#define PIN_BUZZER -1  // active buzzer; -1 to disable

// BOOT button: short press starts a measurement or acknowledges an alert.
#define PIN_BUTTON 0

// ---------------------------------------------------------------------------
// Pin safety check
// ---------------------------------------------------------------------------
// GPIO26-32 connect the SPI flash and PSRAM inside the ESP32-S3-WROOM-1 module,
// and GPIO33-37 are also taken by the octal PSRAM of the N16R8. They are not
// usable as general pins; these checks stop a mistyped pin from compiling.
#define DIAGNOSTIX_PIN_RESERVED(p) ((p) >= 26 && (p) <= 37)
#define DIAGNOSTIX_PIN_IS_ADC1(p) ((p) >= 1 && (p) <= 10)  // ADC2 is unusable while Wi-Fi is on

static_assert(!DIAGNOSTIX_PIN_RESERVED(PIN_ECG_OUTPUT) && !DIAGNOSTIX_PIN_RESERVED(PIN_ECG_LO_PLUS) &&
                  !DIAGNOSTIX_PIN_RESERVED(PIN_ECG_LO_MINUS) && !DIAGNOSTIX_PIN_RESERVED(PIN_ECG_SDN) &&
                  !DIAGNOSTIX_PIN_RESERVED(PIN_NTC) && !DIAGNOSTIX_PIN_RESERVED(PIN_I2C_SDA) &&
                  !DIAGNOSTIX_PIN_RESERVED(PIN_I2C_SCL) && !DIAGNOSTIX_PIN_RESERVED(PIN_ALERT_LED) &&
                  !DIAGNOSTIX_PIN_RESERVED(PIN_BUZZER) && !DIAGNOSTIX_PIN_RESERVED(PIN_BUTTON),
              "A pin in config.h is GPIO26-37, which the ESP32-S3 module uses for its flash and PSRAM");
static_assert(DIAGNOSTIX_PIN_IS_ADC1(PIN_ECG_OUTPUT) && DIAGNOSTIX_PIN_IS_ADC1(PIN_NTC),
              "PIN_ECG_OUTPUT and PIN_NTC must be ADC1 pins (GPIO1-10)");

// ---------------------------------------------------------------------------
// ECG acquisition (must match the training simulation, ml/dgx/config.py)
// ---------------------------------------------------------------------------
#define ECG_SAMPLE_RATE_HZ 500       // the models were trained on 500 Hz features
#define ECG_STREAM_DECIMATION 2      // 250 Hz to the dashboard
#define ECG_INVERT false             // true if the R wave is negative (RA and LA swapped)
#define ECG_DISPLAY_MAINS_HZ 60      // notch on the dashboard trace: 60 Hz (Taiwan, Americas), 50 Hz (Europe, most of Asia)
#define ECG_ADC_REF_MV 1650.0f       // AD8232 REF (mid-supply); removed again by the median
#define ECG_ADC_MAX_MV 3100          // readings at or above this are clipped (11 dB attenuation)
#define ECG_NOMINAL_GAIN 1100.0f     // AD8232 module: in-amp x100, op-amp x11 (calibrate per board)
#define ECG_GAIN_MIN 500.0f          // plausible range for a calibrated gain
#define ECG_GAIN_MAX 2000.0f

// Risk session: the ECG after the warm-up is cut into 10-s windows (the length
// used in training). Each window gives one model score; the risk is the median
// score of the windows that pass the signal-quality index (SQI).
#define ECG_WINDOW_S 10
#define ECG_MAX_WINDOWS 6            // up to 60 s of ECG per session (PSRAM buffer)
#define ECG_MIN_GOOD_WINDOWS 2       // fewer SQI-passing windows -> no result

// Gain calibration: a sine wave of known peak-to-peak amplitude (default 1 mV)
// between RA and LA, RL to the generator ground (docs/hardware.md).
#define GAIN_CAL_DURATION_S 6
#define GAIN_CAL_SETTLE_S 1          // ignore the AD8232 settling at the start
#define GAIN_CAL_DEFAULT_REF_MVPP 1.0f

// ---------------------------------------------------------------------------
// Other sensors
// ---------------------------------------------------------------------------
// MAX30102: 800 samples/s averaged by 4 = 200 Hz effective, 215 us pulses (17 bit).
#define PPG_SAMPLE_RATE_HZ 200
#define PPG_LED_CURRENT 0x24  // ~7.2 mA; raise for dark skin or thick fingers
#define PPG_FINGER_THRESHOLD 50000
#define PPG_STREAM_DECIMATION 4  // 50 Hz to the dashboard

// NTC divider: VCC(3.3 V) -- 10k fixed --+-- NTC -- GND, ADC at '+'.
#define NTC_SERIES_OHM 10000.0f
#define NTC_NOMINAL_OHM 10000.0f
#define NTC_BETA 3950.0f
#define NTC_ON_LOW_SIDE true

#define ENV_READ_INTERVAL_MS 2000

// ---------------------------------------------------------------------------
// Measurement session
// ---------------------------------------------------------------------------
#define SESSION_WARMUP_S 3           // AD8232 settling after the electrodes touch the skin
#define SESSION_DEFAULT_DURATION_S (SESSION_WARMUP_S + 3 * ECG_WINDOW_S)  // three 10-s windows
#define SESSION_MIN_DURATION_S (SESSION_WARMUP_S + ECG_MIN_GOOD_WINDOWS * ECG_WINDOW_S)
#define SESSION_MAX_DURATION_S (SESSION_WARMUP_S + ECG_MAX_WINDOWS * ECG_WINDOW_S)
#define SESSION_MIN_BEATS 15         // valid R-R intervals for the live heart-rate summary
#define SESSION_MIN_PATS 10          // valid pulse arrival times for blood pressure
#define SESSION_MIN_SIGNAL_FRACTION 0.8f  // leads on (and finger present when needed) >= 80 % of the time
#define CALIBRATION_DURATION_S 20

// Cuffless blood pressure (Poon & Zhang model). gamma: vessel elasticity.
#define BP_GAMMA_PER_MMHG 0.017f

// ---------------------------------------------------------------------------
// Local safety alerts (independent of the risk model)
// ---------------------------------------------------------------------------
#define SAFETY_SPO2_THRESHOLD 92.0f
#define SAFETY_FEVER_THRESHOLD_C 38.0f
#define SAFETY_HOLD_S 10

// ---------------------------------------------------------------------------
// Connectivity: access point with captive portal, user accounts
// ---------------------------------------------------------------------------
// The device always runs its own Wi-Fi network. Joining it opens the sign-in page automatically
// (captive portal); each user has an account with their own profile, calibration and results.
#define AP_SSID "DiaGnostix"
#define AP_PASSWORD "diagnostix"     // WPA2, at least 8 characters
#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 4
#define WIFI_HOSTNAME "diagnostix"   // http://diagnostix.local (also reachable at http://192.168.4.1)
#define WIFI_CONNECT_TIMEOUT_MS 15000  // optional second connection to a home network (secrets.h)

#define MAX_USERS 20                 // accounts stored on the FAT partition of the N16R8
#define MAX_LOGIN_SESSIONS 16        // signed-in browsers / phones
#define LOGIN_SESSION_TTL_S (12 * 3600)
#define PASSWORD_MIN_LENGTH 6
#define PBKDF2_ITERATIONS 10000      // PBKDF2-HMAC-SHA256 (hardware SHA on the ESP32-S3)
#define HISTORY_MAX_RESULTS 300      // per user, oldest results are dropped
#define HISTORY_SEND_RESULTS 60      // sent to the dashboard on request

// 0 = Wi-Fi and the web dashboard only (default).
// 1 = also advertise the Bluetooth LE service for the future mobile app; this
//     needs the NimBLE-Arduino library and uses extra flash and RAM.
#define ENABLE_BLE 0

// Optional: a home Wi-Fi network in secrets.h (copy secrets.example.h; not committed). The device
// then also joins that network (dashboard at its IP / diagnostix.local); its own access point and
// captive portal stay on either way.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif
