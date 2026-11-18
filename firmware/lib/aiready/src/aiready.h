// aiready.h - AI-ready runtime + thin HAL for the FYSETC One+ V2.2 (ATmega2560)
//
// Minimal app:
//   #include <aiready.h>
//   void setup() { aiSafeInit(); AiReady.begin("my_app", "0.1"); }
//   void loop()  { AiReady.poll(); /* app code, non-blocking where possible */ }
//
// The runtime shares Serial (115200) with the app. Lines starting with "@ai " are
// runtime commands and get one JSON line back; other lines go to AiReady.onLine().
// The self-description lives in flash at AI_DESC_ADDR (written by the host tool,
// survives normal uploads); the runtime only streams it, it holds no copy.
//
// Requires build_flags = -I../include (one_plus_pins.h) and lib_extra_dirs = ../lib.
// Uses TIMER0_COMPA (1 kHz input polling): do not analogWrite() D13 or define that ISR.
#pragma once
#include <Arduino.h>
#include "one_plus_pins.h"

#define AI_PROTOCOL_VERSION 1
#define AI_BOARD_NAME "FYSETC One+ V2.2"
#define AI_PROFILE "cnc-poc-1"

// Bootstrap pointers reported in the banner, '@ai id' and '@ai help'.
#define AI_TOOLS_URL "https://github.com/laurencecrew/aiready_poc"
#define AI_INSTALL_CMD "pip install mcp git+https://github.com/laurencecrew/aiready_poc#subdirectory=host"
#define AI_MCP_CMD "python -m aiready.mcp_server"

// ---------------------------------------------------------------- descriptor block
#define AI_DESC_ADDR 0x3BC00UL   // 16 KB block directly below the 1 KB Optiboot bootloader (0x3FC00)
#define AI_DESC_SIZE 0x4000U
#define AI_DESC_HEADER 32U       // reserved u32, "AIRD", fmt u16, comp u16, len u32, raw u32, crc u32, id[8]

// ---------------------------------------------------------------- safe state
// Call as the FIRST line of setup(). Heater/bed/fan LOW, all stepper drivers disabled,
// step/dir/beeper LOW, SPI chip-selects HIGH, inputs pulled up. D4/D6/D7 untouched.
void aiSafeInit();
// Force outputs safe again at any time (also '@ai safe'): MOSFETs LOW, drivers disabled.
void aiSafeOutputs();
// Reset cause from MCUSR (Optiboot passes it through) plus a .noinit magic for aiReboot():
// "soft", "power_on", "brownout", "watchdog", "external" (RESET/DTR/after upload), "jtag", "unknown".
const char *aiResetCause();
uint8_t aiResetFlags();   // raw MCUSR at boot
// Software reboot through the watchdog, recorded as "soft".
void aiReboot();

// ---------------------------------------------------------------- thermistors
enum AiTempCh : uint8_t { AI_T_X_DRIVER = 0, AI_T_X_MOTOR = 1 };   // TEMP0 (A13), TEMP1 (A14)
enum AiTempFault : uint8_t { AI_TF_OK = 0, AI_TF_OPEN = 1, AI_TF_SHORT = 2 };
// Reads now (4-sample average). Returns degC (NAN on fault); *fault and *adc optional.
float aiTempRead(AiTempCh ch, AiTempFault *fault = nullptr, int *adc = nullptr);
inline float aiTempC(AiTempCh ch) { return aiTempRead(ch); }
float aiAdcToC(int adc);   // 100k NTC B3950, 4.7k pull-up [R]; NAN outside 6..1014

// ---------------------------------------------------------------- thermal guard (X axis)
#ifndef AI_GUARD_X_DRIVER_MAX_C
#define AI_GUARD_X_DRIVER_MAX_C 85.0f
#endif
#ifndef AI_GUARD_X_MOTOR_MAX_C
#define AI_GUARD_X_MOTOR_MAX_C 75.0f
#endif
// Runs from AiReady.poll() and inside aiStepperMove(), every 100 ms. On over-temperature
// or sensor fault (3 consecutive samples) X ENABLE goes HIGH and the trip latches.
bool aiGuardTripped();
const char *aiGuardReason();   // nullptr when not tripped
void aiGuardClear();           // un-latch (X stays disabled until re-enabled)
void aiGuardEnable(bool on);   // default on; disable only if X is not energised
bool aiGuardEnabled();
void aiGuardService();         // called by the runtime; safe to call more often

