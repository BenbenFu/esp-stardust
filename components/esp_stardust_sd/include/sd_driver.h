/*
 * sd_driver.h - Minimal SPI block driver for SD/SDHC/SDXC cards.
 * Ported from Arduino to ESP-IDF C.
 */
#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_device_handle_t spi;
    int spi_host;
    uint8_t sec_buf[512];
    bool block_addressing;
    int cs_pin;
} sd_driver_t;

/**
 * Initialize SD driver with given pins.
 * spi_host: SPI2_HOST or SPI3_HOST
 * Returns true on success.
 */
bool sd_init(sd_driver_t *sd, spi_host_device_t spi_host,
             int cs_pin, int mosi_pin, int miso_pin, int sck_pin);

/**
 * Read one 512-byte sector.
 */
bool sd_read_sector(sd_driver_t *sd, uint32_t sector, uint8_t *buf);

/**
 * Write one 512-byte sector with verify.
 */
bool sd_write_sector(sd_driver_t *sd, uint32_t sector, const uint8_t *buf);

/**
 * Write one 512-byte sector without verify (raw).
 */
bool sd_write_sector_raw(sd_driver_t *sd, uint32_t sector, const uint8_t *buf);

/**
 * Wait for card to become ready (CMD13 polling).
 */
bool sd_wait_card_ready(sd_driver_t *sd, uint32_t timeout_ms);

/**
 * Switch card to high-speed mode (10 MHz).
 */
void sd_set_high_speed(sd_driver_t *sd);

#ifdef __cplusplus
}
#endif

#endif
