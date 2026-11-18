// AI-ready base firmware for the FYSETC One+ V2.2 (profile cnc-poc-1).
// Runtime (@ai protocol over USB serial) + status screen. No motion, all outputs safe.
// Template for applications: keep aiSafeInit() first, AiReady.begin() and AiReady.poll().
#include <Arduino.h>
#include <aiready.h>
#include <aiready_display.h>

U8G2_ST7565_ZOLEN_128X64_2_4W_HW_SPI u8g2(U8G2_R0, LCD_CS_PIN, LCD_A0_PIN, LCD_RESET_PIN);

static void appLine(char *line) {
  Serial.print(F("{\"r\":\"app\",\"msg\":\"base firmware has no app commands; send '@ai help'\",\"got\":\""));
  Serial.print(line);
  Serial.println(F("\"}"));
}

static void tempText(char *buf, AiTempCh ch) {
  AiTempFault f;
  float t = aiTempRead(ch, &f);
  if (f == AI_TF_OPEN) strcpy(buf, "open");
  else if (f == AI_TF_SHORT) strcpy(buf, "short");
  else dtostrf(t, 5, 1, buf);
}

static void drawStatus() {
  char drv[10], mot[10], line[24], id[17];
  tempText(drv, AI_T_X_DRIVER);
  tempText(mot, AI_T_X_MOTOR);
  AiReady.descriptorId(id);
  uint8_t m = aiEndstopMask();
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 8, "AI-ready  cnc-poc-1");
    u8g2.drawHLine(0, 10, 128);
    snprintf(line, sizeof line, "X drv: %s C", drv);  u8g2.drawStr(0, 21, line);
    snprintf(line, sizeof line, "X mot: %s C", mot);  u8g2.drawStr(0, 31, line);
    u8g2.drawStr(0, 41, aiGuardTripped() ? "guard: TRIPPED" : "guard: ok");
    snprintf(line, sizeof line, "ES %c%c %c%c %c%c  enc %ld%s",
             (m & 1) ? 'X' : 'x', (m & 2) ? 'X' : 'x', (m & 4) ? 'Y' : 'y', (m & 8) ? 'Y' : 'y',
             (m & 16) ? 'Z' : 'z', (m & 32) ? 'Z' : 'z', AiEncoder::position(), AiEncoder::button() ? "*" : "");
    u8g2.drawStr(0, 51, line);
    snprintf(line, sizeof line, "desc %.8s", id);     u8g2.drawStr(0, 62, line);
  } while (u8g2.nextPage());
}

void setup() {
  aiSafeInit();                         // must stay first (CLAUDE.md hard rule)
  AiReady.begin("aiready_base", "0.1.0");
  AiReady.onLine(appLine);
  aiDisplayBegin(u8g2);
}

void loop() {
  AiReady.poll();
  static uint32_t last = 0;
  if (millis() - last >= 250) { last = millis(); drawStatus(); }
}
