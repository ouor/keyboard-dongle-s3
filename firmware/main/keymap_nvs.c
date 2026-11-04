// Keymap persistence in NVS (namespace "keymap", key "map2"; "entries" held the old raw-report format). Provides hw_save() for keymap.c's `save` command.
#include "keymap_nvs.h"

#include "keymap.h"
#include "nvs.h"

bool hw_save(const km_entry_t *entries, int count)
{
    nvs_handle_t h;
    if (nvs_open("keymap", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "map2", entries, sizeof(km_entry_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void keymap_nvs_load(void)
{
    static km_entry_t buf[KM_MAX_ENTRIES];
    size_t size = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open("keymap", NVS_READONLY, &h) != ESP_OK) {
        return;  // nothing saved yet
    }
    if (nvs_get_blob(h, "map2", buf, &size) == ESP_OK && size % sizeof(km_entry_t) == 0) {
        km_load(buf, size / sizeof(km_entry_t));
    }
    nvs_close(h);
}
