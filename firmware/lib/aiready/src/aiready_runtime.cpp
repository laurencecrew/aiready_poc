// aiready_runtime.cpp - "@ai" serial protocol: id, desc, state, watch, pins, peek, safe, clear, reboot
// Every reply is one JSON line with "r" = the command it answers.
#include "aiready.h"
#include <avr/pgmspace.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

AiReadyRuntime AiReady;

static const char HEXD[] = "0123456789abcdef";
static void printHex8(uint8_t b) { Serial.write(HEXD[b >> 4]); Serial.write(HEXD[b & 15]); }
static uint8_t descByte(uint16_t off) { return pgm_read_byte_far(AI_DESC_ADDR + off); }

static int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

static void kv(const __FlashStringHelper *k) { Serial.write('"'); Serial.print(k); Serial.print(F("\":")); }
static void kstr(const __FlashStringHelper *k, const char *v) {
  kv(k);
  if (!v) { Serial.print(F("null")); return; }
  Serial.write('"'); Serial.print(v); Serial.write('"');
}
static void kfloat(const __FlashStringHelper *k, float v) { kv(k); if (isnan(v)) Serial.print(F("null")); else Serial.print(v, 1); }
static void comma() { Serial.write(','); }

// ------------------------------------------------------------------ descriptor
bool AiReadyRuntime::descriptorPresent() {
  return descByte(4) == 'A' && descByte(5) == 'I' && descByte(6) == 'R' && descByte(7) == 'D';
}

void AiReadyRuntime::descriptorId(char out[17]) {
  if (!descriptorPresent()) { strcpy(out, "none"); return; }
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = descByte(24 + i);
    out[2 * i] = HEXD[b >> 4]; out[2 * i + 1] = HEXD[b & 15];
  }
  out[16] = 0;
}

// ------------------------------------------------------------------ lifecycle
static void printIdFields(const char *name, const char *ver) {
  char id[17];
  AiReady.descriptorId(id);
  kv(F("aiready")); Serial.print(AI_PROTOCOL_VERSION); comma();
  kstr(F("board"), AI_BOARD_NAME); comma();
  kstr(F("profile"), AI_PROFILE); comma();
  kstr(F("fw"), name); comma();
  kstr(F("ver"), ver); comma();
  kv(F("built")); Serial.print(F("\"" __DATE__ " " __TIME__ "\"")); comma();
  kstr(F("desc"), id); comma();
  kv(F("desc_addr")); Serial.print(F("\"0x3BC00\"")); comma();
  kstr(F("reset"), aiResetCause()); comma();
  kv(F("mcusr")); Serial.print(aiResetFlags()); comma();
  kv(F("up_ms")); Serial.print(millis()); comma();
  kv(F("ram_free")); Serial.print(freeRam()); comma();
  // Bootstrap for an agent that has never seen this board: where the tools and docs are.
  kv(F("tools")); Serial.print(F("\"" AI_TOOLS_URL "\"")); comma();
  kv(F("install")); Serial.print(F("\"" AI_INSTALL_CMD "\"")); comma();
  kv(F("mcp")); Serial.print(F("\"" AI_MCP_CMD "\""));
}

void AiReadyRuntime::begin(const char *fw_name, const char *fw_version, unsigned long baud) {
  name_ = fw_name; ver_ = fw_version;
  Serial.begin(baud);
  aiInputsBegin();
  Serial.print(F("{\"r\":\"banner\","));
  printIdFields(name_, ver_);
  Serial.println(F(",\"hint\":\"send '@ai help'\"}"));
}

void AiReadyRuntime::poll() {
  aiGuardService();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (overflow_) {
        Serial.println(F("{\"r\":\"err\",\"msg\":\"line too long\"}"));
      } else if (len_) {
        line_[len_] = 0;
        if (strncmp(line_, "@ai", 3) == 0 && (line_[3] == ' ' || line_[3] == 0)) {
          handle(line_ + 3);
        } else if (handler_) {
          handler_(line_);
        } else {
          Serial.println(F("{\"r\":\"err\",\"msg\":\"no app command handler; runtime commands start with '@ai '\"}"));
        }
      }
      len_ = 0; overflow_ = false;
    } else if (len_ < sizeof(line_) - 1) {
      line_[len_++] = c;
    } else {
      overflow_ = true;
    }
  }
  if (watchMs_ && millis() - lastWatch_ >= watchMs_) { lastWatch_ = millis(); printState(); }
}

bool AiReadyRuntime::safeRequested() { bool r = safeReq_; safeReq_ = false; return r; }

// ------------------------------------------------------------------ commands
static char *tok(char *&s) {
  while (*s == ' ') s++;
  if (!*s) return nullptr;
  char *t = s;
  while (*s && *s != ' ') s++;
  if (*s) *s++ = 0;
  return t;
}

