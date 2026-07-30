/*
 * diary_collect.c - Collect daily conversation context.
 *
 * Currently uses fallback context; will integrate claw_memory later.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "cap_diary_collect";

esp_err_t diary_collect_daily_context(const char *date, char *buf, size_t buf_size)
{
    if (!buf || buf_size < 128) return ESP_ERR_INVALID_ARG;

    // Fallback context (memory integration TBD)
    snprintf(buf, buf_size,
             "今天是%s，星屑陪伴了Benben。\n\n"
             "请生成一篇温暖、安静的陪伴日记。",
             date);
    ESP_LOGI(TAG, "Using fallback context for %s", date);
    return ESP_OK;
}
