#include "keymap.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keycodes.h"
#include "version.h"

static km_entry_t s_map[KM_MAX_ENTRIES];
static int s_count;
// shortcut: only one mapped button is held at a time (fine for remotes), track per-rid when chords are needed.
static const km_entry_t *s_held;
static bool s_pass_kbd, s_pass_cc;  // a passthrough key is currently down

static void release_held(void)
{
    if (s_held) {
        hw_key(s_held->page, s_held->usage, false);
        s_held = NULL;
    }
}

void km_load(const km_entry_t *entries, int count)
{
    release_held();
    s_count = count < 0 ? 0 : count > KM_MAX_ENTRIES ? KM_MAX_ENTRIES : count;
    memcpy(s_map, entries, sizeof(km_entry_t) * s_count);
}

const km_entry_t *km_entries(int *count)
{
    *count = s_count;
    return s_map;
}

static const km_entry_t *match(uint8_t rid, const uint8_t *data, size_t len)
{
    for (int i = 0; i < s_count; i++) {
        if (s_map[i].rid == rid && s_map[i].len == len && memcmp(s_map[i].data, data, len) == 0) {
            return &s_map[i];
        }
    }
    return NULL;
}

static void release_pass(void)
{
    if (s_pass_kbd) {
        hw_kbd_report(0, (const uint8_t[6]){0});
        s_pass_kbd = false;
    }
    if (s_pass_cc) {
        hw_key(KC_PAGE_CONSUMER, 0, false);
        s_pass_cc = false;
    }
}

void km_release_all(void)
{
    release_held();
    release_pass();
}

static bool all_zero(const uint8_t *d, size_t n)
{
    while (n && !d[n - 1]) {
        n--;
    }
    return n == 0;
}

void km_on_report(uint8_t rid, const uint8_t *data, size_t len, km_src_t src)
{
    const km_entry_t *e = len <= KM_MAX_DATA ? match(rid, data, len) : NULL;
    if (e) {
        release_pass();
        if (e != s_held) {  // same entry again = key repeat
            release_held();
            hw_key(e->page, e->usage, true);
            s_held = e;
        }
        return;
    }
    release_held();  // any other report (usually all-zero) means the button was released
    // shortcut: passthrough only knows the boot keyboard layout (mods, 0, 6 keys) and one 16-bit consumer usage;
    // parse the report map if a device sends something else that should pass through.
    if (src == KM_SRC_KEYBOARD && len == 8) {
        hw_kbd_report(data[0], data + 2);
        s_pass_kbd = !all_zero(data, len);
    } else if (src == KM_SRC_CONSUMER && len == 2) {
        uint16_t u = (uint16_t)(data[0] | data[1] << 8);
        if (u || s_pass_cc) {
            hw_key(KC_PAGE_CONSUMER, u, u != 0);
        }
        s_pass_cc = u != 0;
    }
}

static const keycode_t *kc_by_name(const char *name)
{
    for (size_t i = 0; i < KEYCODES_COUNT; i++) {
        const char *a = KEYCODES[i].name, *b = name;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++, b++;
        }
        if (!*a && !*b) {
            return &KEYCODES[i];
        }
    }
    return NULL;
}

static const char *kc_name(uint8_t page, uint16_t usage)
{
    for (size_t i = 0; i < KEYCODES_COUNT; i++) {
        if (KEYCODES[i].page == page && KEYCODES[i].usage == usage) {
            return KEYCODES[i].name;
        }
    }
    return "?";
}

// "0a1b" or "0A 1B" style -> bytes. Returns length, or -1 on bad input.
static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0, nib = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == ':') {
            continue;
        }
        if (!isxdigit((unsigned char)*s)) {
            return -1;
        }
        int v = isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10;
        if (nib < 0) {
            nib = v;
        } else {
            if (n >= max) {
                return -1;
            }
            out[n++] = (uint8_t)(nib << 4 | v);
            nib = -1;
        }
    }
    return nib < 0 ? n : -1;
}

static void to_hex(const uint8_t *d, int len, char *out)
{
    for (int i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", d[i]);
    }
    out[len * 2] = 0;
}

static void printf_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void printf_line(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    hw_print(buf);
}

