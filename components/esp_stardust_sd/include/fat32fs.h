/*
 * fat32fs.h - Minimal FAT32 read/write filesystem.
 * Ported from Arduino to ESP-IDF C. Root-directory only (no subdirectories).
 */
#ifndef FAT32FS_H
#define FAT32FS_H

#include <stdint.h>
#include <stdbool.h>
#include "sd_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAT32_MAX_DIR_ENTRIES 100
#define FAT32_MAX_FNAME 13       /* 8.3 + null */

typedef struct {
    sd_driver_t *sd;

    /* DBR / geometry */
    uint32_t part_start;
    uint16_t rsvd_sec_cnt;
    uint8_t  sec_per_clus;
    uint8_t  num_fats;
    uint32_t total_sec;
    uint32_t fat_sz32;
    uint32_t root_clus;
    uint32_t total_clusters;
    uint32_t alloc_start_cluster;

    /* Computed absolute sectors */
    uint32_t abs_fat1;
    uint32_t abs_fat2;
    uint32_t abs_data;

    /* FSInfo */
    uint32_t fsi_sector_rel;
    uint32_t fsi_free_count;
    uint32_t fsi_next_free;

    /* Directory cache */
    char cached_names[FAT32_MAX_DIR_ENTRIES][FAT32_MAX_FNAME];
    int  cached_count;
    bool cache_valid;
} fat32fs_t;

/**
 * Mount the FAT32 filesystem on top of an already-initialized SD driver.
 * Returns true on success.
 */
bool fat32_mount(fat32fs_t *fs, sd_driver_t *sd);

/**
 * Write a file in the root directory (8.3 name).
 */
bool fat32_write_file(fat32fs_t *fs, const char *name8, const char *ext3,
                      const char *data, uint16_t len);

/**
 * Read a file from the root directory into buf.
 * Returns number of bytes read (0 on failure).
 */
uint16_t fat32_read_file(fat32fs_t *fs, const char *name8, const char *ext3,
                         char *buf, uint16_t max_len);

/**
 * Check if a file exists.
 */
bool fat32_file_exists(fat32fs_t *fs, const char *name8, const char *ext3);

/**
 * List files with given extension in the root directory.
 * ext_filter: "MD" for diaries, "TXT" for answers, NULL for all.
 * Returns number of files found.
 */
int fat32_list_files(fat32fs_t *fs, const char *ext_filter,
                     char names[][FAT32_MAX_FNAME], int max_count);

/**
 * Get total file count (cached).
 */
int fat32_get_file_count(fat32fs_t *fs, const char *ext_filter);

/**
 * Lightweight stat: look up file size without reading content.
 * Returns true if file exists, writes size to *out_size.
 */
bool fat32_stat_file(fat32fs_t *fs, const char *name8, const char *ext3,
                     uint32_t *out_size);

/**
 * Walk root directory entries with a callback.
 * callback receives: entry pointer (32 bytes), user data.
 * Return false from callback to stop walking.
 */
typedef bool (*fat32_dir_callback_t)(const uint8_t entry[32], void *user_data);
void fat32_walk_root_dir(fat32fs_t *fs, fat32_dir_callback_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
