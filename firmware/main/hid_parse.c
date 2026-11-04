#include "hid_parse.h"

#include <string.h>

#define MAX_USAGES 16

typedef struct {
    uint16_t page;
    int32_t lmin;
    uint8_t size, count, rid;
} globals_t;

// One explicit usage list entry (Usage items); a variable field with a list gets one field per element.
static bool add(hp_map_t *m, hp_field_t f)
{
    if (m->n >= HP_MAX_FIELDS) {
        return false;
    }
    m->f[m->n++] = f;
    return true;
}

bool hp_parse(const uint8_t *d, size_t len, hp_map_t *m)
{
    globals_t g = {0}, stack[2];
    int sp = 0;
    uint32_t usages[MAX_USAGES];  // high 16 bits = page when given as a 32-bit usage
    int nu = 0;
    uint32_t umin = 0;
    bool have_min = false;
    uint16_t bitpos[256] = {0};  // per report ID

    for (size_t i = 0; i < len;) {
        uint8_t b = d[i];
        if (b == 0xFE) {  // long item: skip
            if (i + 2 >= len) {
                return false;
            }
            i += 3 + d[i + 1];
            continue;
        }
        size_t sz = (size_t[]){0, 1, 2, 4}[b & 3];
        if (i + 1 + sz > len) {
            return false;
        }
        uint32_t v = 0;
        for (size_t k = 0; k < sz; k++) {
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        }
        int32_t sv = sz && sz < 4 && (v >> (8 * sz - 1)) ? (int32_t)(v | ~0u << (8 * sz)) : (int32_t)v;
        i += 1 + sz;

        switch (b & 0xFC) {
        // global items
        case 0x04: g.page = (uint16_t)v; break;
        case 0x14: g.lmin = sv; break;
        case 0x74: g.size = (uint8_t)v; break;
        case 0x84: g.rid = (uint8_t)v; break;
        case 0x94: g.count = (uint8_t)v; break;
        case 0xA4: if (sp < 2) stack[sp++] = g; break;
        case 0xB4: if (sp > 0) g = stack[--sp]; break;
        // local items
        case 0x08: if (nu < MAX_USAGES) usages[nu++] = sz == 4 ? v : (uint32_t)g.page << 16 | v; break;
        case 0x18: umin = sz == 4 ? v : (uint32_t)g.page << 16 | v; have_min = true; break;
        // main items
        case 0x80: {  // Input
            uint16_t *pos = &bitpos[g.rid];
            bool constant = v & 1, variable = v & 2;
            if (!constant && g.size && g.count) {
                hp_field_t f = {.rid = g.rid, .array = !variable, .relative = (v & 4) != 0, .bit = *pos,
                                .size = g.size, .count = g.count, .lmin = g.lmin};
                if (variable && nu > 1 && !have_min) {
                    // e.g. Usage X, Usage Y, Usage Wheel: one field per element (last usage repeats)
                    for (int e = 0; e < g.count; e++) {
                        uint32_t u = usages[e < nu ? e : nu - 1];
                        f.page = (uint16_t)(u >> 16), f.umin = (uint16_t)u, f.count = 1;
                        f.bit = (uint16_t)(*pos + e * g.size);
                        if (!add(m, f)) {
                            return false;
                        }
                    }
                } else {
                    uint32_t u = have_min ? umin : nu ? usages[0] : (uint32_t)g.page << 16;
                    f.page = (uint16_t)(u >> 16), f.umin = (uint16_t)u;
                    if (!add(m, f)) {
                        return false;
                    }
                }
            }
            *pos = (uint16_t)(*pos + g.size * g.count);
        }
            __attribute__((fallthrough));  // main items clear locals
        case 0x90:  // Output
        case 0xB0:  // Feature
        case 0xA0:  // Collection
        case 0xC0:  // End Collection
            nu = 0, have_min = false;
            break;
        default: break;  // Logical Max, Usage Max, units, ...: not needed
        }
    }
    return true;
}

bool hp_has_report(const hp_map_t *m, uint8_t rid)
{
    for (int i = 0; i < m->n; i++) {
        if (m->f[i].rid == rid) {
            return true;
        }
    }
    return false;
}

static int32_t get(const uint8_t *data, size_t len, unsigned bit, unsigned size, bool is_signed)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < size && k < 32; k++) {
        unsigned p = bit + k;
        if (p / 8 < len && (data[p / 8] >> (p % 8) & 1)) {
            v |= 1u << k;
        }
    }
    if (is_signed && size < 32 && (v >> (size - 1) & 1)) {
        v |= ~0u << size;
    }
    return (int32_t)v;
}

static void press(hp_input_t *o, uint16_t page, uint16_t usage)
{
    if (page == HP_PAGE_KEYBOARD && usage <= 3) {
        return;  // "no event" / rollover error codes
    }
    if (o->nkeys < HP_MAX_KEYS) {
        o->keys[o->nkeys++] = (hp_key_t){page, usage};
    }
}

void hp_extract(const hp_map_t *m, uint8_t rid, const uint8_t *data, size_t len, hp_input_t *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < m->n; i++) {
        const hp_field_t *f = &m->f[i];
        if (f->rid != rid || f->page >= 0xFF00) {  // vendor-defined (e.g. voice data): no standard meaning
            continue;
        }
        for (int e = 0; e < f->count; e++) {
            int32_t v = get(data, len, f->bit + (unsigned)e * f->size, f->size, f->lmin < 0);
            if (f->array) {
                int32_t usage = f->umin + (v - f->lmin);
                if (v != f->lmin || f->umin) {  // index lmin with usage min 0 = nothing pressed
                    if (usage > 0 && usage <= 0xFFFF) {
                        press(o, f->page, (uint16_t)usage);
                    }
                }
            } else if (!f->relative && v != 0 &&
                       (f->size == 1 || f->page == HP_PAGE_KEYBOARD || f->page == HP_PAGE_CONSUMER || f->page == HP_PAGE_BUTTON)) {
                press(o, f->page, (uint16_t)(f->umin + e));  // buttons, modifier bits, consumer bitmaps (not axes/values)
            }
        }
    }
}
