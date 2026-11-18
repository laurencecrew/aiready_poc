// FYSETC One+ (AIO v1, ATmega2560) — hardware probe / characterisation console
// Serial 115200, line-based commands. Type 'help'.
//
// Safety: heaters/fan forced LOW at boot, steppers disabled. Any heater/fan
// output turned on by a command is forced off again after at most HEAT_MAX_MS.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <avr/wdt.h>
#include <avr/boot.h>
#include "one_plus_pins.h"

static const uint32_t HEAT_MAX_MS = 5000;
static uint32_t heatOffAt = 0;

struct NamedPin { uint8_t pin; const char *name; };

static const NamedPin INPUTS[] = {
  {X_MIN_PIN, "X_MIN"}, {X_MAX_PIN, "X_MAX"}, {Y_MIN_PIN, "Y_MIN"}, {Y_MAX_PIN, "Y_MAX"},
  {Z_MIN_PIN, "Z_MIN"}, {Z_MAX_PIN, "Z_MAX"}, {BTN_EN1, "ENC_A"}, {BTN_EN2, "ENC_B"},
  {BTN_ENC, "ENC_BTN"}, {SD_DETECT_PIN, "SD_DET"},
};
static const uint8_t N_INPUTS = sizeof(INPUTS) / sizeof(INPUTS[0]);

static bool isPowerPin(uint8_t p) { return p == HEATER_0_PIN || p == HEATER_BED_PIN || p == FAN_PIN; }
// Pins never touched by 'pins'/'watchall': serial, MOSFET gates, PS_ON.
static bool isProtected(uint8_t p) { return p <= 1 || isPowerPin(p) || p == PS_ON_PIN; }

// ------------------------------------------------------------------ safe state
static void safeState() {
  const uint8_t pw[] = {HEATER_0_PIN, HEATER_BED_PIN, FAN_PIN};
  for (uint8_t p : pw) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  const uint8_t en[] = {X_ENABLE_PIN, Y_ENABLE_PIN, Z_ENABLE_PIN, E0_ENABLE_PIN};
  for (uint8_t p : en) { digitalWrite(p, HIGH); pinMode(p, OUTPUT); }   // disabled
  const uint8_t sd[] = {X_STEP_PIN, X_DIR_PIN, Y_STEP_PIN, Y_DIR_PIN, Z_STEP_PIN, Z_DIR_PIN,
                        E0_STEP_PIN, E0_DIR_PIN, BEEPER_PIN};
  for (uint8_t p : sd) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  for (uint8_t i = 0; i < N_INPUTS; i++) pinMode(INPUTS[i].pin, INPUT_PULLUP);
  digitalWrite(SDSS, HIGH); pinMode(SDSS, OUTPUT);
  heatOffAt = 0;
}

// ------------------------------------------------------------------ helpers
static float thermistorC(int adc) {          // 100k NTC, B=3950, 4.7k pull-up
  if (adc >= 1015) return -999;              // open
  if (adc <= 5) return 999;                  // short
  float r = 4700.0f * adc / (1023.0f - adc);
  return 1.0f / (1.0f / 298.15f + log(r / 100000.0f) / 3950.0f) - 273.15f;
}

static void printTemps() {
  const uint8_t ch[] = {TEMP_0_ADC, TEMP_BED_ADC, TEMP_1_ADC};
  for (uint8_t c : ch) {
    int a = analogRead(c); float t = thermistorC(a);
    Serial.print(F("A")); Serial.print(c); Serial.print(F("=")); Serial.print(a);
    Serial.print(F(" (")); if (t < -900) Serial.print(F("open")); else if (t > 900) Serial.print(F("short")); else Serial.print(t, 1);
    Serial.print(F(")  "));
  }
  Serial.println();
}

static bool stopRequested() { if (Serial.available()) { while (Serial.available()) Serial.read(); return true; } return false; }

static char *nextTok(char *&s) {
  while (*s == ' ') s++;
  if (!*s) return nullptr;
  char *t = s; while (*s && *s != ' ') s++;
  if (*s) *s++ = 0;
  return t;
}
static long argOr(char *&s, long def) { char *t = nextTok(s); return t ? atol(t) : def; }

