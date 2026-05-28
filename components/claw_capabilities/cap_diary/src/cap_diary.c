/*
 * cap_diary.c - Stardust Diary Capability entry point.
 *
 * State machine flow:
 *   PENDING → RUNNING → SYNCED_TO_CLOUD → TF_WRITTEN → COMPLETED
 *                     ↳ (network fail) → FAILED → retry later
 *
 * Pure code constraints: LLM is called ONLY for content generation.
 * All flow control is hard-coded, no LLM decision-making.
 *
 * Power-loss resilience: all state in NVS, catch-up on boot.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "nvs_flash.h"

/* ---- forward declarations ---- */
extern esp_err_t diary_nvs_get_state(const char *date, diary_state_t *state);
extern esp_err_t diary_nvs_set_state(const char *date, diary_state_t state);
extern esp_err_t diary_nvs_get_title(const char *date, char *buf, size_t len);
extern esp_err_t diary_nvs_set_title(const char *date, const char *title);
extern esp_err_t diary_nvs_get_tags(const char *date, char *buf, size_t len);
extern esp_err_t diary_nvs_set_tags(const char *date, const char *tags);
extern esp_err_t diary_nvs_get_retry(const char *date, uint8_t *retry);
extern esp_err_t diary_nvs_inc_retry(const char *date, uint8_t *new_retry);
extern esp_err_t diary_nvs_get_last_completed(char *buf, size_t len);
extern esp_err_t diary_nvs_set_last_completed(const char *date);

extern esp_err_t diary_collect_daily_context(const char *date, char *buf, size_t buf_size);
extern esp_err_t diary_llm_generate(const char *date, const char *context,
                                    const char *existing_content, bool is_rewrite,
                                    char *content, size_t content_len,
                                    char *title, size_t title_len,
                                    char *tags, size_t tags_len);
extern esp_err_t diary_supabase_post(const char *date, const char *content,
                                     const char *title, const char *tags);
extern esp_err_t diary_supabase_patch(const char *date, const char *new_content,
                                      const char *existing_id);
extern esp_err_t diary_tf_write(const char *date, const char *content,
                                const char *title, const char *tags);

static const char *TAG = "cap_diary";

#define MAX_CONTENT  8192
#define MAX_TITLE    128
#define MAX_TAGS     256

// ---- helpers ----

/* Get current NTP-synced date string "YYYY-MM-DD" */
static void get_today_str(char *buf, size_t len)
{
    time_t now;
    time(&now);
    struct tm *tm_info = localtime(&now);
    int year  = (int)tm_info->tm_year + 1900;
    int month = (int)tm_info->tm_mon + 1;
    int day   = (int)tm_info->tm_mday;
    if (year < 0) year = 0;
    if (year > 9999) year = 9999;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
    snprintf(buf, len, "%04d-%02d-%02d", year, month, day);
}

/* Check if NTP time is synced (year >= 2026 as rough proxy) */
static bool ntp_synced(void)
{
    time_t now;
    time(&now);
    struct tm *tm_info = localtime(&now);
    return (tm_info->tm_year + 1900) >= 2026;
}

// ---- core state machine ----

static esp_err_t diary_run(const char *date, const char *existing_id)
{
    char content[MAX_CONTENT] = {0};
    char title[MAX_TITLE] = {0};
    char tags[MAX_TAGS] = {0};
    char context[MAX_CONTENT] = {0};
    esp_err_t err;

    // Step 0: Check NTP sync
    if (!ntp_synced()) {
        ESP_LOGW(TAG, "NTP not synced, deferring %s", date);
        diary_nvs_set_state(date, DIARY_STATE_PENDING);
        return ESP_ERR_INVALID_STATE;
    }

    // Transition to RUNNING
    diary_nvs_set_state(date, DIARY_STATE_RUNNING);

    // Step 1: Collect context
    err = diary_collect_daily_context(date, context, sizeof(context));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Collect context failed");
        diary_nvs_set_state(date, DIARY_STATE_FAILED);
        return err;
    }

    // Step 2: LLM generate
    err = diary_llm_generate(date, context, NULL, false,
                             content, sizeof(content),
                             title, sizeof(title),
                             tags, sizeof(tags));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM generate failed");
        diary_nvs_set_state(date, DIARY_STATE_FAILED);
        return err;
    }

    // Persist LLM output to NVS (for power-loss resilience)
    diary_nvs_set_title(date, title);
    diary_nvs_set_tags(date, tags);

    // Step 3: Sync to Supabase
    // Check if record already exists
    if (existing_id && existing_id[0]) {
        err = diary_supabase_patch(date, content, existing_id);
    } else {
        err = diary_supabase_post(date, content, title, tags);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Supabase sync failed, retrying later");
        diary_nvs_set_state(date, DIARY_STATE_RUNNING);
        return err;
    }
    diary_nvs_set_state(date, DIARY_STATE_SYNCED_TO_CLOUD);

    // Step 4: Write to TF card
    err = diary_tf_write(date, content, title, tags);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF write failed");
        diary_nvs_set_state(date, DIARY_STATE_FAILED);
        return err;
    }
    diary_nvs_set_state(date, DIARY_STATE_TF_WRITTEN);

    // Step 5: Complete
    diary_nvs_set_state(date, DIARY_STATE_COMPLETED);
    diary_nvs_set_last_completed(date);
    ESP_LOGI(TAG, "=== Diary %s COMPLETED ===", date);
    return ESP_OK;
}