void AiReadyRuntime::handle(char *s) {
  char *cmd = tok(s);
  if (!cmd || !strcmp(cmd, "help")) cmdHelp();
  else if (!strcmp(cmd, "id")) cmdId();
  else if (!strcmp(cmd, "desc")) cmdDesc(s);
  else if (!strcmp(cmd, "state")) printState();
  else if (!strcmp(cmd, "watch")) {
    char *t = tok(s);
    long ms = t ? atol(t) : 0;
    watchMs_ = ms <= 0 ? 0 : (ms < 50 ? 50 : (ms > 60000 ? 60000 : ms));
    lastWatch_ = millis();
    Serial.print(F("{\"r\":\"watch\",\"ms\":")); Serial.print(watchMs_); Serial.println('}');
  }
  else if (!strcmp(cmd, "pins")) cmdPins();
  else if (!strcmp(cmd, "peek")) cmdPeek(s);
  else if (!strcmp(cmd, "safe")) {
    aiSafeOutputs(); safeReq_ = true;
    Serial.println(F("{\"r\":\"safe\",\"ok\":true,\"msg\":\"MOSFETs LOW, all drivers disabled\"}"));
  }
  else if (!strcmp(cmd, "clear")) {
    aiGuardClear();
    Serial.println(F("{\"r\":\"clear\",\"ok\":true}"));
  }
  else if (!strcmp(cmd, "reboot")) {
    Serial.println(F("{\"r\":\"reboot\",\"ok\":true}"));
    Serial.flush();
    aiReboot();
  }
  else {
    Serial.print(F("{\"r\":\"err\",\"msg\":\"unknown command\",\"cmd\":\"")); Serial.print(cmd); Serial.println(F("\"}"));
  }
}

void AiReadyRuntime::cmdHelp() {
  Serial.println(F("{\"r\":\"help\",\"cmds\":[\"id\",\"desc <off> <len<=128>\",\"state\",\"watch <ms|0>\","
                   "\"pins\",\"peek <addr> <n<=32>\",\"safe\",\"clear\",\"reboot\",\"help\"],"
                   "\"protocol\":\"send '@ai <cmd>' + newline; each reply is one JSON line whose r field names the command\","
                   "\"desc_format\":\"self-description = 16 KB block, read with desc <off> <len> (hex). Header 32 bytes little-endian: "
                   "0 jmp instruction (4), 4 magic AIRD (4), 8 format u16 =1, 10 compression u16 =1 zlib, "
                   "12 payload length u32, 16 JSON length u32, 20 CRC32 of payload u32, 24 id (8, first 8 bytes of SHA-256 of the JSON). "
                   "Payload from offset 32 = zlib stream of minified JSON. Python: json.loads(zlib.decompress(block[32:32+length]))\","
                   "\"tools\":\"" AI_TOOLS_URL "\",\"docs\":\"" AI_TOOLS_URL "/blob/main/docs/protocol.md\"}"));
}

void AiReadyRuntime::cmdId() {
  Serial.print(F("{\"r\":\"id\","));
  printIdFields(name_, ver_);
  Serial.println('}');
}

void AiReadyRuntime::cmdDesc(char *s) {
  char *a = tok(s), *b = tok(s);
  long off = a ? strtol(a, nullptr, 0) : 0, len = b ? strtol(b, nullptr, 0) : AI_DESC_HEADER;
  if (off < 0 || off >= (long)AI_DESC_SIZE) { Serial.println(F("{\"r\":\"err\",\"msg\":\"desc offset out of range\"}")); return; }
  if (len < 1) len = 1;
  if (len > 128) len = 128;
  if (off + len > (long)AI_DESC_SIZE) len = AI_DESC_SIZE - off;
  Serial.print(F("{\"r\":\"desc\",\"off\":")); Serial.print(off);
  Serial.print(F(",\"len\":")); Serial.print(len);
  Serial.print(F(",\"hex\":\""));
  for (long i = 0; i < len; i++) printHex8(descByte(off + i));
  Serial.println(F("\"}"));
}