// ------------------------------------------------------------------ commands
static void cmdHelp() {
  Serial.println(F(
    "Commands (any key stops a running watch):\n"
    "  info                 MCU/fuse/reset info\n"
    "  in                   watch named inputs (endstops, encoder, button, SD detect) + temps\n"
    "  pins                 table: each pin read floating vs pulled-up (finds external pulls)\n"
    "  watchall             report ANY pin change (pull-ups on) - press/turn things to map them\n"
    "  adc                  raw analogRead of A0..A15\n"
    "  temp                 thermistor channels A13/A14/A15\n"
    "  out <pin> <0|1>      digital output (power pins auto-off after 5 s)\n"
    "  pulse <pin> <ms>     drive pin HIGH for ms (max 5000)\n"
    "  pwm <pin> <0-255>    analogWrite (power pins auto-off after 5 s)\n"
    "  step <x|y|z|e> <steps> [us_half_period=500] [dir=1]   needs motor power\n"
    "  beep [ms]            pulse beeper pin\n"
    "  i2c                  scan I2C bus (header SCL/SDA)\n"
    "  sd                   init microSD on SS=53, list root\n"
    "  lcd7920 [cs sid sck] ST7920 text test (D25/D65 driven HIGH first); no args = try candidates\n"
    "  lcd7567 [cs a0]      ST7565-family test on HW SPI, default CS29 A0 27 RST25 (from Marlin)\n"
    "  esp <1|2|3> [baud]   send AT to ESP-01 on SerialN (unplug endstops first!)\n"
    "  wdt                  trigger watchdog reset (tests old-bootloader WDT bootloop bug)\n"
    "  safe                 force all outputs to safe state"));
}

static void cmdInfo() {
  Serial.print(F("MCU signature: "));
  Serial.print(boot_signature_byte_get(0), HEX); Serial.print(' ');
  Serial.print(boot_signature_byte_get(2), HEX); Serial.print(' ');
  Serial.println(boot_signature_byte_get(4), HEX);
  Serial.print(F("Fuses L/H/E/lock: "));
  Serial.print(boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS), HEX); Serial.print(' ');
  Serial.print(boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS), HEX); Serial.print(' ');
  Serial.print(boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS), HEX); Serial.print(' ');
  Serial.println(boot_lock_fuse_bits_get(GET_LOCK_BITS), HEX);
  Serial.print(F("F_CPU: ")); Serial.println(F_CPU);
  extern int __heap_start, *__brkval; int v;
  Serial.print(F("Free RAM: ")); Serial.println((int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval));
}

