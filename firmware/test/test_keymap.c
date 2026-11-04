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
    km_on_report(3, (const uint8_t[]){0x23, 0x02}, 2);
    assert(presses == 1 && last_page == KC_PAGE_CONSUMER && last_usage == 0xE9);
    km_on_report(3, (const uint8_t[]){0x23, 0x02}, 2);
    assert(presses == 1);
    km_on_report(3, (const uint8_t[]){0x00, 0x00}, 2);
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
    km_on_report(9, (const uint8_t[]){0x01}, 1);
    assert(presses == p);

    puts("keymap tests ok");
    return 0;
}