void AiReadyRuntime::printState() {
  static const char *const ES[6] = {"x_min", "x_max", "y_min", "y_max", "z_min", "z_max"};
  static const char *const TF[3] = {"ok", "open", "short"};
  Serial.print(F("{\"r\":\"state\",\"t\":")); Serial.print(millis());

  Serial.print(F(",\"endstops\":{"));
  uint8_t m = aiEndstopMask();
  for (uint8_t i = 0; i < 6; i++) {
    if (i) comma();
    Serial.write('"'); Serial.print(ES[i]); Serial.print(F("\":")); Serial.print((m >> i) & 1);
  }
  Serial.print(F("},\"encoder\":{"));
  kv(F("raw")); Serial.print(AiEncoder::raw()); comma();
  kv(F("pos")); Serial.print(AiEncoder::position()); comma();
  kv(F("btn")); Serial.print(AiEncoder::button() ? 1 : 0);

  Serial.print(F("},\"temp\":{"));
  for (uint8_t ch = 0; ch < 2; ch++) {
    AiTempFault f; int adc;
    float t = aiTempRead((AiTempCh)ch, &f, &adc);
    if (ch) comma();
    Serial.print(ch == 0 ? F("\"x_driver\":{") : F("\"x_motor\":{"));
    kfloat(F("c"), t); comma();
    kv(F("adc")); Serial.print(adc); comma();
    kstr(F("fault"), TF[f]);
    Serial.write('}');
  }
  Serial.print(F("},\"drivers_enabled\":{"));
  kv(F("x")); Serial.print(aiStepperEnabled(AI_AXIS_X)); comma();
  kv(F("y")); Serial.print(aiStepperEnabled(AI_AXIS_Y)); comma();
  kv(F("z")); Serial.print(aiStepperEnabled(AI_AXIS_Z)); comma();
  kv(F("e0")); Serial.print(aiStepperEnabled(AI_AXIS_E0));
  Serial.print(F("},\"guard\":{"));
  kv(F("on")); Serial.print(aiGuardEnabled()); comma();
  kv(F("tripped")); Serial.print(aiGuardTripped()); comma();
  kstr(F("reason"), aiGuardReason()); comma();
  kfloat(F("x_driver_max_c"), AI_GUARD_X_DRIVER_MAX_C); comma();
  kfloat(F("x_motor_max_c"), AI_GUARD_X_MOTOR_MAX_C);
  Serial.print(F("},\"mosfet_level\":{"));
  kv(F("heater")); Serial.print(digitalRead(HEATER_0_PIN)); comma();
  kv(F("bed")); Serial.print(digitalRead(HEATER_BED_PIN)); comma();
  kv(F("fan")); Serial.print(digitalRead(FAN_PIN));
  Serial.print(F("},"));
  kv(F("sd_card")); Serial.print(digitalRead(SD_DETECT_PIN) == LOW); comma();
  kv(F("ram_free")); Serial.print(freeRam());
  Serial.println('}');
}

void AiReadyRuntime::cmdPins() {
  struct P { char n; volatile uint8_t *pin, *ddr, *port; };
  const P ports[] = {
    {'A', &PINA, &DDRA, &PORTA}, {'B', &PINB, &DDRB, &PORTB}, {'C', &PINC, &DDRC, &PORTC},
    {'D', &PIND, &DDRD, &PORTD}, {'E', &PINE, &DDRE, &PORTE}, {'F', &PINF, &DDRF, &PORTF},
    {'G', &PING, &DDRG, &PORTG}, {'H', &PINH, &DDRH, &PORTH}, {'J', &PINJ, &DDRJ, &PORTJ},
    {'K', &PINK, &DDRK, &PORTK}, {'L', &PINL, &DDRL, &PORTL},
  };
  Serial.print(F("{\"r\":\"pins\",\"fmt\":\"PIN DDR PORT\",\"ports\":{"));
  for (uint8_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
    if (i) comma();
    Serial.write('"'); Serial.write(ports[i].n); Serial.print(F("\":\""));
    printHex8(*ports[i].pin); Serial.write(' ');
    printHex8(*ports[i].ddr); Serial.write(' ');
    printHex8(*ports[i].port);
    Serial.write('"');
  }
  Serial.println(F("}}"));
}

// Read-only data-space peek (SRAM + I/O). Registers whose read has side effects
// (UART data, ADC result latch) are skipped and shown as "xx".
static bool peekUnsafe(uint16_t a) {
  return a == 0xC6 || a == 0xCE || a == 0xD6 || a == 0x136 || a == 0x78 || a == 0x79;
}

void AiReadyRuntime::cmdPeek(char *s) {
  char *a = tok(s), *b = tok(s);
  if (!a) { Serial.println(F("{\"r\":\"err\",\"msg\":\"peek <addr> <n>\"}")); return; }
  long addr = strtol(a, nullptr, 0), n = b ? strtol(b, nullptr, 0) : 1;
  if (n < 1) n = 1;
  if (n > 32) n = 32;
  if (addr < 0 || addr + n - 1 > RAMEND) { Serial.println(F("{\"r\":\"err\",\"msg\":\"peek range is 0..RAMEND (0x21FF)\"}")); return; }
  Serial.print(F("{\"r\":\"peek\",\"addr\":")); Serial.print(addr);
  Serial.print(F(",\"hex\":\""));
  for (long i = 0; i < n; i++) {
    uint16_t x = addr + i;
    if (peekUnsafe(x)) Serial.print(F("xx"));
    else printHex8(*(volatile uint8_t *)x);
  }
  Serial.println(F("\"}"));
}