static void cmdIn() {
  safeState();
  Serial.println(F("Watching inputs (pull-ups ON, 0 = LOW/closed). Any key to stop."));
  uint8_t last[N_INPUTS];
  for (uint8_t i = 0; i < N_INPUTS; i++) {
    last[i] = digitalRead(INPUTS[i].pin);
    Serial.print(INPUTS[i].name); Serial.print('='); Serial.print(last[i]); Serial.print(' ');
  }
  Serial.println();
  long enc = 0; uint8_t encPrev = (digitalRead(BTN_EN1) << 1) | digitalRead(BTN_EN2);
  static const int8_t QDEC[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  long encShown = 0; uint32_t tTemp = 0;
  while (!stopRequested()) {
    uint8_t e = (digitalRead(BTN_EN1) << 1) | digitalRead(BTN_EN2);
    if (e != encPrev) { enc += QDEC[(encPrev << 2) | e]; encPrev = e; }
    if (enc != encShown) { Serial.print(F("encoder count = ")); Serial.println(enc); encShown = enc; }
    for (uint8_t i = 0; i < N_INPUTS; i++) {
      uint8_t v = digitalRead(INPUTS[i].pin);
      if (v != last[i] && INPUTS[i].pin != BTN_EN1 && INPUTS[i].pin != BTN_EN2) {
        Serial.print(INPUTS[i].name); Serial.print(F(" (D")); Serial.print(INPUTS[i].pin);
        Serial.print(F(") -> ")); Serial.println(v);
      }
      last[i] = v;
    }
    if (millis() - tTemp > 2000) { tTemp = millis(); printTemps(); }
  }
}

static void cmdPins() {
  Serial.println(F("pin: float pullup   (float=INPUT, no pull; 0 with pullup => external pull-down/load)"));
  for (uint8_t p = 0; p < NUM_DIGITAL_PINS; p++) {
    if (isProtected(p)) continue;
    pinMode(p, INPUT); delay(2); uint8_t f = digitalRead(p);
    pinMode(p, INPUT_PULLUP); delay(2); uint8_t u = digitalRead(p);
    Serial.print('D'); Serial.print(p); Serial.print(F(": ")); Serial.print(f); Serial.print(F("     ")); Serial.print(u);
    if (!u) Serial.print(F("   <-- held LOW"));
    Serial.println();
  }
  safeState();
  Serial.println(F("(outputs restored to safe state)"));
}

static void cmdWatchAll() {
  Serial.println(F("All non-protected pins set INPUT_PULLUP (steppers disabled). Any key to stop."));
  static uint8_t last[NUM_DIGITAL_PINS];
  for (uint8_t p = 0; p < NUM_DIGITAL_PINS; p++) if (!isProtected(p) && p != X_ENABLE_PIN && p != Y_ENABLE_PIN && p != Z_ENABLE_PIN && p != E0_ENABLE_PIN) pinMode(p, INPUT_PULLUP);
  delay(5);
  for (uint8_t p = 0; p < NUM_DIGITAL_PINS; p++) last[p] = digitalRead(p);
  while (!stopRequested()) {
    for (uint8_t p = 0; p < NUM_DIGITAL_PINS; p++) {
      if (isProtected(p)) continue;
      uint8_t v = digitalRead(p);
      if (v != last[p]) { Serial.print('D'); Serial.print(p); Serial.print(F(" -> ")); Serial.println(v); last[p] = v; }
    }
  }
  safeState();
}

static void cmdAdc() {
  for (uint8_t c = 0; c < 16; c++) {
    Serial.print('A'); Serial.print(c); Serial.print('='); Serial.print(analogRead(c));
    Serial.print(c % 8 == 7 ? '\n' : ' ');
  }
}

static void armHeatTimeout(uint8_t pin, bool on) { if (isPowerPin(pin) && on) heatOffAt = millis() + HEAT_MAX_MS; }

static void cmdOut(char *a) {
  long p = argOr(a, -1), v = argOr(a, 0);
  if (p < 2 || p >= NUM_DIGITAL_PINS || p == PS_ON_PIN) { Serial.println(F("bad pin")); return; }
  pinMode(p, OUTPUT); digitalWrite(p, v ? HIGH : LOW); armHeatTimeout(p, v);
  Serial.print(F("D")); Serial.print(p); Serial.print(F(" = ")); Serial.println(v ? 1 : 0);
}

static void cmdPulse(char *a) {
  // NB: constrain() is a macro that evaluates its argument repeatedly — never pass argOr() into it.
  long p = argOr(a, -1), ms = argOr(a, 500); ms = constrain(ms, 1, 5000);
  if (p < 2 || p >= NUM_DIGITAL_PINS || p == PS_ON_PIN) { Serial.println(F("bad pin")); return; }
  pinMode(p, OUTPUT); digitalWrite(p, HIGH); delay(ms); digitalWrite(p, LOW);
  Serial.println(F("done"));
}

static void cmdPwm(char *a) {
  long p = argOr(a, -1), v = argOr(a, 0); v = constrain(v, 0, 255);
  if (p < 2 || p >= NUM_DIGITAL_PINS || digitalPinToTimer(p) == NOT_ON_TIMER) { Serial.println(F("not a PWM pin")); return; }
  pinMode(p, OUTPUT); analogWrite(p, v); armHeatTimeout(p, v);
  Serial.print(F("PWM D")); Serial.print(p); Serial.print(F(" = ")); Serial.println(v);
}

static void cmdStep(char *a) {
  char *ax = nextTok(a); long n = argOr(a, 200), us = argOr(a, 500), dir = argOr(a, 1);
  us = constrain(us, 2, 100000);
  uint8_t s, d, e;
  if (!ax) { Serial.println(F("axis?")); return; }
  switch (ax[0]) {
    case 'x': s = X_STEP_PIN; d = X_DIR_PIN; e = X_ENABLE_PIN; break;
    case 'y': s = Y_STEP_PIN; d = Y_DIR_PIN; e = Y_ENABLE_PIN; break;
    case 'z': s = Z_STEP_PIN; d = Z_DIR_PIN; e = Z_ENABLE_PIN; break;
    case 'e': s = E0_STEP_PIN; d = E0_DIR_PIN; e = E0_ENABLE_PIN; break;
    default: Serial.println(F("axis must be x/y/z/e")); return;
  }
  digitalWrite(d, dir ? HIGH : LOW); digitalWrite(e, LOW); delay(5);
  for (long i = 0; i < n; i++) {
    digitalWrite(s, HIGH); delayMicroseconds(us); digitalWrite(s, LOW); delayMicroseconds(us);
    if ((i & 63) == 0 && stopRequested()) break;
  }
  delay(100); digitalWrite(e, HIGH);
  Serial.println(F("done (driver disabled again)"));
}

static void cmdBeep(char *a) {
  long ms = argOr(a, 200); ms = constrain(ms, 1, 3000);
  uint32_t t = millis();
  while (millis() - t < (uint32_t)ms) { digitalWrite(BEEPER_PIN, HIGH); delayMicroseconds(250); digitalWrite(BEEPER_PIN, LOW); delayMicroseconds(250); }
  Serial.println(F("beeped (2 kHz square wave; a self-oscillating buzzer also sounds on DC)"));
}

static void cmdI2c() {
  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000, true);
#endif
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) { Serial.print(F("found 0x")); Serial.println(addr, HEX); found++; }
  }
  Serial.print(found); Serial.println(F(" device(s)"));
  Wire.end();
}

