/*
 * SPDX-FileCopyrightText: 2026 Stardust Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-level custom device init/deinit stubs for ESP32-S3-DevKitC-1 Stardust.
 *
 * Currently only the WS2812 LED strip is a "custom" device.
 * TF card and OLED are initialized directly by their respective components.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"
#include "periph_rmt.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"
