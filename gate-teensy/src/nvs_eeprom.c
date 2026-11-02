/* compat/nvs.h on the Teensy's EEPROM (emulated in flash). Each slot holds
 * a u16 length, then the blob; erased EEPROM reads 0xffff, no blob.
 * GPL-3.0-or-later.
 *
 * A write only touches the bytes that change, but each one goes to flash
 * with interrupts off, so the receivers' ports can drop a few bytes during
 * a save: a frame or two lost (the status chips' crc count), nothing more. */
#include <string.h>
#include "avr_functions.h"
#include "nvs.h"

typedef struct {
    const char *ns, *key;
    uint16_t addr, size;
} slot_t;

static const slot_t SLOTS[] = {
    {"gate", "cfg", 0, 1024}, /* settings.c */
    {"net", "cfg", 1024, 64}, /* net.cpp: the Ethernet address */
};
#define NSLOTS (sizeof(SLOTS) / sizeof(SLOTS[0]))

/* A handle is the first slot of its namespace. */
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    for (nvs_handle_t i = 0; i < NSLOTS; i++)
        if (!strcmp(SLOTS[i].ns, name)) {
            *out = i;
            return ESP_OK;
        }
    return ESP_ERR_NVS_NOT_FOUND;
}

static const slot_t *find(nvs_handle_t h, const char *key) {
    if (h >= NSLOTS) return NULL;
    for (size_t i = 0; i < NSLOTS; i++)
        if (!strcmp(SLOTS[i].ns, SLOTS[h].ns) && !strcmp(SLOTS[i].key, key)) return &SLOTS[i];
    return NULL;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    const slot_t *s = find(h, key);
    uint16_t n;
    if (!s) return ESP_ERR_NVS_NOT_FOUND;
    eeprom_read_block(&n, (const void *)(uintptr_t)s->addr, sizeof(n));
    if (n > s->size - sizeof(n)) return ESP_ERR_NVS_NOT_FOUND;
    if (out) {
        if (*len < n) return ESP_ERR_NVS_INVALID_LENGTH;
        eeprom_read_block(out, (const void *)(uintptr_t)(s->addr + sizeof(n)), n);
    }
    *len = n;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len) {
    const slot_t *s = find(h, key);
    if (!s) return ESP_ERR_NVS_NOT_FOUND;
    if (len > s->size - sizeof(uint16_t)) return ESP_ERR_NVS_VALUE_TOO_LONG;
    uint16_t n = (uint16_t)len;
    eeprom_write_block(&n, (void *)(uintptr_t)s->addr, sizeof(n));
    eeprom_write_block(value, (void *)(uintptr_t)(s->addr + sizeof(n)), n);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h; /* written already */
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }
