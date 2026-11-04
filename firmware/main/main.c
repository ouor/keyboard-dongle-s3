// App entry: BLE HID host (Bluedroid) -> keymap.c -> USB (usb.c).
// Scan/connect flow is from IDF examples/bluetooth/esp_hid_host, trimmed to BLE only (ESP32-S3 has no BT Classic).
#include <stdio.h>
#include <string.h>

#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_gattc_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keymap.h"
#include "keymap_nvs.h"
#include "nvs_flash.h"
#include "usb.h"

static const char *TAG = "dongle";

#define SCAN_SECONDS 5

// "rpt <rid> <hex>" on CDC: what the tools record/replay and what `map` takes.
static void print_report(uint8_t rid, const uint8_t *data, size_t len)
{
    char line[16 + 64 * 2];
    int n = snprintf(line, sizeof(line), "rpt %u ", rid);
    for (size_t i = 0; i < len && n < (int)sizeof(line) - 3; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%02x", data[i]);
    }
    hw_print(line);
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
        break;
    default:
        ESP_LOGI(TAG, "EVENT: %d", (int)id);
        break;
    }
}

// shortcut: scans once at boot and opens the last BLE HID device found; no device choice or reconnect yet.
bool hw_cmd(int argc, char **argv)
{
    (void)argc, (void)argv;
    return false;
}

static void connect_task(void *arg)
{
    size_t count = 0;
    esp_hid_scan_result_t *results = NULL;
    ESP_LOGI(TAG, "SCAN %ds...", SCAN_SECONDS);
    esp_hid_scan(SCAN_SECONDS, &count, &results);
    ESP_LOGI(TAG, "SCAN: %u results", count);

    esp_hid_scan_result_t *pick = NULL;
    for (esp_hid_scan_result_t *r = results; r; r = r->next) {
        printf("  BLE " ESP_BD_ADDR_STR ", RSSI %d, USAGE %s, APPEARANCE 0x%04x, ADDR_TYPE %s, NAME %s\n",
               ESP_BD_ADDR_HEX(r->bda), r->rssi, esp_hid_usage_str(r->usage), r->ble.appearance,
               ble_addr_type_str(r->ble.addr_type), r->name ? r->name : "");
        if (r->transport == ESP_HID_TRANSPORT_BLE) {
            pick = r;
        }
    }
    if (pick) {
        esp_hidh_dev_open(pick->bda, pick->transport, pick->ble.addr_type);
    }
    esp_hid_scan_results_free(results);
    vTaskDelete(NULL);
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
    usb_init();

    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
    const esp_hidh_config_t config = {.callback = hidh_callback, .event_stack_size = 4096};
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    const uint8_t *own = esp_bt_dev_get_address();
    ESP_LOGI(TAG, "own address " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(own));
    xTaskCreate(connect_task, "connect", 6 * 1024, NULL, 2, NULL);
}