static void cmdSd() {
  Serial.print(F("SD_DETECT (D49) = ")); Serial.print(digitalRead(SD_DETECT_PIN)); Serial.println(F("  (0 = card present, if wired)"));
  if (!SD.begin(SDSS)) { Serial.println(F("SD.begin failed")); return; }
  Serial.println(F("SD OK. Root:"));
  File root = SD.open("/"); uint8_t n = 0;
  for (File f = root.openNextFile(); f && n < 20; f = root.openNextFile(), n++) {
    Serial.print(F("  ")); Serial.print(f.name()); Serial.print(f.isDirectory() ? F("/") : F("  ")); Serial.println(f.size());
    f.close();
  }
  root.close(); SD.end();
}

// ---- ST7920 serial-mode bit-bang (pins selectable so candidate wirings can be tried)
static uint8_t stCs = LCD_ST7920_CS, stSid = LCD_ST7920_SID, stSck = LCD_ST7920_SCK;
static void stShift(uint8_t b) {
  for (int8_t i = 7; i >= 0; i--) { digitalWrite(stSid, (b >> i) & 1); digitalWrite(stSck, HIGH); digitalWrite(stSck, LOW); }
}
static void stSend(bool data, uint8_t v) {
  digitalWrite(stCs, HIGH);
  stShift(data ? 0xFA : 0xF8); stShift(v & 0xF0); stShift((v << 4) & 0xF0);
  digitalWrite(stCs, LOW); delayMicroseconds(100);
}
static void stText(uint8_t addr, const char *s) { stSend(false, addr); while (*s) stSend(true, *s++); }

// Original Marlin drove D25 ("LCD_RESET") and D65 HIGH before LCD init.
static void lcdAux(bool resetPulse) {
  pinMode(65, OUTPUT); digitalWrite(65, HIGH);
  pinMode(25, OUTPUT);
  if (resetPulse) { digitalWrite(25, LOW); delay(20); }
  digitalWrite(25, HIGH); delay(60);
}

