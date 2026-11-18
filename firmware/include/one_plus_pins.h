/*
 * one_plus_pins.h — FYSETC One+ V2.2 (a.k.a. AIO / AIO_I, ATmega2560) pin map
 *
 * Arduino-Mega pin numbers (as used by digitalWrite() etc.).
 * Basis: RAMPS 1.4 pin map (Marlin pins_RAMPS.h), corrected with evidence from
 * the original Marlin 1.1.0-RC6 binary and on-board probe tests. The display is
 * NOT the RRD ST7920 wiring — see LCD section.
 *
 * Status tags (keep in sync with docs/dev/hardware.md):
 *   [V]  verified on this physical board
 *   [R]  RAMPS 1.4 standard, assumed, not yet verified on this board
 *   [?]  unknown / guess — verify before relying on it
 *
 * See docs/dev/hardware.md for port/timer/interrupt details and conflicts.
 */
#pragma once

// ---------------- Stepper drivers (4x Allegro A4988, marked "4988ET") [V] -------
// ENABLE is ACTIVE LOW (LOW = driver energised). STEP: rising edge, >=1 us high & low.
// Microstepping 1/16 as shipped [V]: 3200 steps/rev for a 1.8 deg motor.
#define STEPPER_MICROSTEPS 16
#define X_STEP_PIN      54  // A0  PF0 [V]
#define X_DIR_PIN       55  // A1  PF1 [V]
#define X_ENABLE_PIN    38  //     PD7 [V]

#define Y_STEP_PIN      60  // A6  PF6 [V]
#define Y_DIR_PIN       61  // A7  PF7 [V]
#define Y_ENABLE_PIN    56  // A2  PF2 [V]

#define Z_STEP_PIN      46  //     PL3 (OC5A) [V]
#define Z_DIR_PIN       48  //     PL1 [V]
#define Z_ENABLE_PIN    62  // A8  PK0 [V]

#define E0_STEP_PIN     26  //     PA4 [V]
#define E0_DIR_PIN      28  //     PA6 [V]
#define E0_ENABLE_PIN   24  //     PA2 [V]

// ---------------- Limit switch inputs (S/GND; use INPUT_PULLUP, LOW = closed) --
#define X_MIN_PIN        3  // PE5 INT5 OC3C [V]
#define X_MAX_PIN        2  // PE4 INT4 OC3B [V]
#define Y_MIN_PIN       40  // PG1 - NOT RAMPS D14; no interrupt, poll [V]
#define Y_MAX_PIN       41  // PG0 - NOT RAMPS D15; no interrupt, poll [V]
#define Z_MIN_PIN       18  // PD3 INT3 TXD1 [V]
#define Z_MAX_PIN       19  // PD2 INT2 RXD1 [V]

// ---------------- Power MOSFET outputs (switch load to GND, load V = Vin) -------
// Drive LOW at boot BEFORE anything else. Never leave as INPUT_PULLUP (gate turns on).
#define HEATER_0_PIN    10  // PB4 OC2A Timer2 — hot-end "HEATER" output [V]
#define HEATER_BED_PIN   8  // PH5 OC4C Timer4 — "BED" output (<=10 A) [V]
#define FAN_PIN          9  // PH6 OC2B Timer2 — PWM fan output [V]
// These three are the only load outputs on the board (no extra FAN/LED outputs).

// ---------------- Thermistor inputs (analog channel numbers for analogRead) -----
// RAMPS circuit: 4.7k pull-up to 5V, 10uF to GND (verify). 100k NTC typical.
#define TEMP_0_ADC      13  // A13 = D67 PK5 — "TEMP0" hot end [V] (reads room temp w/ thermistor)
#define TEMP_BED_ADC    14  // A14 = D68 PK6 — "TEMP1"/bed [V]
#define TEMP_1_ADC      15  // A15 = D69 PK7 — probably NOT populated [?]

// ---------------- 12864 display: ST7565/UC1701-family on HARDWARE SPI [V] --------
// Recovered from original Marlin + confirmed with probe test pattern.
// Shares SPI (MOSI 51, SCK 52) with the SD card. SPI mode 0, <= 4 MHz.
// U8g2 [V]: U8G2_ST7565_ZOLEN_128X64_F_4W_HW_SPI u8g2(U8G2_R0, LCD_CS_PIN, LCD_A0_PIN, LCD_RESET_PIN);
//   after EVERY u8g2.begin():  u8g2.sendF("c", LCD_ADC_NORMAL_CMD);
//                              u8g2.getU8x8()->x_offset = LCD_X_OFFSET;
//                              u8g2.setContrast(LCD_CONTRAST);
//   (without 0xA0 the ZOLEN init leaves the image mirrored and 4 columns off)
#define LCD_ADC_NORMAL_CMD 0xA0  // ST7565 segment direction normal [V]
#define LCD_X_OFFSET    0   // column offset with 0xA0 [V]
#define LCD_CONTRAST    56  // U8g2 scale; 40..70 looks good [V]
#define LCD_CS_PIN      29  // PA7, active LOW [V]
#define LCD_A0_PIN      27  // PA5, RS/DC: LOW=command HIGH=data [V]
#define LCD_RESET_PIN   25  // PA3, active LOW, hold HIGH [V]
#define LCD_AUX65_PIN   65  // A11 PK3 — Marlin set HIGH as "backlight"; backlight is on anyway [?]
// NOTE: D16/D17/D23 (RAMPS ST7920 nets) are NOT used by the display.
// Legacy names kept only so the probe's ST7920 negative test still builds:
#define LCD_ST7920_CS   16
#define LCD_ST7920_SID  17
#define LCD_ST7920_SCK  23

// ---------------- Rotary encoder / buttons / beeper ----------------------------
// Port C has NO pin-change interrupts -> encoder must be polled (>=1 kHz).
#define BTN_EN1         31  // PC6 encoder A [V]
#define BTN_EN2         33  // PC4 encoder B [V]
#define BTN_ENC         35  // PC2 encoder push, LOW = pressed [V]
#define BEEPER_PIN      37  // PC0 — HIGH = beep; not a PWM pin; tone() would steal Timer2 [V]
// No KILL button on this board (the button is hardware RESET). D41 is the Y-MAX endstop.

// ---------------- microSD (TF) on hardware SPI ---------------------------------
#define SD_MISO_PIN     50  // PB3 [R]
#define SD_MOSI_PIN     51  // PB2 [R]
#define SD_SCK_PIN      52  // PB1 [R]
#define SDSS            53  // PB0 — must stay OUTPUT for SPI master mode [R]
#define SD_DETECT_PIN   49  // PL0, LOW = card present [R]

// ---------------- I2C expansion header (order on board: SCL, SDA, GND, 5V) -----
#define I2C_SDA_PIN     20  // PD1 INT1 [R]
#define I2C_SCL_PIN     21  // PD0 INT0 [R]

// ---------------- USB serial (CH340G) ------------------------------------------
// Serial (UART0) D0 RX / D1 TX. Opening the port resets the MCU (DTR).

// ---------------- ESP-01 header --------------------------------------------------
// UART NOT YET IDENTIFIED [?]. Candidates: Serial2 = D16 TX / D17 RX (unused by
// display) or Serial3 = D14 TX / D15 RX (Y endstops are on D40/41, not here).
// UART1 = D18/D19 is taken by the Z endstops; UART0 = USB.
// Use probe command 'esp' to find it.
// #define ESP_SERIAL Serial?

// ---------------- Misc RAMPS pins (probably not broken out) ---------------------
#define LED_PIN         13  // PB7 — on-board LED? [?]
#define PS_ON_PIN       12  // PB6 [?]
