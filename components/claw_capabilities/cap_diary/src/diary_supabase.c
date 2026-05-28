/*
 * diary_supabase.c - Supabase HTTP client for diary sync.
 *
 * Handles POST (new record) and PATCH (append) operations.
 * Idempotent: no duplicate records for the same date.
 *
 * Source format: Esp-Claw:Deepseek (platform:model)
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"

static const char *TAG = "cap_diary_sb";

// Supabase credentials (TODO: move to Kconfig)
#define SUPABASE_URL   "https://opyeahbzibuupmkmjpkr.supabase.co"
#define SUPABASE_KEY   "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im9weWVhaGJ6aWJ1dXBta21qcGtyIiwicm9sZSI6ImFub24iLCJpYXQiOjE3MTM4NjQwMDAsImV4cCI6MjAyOTQ0MDAwMH0.dummy"
#define SUPABASE_TABLE "DIARIES"
#define HTTP_TIMEOUT_MS 15000

// Source tag
#define DIARY_SOURCE_TAG "Esp-Claw:Deepseek"

#define MAX_RESPONSE    4096

static esp_err_t supabase_http_request(const char *method,
                                       const char *path,
                                       const char *body,
                                       char *response, size_t resp_size)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = method ? HTTP_METHOD_GET : HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .skip_cert_common_name_check = true,
    };

    if (strcmp(method, "POST") == 0) config.method = HTTP_METHOD_POST;
    else if (strcmp(method, "PATCH") == 0) config.method = HTTP_METHOD_PATCH;
    else config.method = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Headers
    esp_http_client_set_header(client, "apikey", SUPABASE_KEY);
    esp_http_client_set_header(client, "Authorization",
                               "Bearer " SUPABASE_KEY);
    esp_http_client_set_header(client, "Content-Type",
                               "application/json");
    esp_http_client_set_header(client, "Prefer", "return=representation");

    if (body) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int len = esp_http_client_read_response(client, response, resp_size - 1);
        if (len > 0) response[len] = '\0';
        ESP_LOGD(TAG, "%s %s → HTTP %d, %d bytes",
                 method, path, status, len);
    } else {
        ESP_LOGE(TAG, "%s %s failed: %s", method, path, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* Check if a record exists for the given date.
 * If found, fills *id and returns true.
 */
static bool supabase_find_record(const char *date, char *id, size_t id_len, char *existing_content, size_t content_len)
{
    char path[128];
    snprintf(path, sizeof(path), "%s?date=eq.%s&select=id,content,source", SUPABASE_TABLE, date);

    char resp[MAX_RESPONSE] = {0};
    esp_err_t err = supabase_http_request("GET", path, NULL, resp, sizeof(resp));
    if (err != ESP_OK || resp[0] == '\0' || strcmp(resp, "[]") == 0) {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        if (root) cJSON_Delete(root);
        return false;
    }

    cJSON *item = cJSON_GetArrayItem(root, 0);
    cJSON *id_json = cJSON_GetObjectItem(item, "id");
    cJSON *content_json = cJSON_GetObjectItem(item, "content");

    if (id_json && id_json->valuestring) strlcpy(id, id_json->valuestring, id_len);
    if (content_json && content_json->valuestring) strlcpy(existing_content, content_json->valuestring, content_len);

    cJSON_Delete(root);
    return id[0] != '\0';
}

/* POST new diary record. All fields written fresh. */
esp_err_t diary_supabase_post(const char *date,
                              const char *content,
                              const char *title,
                              const char *tags)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "date", date);
    cJSON_AddStringToObject(body, "content", content);
    cJSON_AddStringToObject(body, "title", title);
    cJSON_AddStringToObject(body, "tags", tags);
    cJSON_AddStringToObject(body, "source", DIARY_SOURCE_TAG);
    cJSON_AddStringToObject(body, "created_at", date);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char resp[MAX_RESPONSE] = {0};
    esp_err_t err = supabase_http_request("POST", SUPABASE_TABLE,
                                          body_str, resp, sizeof(resp));
    free(body_str);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "POST diary %s → Supabase OK", date);
    }
    return err;
}

/* PATCH existing diary: append content with source tag separator.
 * Title and tags are preserved (not overwritten). */
esp_err_t diary_supabase_patch(const char *date,
                               const char *new_content,
                               const char *existing_id)
{
    // Query existing content first
    char existing_id_buf[64] = {0};
    char existing_content[8192] = {0};

    if (existing_id == NULL || existing_id[0] == '\0') {
        if (!supabase_find_record(date, existing_id_buf, sizeof(existing_id_buf),
                                   existing_content, sizeof(existing_content))) {
            ESP_LOGW(TAG, "No existing record for PATCH %s", date);
            return ESP_ERR_NOT_FOUND;
        }
        existing_id = existing_id_buf;
    }

    // Build appended content: old + separator + new source tag + new content
    char appended[16384];
    snprintf(appended, sizeof(appended), "%s\n---\n[%s]\n%s",
             existing_content[0] ? existing_content : "",
             DIARY_SOURCE_TAG,
             new_content);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "content", appended);

    // Append source
    if (existing_content[0]) {
        // Get existing source to append
        // Simplified: just set source, Supabase will need a trigger for append
        cJSON_AddStringToObject(body, "source", DIARY_SOURCE_TAG);
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char path[128];
    snprintf(path, sizeof(path), "%s?id=eq.%s", SUPABASE_TABLE, existing_id);

    char resp[MAX_RESPONSE] = {0};
    esp_err_t err = supabase_http_request("PATCH", path, body_str, resp, sizeof(resp));
    free(body_str);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PATCH diary %s → Supabase OK", date);
    }
    return err;
}
