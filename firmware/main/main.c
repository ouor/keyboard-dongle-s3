// App entry: BLE HID host (Bluedroid) -> keymap.c -> USB (usb.c).
// Scan/connect calls are from IDF examples/bluetooth/esp_hid_host, trimmed to BLE only (ESP32-S3 has no BT Classic).
// One saved BLE device is kept connected; the CDC commands status/scan/pair/forget (hw_cmd) and K1 change it.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keymap.h"
#include "keymap_nvs.h"
#include "lcd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "usb.h"

static const char *TAG = "dongle";

#define SCAN_SECONDS 5

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    hw_print(buf);
}

// "rpt <rid> <hex>" on CDC: what the tools record/replay and what `map` takes.
static void print_report(uint8_t rid, const uint8_t *data, size_t len)
{
    char hex[64 * 2 + 1] = "";
    for (size_t i = 0; i < len && i < 64; i++) {
        sprintf(hex + i * 2, "%02x", data[i]);
    }
    line("rpt %u %s", rid, hex);
}

// ---- the saved device ----
// shortcut: reconnects to the address seen at pairing; a device that rotates resolvable private addresses
// would need its identity address from the bond (ESP_GAP_BLE_AUTH_CMPL_EVT) instead.
typedef struct {
    esp_bd_addr_t bda;  // all zero = no device
    uint8_t addr_type;
    char name[32];
} saved_dev_t;

static saved_dev_t s_dev;
static esp_hidh_dev_t *volatile s_conn;  // open connection or NULL
static TaskHandle_t s_ble_task;
static volatile int s_scan_secs;   // >0: scan requested
static volatile bool s_scan_pair;  // K1: pair the strongest result after the scan

static bool have_dev(void)
{
    static const esp_bd_addr_t none;
    return memcmp(s_dev.bda, none, sizeof(none)) != 0;
}

static void dev_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "dev", &s_dev, sizeof(s_dev));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void dev_load(void)
{
    nvs_handle_t h;
    size_t size = sizeof(s_dev);
    if (nvs_open("ble", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "dev", &s_dev, &size) != ESP_OK || size != sizeof(s_dev)) {
            memset(&s_dev, 0, sizeof(s_dev));
        }
        nvs_close(h);
    }
    s_dev.name[sizeof(s_dev.name) - 1] = 0;
}

// `status` reply and the async `evt` line: <prefix> none|connecting|connected <addr> <name>
static void print_state(const char *prefix)
{
    const char *st = !have_dev() ? "none" : s_conn ? "connected" : "connecting";
    line("%s %s " ESP_BD_ADDR_STR " %s", prefix, st, ESP_BD_ADDR_HEX(s_dev.bda), s_dev.name);
}

// Device state changed: tell the config tool (`evt ...`) and the LCD.
static void state_changed(void)
{
    char addr[18];
    snprintf(addr, sizeof(addr), ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(s_dev.bda));
    print_state("evt");
    if (!have_dev()) {
        lcd_show("No device", LCD_GRAY, "Press K1 to", "pair or use DongleConfig");
    } else {
        lcd_show(s_conn ? "Connected" : "Searching..", s_conn ? LCD_GREEN : LCD_YELLOW, s_dev.name, addr);
    }
}

static void bonds_clear(void)
{
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_ble_bond_dev_t *list = calloc((size_t)n, sizeof(*list));
    if (list && esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            esp_ble_remove_bond_device(list[i].bd_addr);
        }
    }
    free(list);
}

// Switch to a new device (bda == NULL: none). Drops the old connection and bonds; ble_task connects.
static void dev_set(const uint8_t *bda, uint8_t addr_type)
{
    memset(&s_dev, 0, sizeof(s_dev));
    if (bda) {
        memcpy(s_dev.bda, bda, sizeof(s_dev.bda));
        s_dev.addr_type = addr_type;
    }
    dev_save();
    esp_hidh_dev_t *c = s_conn;
    if (c) {
        esp_hidh_dev_close(c);  // its CLOSE event clears s_conn
    }
    bonds_clear();  // single-device dongle: the old remote must pair again to come back
    state_changed();
    xTaskNotifyGive(s_ble_task);
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);
        } else {
            ESP_LOGE(TAG, "OPEN failed");
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "BATTERY: %d%%", param->battery.level);
        break;
    case ESP_HIDH_INPUT_EVENT:
        ESP_LOGI(TAG, "INPUT: %8s, MAP: %2u, ID: %3u, Len: %d", esp_hid_usage_str(param->input.usage),
                 param->input.map_index, param->input.report_id, param->input.length);
        ESP_LOG_BUFFER_HEX(TAG, param->input.data, param->input.length);
        print_report(param->input.report_id, param->input.data, param->input.length);
        km_on_report(param->input.report_id, param->input.data, param->input.length,
                     param->input.usage == ESP_HID_USAGE_KEYBOARD ? KM_SRC_KEYBOARD
                     : param->input.usage == ESP_HID_USAGE_CCONTROL ? KM_SRC_CONSUMER
                                                                    : KM_SRC_OTHER);
        break;
    case ESP_HIDH_FEATURE_EVENT:
        ESP_LOGI(TAG, "FEATURE: ID %u, Len %d", param->feature.report_id, param->feature.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "CLOSE: %s", esp_hidh_dev_name_get(param->close.dev));
        if (param->close.dev == s_conn) {
            s_conn = NULL;
            km_release_all();
            state_changed();
            xTaskNotifyGive(s_ble_task);
        }
        break;
    default:
        ESP_LOGI(TAG, "EVENT: %d", (int)id);
        break;
    }
}