static int parse_rid(const char *s)
{
    char *end;
    long v = s ? strtol(s, &end, 0) : -1;
    return (s && *end == 0 && v >= 0 && v <= 255) ? (int)v : -1;
}

void km_cmd(char *line)
{
    char *argv[4] = {0};
    int argc = 0;
    for (char *t = strtok(line, " \t\r\n"); t && argc < 4; t = strtok(NULL, " \t\r\n")) {
        argv[argc++] = t;
    }
    if (argc == 0) {
        return;
    }
    const char *c = argv[0];
    uint8_t data[KM_MAX_DATA];

    if (!strcmp(c, "help")) {
        hw_print("ok cmds: ver | keys | list | map <rid> <hex> <key> | unmap <i> | clear | save | inject <rid> <hex> [kbd|cc]"
                 " | status | scan [sec] | pair <addr> <addr_type> | forget | dfu");
    } else if (!strcmp(c, "ver")) {
        hw_print("ok " FW_VERSION);
    } else if (!strcmp(c, "keys")) {
        for (size_t i = 0; i < KEYCODES_COUNT; i++) {
            printf_line("key %s %u 0x%02X", KEYCODES[i].name, KEYCODES[i].page, KEYCODES[i].usage);
        }
        hw_print("ok");
    } else if (!strcmp(c, "list")) {
        char hex[KM_MAX_DATA * 2 + 1];
        for (int i = 0; i < s_count; i++) {
            to_hex(s_map[i].data, s_map[i].len, hex);
            printf_line("map %d %u %s %s", i, s_map[i].rid, hex, kc_name(s_map[i].page, s_map[i].usage));
        }
        printf_line("ok %d", s_count);
    } else if (!strcmp(c, "map")) {
        int rid = parse_rid(argv[1]);
        int len = argv[2] ? parse_hex(argv[2], data, KM_MAX_DATA) : -1;
        const keycode_t *k = argv[3] ? kc_by_name(argv[3]) : NULL;
        if (rid < 0 || len <= 0 || !k) {
            hw_print("err usage: map <rid> <hex> <key>");
            return;
        }
        km_entry_t *e = (km_entry_t *)match((uint8_t)rid, data, (size_t)len);
        if (!e) {
            if (s_count >= KM_MAX_ENTRIES) {
                hw_print("err full");
                return;
            }
            e = &s_map[s_count++];
        }
        release_held();
        *e = (km_entry_t){.rid = (uint8_t)rid, .len = (uint8_t)len, .page = k->page, .usage = k->usage};
        memcpy(e->data, data, (size_t)len);
        printf_line("ok %d", (int)(e - s_map));
    } else if (!strcmp(c, "unmap")) {
        int i = argv[1] ? atoi(argv[1]) : -1;
        if (i < 0 || i >= s_count || (argv[1][0] < '0' || argv[1][0] > '9')) {
            hw_print("err bad index");
            return;
        }
        release_held();
        memmove(&s_map[i], &s_map[i + 1], sizeof(km_entry_t) * (size_t)(s_count - i - 1));
        s_count--;
        hw_print("ok");
    } else if (!strcmp(c, "clear")) {
        release_held();
        s_count = 0;
        hw_print("ok");
    } else if (!strcmp(c, "save")) {
        hw_print(hw_save(s_map, s_count) ? "ok" : "err save failed");
    } else if (!strcmp(c, "inject")) {
        int rid = parse_rid(argv[1]);
        int len = argv[2] ? parse_hex(argv[2], data, KM_MAX_DATA) : -1;
        km_src_t src = !argv[3] ? KM_SRC_OTHER : !strcmp(argv[3], "kbd") ? KM_SRC_KEYBOARD : !strcmp(argv[3], "cc") ? KM_SRC_CONSUMER : (km_src_t)-1;
        if (rid < 0 || len < 0 || (int)src < 0) {
            hw_print("err usage: inject <rid> <hex> [kbd|cc]");
            return;
        }
        km_on_report((uint8_t)rid, data, (size_t)len, src);
        hw_print("ok");
    } else if (!strcmp(c, "dfu")) {
        hw_print("ok rebooting to download mode");
        hw_dfu();
    } else if (!hw_cmd(argc, argv)) {
        hw_print("err unknown command (try help)");
    }
}
