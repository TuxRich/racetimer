/* The NVS calls settings.c makes (ESP-IDF's nvs.h), on the Teensy's
 * EEPROM: one blob per namespace and key, each in a fixed slot
 * (src/nvs_eeprom.c). GPL-3.0-or-later. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_VALUE_TOO_LONG 0x110e

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
/* *len: the room at out; set to the blob's length. */
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);

#ifdef __cplusplus
}
#endif
