// aiready_hal.cpp - safe state, thermistors, X thermal guard, polled inputs, steppers, beeper
#include "aiready.h"
#include <avr/wdt.h>
#include <util/atomic.h>
#include <math.h>

// ================================================================= safe state / reset cause
#define AI_MAGIC_SOFT 0x5AFE2B07UL
// volatile: aiReboot() never returns, so without it the compiler drops the store as dead.
static volatile uint32_t ai_noinit_magic __attribute__((section(".noinit")));
// Optiboot passes MCUSR through to the app. Captured in .init3, before .bss/.data init
// (.init4), so it must live in .noinit too.
static volatile uint8_t ai_mcusr __attribute__((section(".noinit")));
static const char *ai_reset_cause = "unknown";

// Save the reset flags, then disable the WDT before constructors and init() (.init3 idiom).
extern "C" void ai_wdt_off_early(void) __attribute__((naked, used, section(".init3")));
extern "C" void ai_wdt_off_early(void) {
  ai_mcusr = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

void aiSafeOutputs() {
  const uint8_t pw[] = {HEATER_0_PIN, HEATER_BED_PIN, FAN_PIN};
  for (uint8_t p : pw) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  const uint8_t en[] = {X_ENABLE_PIN, Y_ENABLE_PIN, Z_ENABLE_PIN, E0_ENABLE_PIN};
  for (uint8_t p : en) { digitalWrite(p, HIGH); pinMode(p, OUTPUT); }   // HIGH = disabled
  aiToneStop();                                  // passive buzzer: stop Timer3 tone, pin LOW
  pinMode(BEEPER_PIN, OUTPUT);
}

void aiSafeInit() {
  MCUSR = 0;
  wdt_disable();
  aiSafeOutputs();
  const uint8_t sd[] = {X_STEP_PIN, X_DIR_PIN, Y_STEP_PIN, Y_DIR_PIN, Z_STEP_PIN, Z_DIR_PIN,
                        E0_STEP_PIN, E0_DIR_PIN};
  for (uint8_t p : sd) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  digitalWrite(SDSS, HIGH); pinMode(SDSS, OUTPUT);          // SPI master needs SS as output
  digitalWrite(LCD_CS_PIN, HIGH); pinMode(LCD_CS_PIN, OUTPUT);
  const uint8_t in[] = {X_MIN_PIN, X_MAX_PIN, Y_MIN_PIN, Y_MAX_PIN, Z_MIN_PIN, Z_MAX_PIN,
                        BTN_EN1, BTN_EN2, BTN_ENC, SD_DETECT_PIN};
  for (uint8_t p : in) pinMode(p, INPUT_PULLUP);
  // D4, D6, D7 deliberately left as inputs (unidentified external loads).
  const uint8_t f = ai_mcusr;
  if ((f & _BV(WDRF)) && ai_noinit_magic == AI_MAGIC_SOFT) ai_reset_cause = "soft";
  else if (f & _BV(PORF))  ai_reset_cause = "power_on";
  else if (f & _BV(BORF))  ai_reset_cause = "brownout";
  else if (f & _BV(WDRF))  ai_reset_cause = "watchdog";
  else if (f & _BV(EXTRF)) ai_reset_cause = "external";   // RESET button, DTR auto-reset, after an upload
  else if (f & _BV(JTRF))  ai_reset_cause = "jtag";
  ai_noinit_magic = 0;
}

const char *aiResetCause() { return ai_reset_cause; }
uint8_t aiResetFlags() { return ai_mcusr; }

void aiReboot() {
  aiSafeOutputs();
  ai_noinit_magic = AI_MAGIC_SOFT;
  // Optiboot sees WDRF without EXTRF and jumps straight to the app (no bootloader window).
  wdt_enable(WDTO_15MS);
  for (;;) {}
}

// ================================================================= thermistors
float aiAdcToC(int adc) {
  if (adc >= 1015 || adc <= 5) return NAN;
  float r = 4700.0f * adc / (1023.0f - adc);
  return 1.0f / (1.0f / 298.15f + log(r / 100000.0f) / 3950.0f) - 273.15f;
}

float aiTempRead(AiTempCh ch, AiTempFault *fault, int *adc) {
  uint8_t c = (ch == AI_T_X_DRIVER) ? TEMP_0_ADC : TEMP_BED_ADC;
  analogRead(c);                                  // discard first sample after mux change
  int sum = 0;
  for (uint8_t i = 0; i < 4; i++) sum += analogRead(c);
  int a = (sum + 2) / 4;
  if (adc) *adc = a;
  AiTempFault f = (a >= 1015) ? AI_TF_OPEN : (a <= 5) ? AI_TF_SHORT : AI_TF_OK;
  if (fault) *fault = f;
  return f == AI_TF_OK ? aiAdcToC(a) : NAN;
}

// ================================================================= thermal guard
static bool g_on = true, g_tripped = false;
static const char *g_reason = nullptr;
static uint8_t g_bad = 0;
static uint32_t g_last = 0;

bool aiGuardTripped() { return g_tripped; }
const char *aiGuardReason() { return g_reason; }
bool aiGuardEnabled() { return g_on; }
void aiGuardEnable(bool on) { g_on = on; if (!on) g_bad = 0; }
void aiGuardClear() { g_tripped = false; g_reason = nullptr; g_bad = 0; }

static const char *guardCheck() {
  AiTempFault f; float t;
  t = aiTempRead(AI_T_X_DRIVER, &f);
  if (f == AI_TF_OPEN) return "x_driver_sensor_open";
  if (f == AI_TF_SHORT) return "x_driver_sensor_short";
  if (t > AI_GUARD_X_DRIVER_MAX_C) return "x_driver_over_temp";
  t = aiTempRead(AI_T_X_MOTOR, &f);
  if (f == AI_TF_OPEN) return "x_motor_sensor_open";
  if (f == AI_TF_SHORT) return "x_motor_sensor_short";
  if (t > AI_GUARD_X_MOTOR_MAX_C) return "x_motor_over_temp";
  return nullptr;
}

void aiGuardService() {
  uint32_t now = millis();
  if (now - g_last < 100) return;
  g_last = now;
  if (!g_on || g_tripped) return;
  const char *r = guardCheck();
  if (!r) { g_bad = 0; return; }
  if (++g_bad >= 3) {
    g_tripped = true;
    g_reason = r;
    digitalWrite(X_ENABLE_PIN, HIGH);
  }
}

// ================================================================= 1 kHz input polling
// Encoder: PC6 (A, D31), PC4 (B, D33), PC2 (push, D35).
// Endstops: X_MIN PE5, X_MAX PE4, Y_MIN PG1, Y_MAX PG0, Z_MIN PD3, Z_MAX PD2.
static volatile long enc_raw = 0;
static volatile uint8_t es_stable = 0, btn_stable = 0, btn_clicks = 0;
static uint8_t enc_prev = 0, es_last = 0, es_cnt = 0, btn_last = 0, btn_cnt = 0;
static bool inputs_started = false;

static inline uint8_t sampleEndstops() {   // bit = triggered (LOW)
  uint8_t e = PINE, g = PING, d = PIND, m = 0;
  if (!(e & _BV(5))) m |= 1 << AI_X_MIN;
  if (!(e & _BV(4))) m |= 1 << AI_X_MAX;
  if (!(g & _BV(1))) m |= 1 << AI_Y_MIN;
  if (!(g & _BV(0))) m |= 1 << AI_Y_MAX;
  if (!(d & _BV(3))) m |= 1 << AI_Z_MIN;
  if (!(d & _BV(2))) m |= 1 << AI_Z_MAX;
  return m;
}

ISR(TIMER0_COMPA_vect) {
  static const int8_t QT[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t c = PINC;
  uint8_t ab = ((c >> 5) & 2) | ((c >> 4) & 1);          // A = PC6 -> bit1, B = PC4 -> bit0
  enc_raw += QT[(enc_prev << 2) | ab];
  enc_prev = ab;

  uint8_t b = (c & _BV(2)) ? 0 : 1;
  if (b == btn_last) { if (btn_cnt < 3 && ++btn_cnt == 3) { if (b && !btn_stable) btn_clicks++; btn_stable = b; } }
  else { btn_last = b; btn_cnt = 0; }

  uint8_t m = sampleEndstops();
  if (m == es_last) { if (es_cnt < 3 && ++es_cnt == 3) es_stable = m; }
  else { es_last = m; es_cnt = 0; }
}

void aiInputsBegin() {
  if (inputs_started) return;
  inputs_started = true;
  uint8_t c = PINC;
  enc_prev = ((c >> 5) & 2) | ((c >> 4) & 1);
  es_stable = es_last = sampleEndstops();
  OCR0A = 0x80;                 // Timer0 overflows every 1.024 ms (millis); compare A mid-way
  TIMSK0 |= _BV(OCIE0A);
}

bool aiEndstop(AiEndstop e) { return es_stable & (1 << e); }
uint8_t aiEndstopMask() { return es_stable; }

namespace AiEncoder {
  long raw() { long v; ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { v = enc_raw; } return v; }
  long position() { return raw() / AI_ENC_STEPS_PER_DETENT; }
  void reset() { ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { enc_raw = 0; } }
  bool button() { return btn_stable; }
  bool clicked() {
    bool r = false;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { if (btn_clicks) { btn_clicks--; r = true; } }
    return r;
  }
}

// ================================================================= steppers
struct AxisPins { uint8_t step, dir, en; int8_t emin, emax; };
static const AxisPins AX[4] = {
  {X_STEP_PIN, X_DIR_PIN, X_ENABLE_PIN, AI_X_MIN, AI_X_MAX},
  {Y_STEP_PIN, Y_DIR_PIN, Y_ENABLE_PIN, AI_Y_MIN, AI_Y_MAX},
  {Z_STEP_PIN, Z_DIR_PIN, Z_ENABLE_PIN, AI_Z_MIN, AI_Z_MAX},
  {E0_STEP_PIN, E0_DIR_PIN, E0_ENABLE_PIN, -1, -1},
};

bool aiStepperEnable(AiAxis a, bool on) {
  if (a > AI_AXIS_E0) return false;
  if (on && a == AI_AXIS_X && g_tripped) { digitalWrite(AX[a].en, HIGH); return false; }
  digitalWrite(AX[a].en, on ? LOW : HIGH);
  return true;
}

bool aiStepperEnabled(AiAxis a) { return a <= AI_AXIS_E0 && digitalRead(AX[a].en) == LOW; }

long aiStepperMove(AiAxis a, long steps, uint16_t sps, bool stop_on_endstop) {
  if (a > AI_AXIS_E0 || steps == 0) return 0;
  if (sps < 1) sps = 1;
  if (sps > 20000) sps = 20000;
  const AxisPins &p = AX[a];
  bool fwd = steps > 0;
  long n = fwd ? steps : -steps, done = 0;
  int8_t stop = fwd ? p.emax : p.emin;
  digitalWrite(p.dir, fwd ? HIGH : LOW);
  delayMicroseconds(2);
  uint32_t period = 1000000UL / sps, next = micros();
  while (done < n) {
    if (stop_on_endstop && stop >= 0 && aiEndstop((AiEndstop)stop)) break;
    if (a == AI_AXIS_X) { aiGuardService(); if (g_tripped) break; }
    while ((int32_t)(micros() - next) < 0) {}
    next += period;
    digitalWrite(p.step, HIGH);
    delayMicroseconds(2);
    digitalWrite(p.step, LOW);
    done++;
  }
  return fwd ? done : -done;
}

// ================================================================= beeper (passive)
static volatile bool tone_on = false;

ISR(TIMER3_COMPA_vect) { if (tone_on) PINC = _BV(0); }   // writing PINC toggles PC0 = D37

void aiToneStart(uint16_t hz) {
  if (hz < 31) hz = 31;              // OCR3A is 16-bit at 2 MHz
  if (hz > 20000) hz = 20000;
  TIMSK3 &= ~_BV(OCIE3A);
  TCCR3A = 0;
  TCCR3B = _BV(WGM32) | _BV(CS31);   // CTC, clk/8 = 2 MHz
  OCR3A = (uint16_t)(1000000UL / hz) - 1;   // toggle at 2*hz
  TCNT3 = 0;
  tone_on = true;
  TIMSK3 |= _BV(OCIE3A);
}

void aiToneStop() {
  TIMSK3 &= ~_BV(OCIE3A);
  tone_on = false;
  TCCR3B = 0;                        // stop Timer3
  digitalWrite(BEEPER_PIN, LOW);     // no DC through the buzzer coil
}

void aiBeep(uint16_t ms, uint16_t hz) {
  aiToneStart(hz);
  delay(ms);
  aiToneStop();
}
