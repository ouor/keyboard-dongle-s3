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
static const hp_map_t *s_maps;
static int s_nmaps;

// A key that is down right now, and what we sent for it.
enum { OUT_NONE, OUT_KEY, OUT_MOUSE };
typedef struct {
    uint8_t rid, out;
    hp_key_t in;
    uint8_t page;    // OUT_KEY: KC_PAGE_*
    uint16_t usage;  // OUT_KEY: usage, OUT_MOUSE: button bit
} held_t;
static held_t s_held[HP_MAX_KEYS * 2];
static int s_nheld;
static uint8_t s_buttons;  // mouse buttons we report

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

// HID page -> our output page, for the keys keycodes.json knows.
static int kc_page(uint16_t hid_page)
{
    return hid_page == HP_PAGE_KEYBOARD ? KC_PAGE_KEYBOARD : hid_page == HP_PAGE_CONSUMER ? KC_PAGE_CONSUMER : -1;
}

static const char *kc_name(int page, uint16_t usage)
{
    for (size_t i = 0; i < KEYCODES_COUNT; i++) {
        if (KEYCODES[i].page == page && KEYCODES[i].usage == usage) {
            return KEYCODES[i].name;
        }
    }
    return NULL;
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

static const char *in_name(hp_key_t k)
{
    const char *n = kc_name(kc_page(k.page), k.usage);
    return n ? n : k.page == HP_PAGE_BUTTON ? "Button" : "";
}

void km_load(const km_entry_t *entries, int count)
{
    km_release_all();
    s_count = count < 0 ? 0 : count > KM_MAX_ENTRIES ? KM_MAX_ENTRIES : count;
    memcpy(s_map, entries, sizeof(km_entry_t) * s_count);
}

const km_entry_t *km_entries(int *count)
{
    *count = s_count;
    return s_map;
}

static km_entry_t *match(hp_key_t k)
{
    for (int i = 0; i < s_count; i++) {
        if (s_map[i].in_page == k.page && s_map[i].in_usage == k.usage) {
            return &s_map[i];
        }
    }
    return NULL;
}

static void send_mouse(int32_t dx, int32_t dy, int32_t wheel)
{
#define CLAMP(v) (int8_t)((v) < -127 ? -127 : (v) > 127 ? 127 : (v))
    hw_mouse_report(s_buttons, CLAMP(dx), CLAMP(dy), CLAMP(wheel));  // shortcut: big 12/16-bit moves clamp to 127
}

static void release(held_t *h)
{
    if (h->out == OUT_KEY) {
        hw_key(h->page, h->usage, false);
    } else if (h->out == OUT_MOUSE) {
        s_buttons &= (uint8_t)~h->usage;
        send_mouse(0, 0, 0);
    }
}

void km_release_all(void)
{
    for (int i = 0; i < s_nheld; i++) {
        release(&s_held[i]);
    }
    s_nheld = 0;
}

void km_set_maps(const hp_map_t *maps, int n)
{
    km_release_all();
    s_maps = maps;
    s_nmaps = n;
}

// Mapped key -> its USB key. Unmapped: keyboard/consumer usages and mouse buttons pass through as they are.
static void press(uint8_t rid, hp_key_t k)
{
    if (s_nheld >= (int)(sizeof(s_held) / sizeof(s_held[0]))) {
        return;
    }
    held_t h = {.rid = rid, .in = k, .out = OUT_NONE};
    const km_entry_t *e = match(k);
    int kp = kc_page(k.page);
    if (e) {
        h.out = OUT_KEY, h.page = e->page, h.usage = e->usage;
    } else if (kp >= 0) {
        h.out = OUT_KEY, h.page = (uint8_t)kp, h.usage = k.usage;
    } else if (k.page == HP_PAGE_BUTTON && k.usage >= 1 && k.usage <= 8) {
        h.out = OUT_MOUSE, h.usage = (uint16_t)(1u << (k.usage - 1));
    }
    if (h.out == OUT_KEY) {
        hw_key(h.page, h.usage, true);
    } else if (h.out == OUT_MOUSE) {
        s_buttons |= (uint8_t)h.usage;
    }
    s_held[s_nheld++] = h;
}

bool km_on_report(uint8_t rid, const uint8_t *data, size_t len)
{
    const hp_map_t *m = NULL;
    for (int i = 0; i < s_nmaps && !m; i++) {
        m = hp_has_report(&s_maps[i], rid) ? &s_maps[i] : NULL;
    }
    if (!m) {
        return false;
    }
    hp_input_t in;
    hp_extract(m, rid, data, len, &in);
    bool changed = false;
    uint8_t buttons = s_buttons;

    for (int i = 0; i < s_nheld;) {  // released: held from this report, no longer in it
        held_t *h = &s_held[i];
        bool still = false;
        for (int k = 0; k < in.nkeys && !still; k++) {
            still = in.keys[k].page == h->in.page && in.keys[k].usage == h->in.usage;
        }
        if (h->rid != rid || still) {
            i++;
            continue;
        }
        printf_line("key %02x:%04x 0 %s", h->in.page, h->in.usage, in_name(h->in));
        if (h->out == OUT_MOUSE) {
            s_buttons &= (uint8_t)~h->usage;  // sent below with the motion
        } else {
            release(h);
        }
        *h = s_held[--s_nheld];
        changed = true;
    }
    for (int k = 0; k < in.nkeys; k++) {  // pressed: in the report, not held yet
        bool held = false;
        for (int i = 0; i < s_nheld && !held; i++) {
            held = s_held[i].rid == rid && s_held[i].in.page == in.keys[k].page && s_held[i].in.usage == in.keys[k].usage;
        }
        if (!held) {
            printf_line("key %02x:%04x 1 %s", in.keys[k].page, in.keys[k].usage, in_name(in.keys[k]));
            press(rid, in.keys[k]);
            changed = true;
        }
    }
    if (in.dx || in.dy || in.wheel || buttons != s_buttons) {
        send_mouse(in.dx, in.dy, in.wheel);
    }
    return changed;
}

// "c:224" (hex page:usage) or a key name from keycodes.json ("AppBack").
static bool parse_in(const char *s, hp_key_t *out)
{
    char *end;
    unsigned long page = s ? strtoul(s, &end, 16) : 0;
    if (s && *end == ':' && end != s) {
        const char *u = end + 1;
        unsigned long usage = strtoul(u, &end, 16);
        if (*end || end == u || page > 0xFFFF || usage > 0xFFFF) {
            return false;
        }
        *out = (hp_key_t){(uint16_t)page, (uint16_t)usage};
        return true;
    }
    const keycode_t *k = s ? kc_by_name(s) : NULL;
    if (!k) {
        return false;
    }
    *out = (hp_key_t){k->page == KC_PAGE_KEYBOARD ? HP_PAGE_KEYBOARD : HP_PAGE_CONSUMER, k->usage};
    return true;
}

// "0a1b" -> bytes. Returns length, or -1 on bad input.
static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0, nib = -1;
    for (; *s; s++) {
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

    if (!strcmp(c, "help")) {
        hw_print("ok cmds: ver | keys | list | map <page:usage|keyname> <key> | unmap <i> | clear | save | hidmap"
                 " | inject <rid> <hex> | status | scan [sec] | pair <addr> <addr_type> | forget | dfu");
    } else if (!strcmp(c, "ver")) {
        hw_print("ok " FW_VERSION);
    } else if (!strcmp(c, "keys")) {
        for (size_t i = 0; i < KEYCODES_COUNT; i++) {
            printf_line("keyname %s %u 0x%02X", KEYCODES[i].name, KEYCODES[i].page, KEYCODES[i].usage);
        }
        hw_print("ok");
    } else if (!strcmp(c, "list")) {
        for (int i = 0; i < s_count; i++) {
            const km_entry_t *e = &s_map[i];
            const char *out = kc_name(e->page, e->usage);
            printf_line("map %d %02x:%04x %s %s", i, e->in_page, e->in_usage, out ? out : "?",
                        in_name((hp_key_t){e->in_page, e->in_usage}));
        }
        printf_line("ok %d", s_count);
    } else if (!strcmp(c, "map")) {
        hp_key_t in;
        const keycode_t *k = argv[2] ? kc_by_name(argv[2]) : NULL;
        if (!parse_in(argv[1], &in) || !k) {
            hw_print("err usage: map <page:usage|keyname> <key>");
            return;
        }
        km_entry_t *e = match(in);
        if (!e) {
            if (s_count >= KM_MAX_ENTRIES) {
                hw_print("err full");
                return;
            }
            e = &s_map[s_count++];
        }
        km_release_all();
        *e = (km_entry_t){.in_page = in.page, .in_usage = in.usage, .page = k->page, .usage = k->usage};
        printf_line("ok %d", (int)(e - s_map));
    } else if (!strcmp(c, "unmap")) {
        int i = argv[1] ? atoi(argv[1]) : -1;
        if (i < 0 || i >= s_count || (argv[1][0] < '0' || argv[1][0] > '9')) {
            hw_print("err bad index");
            return;
        }
        km_release_all();
        memmove(&s_map[i], &s_map[i + 1], sizeof(km_entry_t) * (size_t)(s_count - i - 1));
        s_count--;
        hw_print("ok");
    } else if (!strcmp(c, "clear")) {
        km_release_all();
        s_count = 0;
        hw_print("ok");
    } else if (!strcmp(c, "save")) {
        hw_print(hw_save(s_map, s_count) ? "ok" : "err save failed");
    } else if (!strcmp(c, "hidmap")) {  // what the connected device's report map decoded to
        int n = 0;
        for (int m = 0; m < s_nmaps; m++) {
            for (int i = 0; i < s_maps[m].n; i++, n++) {
                const hp_field_t *f = &s_maps[m].f[i];
                printf_line("field %d rid %u %02x:%04x bit %u size %u count %u %s%s", m, f->rid, f->page, f->umin, f->bit,
                            f->size, f->count, f->array ? "array" : "var", f->relative ? " rel" : "");
            }
        }
        printf_line("ok %d", n);
    } else if (!strcmp(c, "inject")) {
        uint8_t data[64];
        int rid = parse_rid(argv[1]);
        int len = argv[2] ? parse_hex(argv[2], data, sizeof(data)) : -1;
        if (rid < 0 || len < 0) {
            hw_print("err usage: inject <rid> <hex>");
            return;
        }
        km_on_report((uint8_t)rid, data, (size_t)len);
        hw_print("ok");
    } else if (!strcmp(c, "dfu")) {
        hw_print("ok rebooting to download mode");
        hw_dfu();
    } else if (!hw_cmd(argc, argv)) {
        hw_print("err unknown command (try help)");
    }
}
