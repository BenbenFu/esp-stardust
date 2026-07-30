/*
 * diary_llm.c - LLM interaction for diary generation.
 *
 * Currently returns error so diary_run uses fallback content.
 * Will activate when LLM is configured via web interface.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "cap_diary_llm";

esp_err_t diary_llm_generate(const char *date,
                             const char *context,
                             const char *existing_content,
                             bool is_rewrite,
                             char *content, size_t content_len,
                             char *title, size_t title_len,
                             char *tags, size_t tags_len)
{
    (void)date; (void)context; (void)existing_content; (void)is_rewrite;
    ESP_LOGW(TAG, "LLM not configured, returning fallback signal");
    // Return error → diary_run uses default content
    return ESP_ERR_NOT_SUPPORTED;
}
