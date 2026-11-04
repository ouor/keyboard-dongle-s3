// Host test for keymap.c: dev.ps1 test (clang). No ESP-IDF needed.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/keycodes.h"
#include "../main/keymap.h"

static char last[128];
static int presses, releases, saves;
static uint16_t last_usage;
static uint8_t last_page;

void hw_print(const char *line) { snprintf(last, sizeof(last), "%s", line); }
void hw_key(uint8_t page, uint16_t usage, bool pressed)
{
    last_page = page, last_usage = usage;
    pressed ? presses++ : releases++;
}
bool hw_save(const km_entry_t *e, int n) { (void)e; saves = n; return true; }
void hw_dfu(void) {}
static uint8_t kbd_mods, kbd_keys[6];
static int kbd_reports;
void hw_kbd_report(uint8_t mods, const uint8_t keys[6])
{
    kbd_mods = mods, memcpy(kbd_keys, keys, 6), kbd_reports++;
}
static int hw_cmds;
bool hw_cmd(int argc, char **argv) { (void)argc; return !strcmp(argv[0], "status") && ++hw_cmds; }

static void cmd(const char *s)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    km_cmd(buf);
}

int main(void)
{
    cmd("map 3 2302 AppHome");  // consumer report from a remote: rid 3, usage 0x0223 LE
    assert(!strcmp(last, "ok 0"));
    cmd("map 3 2402 appback");   // case-insensitive key names
    assert(!strcmp(last, "ok 1"));
    cmd("map 3 2302 VolumeUp");  // same report -> overwrite, not duplicate
    assert(!strcmp(last, "ok 0"));
    int n;
    km_entries(&n);
    assert(n == 2);

    // press -> repeat -> release
    km_on_report(3, (const uint8_t[]){0x23, 0x02}, 2, KM_SRC_OTHER);
    assert(presses == 1 && last_page == KC_PAGE_CONSUMER && last_usage == 0xE9);
    km_on_report(3, (const uint8_t[]){0x23, 0x02}, 2, KM_SRC_OTHER);
    assert(presses == 1);
    km_on_report(3, (const uint8_t[]){0x00, 0x00}, 2, KM_SRC_OTHER);
    assert(releases == 1);

    // switching directly between buttons releases the first
    cmd("inject 3 2302");
    cmd("inject 3 24:02");
    assert(presses == 3 && releases == 2 && last_usage == 0x224);
    cmd("clear");  // clearing while held must release
    assert(releases == 3 && !strcmp(last, "ok"));

    // bad input
    cmd("map 3 zz A");
    assert(!strncmp(last, "err", 3));
    cmd("map 3 123 A");  // odd nibble count
    assert(!strncmp(last, "err", 3));
    cmd("map 256 01 A");
    assert(!strncmp(last, "err", 3));
    cmd("map 1 01 NoSuchKey");
    assert(!strncmp(last, "err", 3));
    cmd("map 1 010203040506070809 A");  // longer than KM_MAX_DATA
    assert(!strncmp(last, "err", 3));
    cmd("unmap 5");
    assert(!strncmp(last, "err", 3));
    cmd("bogus");
    assert(!strncmp(last, "err", 3));

    cmd("map 1 0000040000000000 Enter");
    cmd("unmap 0");
    km_entries(&n);
    assert(n == 0 && !strcmp(last, "ok"));
    cmd("map 1 01 A");
    cmd("save");
    assert(saves == 1);

    // unmapped reports do nothing
    int p = presses;
    km_on_report(9, (const uint8_t[]){0x01}, 1, KM_SRC_OTHER);
    assert(presses == p);

    // platform commands go to hw_cmd, unknown ones still error
    cmd("status");
    assert(hw_cmds == 1);

    // unmapped keyboard report passes through, mapped one wins and clears the passthrough
    cmd("clear");
    cmd("map 1 0000050000000000 F13");  // 'B' on a BLE keyboard -> F13
    km_on_report(1, (const uint8_t[]){0x02, 0, 0x04, 0, 0, 0, 0, 0}, 8, KM_SRC_KEYBOARD);  // Shift+A
    assert(kbd_reports == 1 && kbd_mods == 0x02 && kbd_keys[0] == 0x04);
    p = presses;
    km_on_report(1, (const uint8_t[]){0, 0, 0x05, 0, 0, 0, 0, 0}, 8, KM_SRC_KEYBOARD);
    assert(kbd_reports == 2 && kbd_mods == 0 && kbd_keys[0] == 0 && presses == p + 1 && last_usage == 0x68);
    km_on_report(1, (const uint8_t[]){0, 0, 0, 0, 0, 0, 0, 0}, 8, KM_SRC_KEYBOARD);
    assert(releases == 4 && kbd_reports == 3);
    // keyboard layout only for keyboard reports
    km_on_report(1, (const uint8_t[]){0, 0, 0x04, 0, 0, 0, 0, 0}, 8, KM_SRC_OTHER);
    assert(kbd_reports == 3);

    // unmapped consumer report passes through as its usage; release-all lets it go
    km_on_report(3, (const uint8_t[]){0xE9, 0x00}, 2, KM_SRC_CONSUMER);
    assert(last_page == KC_PAGE_CONSUMER && last_usage == 0xE9 && presses == p + 2);
    km_release_all();
    assert(releases == 5);
    km_release_all();  // nothing held -> nothing sent
    assert(releases == 5);
    cmd("inject 3 e900 cc");
    assert(presses == p + 3);
    cmd("inject 3 0000 cc");
    assert(releases == 6);
    cmd("inject 3 0000 xx");
    assert(!strncmp(last, "err", 3));

    puts("keymap tests ok");
    return 0;
}
