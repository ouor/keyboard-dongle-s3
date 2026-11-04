#pragma once
#include <stdint.h>

enum { LCD_GRAY = 0x8410, LCD_GREEN = 0x07E0, LCD_YELLOW = 0xFFE0, LCD_CYAN = 0x07FF };

// 0.96" ST7735S status screen: one state line in `color`, then the device name and address.
void lcd_init(void);
void lcd_show(const char *state, uint16_t color, const char *name, const char *addr);
