// HID report descriptor (BLE "Report Map") -> input fields, and input report -> pressed usages (axes are ignored).
// Pure C (no ESP-IDF) so it runs in host tests. Makes mapping independent of each device's layout.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HP_PAGE_DESKTOP = 0x01, HP_PAGE_KEYBOARD = 0x07, HP_PAGE_BUTTON = 0x09, HP_PAGE_CONSUMER = 0x0C };

#define HP_MAX_FIELDS 48
#define HP_MAX_KEYS 16  // pressed usages reported per input report

typedef struct {
    uint8_t rid;
    bool array;        // array: each element holds an index into umin..; variable: element i is usage umin + i
    bool relative;
    uint16_t page;
    uint16_t umin;
    uint16_t bit;      // offset in the report (after the report ID byte)
    uint8_t size;      // bits per element
    uint8_t count;
    int32_t lmin;
} hp_field_t;

typedef struct {
    hp_field_t f[HP_MAX_FIELDS];
    int n;
} hp_map_t;

typedef struct {
    uint16_t page, usage;
} hp_key_t;

typedef struct {
    hp_key_t keys[HP_MAX_KEYS];  // usages currently down in this report
    int nkeys;
} hp_input_t;

// Parses a report descriptor into `out` (appends). Returns false on a malformed descriptor or too many fields.
bool hp_parse(const uint8_t *desc, size_t len, hp_map_t *out);
// True if `map` describes input report `rid`.
bool hp_has_report(const hp_map_t *map, uint8_t rid);
// Decodes one input report. Unknown rid -> nothing pressed.
void hp_extract(const hp_map_t *map, uint8_t rid, const uint8_t *data, size_t len, hp_input_t *out);
