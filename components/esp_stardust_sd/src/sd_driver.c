/*
 * sd_driver.c - Minimal SPI block driver for SD/SDHC/SDXC cards.
 * Ported from Arduino to ESP-IDF C.
 */
#include "sd_driver.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd";

/* ---- SPI helpers ---- */

static uint8_t sd_spi_xfer(sd_driver_t *sd, uint8_t data)
{
    uint8_t rx;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .rx_buffer = &rx,
    };
    spi_device_polling_transmit(sd->spi, &t);
    return rx;
}

static void sd_cs_low(sd_driver_t *sd)
{
    gpio_set_level(sd->cs_pin, 0);
}

static void sd_cs_high(sd_driver_t *sd)
{
    gpio_set_level(sd->cs_pin, 1);
    sd_spi_xfer(sd, 0xFF);
}

static bool sd_wait_ready(sd_driver_t *sd, uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < (int64_t)timeout_ms) {
        if (sd_spi_xfer(sd, 0xFF) == 0xFF) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static void sd_deselect(sd_driver_t *sd)
{
    sd_cs_high(sd);
}

static void sd_select(sd_driver_t *sd)
{
    sd_cs_low(sd);
}

static uint32_t sd_cmd_addr(sd_driver_t *sd, uint32_t sector)
{
    return sd->block_addressing ? sector : sector * 512UL;
}

static uint8_t sd_send_cmd(sd_driver_t *sd, uint8_t cmd, uint32_t arg)
{
    sd_deselect(sd);
    sd_select(sd);
    if (!sd_wait_ready(sd, 500)) {
        sd_deselect(sd);
        return 0xFF;
    }

    sd_spi_xfer(sd, 0x40 | cmd);
    sd_spi_xfer(sd, (arg >> 24) & 0xFF);
    sd_spi_xfer(sd, (arg >> 16) & 0xFF);
    sd_spi_xfer(sd, (arg >> 8) & 0xFF);
    sd_spi_xfer(sd, arg & 0xFF);

    uint8_t crc = 0xFF;
    if (cmd == 0) crc = 0x95;
    else if (cmd == 8) crc = 0x87;
    sd_spi_xfer(sd, crc);

    uint8_t resp = 0xFF;
    for (int i = 0; i < 10; i++) {
        resp = sd_spi_xfer(sd, 0xFF);
        if ((resp & 0x80) == 0) break;
    }
    return resp;
}

/* ---- SPI bus configuration ---- */

static void sd_set_spi_freq(sd_driver_t *sd, int freq_hz)
{
    // Remove device and re-add with new frequency
    spi_bus_remove_device(sd->spi);
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = freq_hz,
        .spics_io_num = -1,  // manual CS
        .queue_size = 1,
    };
    spi_bus_add_device((spi_host_device_t)sd->spi_host, &dev_cfg, &sd->spi);
}

void sd_set_high_speed(sd_driver_t *sd)
{
    sd_set_spi_freq(sd, 10000000);  // 10 MHz
    ESP_LOGI(TAG, "High-speed mode (10 MHz)");
}

/* ---- Public API ---- */

bool sd_init(sd_driver_t *sd, spi_host_device_t spi_host,
             int cs_pin, int mosi_pin, int miso_pin, int sck_pin)
{
    memset(sd, 0, sizeof(*sd));
    sd->cs_pin = cs_pin;
    sd->spi_host = spi_host;

    // Configure CS pin
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << cs_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(cs_pin, 1);

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO));

    // Add device at 400 kHz for init
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 400000,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host, &dev_cfg, &sd->spi));

    // Send 80 dummy clocks
    for (int i = 0; i < 80; i++) sd_spi_xfer(sd, 0xFF);

    // CMD0: GO_IDLE_STATE
    bool cmd0_ok = false;
    for (int retry = 0; retry < 10; retry++) {
        if (sd_send_cmd(sd, 0, 0) == 0x01) {
            cmd0_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        sd_deselect(sd);
        for (int i = 0; i < 20; i++) sd_spi_xfer(sd, 0xFF);
    }
    if (!cmd0_ok) {
        ESP_LOGE(TAG, "CMD0 failed");
        return false;
    }
    ESP_LOGI(TAG, "CMD0 OK");

    // CMD8: SEND_IF_COND
    if (sd_send_cmd(sd, 8, 0x000001AA) != 0x01) {
        ESP_LOGE(TAG, "CMD8 failed");
        sd_deselect(sd);
        return false;
    }
    for (int i = 0; i < 4; i++) sd_spi_xfer(sd, 0xFF);
    sd_deselect(sd);
    ESP_LOGI(TAG, "CMD8 OK");

    // ACMD41: init
    bool acmd41_ok = false;
    for (int i = 0; i < 1000; i++) {
        sd_send_cmd(sd, 55, 0);
        sd_deselect(sd);
        if (sd_send_cmd(sd, 41, 0x40000000) == 0x00) {
            sd_deselect(sd);
            acmd41_ok = true;
            break;
        }
        sd_deselect(sd);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!acmd41_ok) {
        ESP_LOGE(TAG, "ACMD41 timeout");
        return false;
    }
    ESP_LOGI(TAG, "ACMD41 OK");

    // CMD58: Read OCR
    if (sd_send_cmd(sd, 58, 0) != 0x00) {
        ESP_LOGE(TAG, "CMD58 failed");
        sd_deselect(sd);
        return false;
    }
    uint8_t ocr0 = sd_spi_xfer(sd, 0xFF);
    uint8_t ocr1 = sd_spi_xfer(sd, 0xFF);
    uint8_t ocr2 = sd_spi_xfer(sd, 0xFF);
    uint8_t ocr3 = sd_spi_xfer(sd, 0xFF);
    sd_deselect(sd);

    sd->block_addressing = (ocr0 & 0x40) != 0;
    ESP_LOGI(TAG, "CMD58 OCR=%02X%02X%02X%02X addr=%s",
             ocr0, ocr1, ocr2, ocr3,
             sd->block_addressing ? "block" : "byte");

    ESP_LOGI(TAG, "SD init done");
    sd_set_high_speed(sd);
    return true;
}

