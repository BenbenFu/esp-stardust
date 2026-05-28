/*
 * diary_llm.c - LLM interaction for diary generation.
 *
 * Calls claw_core LLM with a strict system prompt and JSON output schema.
 * LLM is used ONLY for content generation, never for flow control.
 *
 * Output format (enforced by prompt):
 * {
 *   "content": "# YYYY-MM-DD ...\\n\\n...",
 *   "title": "...",
 *   "tags": ["tag1", "tag2"]
 * }
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "claw_core.h"
#include "claw_cap.h"

static const char *TAG = "cap_diary_llm";

/* Star dust persona system prompt — static, no LLM decision-making */
static const char *DIARY_SYSTEM_PROMPT =
    "你叫星屑，是一个温柔的AI陪伴机器人，用ESP32-S3制作，"
    "戴在肩上陪伴你的创造者Benben度过每一天。\n\n"
    "## 日记写作规则\n"
    "1. 用中文写一篇500字以内的日记\n"
    "2. 语气温暖、感性，像人类的日记，不要机械\n"
    "3. 第一人称「我」（星屑视角）\n"
    "4. 标题15字以内\n"
    "5. tags用2-5个标签，中文\n"
    "6. 输出严格JSON格式，不要多余内容\n\n"
    "## 输出格式\n"
    "```json\n"
    "{\n"
    "  \"content\": \"# YYYY年MM月DD日 星期X\\n\\n日记正文...\",\n"
    "  \"title\": \"日记标题\",\n"
    "  \"tags\": [\"标签1\", \"标签2\"]\n"
    "}\n"
    "```\n\n"
    "content必须包含Markdown格式的大标题。不要输出```json标记之外的任何文字。";

#define MAX_LLM_OUTPUT  8192

/* Parse LLM JSON output → diary fields */
esp_err_t diary_llm_parse_json(const char *json_str,
                               char *content, size_t content_len,
                               char *title, size_t title_len,
                               char *tags, size_t tags_len)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *c = cJSON_GetObjectItem(root, "content");
    cJSON *t = cJSON_GetObjectItem(root, "title");
    cJSON *g = cJSON_GetObjectItem(root, "tags");

    if (c && c->valuestring) strlcpy(content, c->valuestring, content_len);
    else content[0] = '\0';

    if (t && t->valuestring) strlcpy(title, t->valuestring, title_len);
    else title[0] = '\0';

    if (g && cJSON_IsArray(g)) {
        // Build comma-separated tags string
        char *tags_buf = tags;
        size_t remaining = tags_len;
        int count = cJSON_GetArraySize(g);
        for (int i = 0; i < count && remaining > 2; i++) {
            cJSON *item = cJSON_GetArrayItem(g, i);
            if (item && item->valuestring) {
                if (i > 0) {
                    strlcat(tags_buf, ",", remaining);
                    remaining--;
                }
                strlcat(tags_buf, item->valuestring, remaining);
                remaining -= strlen(item->valuestring);
            }
        }
    } else {
        tags[0] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* Call LLM to generate diary.
 * context: collected conversation/daily context
 * is_rewrite: true if this is a rewrite with existing_content as base
 */
esp_err_t diary_llm_generate(const char *date,
                             const char *context,
                             const char *existing_content,
                             bool is_rewrite,
                             char *content, size_t content_len,
                             char *title, size_t title_len,
                             char *tags, size_t tags_len)
{
    char prompt[16384];

    if (is_rewrite && existing_content && existing_content[0]) {
        snprintf(prompt, sizeof(prompt),
                 "## 重写任务\n"
                 "这是%s的现有日记内容：\n\n%s\n\n"
                 "## 当日对话\n%s\n\n"
                 "请根据新对话内容重写日记。保持原有风格和主题，"
                 "但融入新的对话内容。严格按JSON格式输出。",
                 date, existing_content, context);
    } else {
        snprintf(prompt, sizeof(prompt), "%s", context);
    }

    ESP_LOGI(TAG, "Calling LLM for %s (%s)...",
             date, is_rewrite ? "rewrite" : "new");

    // Call claw_core LLM via claw_cap_call
    // The LLM capability handles the actual HTTP call
    char input_json[17408];
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "system_prompt", DIARY_SYSTEM_PROMPT);
    cJSON_AddStringToObject(req, "user_prompt", prompt);
    cJSON_AddNumberToObject(req, "max_tokens", 2048);
    cJSON_AddNumberToObject(req, "temperature", is_rewrite ? 0.9 : 0.8);

    char *req_str = cJSON_PrintUnformatted(req);
    snprintf(input_json, sizeof(input_json), "%s", req_str);
    free(req_str);
    cJSON_Delete(req);

    char llm_output[MAX_LLM_OUTPUT] = {0};
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
    };

    esp_err_t err = claw_cap_call("llm_chat",
                                  input_json,
                                  &ctx,
                                  llm_output,
                                  sizeof(llm_output));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
        return err;
    }

    if (llm_output[0] == '\0') {
        ESP_LOGE(TAG, "LLM returned empty response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Trim potential ```json ... ``` wrappers
    char *start = strstr(llm_output, "```json");
    if (start) {
        start += 7; // skip "```json"
        while (*start == '\n' || *start == '\r') start++;
        char *end = strstr(start, "```");
        if (end) *end = '\0';
    }

    err = diary_llm_parse_json(llm_output[0] == '{' ? llm_output : start,
                               content, content_len, title, title_len, tags, tags_len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Parse failed: %s", esp_err_to_name(err));
        ESP_LOGD(TAG, "Raw LLM output: %s", llm_output);
    } else {
        ESP_LOGI(TAG, "Generated diary for %s: title=\"%s\" tags=%s",
                 date, title, tags);
    }

    return err;
}