// ---- catch-up on boot ----

__attribute__((unused))
static void diary_catch_up(void)
{
    char last_date[16] = {0};
    diary_nvs_get_last_completed(last_date, sizeof(last_date));

    char today[16];
    get_today_str(today, sizeof(today));

    ESP_LOGI(TAG, "Boot catch-up: last=%s today=%s",
             last_date[0] ? last_date : "(none)", today);

    if (last_date[0] == '\0') {
        // First boot ever, nothing to catch up
        return;
    }

    // Check all dates between last completed and today
    // For each date, if state != COMPLETED, enqueue
    struct tm last_tm = {0};
    sscanf(last_date, "%d-%d-%d", &last_tm.tm_year, &last_tm.tm_mon, &last_tm.tm_mday);
    last_tm.tm_year -= 1900;
    last_tm.tm_mon -= 1;

    struct tm today_tm = {0};
    sscanf(today, "%d-%d-%d", &today_tm.tm_year, &today_tm.tm_mon, &today_tm.tm_mday);
    today_tm.tm_year -= 1900;
    today_tm.tm_mon -= 1;

    // Simple day-by-day iteration
    time_t t = mktime(&last_tm);
    time_t t_today = mktime(&today_tm);

    // Add 1 day to last
    t += 86400;

    while (t <= t_today) {
        struct tm *tm = localtime(&t);
        char date_str[16];
        int year  = (int)tm->tm_year + 1900;
        int month = (int)tm->tm_mon + 1;
        int day   = (int)tm->tm_mday;
        if (year < 0) year = 0;
        if (year > 9999) year = 9999;
        if (month < 1) month = 1;
        if (month > 12) month = 12;
        if (day < 1) day = 1;
        if (day > 31) day = 31;
        snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, month, day);

        diary_state_t state;
        esp_err_t err = diary_nvs_get_state(date_str, &state);
        if (err != ESP_OK || state != DIARY_STATE_COMPLETED) {
            ESP_LOGI(TAG, "Catch-up: processing %s", date_str);
            diary_run(date_str, NULL);
        }

        t += 86400;
    }
}

// ---- public API ----

esp_err_t cap_diary_trigger(const char *date_str)
{
    char date[16];
    if (date_str) {
        strlcpy(date, date_str, sizeof(date));
    } else {
        get_today_str(date, sizeof(date));
    }

    ESP_LOGI(TAG, "Manual trigger: %s", date);
    return diary_run(date, NULL);
}

esp_err_t cap_diary_rewrite(const char *date_str)
{
    char date[16];
    if (date_str) {
        strlcpy(date, date_str, sizeof(date));
    } else {
        get_today_str(date, sizeof(date));
    }

    ESP_LOGI(TAG, "Rewrite: %s", date);

    // Collect context and run with rewrite flag
    char context[MAX_CONTENT] = {0};
    diary_collect_daily_context(date, context, sizeof(context));

    char content[MAX_CONTENT] = {0};
    char title[MAX_TITLE] = {0};
    char tags[MAX_TAGS] = {0};

    // Use empty existing content for simplicity
    esp_err_t err = diary_llm_generate(date, context, NULL, true,
                                       content, sizeof(content),
                                       title, sizeof(title),
                                       tags, sizeof(tags));
    if (err != ESP_OK) return err;

    // Patch Supabase
    err = diary_supabase_patch(date, content, NULL);
    if (err != ESP_OK) return err;

    // Overwrite TF
    err = diary_tf_write(date, content, title, tags);
    return err;
}

esp_err_t cap_diary_get_state(const char *date_str, diary_state_t *state)
{
    char date[16];
    if (date_str) {
        strlcpy(date, date_str, sizeof(date));
    } else {
        get_today_str(date, sizeof(date));
    }
    return diary_nvs_get_state(date, state);
}

// ---- capability descriptors ----

static esp_err_t cap_diary_trigger_exec(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    (void)ctx;
    const char *date = NULL;

    if (input_json) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *d = cJSON_GetObjectItem(root, "date");
            if (d && d->valuestring) date = d->valuestring;
            cJSON_Delete(root);
        }
    }

    esp_err_t err = cap_diary_trigger(date);
    if (output_size > 0) {
        snprintf(output, output_size, "{\"status\":\"%s\"}",
                 err == ESP_OK ? "ok" : "failed");
    }
    return err;
}

static claw_cap_descriptor_t s_diary_descriptors[] = {
    {
        .id = "diary_trigger",
        .name = "Trigger Diary",
        .family = "diary",
        .description = "Trigger diary generation for today or a specific date",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\",\"description\":\"Optional date YYYY-MM-DD\"}}}",
        .execute = cap_diary_trigger_exec,
    },
};

static const claw_cap_group_t s_diary_group = {
    .group_id = "cap_diary",
    .descriptors = s_diary_descriptors,
    .descriptor_count = sizeof(s_diary_descriptors) / sizeof(s_diary_descriptors[0]),
};

esp_err_t cap_diary_register_group(void)
{
    if (claw_cap_group_exists(s_diary_group.group_id)) {
        return ESP_OK;
    }

    esp_err_t err = claw_cap_register_group(&s_diary_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register group failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Diary capability registered");

    // Catch-up is deferred: cap_diary_trigger(NULL) can be called
    // after NTP sync via scheduler or REPL command.
    // Synchronous catch-up here would run too early (no NTP, no network).

    return ESP_OK;
}