// ---------------------------------------------------------------- endstops + encoder
// Polled at 1 kHz in TIMER0_COMPA, debounced (3 ms). Started by AiReady.begin()
// (or aiInputsBegin() if the runtime is not used).
enum AiEndstop : uint8_t { AI_X_MIN = 0, AI_X_MAX, AI_Y_MIN, AI_Y_MAX, AI_Z_MIN, AI_Z_MAX };
void aiInputsBegin();
bool aiEndstop(AiEndstop e);   // true = triggered (input LOW)
uint8_t aiEndstopMask();       // bit n = AiEndstop n triggered

#ifndef AI_ENC_STEPS_PER_DETENT
#define AI_ENC_STEPS_PER_DETENT 2   // [V] measured: 5 detents = 10 transitions (2026-11-18)
#endif
namespace AiEncoder {
  long raw();                  // quadrature transitions
  long position();             // raw / AI_ENC_STEPS_PER_DETENT
  void reset();
  bool button();               // debounced, true = pressed
  bool clicked();              // true once per press
}

// ---------------------------------------------------------------- steppers
enum AiAxis : uint8_t { AI_AXIS_X = 0, AI_AXIS_Y, AI_AXIS_Z, AI_AXIS_E0 };
// Returns false (and stays disabled) if enabling X while the guard is tripped.
bool aiStepperEnable(AiAxis a, bool on);
bool aiStepperEnabled(AiAxis a);
// Blocking move. steps sign = direction (positive = DIR HIGH [?]). Stops early on
// the endstop in the direction of travel (if stop_on_endstop and the axis has one)
// or when the X guard trips. Returns the signed number of steps done.
// The runtime does not answer serial during the move.
long aiStepperMove(AiAxis a, long steps, uint16_t steps_per_s, bool stop_on_endstop = true);

// ---------------------------------------------------------------- beeper
// PASSIVE buzzer on D37 [V]: a steady HIGH only clicks; it needs a square wave.
// Generated with Timer3 (CTC interrupt toggling PC0) only while sounding, so Timer1
// (motion) and Timer2 (heater/fan PWM) are untouched. Never use tone() (takes Timer2).
// While a tone plays, Timer3 is owned by the beeper; apps using Timer3 must not beep.
#ifndef AI_BEEP_HZ
#define AI_BEEP_HZ 2700        // 2000, 2700 and 4000 Hz all audible [V]; loudest not determined
#endif
void aiToneStart(uint16_t hz = AI_BEEP_HZ);   // non-blocking, 31..20000 Hz
void aiToneStop();
void aiBeep(uint16_t ms, uint16_t hz = AI_BEEP_HZ);   // blocking

// ---------------------------------------------------------------- runtime
typedef void (*AiLineHandler)(char *line);

class AiReadyRuntime {
public:
  void begin(const char *fw_name, const char *fw_version, unsigned long baud = 115200);
  void poll();                       // call every loop(); keep loop() iterations short
  void onLine(AiLineHandler h) { handler_ = h; }
  bool descriptorPresent();          // valid "AIRD" header in flash
  void descriptorId(char out[17]);   // hex id from the header, "none" if absent
  bool safeRequested();              // true once after '@ai safe' (app should stop motion)
  void printState();                 // JSON state line (r = "state")

private:
  void handle(char *cmd);
  void cmdId();
  void cmdDesc(char *args);
  void cmdPins();
  void cmdPeek(char *args);
  void cmdHelp();
  const char *name_ = "app";
  const char *ver_ = "0";
  AiLineHandler handler_ = nullptr;
  char line_[72];
  uint8_t len_ = 0;
  bool overflow_ = false;
  uint16_t watchMs_ = 0;
  uint32_t lastWatch_ = 0;
  bool safeReq_ = false;
};

extern AiReadyRuntime AiReady;
