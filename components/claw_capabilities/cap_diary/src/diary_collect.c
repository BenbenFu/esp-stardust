/*
 * diary_collect.c - Collect daily conversation memory from claw_memory.
 *
 * Queries the esp-claw memory system for today's digest/records
 * and formats them for LLM prompt injection.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "claw_cap.h"

static const char *TAG = "cap_diary_collect";

#define MAX_RAW_INPUT 16384

esp_err_t diary_collect_daily_context(const char *date, char *buf, size_t buf_size)
{
    if (!buf || buf_size < 128) return ESP_ERR_INVALID_ARG;

    // Attempt to collect from claw_memory capability.
    char memory_output[8192] = {0};

    // Build the input query for memory retrieval.
    // Format: {"action":"get_daily_context","date":"2026-05-28"}
    char query[256];
    snprintf(query, sizeof(query),
             "{\"action\":\"get_daily_context\",\"date\":\"%s\"}", date);

    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
    };

    esp_err_t err = claw_cap_call("memory_get_context",
                                  query,
                                  &ctx,
                                  memory_output,
                                  sizeof(memory_output));

    if (err == ESP_OK && memory_output[0] != '\0') {
        // Got memory context. Format for LLM.
        snprintf(buf, buf_size,
                 "以下是 %s 的对话记录摘要：\n\n%s\n\n"
                 "请基于以上内容为星屑写一篇日记。",
                 date, memory_output);
        ESP_LOGI(TAG, "Collected %u bytes of context for %s",
                 (unsigned)strlen(buf), date);
        return ESP_OK;
    }

    // Fallback: no conversation for today
    if (err == ESP_ERR_NOT_FOUND || memory_output[0] == '\0') {
        snprintf(buf, buf_size,
                 "今天是%s，星屑陪伴了Benben，但今天没有对话记录。\n\n"
                 "请生成一篇温暖、安静的陪伴日记。",
                 date);
        ESP_LOGI(TAG, "No conversation for %s, using fallback context", date);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Memory query failed: %s", esp_err_to_name(err));
    return err;
}
