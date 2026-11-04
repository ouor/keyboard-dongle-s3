// Host test for hid_parse.c + keymap.c: dev.ps1 test (clang). No ESP-IDF needed.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/keycodes.h"
#include "../main/keymap.h"

static char last[128], last_key_line[128];
static int presses, releases, saves, hw_cmds;
static uint16_t last_usage;
static uint8_t last_page;

void hw_print(const char *line)
{
    snprintf(last, sizeof(last), "%s", line);
    if (!strncmp(line, "key ", 4)) {
        snprintf(last_key_line, sizeof(last_key_line), "%s", line);
    }
}
void hw_key(uint8_t page, uint16_t usage, bool pressed)
{
    last_page = page, last_usage = usage;
    pressed ? presses++ : releases++;
}
bool hw_save(const km_entry_t *e, int n) { (void)e; saves = n; return true; }
void hw_dfu(void) {}
bool hw_cmd(int argc, char **argv) { (void)argc; return !strcmp(argv[0], "status") && ++hw_cmds; }

static void cmd(const char *s)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    km_cmd(buf);
}

// Shaped like the G20BTS remote: rid 1 consumer (16-bit array + pad = 3 bytes), rid 11 keyboard, rid 3 mouse.
static const uint8_t G20[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x01, 0xC0,
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x0B, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0,
};
// Mouse without report ID: 5 buttons + pad, 12-bit X/Y.
static const uint8_t MOUSE12[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};
// Consumer bitmap: one bit per key (VolUp, VolDown, Mute, Play, Next, Prev, Home, Back).
static const uint8_t CC_BITS[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x09, 0xE9,
    0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x0A, 0x23, 0x02, 0x0A, 0x24, 0x02, 0x81, 0x02, 0xC0,
};

// Vendor report like the G20BTS rid 2 (page ff01, 20 bytes, probably voice): never a key.
static const uint8_t VENDOR[] = {0x06, 0x01, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x05, 0x09, 0x14, 0x15, 0x00, 0x26, 0xFF, 0x00,
                                 0x75, 0x08, 0x95, 0x14, 0x81, 0x02, 0xC0};

static hp_map_t maps[4];

#define REPORT(rid, ...) km_on_report(rid, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

int main(void)
{
    assert(hp_parse(G20, sizeof(G20), &maps[0]));
    assert(hp_parse(MOUSE12, sizeof(MOUSE12), &maps[1]));
    assert(hp_parse(CC_BITS, sizeof(CC_BITS), &maps[2]));
    assert(hp_parse(VENDOR, sizeof(VENDOR), &maps[3]));
    assert(!hp_parse(G20, 5, &(hp_map_t){0}));  // truncated item
    km_set_maps(maps, 4);

    // nothing mapped: keys are reported to the config tool but nothing reaches USB
    assert(REPORT(1, 0x24, 0x02, 0x00));  // consumer array, 3-byte report
    assert(!strcmp(last_key_line, "key 0c:0224 1 AppBack") && presses == 0);
    assert(!REPORT(1, 0x24, 0x02, 0x00));  // repeat: no new event
    assert(REPORT(1, 0x00, 0x00, 0x00));
    assert(!strcmp(last_key_line, "key 0c:0224 0 AppBack") && releases == 0);
    REPORT(11, 0x02, 0, 0x04, 0, 0, 0, 0, 0);  // keyboard: Left Shift (E1) + A
    assert(!strcmp(last_key_line, "key 07:0004 1 A") && presses == 0);
    REPORT(11, 0, 0, 0, 0, 0, 0, 0, 0);
    assert(REPORT(3, 0x01, 0x00, 0x00, 0x00));  // OK = mouse button 1
    assert(!strcmp(last_key_line, "key 09:0001 1 Button"));
    assert(!REPORT(3, 0x01, 0x05, 0xFE, 0x00));  // motion is never a key
    assert(REPORT(3, 0x00, 0x00, 0x00, 0x00));
    assert(!REPORT(0, 0x00, 0xD4, 0x5E, 0x00));  // 12-bit mouse motion
    assert(!REPORT(5, 0x12, 0x34, 0x56, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78));  // vendor data
    assert(presses == 0 && releases == 0);

    // mapped keys go out, by usage, whatever report layout carries them
    cmd("map c:224 AppHome");
    assert(!strcmp(last, "ok 0"));
    REPORT(1, 0x24, 0x02, 0x00);
    assert(presses == 1 && last_page == KC_PAGE_CONSUMER && last_usage == 0x223);
    REPORT(1, 0x00, 0x00, 0x00);
    assert(releases == 1);
    cmd("map 7:50 F13");
    REPORT(11, 0, 0, 0x50, 0x04, 0, 0, 0, 0);  // mapped arrow + unmapped A: only F13
    assert(presses == 2 && last_usage == 0x68);
    REPORT(11, 0, 0, 0, 0, 0, 0, 0, 0);
    assert(releases == 2);
    cmd("map 9:1 Enter");  // mapped click works while the cursor moves
    REPORT(3, 0x01, 0x04, 0x00, 0x00);
    assert(presses == 3 && last_usage == 0x28);
    assert(!REPORT(3, 0x01, 0x09, 0x00, 0x00));
    REPORT(3, 0x00, 0x00, 0x00, 0x00);
    assert(releases == 3);

    // consumer bitmap: two mapped keys at once (chord), by key name
    cmd("map VolumeUp F14");
    cmd("map AppHome Esc");
    assert(!strncmp(last, "ok", 2));
    REPORT(2, 0x41);  // VolumeUp + Home
    assert(presses == 5);
    REPORT(2, 0x40);  // VolumeUp released, Home still down
    assert(releases == 4);
    km_release_all();  // disconnect releases Home
    assert(releases == 5 && last_usage == 0x29);
    km_release_all();
    assert(releases == 5);

    // unknown report id / no device: ignored
    assert(!REPORT(9, 0x01));
    km_set_maps(NULL, 0);
    assert(!REPORT(1, 0x24, 0x02, 0x00));
    km_set_maps(maps, 4);
    cmd("inject 1 240200");
    assert(!strcmp(last, "ok") && last_usage == 0x223);
    cmd("inject 1 000000");

    // commands
    cmd("list");
    assert(!strcmp(last, "ok 5"));
    int n;
    km_entries(&n);
    assert(n == 5);
    cmd("map 9:1 Space");  // same input -> overwrite, not duplicate
    km_entries(&n);
    assert(n == 5 && !strcmp(last, "ok 2"));
    cmd("hidmap");
    assert(!strncmp(last, "ok ", 3));
    cmd("save");
    assert(saves == 5);
    cmd("unmap 0");
    km_entries(&n);
    assert(n == 4 && !strcmp(last, "ok"));
    cmd("status");
    assert(hw_cmds == 1);
    const char *bad[] = {"map", "map 7:50", "map 7:50 NoSuchKey", "map zz:1 A", "map 7: A", "map NoSuchKey A",
                         "unmap 5", "unmap x", "inject 1 zz", "inject 300 00", "bogus"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cmd(bad[i]);
        assert(!strncmp(last, "err", 3));
    }
    cmd("clear");
    km_entries(&n);
    assert(n == 0);

    puts("keymap tests ok");
    return 0;
}
