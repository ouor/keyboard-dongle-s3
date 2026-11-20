// Status LED for boards without a screen (ESP32-S3-Zero: one WS2812 on GPIO21). Implements lcd.h: only the color is shown.
#include "sdkconfig.h"
#if CONFIG_DONGLE_BOARD_S3ZERO
#include "esp_log.h"
#include "lcd.h"
#include "led_strip.h"

#define LED_GPIO 21
#define DIM 3  // RGB565 -> 8-bit, then >> DIM: full brightness is blinding

static led_strip_handle_t s_led;

void lcd_show(const char *state, uint16_t color, const char *name, const char *addr)
{
    if (!s_led) {
        return;
    }
    const uint32_t r = ((color >> 11) & 0x1F) << 3, g = ((color >> 5) & 0x3F) << 2, b = (color & 0x1F) << 3;
    led_strip_set_pixel(s_led, 0, r >> DIM, g >> DIM, b >> DIM);
    led_strip_refresh(s_led);
}

void lcd_init(void)
{
    const led_strip_config_t cfg = {.strip_gpio_num = LED_GPIO, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                                    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
    const led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_led) != ESP_OK) {
        ESP_LOGE("led", "init failed");  // the dongle works without its LED
        s_led = NULL;
    }
}
#endif
