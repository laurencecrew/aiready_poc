// U8g2 display test for the FYSETC One+ 12864 (verification item J), round 2.
//
// Round 1 result: only ST7565_ZOLEN gave a usable image, but it needed MIRROR and
// still wrapped by ~5-10 px (column offset wrong). This round tunes the column
// offset (u8x8 x_offset) and the mirroring method.
//
// Screen: 1-px frame on the very edge, solid block TOP-LEFT, ruler ticks along the
// top every 8 px (long tick every 32 px), settings text.
// Correct = frame complete on all 4 sides, nothing wrapped from one side to the
// other, block top-left, text upright and reading left-to-right.
//
// Controls (knob or serial 115200):
//   turn knob / '+' '-'  column offset up / down (0..12)
//   press knob / 'm'     next mirror mode
//   'c' / 'C'            contrast down / up
//   'h'                  print settings
#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>
#include "one_plus_pins.h"

U8G2_ST7565_ZOLEN_128X64_1_4W_HW_SPI u8g2(U8G2_R0, LCD_CS_PIN, LCD_A0_PIN, LCD_RESET_PIN);

// Mirror modes:
//   0 SW-MIRROR : U8G2_MIRROR rotation (software), driver's own segment direction
//   1 HW A0     : R0, then send ADC-select 0xA0 (segment normal, as original Marlin)
//   2 HW A1     : R0, then send ADC-select 0xA1 (segment reversed)
static const char *MODE_NAMES[] = {"SW-MIRROR", "HW A0", "HW A1"};
static const uint8_t N_MODES = 3;

// Verified result: mode 1 (HW A0) + x_offset 0 is correct; this is the default.
// (Mode 0 + offset 4 also correct; mode 2 + offset 4 aligned but mirrored.)
static uint8_t mode = 1;
static uint8_t xoff = LCD_X_OFFSET;
static int contrast = LCD_CONTRAST;

static void safeState() {
  pinMode(HEATER_0_PIN, OUTPUT);   digitalWrite(HEATER_0_PIN, LOW);
  pinMode(HEATER_BED_PIN, OUTPUT); digitalWrite(HEATER_BED_PIN, LOW);
  pinMode(FAN_PIN, OUTPUT);        digitalWrite(FAN_PIN, LOW);
  const uint8_t en[] = {X_ENABLE_PIN, Y_ENABLE_PIN, Z_ENABLE_PIN, E0_ENABLE_PIN};
  for (uint8_t p : en) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  pinMode(SDSS, OUTPUT); digitalWrite(SDSS, HIGH);  // deselect SD; SPI master needs SS output
}

static void report() {
  Serial.print(F("mode ")); Serial.print(MODE_NAMES[mode]);
  Serial.print(F("  x_offset ")); Serial.print(xoff);
  Serial.print(F("  contrast ")); Serial.print(contrast);
  Serial.print(F("  (driver default x_offset was "));
  Serial.print(u8g2.getU8x8()->display_info->default_x_offset);
  Serial.println(')');
}

static void draw() {
  char line[24];
  u8g2.firstPage();
  do {
    u8g2.drawFrame(0, 0, 128, 64);            // must touch all 4 edges
    u8g2.drawBox(0, 0, 12, 12);               // TOP-LEFT marker
    for (uint8_t x = 16; x < 128; x += 8)     // ruler along top edge
      u8g2.drawVLine(x, 1, (x % 32 == 0) ? 6 : 3);
    u8g2.drawBox(120, 56, 8, 8);              // small BOTTOM-RIGHT marker
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(16, 22, "TOP-LEFT");
    snprintf(line, sizeof line, "%s off=%u", MODE_NAMES[mode], xoff);
    u8g2.drawStr(3, 36, line);
    snprintf(line, sizeof line, "contrast %d", contrast);
    u8g2.drawStr(3, 48, line);
    u8g2.drawStr(3, 60, "turn=off push=mode");
  } while (u8g2.nextPage());
}

static void apply() {
  u8g2.begin();                                     // full re-init (resets x_offset)
  u8g2.setDisplayRotation(mode == 0 ? U8G2_MIRROR : U8G2_R0);
  if (mode == 1) u8g2.sendF("c", LCD_ADC_NORMAL_CMD);
  if (mode == 2) u8g2.sendF("c", 0xA1);
  u8g2.getU8x8()->x_offset = xoff;
  u8g2.setContrast(contrast);
  report();
  draw();
}

// Encoder polling (port C has no pin-change interrupts).
static uint8_t encLast = 0;
static int8_t encAcc = 0;
static bool btnLast = true;
static uint32_t btnTime = 0;
static uint8_t encRead() { return (digitalRead(BTN_EN1) ? 2 : 0) | (digitalRead(BTN_EN2) ? 1 : 0); }

static void changeOffset(int d) { xoff = constrain((int)xoff + d, 0, 12); apply(); }

void setup() {
  safeState();
  pinMode(BTN_EN1, INPUT_PULLUP);
  pinMode(BTN_EN2, INPUT_PULLUP);
  pinMode(BTN_ENC, INPUT_PULLUP);
  encLast = encRead();
  Serial.begin(115200);
  Serial.println(F("\nOne+ U8g2 test round 2 (ST7565_ZOLEN). turn=offset push=mode c/C=contrast"));
  apply();
}

void loop() {
  static const int8_t TBL[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t e = encRead();
  if (e != encLast) {
    encAcc += TBL[(encLast << 2) | e];
    encLast = e;
    if (encAcc >= 4)  { encAcc = 0; changeOffset(+1); }
    if (encAcc <= -4) { encAcc = 0; changeOffset(-1); }
  }

  bool b = digitalRead(BTN_ENC);
  if (b != btnLast && millis() - btnTime > 30) {
    btnTime = millis();
    btnLast = b;
    if (!b) { mode = (mode + 1) % N_MODES; apply(); }
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case '+': changeOffset(+1); break;
      case '-': changeOffset(-1); break;
      case 'm': mode = (mode + 1) % N_MODES; apply(); break;
      case 'c': contrast = constrain(contrast - 4, 0, 255); u8g2.setContrast(contrast); report(); draw(); break;
      case 'C': contrast = constrain(contrast + 4, 0, 255); u8g2.setContrast(contrast); report(); draw(); break;
      case 'h': report(); break;
    }
  }
}
