// USB composite: CDC (config protocol) + MSC (FAT "storage" partition with DongleConfig.exe) + HID (keyboard + consumer).
#include "usb.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "keycodes.h"
#include "keymap.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

static const char *TAG = "usb";

// Bump USB_BCD whenever descriptors change, otherwise Windows may keep the old cached interface layout.
#define USB_VID 0x303A
#define USB_PID 0x4007  // shortcut: TinyUSB-style dev PID (CDC|MSC|HID bitmap), get a real VID/PID before shipping
#define USB_BCD 0x0101  // bump on descriptor changes (Windows caches them)

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_MSC, ITF_HID, ITF_TOTAL };
enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_MSC, STR_HID };
enum { RID_KEYBOARD = 1, RID_CONSUMER = 2, RID_MOUSE = 3 };

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
};

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, 0x81, 8, 0x02, 0x82, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC, STR_MSC, 0x03, 0x83, 64),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), 0x84, 16, 5),
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,  // IAD (required for CDC in a composite)
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const char *s_strings[] = {
    [STR_LANG] = (const char[]){0x09, 0x04},
    [STR_MANUF] = "esp-dongle",
    [STR_PRODUCT] = "BLE Remote Dongle",
    [STR_SERIAL] = NULL,  // filled from eFuse MAC by esp_tinyusb
    [STR_CDC] = "Dongle Config",
    [STR_MSC] = "Dongle Drive",
    [STR_HID] = "Dongle Keys",
};

// ---------------- HID ----------------
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    (void)instance, (void)report_id, (void)type, (void)buf, (void)len;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const *buf, uint16_t len)
{
    (void)instance, (void)report_id, (void)type, (void)buf, (void)len;  // keyboard LEDs: ignored
}

static uint8_t s_mods, s_keys[6];

static bool hid_wait_ready(void)
{
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return tud_hid_ready();
}

void hw_key(uint8_t page, uint16_t usage, bool pressed)
{
    if (!hid_wait_ready()) {
        ESP_LOGW(TAG, "HID not ready, key 0x%x dropped", usage);
        return;
    }
    if (page == KC_PAGE_CONSUMER) {
        uint16_t u = pressed ? usage : 0;
        tud_hid_report(RID_CONSUMER, &u, sizeof(u));
        return;
    }
    if (usage >= 0xE0 && usage <= 0xE7) {
        uint8_t bit = 1u << (usage - 0xE0);
        s_mods = pressed ? (s_mods | bit) : (s_mods & ~bit);
    } else {
        for (int i = 0; i < 6; i++) {
            if (pressed ? s_keys[i] == 0 : s_keys[i] == usage) {
                s_keys[i] = pressed ? (uint8_t)usage : 0;
                break;
            }
        }
    }
    tud_hid_keyboard_report(RID_KEYBOARD, s_mods, s_keys);
}

// ---------------- CDC ----------------
void hw_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (hid_wait_ready()) {
        tud_hid_mouse_report(RID_MOUSE, buttons, x, y, wheel, 0);
    }
}

#define CMD_LINE_MAX 128
static QueueHandle_t s_lines;
static SemaphoreHandle_t s_tx_lock;

void hw_print(const char *line)
{
    if (!tud_cdc_connected()) {
        return;  // nobody listening (DTR low): don't fill the buffer
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, strlen(line));
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)"\r\n", 2);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
    xSemaphoreGive(s_tx_lock);
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    static char line[CMD_LINE_MAX];
    static size_t n;
    uint8_t buf[64];
    size_t got = 0;
    (void)event;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &got) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < got; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                xQueueSend(s_lines, line, 0);  // runs km_cmd outside the TinyUSB task (it may flush with a timeout)
                n = 0;
            }
        } else if (n < CMD_LINE_MAX - 1) {
            line[n++] = c;
        }
    }
}

static void cmd_task(void *arg)
{
    char line[CMD_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_lines, line, portMAX_DELAY)) {
            km_cmd(line);
        }
    }
}

// shortcut: not verified on hardware yet; fallback is holding BOOT (GPIO0) while plugging in.
void hw_dfu(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));  // let the "ok" reply go out
    tinyusb_driver_uninstall();
    // Hand the internal PHY back to USB-Serial/JTAG so the ROM download mode shows up as 303A:1001 (same as Arduino-ESP32).
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    // Pull D-/D+ low briefly so the host sees a disconnect and re-enumerates.
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_set_level(GPIO_NUM_20, 0);
    esp_rom_delay_us(100 * 1000);  // D+/D- low: host sees a disconnect
    gpio_reset_pin(GPIO_NUM_19);
    gpio_reset_pin(GPIO_NUM_20);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);  // pads back to USB-Serial/JTAG, or ROM download mode has no USB
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

// ---------------- MSC ----------------
static void msc_init(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!p) {
        ESP_LOGE(TAG, "no 'storage' partition");
        return;
    }
    wl_handle_t wl;
    ESP_ERROR_CHECK(wl_mount(p, &wl));
    const tinyusb_msc_storage_config_t cfg = {
        .medium.wl_handle = wl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {.config = {.max_files = 4}, .do_not_format = true},  // never wipe DongleConfig.exe
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&cfg, NULL));
}

void usb_init(void)
{
    s_lines = xQueueCreate(4, CMD_LINE_MAX);
    s_tx_lock = xSemaphoreCreateMutex();
    xTaskCreate(cmd_task, "cmd", 4096, NULL, 4, NULL);

    msc_init();

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &s_dev_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = s_cfg_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));

    const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = cdc_rx_cb};
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
    ESP_LOGI(TAG, "USB composite up (%04x:%04x)", USB_VID, USB_PID);
}
