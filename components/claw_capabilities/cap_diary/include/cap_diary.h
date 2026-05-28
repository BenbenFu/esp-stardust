/*
 * cap_diary.h - Stardust Diary Capability
 *
 * Generates daily diary entries from conversation memory,
 * syncs to Supabase, and writes to TF card.
 *
 * All state is persisted in NVS for power-loss resilience.
 * LLM is called only for content generation, never for flow control.
 */
#ifndef CAP_DIARY_H
#define CAP_DIARY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- State Machine ---

typedef enum {
    DIARY_STATE_IDLE = 0,
    DIARY_STATE_PENDING = 1,
    DIARY_STATE_RUNNING = 2,
    DIARY_STATE_SYNCED_TO_CLOUD = 3,
    DIARY_STATE_TF_WRITTEN = 4,
    DIARY_STATE_COMPLETED = 5,
    DIARY_STATE_FAILED = 6,
} diary_state_t;

const char *diary_state_to_str(diary_state_t state);

// --- Core API ---

/**
 * Register diary capability group with esp-claw capability system.
 * Called from app_capabilities.c during startup.
 */
esp_err_t cap_diary_register_group(void);

/**
 * Manually trigger diary generation for a specific date.
 * If date_str is NULL, uses today's date from NTP.
 */
esp_err_t cap_diary_trigger(const char *date_str);

/**
 * Rewrite diary for a specific date.
 * Reloads today's conversation, calls LLM with rewrite context,
 * patches Supabase and overwrites TF card.
 */
esp_err_t cap_diary_rewrite(const char *date_str);

/**
 * Get current state for a date.
 */
esp_err_t cap_diary_get_state(const char *date_str, diary_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
