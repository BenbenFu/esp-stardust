/*
 * diary_tf.c - TF card diary writer.
 *
 * Writes diary to /sdcard/diaries/YYYY-MM-DD.md
 * Idempotent: overwrites existing file (same date).
 */

#include "cap_diary.h"
#include "sdcard_vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "cap_diary_tf";

esp_err_t diary_tf_write(const char *date,
                         const char *content,
                         const char *title,
                         const char *tags)
{
    // Build virtual path for translation: /diaries/YYYY-MM-DD.md → 2026XXXX.MD
    char vpath[64];
    snprintf(vpath, sizeof(vpath), "/diaries/%s.md", date);

    ESP_LOGI(TAG, "Target: %s, content=%u chars", vpath, (unsigned)strlen(content));

    // Build YAML front matter
    char yaml_head[512];
    int yaml_len = snprintf(yaml_head, sizeof(yaml_head),
             "---\n"
             "id: %s\n"
             "date: %s\n"
             "title: %s\n"
             "tags: %s\n"
             "source: Esp-Claw:Deepseek\n"
             "created_at: %s\n"
             "---\n\n",
             date, date, title[0] ? title : "untitled",
             tags[0] ? tags : "daily",
             date);

    // Combine into heap buffer
    size_t clen = strlen(content);
    size_t total = (yaml_len > 0 ? (size_t)yaml_len : 0) + clen;
    char *buf = malloc(total + 1);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%u) failed", (unsigned)total);
        return ESP_ERR_NO_MEM;
    }
    if (yaml_len > 0) memcpy(buf, yaml_head, yaml_len);
    memcpy(buf + yaml_len, content, clen);
    buf[total] = '\0';

    // Write directly through FAT32 layer (bypasses VFS to avoid callback crashes)
    bool ok = sdcard_vfs_write_direct(vpath, buf, (uint16_t)total);
    free(buf);

    if (ok) {
        ESP_LOGI(TAG, "Diary saved: %s (%u bytes)", vpath, (unsigned)total);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Direct write FAILED for %s", vpath);
    return ESP_FAIL;
}
