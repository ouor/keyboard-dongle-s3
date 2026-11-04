// BLE report -> USB key mapping and the CDC line protocol. Pure C (no ESP-IDF) so it runs in host tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KM_MAX_ENTRIES 64
#define KM_MAX_DATA 8

// One remote button: the exact BLE input report it sends -> one USB usage.
typedef struct {
    uint8_t rid;
    uint8_t len;
    uint8_t data[KM_MAX_DATA];
    uint8_t page;   // KC_PAGE_*
    uint16_t usage;
} km_entry_t;

void km_load(const km_entry_t *entries, int count);
const km_entry_t *km_entries(int *count);

// What the BLE device's report map says a report is. Unmapped KBD/CONSUMER reports pass through as-is.
typedef enum { KM_SRC_OTHER, KM_SRC_KEYBOARD, KM_SRC_CONSUMER } km_src_t;

// Feed one BLE input report (also used by the `inject` command).
void km_on_report(uint8_t rid, const uint8_t *data, size_t len, km_src_t src);
// Release everything held (BLE device disconnected).
void km_release_all(void);

// Execute one protocol line (modified in place). Replies go through hw_print().
void km_cmd(char *line);

// Implemented by the platform (firmware: usb.c/main, host tests: stubs).
void hw_print(const char *line);
void hw_key(uint8_t page, uint16_t usage, bool pressed);
void hw_kbd_report(uint8_t mods, const uint8_t keys[6]);  // whole boot-keyboard report (passthrough)
// Platform commands (BLE scan/pair...). Return false if `argv[0]` is not one of them.
bool hw_cmd(int argc, char **argv);
bool hw_save(const km_entry_t *entries, int count);
void hw_dfu(void);
