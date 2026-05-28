/*
 * diary_state.c - State machine + NVS persistence for diary generation.
 *
 * All state is in NVS for power-loss resilience.
 * On boot, detects missed dates and enqueues catch-up generation.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "cap_diary_state";

#define NVS_NAMESPACE     "diary"
#define MAX_DATE_LEN      11       // "YYYY-MM-DD\0"

// --- state helpers ---

const char *diary_state_to_str(diary_state_t state)
{
    switch (state) {
        case DIARY_STATE_IDLE:            return "IDLE";
        case DIARY_STATE_PENDING:         return "PENDING";
        case DIARY_STATE_RUNNING:         return "RUNNING";
        case DIARY_STATE_SYNCED_TO_CLOUD: return "SYNCED_TO_CLOUD";
        case DIARY_STATE_TF_WRITTEN:      return "TF_WRITTEN";
        case DIARY_STATE_COMPLETED:       return "COMPLETED";
        case DIARY_STATE_FAILED:          return "FAILED";
        default:                          return "UNKNOWN";
    }
}

// --- NVS key helpers ---

static void make_state_key(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "st_%s", date);
}

static void make_title_key(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "ti_%s", date);
}

static void make_tags_key(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "tg_%s", date);
}

static void make_retry_key(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "rt_%s", date);
}

// --- NVS read/write ---

esp_err_t diary_nvs_get_state(const char *date, diary_state_t *state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_state_key(date, key, sizeof(key));
    uint8_t val = 0;
    err = nvs_get_u8(handle, key, &val);
    if (err == ESP_OK) *state = (diary_state_t)val;
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_set_state(const char *date, diary_state_t state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_state_key(date, key, sizeof(key));
    err = nvs_set_u8(handle, key, (uint8_t)state);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGD(TAG, "state %s -> %s", date, diary_state_to_str(state));
    return err;
}

esp_err_t diary_nvs_get_title(const char *date, char *buf, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_title_key(date, key, sizeof(key));
    size_t slen = len;
    err = nvs_get_str(handle, key, buf, &slen);
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_set_title(const char *date, const char *title)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_title_key(date, key, sizeof(key));
    err = nvs_set_str(handle, key, title);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_get_tags(const char *date, char *buf, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_tags_key(date, key, sizeof(key));
    size_t slen = len;
    err = nvs_get_str(handle, key, buf, &slen);
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_set_tags(const char *date, const char *tags)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_tags_key(date, key, sizeof(key));
    err = nvs_set_str(handle, key, tags);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_get_retry(const char *date, uint8_t *retry)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_retry_key(date, key, sizeof(key));
    err = nvs_get_u8(handle, key, retry);
    if (err != ESP_OK) *retry = 0;
    nvs_close(handle);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t diary_nvs_inc_retry(const char *date, uint8_t *new_retry)
{
    uint8_t cur = 0;
    diary_nvs_get_retry(date, &cur);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[24];
    make_retry_key(date, key, sizeof(key));
    cur++;
    err = nvs_set_u8(handle, key, cur);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    *new_retry = cur;
    return err;
}

// --- last-completed date ---

esp_err_t diary_nvs_get_last_completed(char *buf, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t slen = len;
    err = nvs_get_str(handle, "last_done", buf, &slen);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        err = ESP_OK;
    }
    nvs_close(handle);
    return err;
}

esp_err_t diary_nvs_set_last_completed(const char *date)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "last_done", date);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
