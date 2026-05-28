/*
 * diary_tf.c - TF card diary writer.
 *
 * Writes diary to /sdcard/diaries/YYYY-MM-DD.md
 * Idempotent: overwrites existing file (same date).
 *
 * Front-end "download to TF" feature will also use the same path,
 * so overwrite semantics are by design.
 */
#include "cap_diary.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"

static const char *TAG = "cap_diary_tf";

#define TF_DIARY_DIR "/sdcard/diaries"
#define MAX_PATH      128
#define MAX_RETRIES   3

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
        ESP_LOGI(TAG, "Created directory: %s", path);
    }
}

esp_err_t diary_tf_write(const char *date,
                         const char *content,
                         const char *title,
                         const char *tags)
{
    ensure_dir(TF_DIARY_DIR);

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s.md", TF_DIARY_DIR, date);

    // Build full file content with YAML front matter
    char yaml_head[512];
    snprintf(yaml_head, sizeof(yaml_head),
             "---\n"
             "id: %s\n"
             "date: %s\n"
             "title: %s\n"
             "tags: %s\n"
             "source: Esp-Claw\n"
             "created_at: %s\n"
             "---\n\n",
             date, date, title[0] ? title : "无标题",
             tags[0] ? tags : "日常",
             date);

    char full_content[16384];
    snprintf(full_content, sizeof(full_content), "%s%s", yaml_head, content);

    // Write with retries
    for (int retry = 1; retry <= MAX_RETRIES; retry++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            ESP_LOGW(TAG, "open %s failed (retry %d/%d): errno=%d",
                     path, retry, MAX_RETRIES, errno);
            if (retry == MAX_RETRIES) {
                ESP_LOGE(TAG, "TF write failed after %d retries", MAX_RETRIES);
                return ESP_FAIL;
            }
            continue;
        }

        ssize_t written = write(fd, full_content, strlen(full_content));
        close(fd);

        if (written < 0) {
            ESP_LOGW(TAG, "write %s failed (retry %d/%d): errno=%d",
                     path, retry, MAX_RETRIES, errno);
            if (retry == MAX_RETRIES) {
                ESP_LOGE(TAG, "TF write failed after %d retries", MAX_RETRIES);
                return ESP_FAIL;
            }
            continue;
        }

        ESP_LOGI(TAG, "Written %s (%d bytes)", path, (int)written);
        return ESP_OK;
    }

    return ESP_FAIL;
}