static void st7920Try(uint8_t cs, uint8_t sid, uint8_t sck) {
  stCs = cs; stSid = sid; stSck = sck;
  pinMode(cs, OUTPUT); pinMode(sid, OUTPUT); pinMode(sck, OUTPUT);
  digitalWrite(cs, LOW); digitalWrite(sck, LOW); delay(5);
  stSend(false, 0x30); stSend(false, 0x30); stSend(false, 0x0C); stSend(false, 0x01); delay(3); stSend(false, 0x06);
  char buf[17];
  snprintf(buf, sizeof buf, "CS%u SID%u SCK%u", cs, sid, sck);
  stText(0x80, "ONE+ probe");
  stText(0x90, "ST7920 OK");
  stText(0x88, buf);
  Serial.print(F("  tried ")); Serial.println(buf);
  pinMode(cs, INPUT); pinMode(sid, INPUT); pinMode(sck, INPUT);   // release for next candidate
}

// lcd7920            -> try standard RRD wiring and a few alternatives
// lcd7920 cs sid sck -> try one specific wiring
static void cmdLcd7920(char *a) {
  long cs = argOr(a, -1), sid = argOr(a, -1), sck = argOr(a, -1);
  lcdAux(true);
  if (cs >= 0 && sid >= 0 && sck >= 0) { st7920Try(cs, sid, sck); }
  else {
    const uint8_t cand[][3] = { {16, 17, 23}, {53, 51, 52}, {16, 51, 52}, {27, 51, 52}, {23, 17, 16} };
    for (auto &c : cand) st7920Try(c[0], c[1], c[2]);
    st7920Try(16, 17, 23);   // finish on the standard one
  }
  digitalWrite(SDSS, HIGH); pinMode(SDSS, OUTPUT);
  Serial.println(F("Done. Line 3 on the display names the wiring that worked (if any)."));
}

// ---- ST7565/ST7567/UC1701-family panel on hardware SPI (wiring recovered from original Marlin:
//      u8g_InitHWSPI(cs=29, a0=27), LCD_RESET=25, D65 HIGH). Uses the same init sequence.
static const uint8_t GLCD_CS = 29, GLCD_A0 = 27, GLCD_RST = 25;
static void glcdByte(uint8_t b) { SPDR = b; while (!(SPSR & _BV(SPIF))) {} }
static void glcdCmd(uint8_t c) { digitalWrite(GLCD_A0, LOW); glcdByte(c); }

static void cmdLcd7567(char *a) {
  uint8_t cs = argOr(a, GLCD_CS), a0 = argOr(a, GLCD_A0);
  pinMode(65, OUTPUT); digitalWrite(65, HIGH);
  pinMode(GLCD_RST, OUTPUT); digitalWrite(GLCD_RST, LOW); delay(10); digitalWrite(GLCD_RST, HIGH); delay(10);
  pinMode(cs, OUTPUT); pinMode(a0, OUTPUT); digitalWrite(cs, HIGH);
  digitalWrite(SDSS, HIGH); pinMode(SDSS, OUTPUT);           // keep SD deselected
  SPI.begin(); SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  const uint8_t init[] = {0xE2, 0x40, 0xA0, 0xC8, 0xA6, 0xA3, 0x2F, 0xF8, 0x00, 0x23, 0x81, 0x27, 0xAC, 0x00, 0xAF};
  for (uint8_t c : init) { glcdCmd(c); if (c == 0xE2) delay(10); }
  glcdCmd(0xA5); Serial.println(F("All pixels ON for 2 s..."));
  delay(2000);
  glcdCmd(0xA4);
  for (uint8_t page = 0; page < 8; page++) {                 // stripes + checkerboard pattern
    glcdCmd(0xB0 | page); glcdCmd(0x10); glcdCmd(0x00);
    digitalWrite(a0, HIGH);
    for (uint8_t col = 0; col < 132; col++) glcdByte(page < 4 ? ((col & 8) ? 0xFF : 0x00) : ((col & 1) ? 0xAA : 0x55));
  }
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
  Serial.print(F("Pattern sent (cs=")); Serial.print(cs); Serial.print(F(" a0=")); Serial.print(a0);
  Serial.println(F("): top half vertical bars, bottom half checkerboard."));
}

