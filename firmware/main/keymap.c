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

void km_on_report(uint8_t rid, const uint8_t *data, size_t len)
{
    const km_entry_t *e = len <= KM_MAX_DATA ? match(rid, data, len) : NULL;
    if (e == s_held) {
        return;  // key repeat / same state
    }
    release_held();  // any other report (usually all-zero) means the button was released
    if (e) {
        hw_key(e->page, e->usage, true);
        s_held = e;
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
        hw_print("ok cmds: ver | keys | list | map <rid> <hex> <key> | unmap <i> | clear | save | inject <rid> <hex> | dfu");
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
        if (rid < 0 || len < 0) {
            hw_print("err usage: inject <rid> <hex>");
            return;
        }
        km_on_report((uint8_t)rid, data, (size_t)len);
        hw_print("ok");
    } else if (!strcmp(c, "dfu")) {
        hw_print("ok rebooting to download mode");
        hw_dfu();
    } else {
        hw_print("err unknown command (try help)");
    }
}
