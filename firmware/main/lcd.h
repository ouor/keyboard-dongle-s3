#pragma once
#include <stdint.h>

enum { LCD_GRAY = 0x8410, LCD_GREEN = 0x07E0, LCD_YELLOW = 0xFFE0, LCD_CYAN = 0x07FF };

// Status display: the 0.96" ST7735S screen (lcd.c) or, on boards without one, an RGB LED in `color` (led.c).
void lcd_init(void);
void lcd_show(const char *state, uint16_t color, const char *name, const char *addr);