static void cmdEsp(char *a) {
  long n = argOr(a, 0), baud = argOr(a, 115200);
  HardwareSerial *port = n == 1 ? &Serial1 : n == 2 ? &Serial2 : n == 3 ? &Serial3 : nullptr;
  if (!port) { Serial.println(F("usage: esp <1|2|3> [baud]")); return; }
  port->begin(baud); delay(50);
  while (port->available()) port->read();
  const char *cmds[] = {"AT\r\n", "AT+GMR\r\n"};
  for (const char *c : cmds) {
    port->print(c); uint32_t t = millis(); Serial.print(F(">> ")); Serial.print(c);
    while (millis() - t < 1500) if (port->available()) Serial.write(port->read());
    Serial.println();
  }
  port->end(); safeState();
}

static void cmdWdt() {
  Serial.println(F("Enabling 1 s watchdog and hanging. If the banner reappears, WDT reset is OK."));
  Serial.println(F("If the board hangs (no banner), the bootloader has the WDT bug: power-cycle."));
  Serial.flush();
  wdt_enable(WDTO_1S);
  for (;;) {}
}

// ------------------------------------------------------------------ main
static char line[64];
static uint8_t lineLen = 0;

static void dispatch(char *s) {
  char *cmd = nextTok(s);
  if (!cmd) return;
  if      (!strcmp(cmd, "help"))     cmdHelp();
  else if (!strcmp(cmd, "info"))     cmdInfo();
  else if (!strcmp(cmd, "in"))       cmdIn();
  else if (!strcmp(cmd, "pins"))     cmdPins();
  else if (!strcmp(cmd, "watchall")) cmdWatchAll();
  else if (!strcmp(cmd, "adc"))      cmdAdc();
  else if (!strcmp(cmd, "temp"))     printTemps();
  else if (!strcmp(cmd, "out"))      cmdOut(s);
  else if (!strcmp(cmd, "pulse"))    cmdPulse(s);
  else if (!strcmp(cmd, "pwm"))      cmdPwm(s);
  else if (!strcmp(cmd, "step"))     cmdStep(s);
  else if (!strcmp(cmd, "beep"))     cmdBeep(s);
  else if (!strcmp(cmd, "i2c"))      cmdI2c();
  else if (!strcmp(cmd, "sd"))       cmdSd();
  else if (!strcmp(cmd, "lcd7920"))  cmdLcd7920(s);
  else if (!strcmp(cmd, "lcd7567"))  cmdLcd7567(s);
  else if (!strcmp(cmd, "esp"))      cmdEsp(s);
  else if (!strcmp(cmd, "wdt"))      cmdWdt();
  else if (!strcmp(cmd, "safe"))     { safeState(); Serial.println(F("safe")); }
  else { Serial.print(F("unknown: ")); Serial.println(cmd); }
}

void setup() {
  uint8_t mcusr = MCUSR; MCUSR = 0; wdt_disable();
  safeState();                               // FIRST: heaters off, drivers disabled
  Serial.begin(115200);
  Serial.println(F("\n=== FYSETC One+ probe v1 ==="));
  Serial.print(F("Reset cause MCUSR=0x")); Serial.print(mcusr, HEX);
  Serial.println(F("  (1=power-on 2=external 4=brown-out 8=watchdog)"));
  Serial.println(F("Type 'help'."));
}

void loop() {
  if (heatOffAt && (int32_t)(millis() - heatOffAt) >= 0) {
    analogWrite(HEATER_0_PIN, 0); analogWrite(HEATER_BED_PIN, 0); analogWrite(FAN_PIN, 0);
    safeState(); Serial.println(F("[auto-off: power outputs forced LOW]"));
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineLen) { line[lineLen] = 0; Serial.print(F("> ")); Serial.println(line); dispatch(line); lineLen = 0; }
    } else if (lineLen < sizeof(line) - 1) line[lineLen++] = tolower(c);
  }
}
