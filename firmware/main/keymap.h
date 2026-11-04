// BLE input -> USB output: keys are HID usages (page:usage) decoded with the device's report map (hid_parse.h),
// so mapping doesn't depend on how a device lays out its reports. Only mapped keys reach USB. Also the CDC protocol.
// Pure C (no ESP-IDF) so it runs in host tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_parse.h"

#define KM_MAX_ENTRIES 64
#define KM_MAX_MAPS 4  // report maps per BLE device

// One remote key (HID usage it reports) -> one USB usage (KC_PAGE_* / usage from keycodes.h).
typedef struct {
    uint16_t in_page, in_usage;
    uint16_t usage;
    uint8_t page;
} km_entry_t;

void km_load(const km_entry_t *entries, int count);
const km_entry_t *km_entries(int *count);

// Report maps of the connected device (n = 0: none). Releases anything held.
void km_set_maps(const hp_map_t *maps, int n);
// Feed one BLE input report (also used by `inject`). Prints "key <page>:<usage> <1|0> [name]" per change.
// Returns true if any key went down or up (mouse motion is not a key).
bool km_on_report(uint8_t rid, const uint8_t *data, size_t len);
// Release everything held (BLE device disconnected).
void km_release_all(void);

// Execute one protocol line (modified in place). Replies go through hw_print().
void km_cmd(char *line);

// Implemented by the platform (firmware: usb.c/main, host tests: stubs).
void hw_print(const char *line);
void hw_key(uint8_t page, uint16_t usage, bool pressed);
bool hw_save(const km_entry_t *entries, int count);
void hw_dfu(void);
// Platform commands (BLE scan/pair...). Return false if `argv[0]` is not one of them.
bool hw_cmd(int argc, char **argv);
