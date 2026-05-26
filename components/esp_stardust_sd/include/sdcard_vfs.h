/*
 * sdcard_vfs.h - ESP-IDF VFS adapter for Stardust SD card.
 *
 * Translates POSIX paths (e.g. /sdcard/diaries/2026-05-21.md)
 * into FAT32FS 8.3 root-directory operations.
 */
#ifndef SDCARD_VFS_H
#define SDCARD_VFS_H

#include <stdbool.h>
#include "sd_driver.h"
#include "fat32fs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SD card and mount at the given VFS path.
 *
 * @param mount_point  VFS mount point, e.g. "/sdcard"
 * @param spi_host     SPI host (SPI2_HOST or SPI3_HOST)
 * @param cs_pin       Chip-select GPIO
 * @param mosi_pin     MOSI GPIO
 * @param miso_pin     MISO GPIO
 * @param sck_pin      SCK GPIO
 * @return true on success
 */
bool sdcard_vfs_mount(const char *mount_point,
                      int spi_host,
                      int cs_pin, int mosi_pin, int miso_pin, int sck_pin);

/**
 * Unmount and deinitialize.
 */
void sdcard_vfs_unmount(void);

#ifdef __cplusplus
}
#endif

#endif