static void do_scan(void)
{
    int secs = s_scan_secs;
    bool pair = s_scan_pair;
    s_scan_secs = 0, s_scan_pair = false;
    line("evt scan %d", secs);
    lcd_show("Scanning..", LCD_CYAN, pair ? "pairing the" : "", pair ? "nearest device" : "");
    size_t count = 0;
    esp_hid_scan_result_t *results = NULL, *best = NULL;
    esp_hid_scan((uint32_t)secs, &count, &results);
    for (esp_hid_scan_result_t *r = results; r; r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BLE) {
            continue;
        }
        line("dev " ESP_BD_ADDR_STR " %u %d %s", ESP_BD_ADDR_HEX(r->bda), r->ble.addr_type, r->rssi, r->name ? r->name : "");
        if (!best || r->rssi > best->rssi) {
            best = r;
        }
    }
    line("evt scan end %u", (unsigned)count);
    if (pair && best) {
        dev_set(best->bda, best->ble.addr_type);
    } else {
        state_changed();  // back from the "Scanning" screen
    }
    esp_hid_scan_results_free(results);
}

// Owns all blocking BLE calls: scans, and (re)connects to the saved device.
static void ble_task(void *arg)
{
    for (;;) {
        if (s_scan_secs) {
            do_scan();
            continue;
        }
        if (!s_conn && have_dev()) {
            saved_dev_t want = s_dev;
            // blocks up to BT_BLE_ESTAB_LINK_CONN_TOUT, so a scan request waits at most that long
            esp_hidh_dev_t *c = esp_hidh_dev_open(want.bda, ESP_HID_TRANSPORT_BLE, want.addr_type);
            if (c && memcmp(want.bda, s_dev.bda, sizeof(want.bda))) {
                esp_hidh_dev_close(c);  // device changed while we were connecting
                c = NULL;
            }
            if (c) {
                const char *name = esp_hidh_dev_name_get(c);
                s_conn = c;
                if (name && strncmp(name, s_dev.name, sizeof(s_dev.name) - 1)) {
                    snprintf(s_dev.name, sizeof(s_dev.name), "%s", name);
                    dev_save();
                }
                state_changed();
            } else {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));  // asleep / out of range: retry
            }
            continue;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// K1 (GPIO0, BOOT): scan and pair the strongest device in pairing mode - pairing without a PC.
static void button_task(void *arg)
{
    const gpio_config_t io = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    int prev = 1;
    for (;;) {
        int v = gpio_get_level(GPIO_NUM_0);
        if (!v && prev) {
            s_scan_pair = true;
            s_scan_secs = SCAN_SECONDS;
            xTaskNotifyGive(s_ble_task);
        }
        prev = v;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool parse_addr(const char *s, esp_bd_addr_t out)
{
    unsigned b[6];
    char tail;
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff) {
            return false;
        }
        out[i] = (uint8_t)b[i];
    }
    return true;
}

bool hw_cmd(int argc, char **argv)
{
    const char *c = argv[0];
    if (!strcmp(c, "status")) {
        print_state("ok");
    } else if (!strcmp(c, "scan")) {
        int secs = argc > 1 ? atoi(argv[1]) : SCAN_SECONDS;
        if (secs < 1 || secs > 30) {
            hw_print("err usage: scan [1-30]");
            return true;
        }
        s_scan_pair = false;
        s_scan_secs = secs;
        xTaskNotifyGive(s_ble_task);
        hw_print("ok");  // then "evt scan <s>", "dev <addr> <addr_type> <rssi> <name>"..., "evt scan end <n>"
    } else if (!strcmp(c, "pair")) {
        esp_bd_addr_t bda;
        int type = argc > 2 ? atoi(argv[2]) : -1;
        if (!parse_addr(argv[1], bda) || type < 0 || type > 3) {
            hw_print("err usage: pair <aa:bb:cc:dd:ee:ff> <addr_type 0-3>");
            return true;
        }
        dev_set(bda, (uint8_t)type);
        hw_print("ok");
    } else if (!strcmp(c, "forget")) {
        dev_set(NULL, 0);
        hw_print("ok");
    } else {
        return false;
    }
    return true;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    keymap_nvs_load();
    dev_load();
    usb_init();  // HID is up before BLE: the stick is a keyboard right away
    lcd_init();
    state_changed();

    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
    const esp_hidh_config_t config = {.callback = hidh_callback, .event_stack_size = 4096};
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    const uint8_t *own = esp_bt_dev_get_address();
    ESP_LOGI(TAG, "own address " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(own));
    xTaskCreate(ble_task, "ble", 6 * 1024, NULL, 2, &s_ble_task);
    xTaskCreate(button_task, "k1", 2048, NULL, 1, NULL);
}