bool sd_read_sector(sd_driver_t *sd, uint32_t sector, uint8_t *buf)
{
    if (sd_send_cmd(sd, 17, sd_cmd_addr(sd, sector)) != 0x00) {
        ESP_LOGE(TAG, "CMD17 fail LBA %lu", sector);
        sd_deselect(sd);
        return false;
    }

    uint16_t t = 10000;
    while (sd_spi_xfer(sd, 0xFF) != 0xFE && --t);
    if (t == 0) {
        ESP_LOGE(TAG, "Read token timeout LBA %lu", sector);
        sd_deselect(sd);
        return false;
    }

    // Read 512 bytes + 2 CRC bytes
    for (int i = 0; i < 512; i++) buf[i] = sd_spi_xfer(sd, 0xFF);
    sd_spi_xfer(sd, 0xFF);
    sd_spi_xfer(sd, 0xFF);
    sd_deselect(sd);
    return true;
}

bool sd_write_sector_raw(sd_driver_t *sd, uint32_t sector, const uint8_t *buf)
{
    if (sd_send_cmd(sd, 24, sd_cmd_addr(sd, sector)) != 0x00) {
        ESP_LOGE(TAG, "CMD24 fail LBA %lu", sector);
        sd_deselect(sd);
        return false;
    }

    for (int i = 0; i < 10; i++) sd_spi_xfer(sd, 0xFF);
    sd_spi_xfer(sd, 0xFE);

    for (int i = 0; i < 512; i++) sd_spi_xfer(sd, buf[i]);
    sd_spi_xfer(sd, 0xFF);
    sd_spi_xfer(sd, 0xFF);

    uint8_t resp = sd_spi_xfer(sd, 0xFF);
    if ((resp & 0x1F) != 0x05) {
        ESP_LOGE(TAG, "Data rejected 0x%02X LBA %lu", resp, sector);
        sd_deselect(sd);
        return false;
    }

    uint16_t t = 60000;
    while (sd_spi_xfer(sd, 0xFF) == 0x00 && --t);
    sd_wait_ready(sd, 500);
    sd_deselect(sd);
    if (t == 0) {
        ESP_LOGE(TAG, "Write busy timeout LBA %lu", sector);
        return false;
    }
    return true;
}

bool sd_write_sector(sd_driver_t *sd, uint32_t sector, const uint8_t *buf)
{
    uint8_t verify_buf[512];
    if (!sd_write_sector_raw(sd, sector, buf)) {
        ESP_LOGE(TAG, "Write fail LBA %lu", sector);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    for (int retry = 0; retry < 4; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Verify retry %d LBA %lu", retry, sector);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (!sd_read_sector(sd, sector, verify_buf)) {
            ESP_LOGE(TAG, "Verify read fail LBA %lu", sector);
            continue;
        }

        if (memcmp(verify_buf, buf, 512) == 0) {
            if (retry > 0) {
                ESP_LOGI(TAG, "Verify ok on retry %d LBA %lu", retry, sector);
            }
            return true;
        }

        ESP_LOGD(TAG, "LBA %lu verify mismatch:", sector);
        ESP_LOGD(TAG, "  Write: %02X%02X%02X%02X ...",
                 buf[0], buf[1], buf[2], buf[3]);
        ESP_LOGD(TAG, "  Read:  %02X%02X%02X%02X ...",
                 verify_buf[0], verify_buf[1], verify_buf[2], verify_buf[3]);
    }

    ESP_LOGE(TAG, "Data verify mismatch LBA %lu (4 retries)", sector);
    return false;
}

bool sd_wait_card_ready(sd_driver_t *sd, uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000) - start < (int64_t)timeout_ms) {
        sd_deselect(sd);
        sd_select(sd);
        if (!sd_wait_ready(sd, 500)) {
            sd_deselect(sd);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        sd_spi_xfer(sd, 0x40 | 13);
        sd_spi_xfer(sd, 0x00);
        sd_spi_xfer(sd, 0x00);
        sd_spi_xfer(sd, 0x00);
        sd_spi_xfer(sd, 0x00);
        sd_spi_xfer(sd, 0xFF);

        uint8_t resp = 0xFF;
        for (int i = 0; i < 10; i++) {
            resp = sd_spi_xfer(sd, 0xFF);
            if ((resp & 0x80) == 0) break;
        }
        sd_spi_xfer(sd, 0xFF);
        sd_spi_xfer(sd, 0xFF);
        sd_deselect(sd);

        if (resp != 0xFF) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
