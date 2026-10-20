/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "settings";

#define PARTITION   "jd_nvs"
#define NAMESPACE   "jd"
#define KEY_CFG     "cfg"
#define CFG_VERSION 1u /* bump when settings_t changes */

typedef struct {
    uint32_t version, size;
    settings_t s;
} stored_t;

static const char *const s_base_key[BAND_COUNT] = {"base24", "base5"};
static settings_t s_cur;
static uint32_t s_boot;
static SemaphoreHandle_t s_lock;

void settings_default(settings_t *s)
{
    memset(s, 0, sizeof(*s)); /* padding too: the blob is compared bytewise */
    detect_default_config(&s->det);
    s->weight[BAND_24] = 2;
    s->weight[BAND_5] = 1;
    s->gain[BAND_24] = s->gain[BAND_5] = SETTINGS_GAIN_NONE;
    s->beep = false;
    s->brightness = 80;
}

int settings_check(const settings_t *s)
{
    for (int b = 0; b < BAND_COUNT; b++) {
        if (s->weight[b] < 1 || s->weight[b] > 8) {
            return -1;
        }
    }
    return detect_check_config(&s->det) == 0 && s->brightness >= 5 && s->brightness <= 100 ? 0 : -1;
}

static esp_err_t write_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t read_blob(const char *key, void *data, size_t cap, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND; /* namespace not created yet */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open");
    size_t n = cap;
    err = nvs_get_blob(h, key, data, &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    *len = n;
    return err;
}

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s: %s, erasing", PARTITION, esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(PARTITION), TAG, "erase");
        err = nvs_flash_init_partition(PARTITION);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init %s (partition table flashed?)", PARTITION);

    nvs_handle_t h;
    if (nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot", &s_boot); /* stays 0 the first time */
        s_boot++;
        if (nvs_set_u32(h, "boot", s_boot) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "boot counter not saved");
        }
        nvs_close(h);
    }

    settings_default(&s_cur);
    stored_t st;
    size_t len = 0;
    err = read_blob(KEY_CFG, &st, sizeof(st), &len);
    if (err == ESP_OK && len == sizeof(st) && st.version == CFG_VERSION && st.size == sizeof(settings_t) &&
        settings_check(&st.s) == 0) {
        s_cur = st.s;
        ESP_LOGI(TAG, "loaded; gains %u / %u", s_cur.gain[BAND_24], s_cur.gain[BAND_5]);
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "none stored, using defaults");
    } else {
        ESP_LOGW(TAG, "stored settings unusable (%s, %u bytes), using defaults", esp_err_to_name(err),
                 (unsigned)len);
    }
    return ESP_OK;
}

uint32_t settings_boot(void)
{
    return s_boot;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set(const settings_t *s)
{
    ESP_RETURN_ON_FALSE(settings_check(s) == 0, ESP_ERR_INVALID_ARG, TAG, "out of range");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (memcmp(&s_cur, s, sizeof(*s))) {
        stored_t st = {.version = CFG_VERSION, .size = sizeof(settings_t), .s = *s};
        err = write_blob(KEY_CFG, &st, sizeof(st));
        if (err == ESP_OK) {
            s_cur = *s;
            ESP_LOGI(TAG, "saved");
        }
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "write");
    return ESP_OK;
}

esp_err_t settings_save_baseline(band_id_t band, const void *rec, size_t len)
{
    ESP_RETURN_ON_FALSE(band < BAND_COUNT && rec && len, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_ERROR(write_blob(s_base_key[band], rec, len), TAG, "baseline %s", s_base_key[band]);
    return ESP_OK;
}

esp_err_t settings_load_baseline(band_id_t band, void *rec, size_t cap, size_t *len)
{
    ESP_RETURN_ON_FALSE(band < BAND_COUNT && rec && len, ESP_ERR_INVALID_ARG, TAG, "bad args");
    return read_blob(s_base_key[band], rec, cap, len);
}

esp_err_t settings_erase_baselines(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = ESP_OK;
    for (int b = 0; b < BAND_COUNT && err == ESP_OK; b++) {
        err = nvs_erase_key(h, s_base_key[b]);
        err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "erase baselines");
    return ESP_OK;
}
