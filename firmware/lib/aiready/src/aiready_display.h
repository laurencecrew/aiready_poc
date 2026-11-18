// aiready_display.h - verified U8g2 setup for the One+ ST7565 display (header-only).
// Needs lib_deps = olikraus/U8g2. Usage:
//   #include <aiready_display.h>
//   U8G2_ST7565_ZOLEN_128X64_2_4W_HW_SPI u8g2(U8G2_R0, LCD_CS_PIN, LCD_A0_PIN, LCD_RESET_PIN);
//   aiDisplayBegin(u8g2);        // after aiSafeInit()
// Call aiDisplayBegin() again instead of u8g2.begin()/setFlipMode(): both re-send the
// ZOLEN init, which leaves the image mirrored and shifted unless 0xA0 is re-applied.
#pragma once
#include <U8g2lib.h>
#include "one_plus_pins.h"

inline void aiDisplayBegin(U8G2 &u8g2, uint8_t contrast = LCD_CONTRAST) {
  u8g2.begin();
  u8g2.sendF("c", LCD_ADC_NORMAL_CMD);          // segment direction normal
  u8g2.getU8x8()->x_offset = LCD_X_OFFSET;
  u8g2.setContrast(contrast);
}
