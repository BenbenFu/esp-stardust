/*
 * fat32fs.c - Minimal FAT32 read/write filesystem.
 * Ported from Arduino to ESP-IDF C.
 */
#include "fat32fs.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fat32";

/* ---- 8.3 name padding helpers ---- */

static void pad83(char padded[8], const char *name)
{
    memset(padded, ' ', 8);
    for (int i = 0; i < 8 && name[i] && name[i] != ' '; i++) padded[i] = name[i];
}

static void pad_ext3(char padded[3], const char *ext)
{
    memset(padded, ' ', 3);
    for (int i = 0; i < 3 && ext[i] && ext[i] != ' '; i++) padded[i] = ext[i];
}

/* ---- FAT entry helpers ---- */

static uint32_t fat_cluster_to_sector(fat32fs_t *fs, uint32_t cluster)
{
    return fs->abs_data + (cluster - 2) * fs->sec_per_clus;
}

static uint32_t fat_read_entry_from(fat32fs_t *fs, uint32_t fat_base, uint32_t cluster)
{
    uint32_t off = cluster * 4;
    uint32_t sec = fat_base + off / 512;
    uint32_t o = off % 512;
    if (!sd_read_sector(fs->sd, sec, fs->sd->sec_buf)) return 0xFFFFFFFF;
    return (fs->sd->sec_buf[o] | (fs->sd->sec_buf[o + 1] << 8) |
            (fs->sd->sec_buf[o + 2] << 16) | (fs->sd->sec_buf[o + 3] << 24)) & 0x0FFFFFFF;
}

static uint32_t fat_read_entry(fat32fs_t *fs, uint32_t cluster)
{
    return fat_read_entry_from(fs, fs->abs_fat1, cluster);
}

static bool fat_write_entry(fat32fs_t *fs, uint32_t cluster, uint32_t value)
{
    uint8_t buf[512];
    uint32_t max_c = fs->total_clusters + 1;
    if (cluster < 2 || cluster > max_c) {
        ESP_LOGE(TAG, "Cluster %lu out of range (max %lu)", cluster, max_c);
        return false;
    }

    uint32_t off = cluster * 4;
    uint32_t f1s = fs->abs_fat1 + off / 512;
    uint32_t f2s = fs->abs_fat2 + off / 512;
    uint32_t o = off % 512;
    value &= 0x0FFFFFFF;

    if (f1s < fs->abs_fat1 || f1s >= fs->abs_fat1 + fs->fat_sz32) return false;
    if (f2s < fs->abs_fat2 || f2s >= fs->abs_fat2 + fs->fat_sz32) return false;

    // Write FAT1
    if (!sd_read_sector(fs->sd, f1s, buf)) return false;
    buf[o] = value & 0xFF;
    buf[o + 1] = (value >> 8) & 0xFF;
    buf[o + 2] = (value >> 16) & 0xFF;
    buf[o + 3] = (buf[o + 3] & 0xF0) | ((value >> 24) & 0x0F);
    if (!sd_write_sector(fs->sd, f1s, buf)) return false;
    if (fat_read_entry(fs, cluster) != value) {
        ESP_LOGE(TAG, "FAT1 verify fail cluster=%lu", cluster);
        return false;
    }

    sd_wait_card_ready(fs->sd, 500);

    // Write FAT2 (mirror)
    if (!sd_read_sector(fs->sd, f2s, buf)) return false;
    buf[o] = value & 0xFF;
    buf[o + 1] = (value >> 8) & 0xFF;
    buf[o + 2] = (value >> 16) & 0xFF;
    buf[o + 3] = (buf[o + 3] & 0xF0) | ((value >> 24) & 0x0F);

    if (!sd_write_sector(fs->sd, f2s, buf)) {
        ESP_LOGE(TAG, "FAT2 write fail cluster=%lu", cluster);
        return false;
    }

    sd_wait_card_ready(fs->sd, 500);
    bool fat2_ok = false;
    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) sd_wait_card_ready(fs->sd, 500);
        if (!sd_read_sector(fs->sd, f2s, buf)) continue;
        uint32_t verify = (buf[o] | (buf[o + 1] << 8) |
                           (buf[o + 2] << 16) | (buf[o + 3] << 24)) & 0x0FFFFFFF;
        if (verify == value) { fat2_ok = true; break; }
        ESP_LOGW(TAG, "FAT2 verify retry %d cluster=%lu", retry, cluster);
    }
    if (!fat2_ok) {
        ESP_LOGW(TAG, "FAT2 inconsistent cluster=%lu (FAT1 OK)", cluster);
    }
    return true;
}

static bool fat_check_header(fat32fs_t *fs)
{
    uint32_t fat0 = fat_read_entry(fs, 0);
    uint32_t fat1 = fat_read_entry(fs, 1);
    uint32_t fat_root = fat_read_entry(fs, fs->root_clus);

    bool ok0 = (fat0 & 0x0FFFFFF8) == 0x0FFFFFF8;
    bool ok1 = fat1 >= 0x0FFFFFF8;
    bool ok_root = fat_root >= 0x0FFFFFF8;
    if (!ok0 || !ok1 || !ok_root) {
        ESP_LOGE(TAG, "FAT header damaged FAT0=%08lX FAT1=%08lX root=%08lX",
                 fat0, fat1, fat_root);
        return false;
    }
    return true;
}

static uint32_t fat_find_free_cluster(fat32fs_t *fs)
{
    uint32_t max_c = fs->total_clusters + 1;
    for (uint32_t s = 0; s < fs->fat_sz32; s++) {
        if (!sd_read_sector(fs->sd, fs->abs_fat1 + s, fs->sd->sec_buf)) return 0;
        for (int i = 0; i < 128; i++) {
            uint32_t c = s * 128 + i;
            if (c > max_c) return 0;
            if (c < fs->alloc_start_cluster || c == fs->root_clus) continue;

            uint32_t e = (fs->sd->sec_buf[i * 4] | (fs->sd->sec_buf[i * 4 + 1] << 8) |
                          (fs->sd->sec_buf[i * 4 + 2] << 16) | (fs->sd->sec_buf[i * 4 + 3] << 24)) & 0x0FFFFFFF;
            if (e == 0) return c;
            if (e == 0x0FFFFFF7) continue;
        }
    }
    return 0;
}

/* ---- FSInfo ---- */

static bool fat_read_fsinfo(fat32fs_t *fs)
{
    uint32_t fsi_abs = fs->part_start + fs->fsi_sector_rel;
    if (!sd_read_sector(fs->sd, fsi_abs, fs->sd->sec_buf)) {
        fs->fsi_free_count = 0xFFFFFFFF;
        fs->fsi_next_free = 0xFFFFFFFF;
        return false;
    }

    uint32_t lead_sig = fs->sd->sec_buf[0] | (fs->sd->sec_buf[1] << 8) |
                        (fs->sd->sec_buf[2] << 16) | (fs->sd->sec_buf[3] << 24);
    uint32_t struc_sig = fs->sd->sec_buf[0x1E4] | (fs->sd->sec_buf[0x1E5] << 8) |
                         (fs->sd->sec_buf[0x1E6] << 16) | (fs->sd->sec_buf[0x1E7] << 24);

    if (lead_sig != 0x52526141 || struc_sig != 0x72724161 ||
        fs->sd->sec_buf[0x1FE] != 0x55 || fs->sd->sec_buf[0x1FF] != 0xAA) {
        fs->fsi_free_count = 0xFFFFFFFF;
        fs->fsi_next_free = 0xFFFFFFFF;
        return false;
    }

    fs->fsi_free_count = fs->sd->sec_buf[0x1E8] | (fs->sd->sec_buf[0x1E9] << 8) |
                         (fs->sd->sec_buf[0x1EA] << 16) | (fs->sd->sec_buf[0x1EB] << 24);
    fs->fsi_next_free = fs->sd->sec_buf[0x1EC] | (fs->sd->sec_buf[0x1ED] << 8) |
                        (fs->sd->sec_buf[0x1EE] << 16) | (fs->sd->sec_buf[0x1EF] << 24);

    ESP_LOGI(TAG, "FSInfo: free=%lu nextFree=%lu",
             fs->fsi_free_count == 0xFFFFFFFF ? 0xFFFFFFFFUL : fs->fsi_free_count,
             fs->fsi_next_free == 0xFFFFFFFF ? 0xFFFFFFFFUL : fs->fsi_next_free);
    return true;
}

static bool fat_write_fsinfo(fat32fs_t *fs)
{
    uint32_t fsi_abs = fs->part_start + fs->fsi_sector_rel;
    if (!sd_read_sector(fs->sd, fsi_abs, fs->sd->sec_buf)) return false;

    fs->sd->sec_buf[0x1E8] = fs->fsi_free_count & 0xFF;
    fs->sd->sec_buf[0x1E9] = (fs->fsi_free_count >> 8) & 0xFF;
    fs->sd->sec_buf[0x1EA] = (fs->fsi_free_count >> 16) & 0xFF;
    fs->sd->sec_buf[0x1EB] = (fs->fsi_free_count >> 24) & 0xFF;

    fs->sd->sec_buf[0x1EC] = fs->fsi_next_free & 0xFF;
    fs->sd->sec_buf[0x1ED] = (fs->fsi_next_free >> 8) & 0xFF;
    fs->sd->sec_buf[0x1EE] = (fs->fsi_next_free >> 16) & 0xFF;
    fs->sd->sec_buf[0x1EF] = (fs->fsi_next_free >> 24) & 0xFF;

    if (!sd_write_sector(fs->sd, fsi_abs, fs->sd->sec_buf)) return false;
    return true;
}

static void fat_update_fsinfo_after_alloc(fat32fs_t *fs, uint32_t cluster)
{
    if (fs->fsi_free_count != 0xFFFFFFFF && fs->fsi_free_count > 0) {
        fs->fsi_free_count--;
    }
    if (cluster + 1 <= fs->total_clusters + 1) {
        fs->fsi_next_free = cluster + 1;
    } else {
        fs->fsi_next_free = 0xFFFFFFFF;
    }
}

/* ---- Directory operations ---- */

static bool fat_find_file(fat32fs_t *fs, const char *name8, const char *ext3,
                           uint32_t *out_cluster, uint32_t *out_size)
{
    char pn[8], pe_ext[3];
    pad83(pn, name8);
    pad_ext3(pe_ext, ext3);
    uint32_t cur_clus = fs->root_clus;
    while (cur_clus >= 2 && cur_clus < 0x0FFFFFF8) {
        uint32_t root_sec = fat_cluster_to_sector(fs, cur_clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) return false;
            for (int i = 0; i < 16; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                uint8_t fb = ent[0];
                if (fb == 0x00) return false;
                if (fb == 0xE5) continue;
                if (ent[11] == 0x0F) continue;

                if (memcmp(ent, pn, 8) == 0 && memcmp(ent + 8, pe_ext, 3) == 0) {
                    *out_cluster = ((uint32_t)ent[20] << 16) | ((uint32_t)ent[21] << 24) |
                                   ent[26] | ((uint32_t)ent[27] << 8);
                    *out_size = ent[28] | ((uint32_t)ent[29] << 8) |
                                ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                    return true;
                }
            }
        }
        cur_clus = fat_read_entry(fs, cur_clus);
    }
    return false;
}

static void fat_invalidate_cache(fat32fs_t *fs)
{
    fs->cache_valid = false;
}

static bool fat_check_is_diary_entry(const uint8_t *ent, const char *ext_filter)
{
    // Check extension matches
    if (ext_filter) {
        if (ent[8] != ext_filter[0] || ent[9] != ext_filter[1] || ent[10] != ext_filter[2]) {
            return false;
        }
    }

    // For .MD or .TXT: check if it's a diary file (8 digit date name)
    if (!ext_filter || strcmp(ext_filter, "MD") == 0 || strcmp(ext_filter, "TXT") == 0) {
        int digit_count = 0;
        while (digit_count < 8 && ent[digit_count] >= '0' && ent[digit_count] <= '9') digit_count++;
        bool is_diary = (digit_count == 8) || (ent[0] == 'T' && ent[7] == 'Z');
        if (!is_diary) return false;
        return true;
    }

    return true;
}

static void fat_refresh_cache(fat32fs_t *fs, const char *ext_filter)
{
    if (fs->cache_valid) return;

    fs->cached_count = 0;
    bool done = false;
    uint32_t cur_clus = fs->root_clus;

    while (!done && cur_clus >= 2 && cur_clus < 0x0FFFFFF8 &&
           fs->cached_count < FAT32_MAX_DIR_ENTRIES)
    {
        uint32_t root_sec = fat_cluster_to_sector(fs, cur_clus);
        for (uint32_t s = 0; s < fs->sec_per_clus && !done &&
             fs->cached_count < FAT32_MAX_DIR_ENTRIES; s++)
        {
            if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) {
                done = true; break;
            }
            for (int i = 0; i < 16 && fs->cached_count < FAT32_MAX_DIR_ENTRIES; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                uint8_t fb = ent[0];
                if (fb == 0x00) { done = true; break; }
                if (fb == 0xE5) continue;
                if (ent[11] == 0x0F) continue;

                if (!fat_check_is_diary_entry(ent, ext_filter)) continue;

                char name[FAT32_MAX_FNAME];
                int ni = 0;
                for (int j = 0; j < 8 && ent[j] != ' '; j++) name[ni++] = ent[j];
                name[ni++] = '.';
                for (int j = 8; j < 11 && ent[j] != ' '; j++) name[ni++] = ent[j];
                name[ni] = '\0';

                strncpy(fs->cached_names[fs->cached_count], name, FAT32_MAX_FNAME - 1);
                fs->cached_names[fs->cached_count][FAT32_MAX_FNAME - 1] = '\0';
                fs->cached_count++;
            }
        }
        if (!done) cur_clus = fat_read_entry(fs, cur_clus);
    }

    // Bubble sort
    for (int i = 0; i < fs->cached_count - 1; i++) {
        for (int j = 0; j < fs->cached_count - 1 - i; j++) {
            if (strcmp(fs->cached_names[j], fs->cached_names[j + 1]) > 0) {
                char tmp[FAT32_MAX_FNAME];
                strcpy(tmp, fs->cached_names[j]);
                strcpy(fs->cached_names[j], fs->cached_names[j + 1]);
                strcpy(fs->cached_names[j + 1], tmp);
            }
        }
    }
    fs->cache_valid = true;
    ESP_LOGI(TAG, "Cached %d files (ext=%s)", fs->cached_count,
             ext_filter ? ext_filter : "all");
}

/* ---- Verify & cleanup ---- */

static bool fat_verify_file(fat32fs_t *fs, uint32_t dir_sector, int dir_slot,
                             uint32_t first_cluster, uint32_t size)
{
    uint32_t fat_value = fat_read_entry(fs, first_cluster);
    if (fat_value < 0x0FFFFFF8) {
        ESP_LOGE(TAG, "FAT verify fail cluster=%lu value=0x%08lX", first_cluster, fat_value);
        return false;
    }

    if (!sd_read_sector(fs->sd, dir_sector, fs->sd->sec_buf)) return false;
    uint8_t *p = &fs->sd->sec_buf[dir_slot * 32];
    uint32_t dir_cluster = ((uint32_t)p[20] << 16) | ((uint32_t)p[21] << 24) |
                           p[26] | ((uint32_t)p[27] << 8);
    uint32_t dir_size = p[28] | ((uint32_t)p[29] << 8) |
                        ((uint32_t)p[30] << 16) | ((uint32_t)p[31] << 24);
    if (dir_cluster != first_cluster || dir_size != size) {
        ESP_LOGE(TAG, "DIR verify fail cluster=%lu/%lu size=%lu/%lu",
                 dir_cluster, first_cluster, dir_size, size);
        return false;
    }
    return true;
}

static void fat_cleanup_failed_write(fat32fs_t *fs, uint32_t cluster,
                                      uint32_t slot_sector, int slot)
{
    ESP_LOGI(TAG, "Cleanup: cluster=%lu", cluster);

    if (sd_read_sector(fs->sd, slot_sector, fs->sd->sec_buf)) {
        fs->sd->sec_buf[slot * 32] = 0xE5;
        sd_write_sector_raw(fs->sd, slot_sector, fs->sd->sec_buf);
    }

    uint32_t max_c = fs->total_clusters + 1;
    if (cluster > 1 && cluster <= max_c) {
        fat_write_entry(fs, cluster, 0x0FFFFFF7);
    }
}

/* ---- DBR / MBR parsing ---- */

static bool fat_parse_mbr(fat32fs_t *fs)
{
    if (!sd_read_sector(fs->sd, 0, fs->sd->sec_buf)) return false;
    if (fs->sd->sec_buf[510] != 0x55 || fs->sd->sec_buf[511] != 0xAA) {
        ESP_LOGE(TAG, "Invalid MBR signature");
        return false;
    }

    for (int i = 0; i < 4; i++) {
        int base = 0x1BE + i * 16;
        uint8_t pt = fs->sd->sec_buf[base + 4];
        if (pt == 0x0B || pt == 0x0C) {
            fs->part_start = fs->sd->sec_buf[base + 8] | (fs->sd->sec_buf[base + 9] << 8) |
                             (fs->sd->sec_buf[base + 10] << 16) | (fs->sd->sec_buf[base + 11] << 24);
            ESP_LOGI(TAG, "MBR part%d type=0x%02X LBA=%lu", i, pt, fs->part_start);
            return true;
        }
    }

    ESP_LOGE(TAG, "No FAT32 partition found");
    return false;
}

static bool fat_parse_dbr(fat32fs_t *fs)
{
    if (!sd_read_sector(fs->sd, fs->part_start, fs->sd->sec_buf)) return false;

    fs->rsvd_sec_cnt = fs->sd->sec_buf[14] | (fs->sd->sec_buf[15] << 8);
    fs->sec_per_clus = fs->sd->sec_buf[13];
    fs->num_fats = fs->sd->sec_buf[16];
    fs->total_sec = fs->sd->sec_buf[32] | (fs->sd->sec_buf[33] << 8) |
                    (fs->sd->sec_buf[34] << 16) | (fs->sd->sec_buf[35] << 24);
    fs->fat_sz32 = fs->sd->sec_buf[36] | (fs->sd->sec_buf[37] << 8) |
                   (fs->sd->sec_buf[38] << 16) | (fs->sd->sec_buf[39] << 24);
    fs->root_clus = fs->sd->sec_buf[44] | (fs->sd->sec_buf[45] << 8) |
                    (fs->sd->sec_buf[46] << 16) | (fs->sd->sec_buf[47] << 24);

    uint16_t bpb_fsinfo = fs->sd->sec_buf[48] | (fs->sd->sec_buf[49] << 8);
    fs->fsi_sector_rel = (bpb_fsinfo > 0) ? bpb_fsinfo : 1;

    ESP_LOGI(TAG, "DBR: Rsvd=%u FATSz=%lu SPC=%u NF=%u Root=%lu",
             fs->rsvd_sec_cnt, fs->fat_sz32, fs->sec_per_clus, fs->num_fats, fs->root_clus);

    if (fs->sec_per_clus == 0 || fs->fat_sz32 == 0 || fs->num_fats == 0 || fs->root_clus < 2) {
        ESP_LOGE(TAG, "Invalid DBR parameters");
        return false;
    }

    uint32_t data_sec = fs->total_sec - (fs->rsvd_sec_cnt + fs->num_fats * fs->fat_sz32);
    fs->total_clusters = data_sec / fs->sec_per_clus;
    ESP_LOGI(TAG, "DBR: TotalSec=%lu DataSec=%lu Clusters=%lu",
             fs->total_sec, data_sec, fs->total_clusters);
    return true;
}

static bool fat_init_tables(fat32fs_t *fs)
{
    uint8_t buf[512];
    bool needs_init = false;

    if (!sd_read_sector(fs->sd, fs->abs_fat1, buf)) return false;
    if (!(buf[0] == 0xF8 && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF)) {
        needs_init = true;
    }

    if (!needs_init) {
        uint32_t fat1 = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
        if ((fat1 & 0x0FFFFFFF) != 0x0FFFFFFF) needs_init = true;
    }

    if (!needs_init) {
        uint32_t root_off = fs->root_clus * 4;
        uint32_t root_sec_off = root_off / 512;
        uint32_t root_off_in_sec = root_off % 512;
        if (root_sec_off > 0) {
            if (!sd_read_sector(fs->sd, fs->abs_fat1 + root_sec_off, buf)) return false;
        }
        uint32_t root_entry = (buf[root_off_in_sec] | (buf[root_off_in_sec + 1] << 8) |
                               (buf[root_off_in_sec + 2] << 16) | (buf[root_off_in_sec + 3] << 24)) & 0x0FFFFFFF;
        if (root_entry != 0x0FFFFFFF) needs_init = true;
    }

    if (!needs_init) {
        ESP_LOGI(TAG, "FAT already valid, skipping init");
        return true;
    }

    ESP_LOGI(TAG, "FAT needs init, fixing critical entries");

    memset(buf, 0, 512);
    buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0x0F;
    if (!sd_write_sector(fs->sd, fs->abs_fat1, buf)) return false;
    if (!sd_write_sector(fs->sd, fs->abs_fat2, buf)) {
        ESP_LOGW(TAG, "FAT2 sector 0 write failed during init (continuing with FAT1)");
    }

    uint32_t root_off = fs->root_clus * 4;
    uint32_t root_sec = fs->abs_fat1 + root_off / 512;
    uint32_t root_off_in_sec = root_off % 512;
    if (!sd_read_sector(fs->sd, root_sec, buf)) return false;
    buf[root_off_in_sec] = 0xFF;
    buf[root_off_in_sec + 1] = 0xFF;
    buf[root_off_in_sec + 2] = 0xFF;
    buf[root_off_in_sec + 3] = 0x0F;
    if (!sd_write_sector(fs->sd, root_sec, buf)) return false;
    if (!sd_write_sector(fs->sd, fs->abs_fat2 + root_off / 512, buf)) {
        ESP_LOGW(TAG, "FAT[root] FAT2 write failed (continuing with FAT1)");
    }

    return true;
}

/* ---- Public API ---- */

bool fat32_mount(fat32fs_t *fs, sd_driver_t *sd)
{
    memset(fs, 0, sizeof(*fs));
    fs->sd = sd;
    fs->cache_valid = false;
    fs->alloc_start_cluster = 256;

    if (!fat_parse_mbr(fs)) {
        ESP_LOGE(TAG, "MBR parse failed");
        return false;
    }
    if (!fat_parse_dbr(fs)) {
        ESP_LOGE(TAG, "DBR parse failed");
        return false;
    }

    fs->abs_fat1 = fs->part_start + fs->rsvd_sec_cnt;
    fs->abs_fat2 = fs->abs_fat1 + fs->fat_sz32;
    fs->abs_data = fs->abs_fat2 + fs->fat_sz32;

    ESP_LOGI(TAG, "FAT1=%lu FAT2=%lu DATA=%lu", fs->abs_fat1, fs->abs_fat2, fs->abs_data);

    sd_wait_card_ready(fs->sd, 1000);

    if (!fat_init_tables(fs)) {
        ESP_LOGE(TAG, "FAT init failed");
        return false;
    }

    fat_read_fsinfo(fs);
    ESP_LOGI(TAG, "FAT32 ready");
    return true;
}

bool fat32_file_exists(fat32fs_t *fs, const char *name8, const char *ext3)
{
    char pn[8], pe_ext[3];
    pad83(pn, name8);
    pad_ext3(pe_ext, ext3);
    uint32_t cur_clus = fs->root_clus;
    while (cur_clus >= 2 && cur_clus < 0x0FFFFFF8) {
        uint32_t root_sec = fat_cluster_to_sector(fs, cur_clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) return false;
            for (int i = 0; i < 16; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                uint8_t fb = ent[0];
                if (fb == 0x00) return false;
                if (fb == 0xE5) continue;
                if (memcmp(ent, pn, 8) == 0 && memcmp(ent + 8, pe_ext, 3) == 0) {
                    return true;
                }
            }
        }
        cur_clus = fat_read_entry(fs, cur_clus);
    }
    return false;
}

uint16_t fat32_read_file(fat32fs_t *fs, const char *name8, const char *ext3,
                          char *buf, uint16_t max_len)
{
    uint32_t first_cluster = 0, file_size = 0;
    if (!fat_find_file(fs, name8, ext3, &first_cluster, &file_size)) {
        ESP_LOGD(TAG, "File %.8s.%.3s not found", name8, ext3);
        return 0;
    }

    uint16_t to_read = (file_size < (uint32_t)(max_len - 1)) ? file_size : (max_len - 1);
    uint16_t read_bytes = 0;
    uint32_t current_cluster = first_cluster;

    while (read_bytes < to_read) {
        uint32_t sec = fat_cluster_to_sector(fs, current_cluster);
        for (int s = 0; s < fs->sec_per_clus && read_bytes < to_read; s++) {
            if (!sd_read_sector(fs->sd, sec + s, (uint8_t *)buf + read_bytes)) {
                buf[read_bytes] = '\0';
                return read_bytes;
            }
            uint16_t chunk = 512;
            if (read_bytes + chunk > to_read) chunk = to_read - read_bytes;
            read_bytes += chunk;
        }
        current_cluster = fat_read_entry(fs, current_cluster);
        if (current_cluster >= 0x0FFFFFF8) break;
    }

    buf[read_bytes] = '\0';

    // Strip UTF-8 BOM
    if (read_bytes >= 3 && (uint8_t)buf[0] == 0xEF &&
        (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) {
        memmove(buf, buf + 3, read_bytes - 3 + 1);
        read_bytes -= 3;
    }

    return read_bytes;
}

bool fat32_write_file(fat32fs_t *fs, const char *name8, const char *ext3,
                       const char *data, uint16_t len)
{
    ESP_LOGI(TAG, "writeFile: %.8s.%.3s (%u bytes)", name8, ext3, len);

    // Strip UTF-8 BOM
    const char *write_data = data;
    char norm_buf[512];
    bool has_bom = (len >= 3 && (uint8_t)data[0] == 0xEF &&
                    (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF);
    if (has_bom && (len - 3) < (uint16_t)sizeof(norm_buf)) {
        memcpy(norm_buf, data + 3, len - 3);
        norm_buf[len - 3] = '\0';
        write_data = norm_buf;
        len = len - 3;
    }

    if (!fat_check_header(fs)) {
        ESP_LOGE(TAG, "FAT header invalid, aborting write");
        return false;
    }

    fat_invalidate_cache(fs);

    uint32_t root_sec = fat_cluster_to_sector(fs, fs->root_clus);
    int empty_slot = -1;
    uint32_t slot_sector = 0;
    char pn[8], pe_ext[3];
    pad83(pn, name8);
    pad_ext3(pe_ext, ext3);

    for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
        if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) return false;
        for (int i = 0; i < 16; i++) {
            uint8_t *ent = &fs->sd->sec_buf[i * 32];
            uint8_t fb = ent[0];
            if (fb == 0x00) {
                if (empty_slot < 0) { empty_slot = i; slot_sector = root_sec + s; }
                s = fs->sec_per_clus; break;
            }
            if (fb == 0xE5) {
                if (empty_slot < 0) { empty_slot = i; slot_sector = root_sec + s; }
                continue;
            }
            if (memcmp(ent, pn, 8) == 0 && memcmp(ent + 8, pe_ext, 3) == 0) {
                ESP_LOGI(TAG, "SKIP: %.8s.%.3s exists", name8, ext3);
                return false;
            }
        }
    }

    if (empty_slot < 0) {
        ESP_LOGE(TAG, "No directory slot");
        return false;
    }

    uint32_t fc = fat_find_free_cluster(fs);
    if (fc == 0) { ESP_LOGE(TAG, "Disk full"); return false; }

    uint32_t c_sec = fat_cluster_to_sector(fs, fc);
    uint16_t secs_needed = (len + 511) / 512;
    if (secs_needed == 0) secs_needed = 1;
    if (secs_needed > fs->sec_per_clus) {
        ESP_LOGE(TAG, "Content > 1 cluster"); return false;
    }

    for (uint16_t s = 0; s < secs_needed; s++) {
        memset(fs->sd->sec_buf, 0, 512);
        uint16_t off = s * 512;
        uint16_t n = len - off;
        if (n > 512) n = 512;
        memcpy(fs->sd->sec_buf, write_data + off, n);
        if (!sd_write_sector(fs->sd, c_sec + s, fs->sd->sec_buf)) {
            fat_cleanup_failed_write(fs, fc, slot_sector, empty_slot);
            return false;
        }
    }

    if (!fat_write_entry(fs, fc, 0x0FFFFFFF)) {
        fat_cleanup_failed_write(fs, fc, slot_sector, empty_slot);
        return false;
    }

    if (!sd_read_sector(fs->sd, slot_sector, fs->sd->sec_buf)) return false;
    uint8_t *p = &fs->sd->sec_buf[empty_slot * 32];
    memset(p, 0, 32);
    memcpy(p, name8, 8);
    memcpy(p + 8, ext3, 3);
    p[11] = 0x20;

    // Simple FAT timestamp (12:00:00, 2026-05-10)
    uint16_t fat_time = (12 << 11);
    uint16_t fat_date = ((2026 - 1980) << 9) | (5 << 5) | 10;
    p[14] = fat_time & 0xFF;       p[15] = (fat_time >> 8) & 0xFF;
    p[16] = fat_date & 0xFF;       p[17] = (fat_date >> 8) & 0xFF;
    p[18] = fat_date & 0xFF;       p[19] = (fat_date >> 8) & 0xFF;
    p[22] = fat_time & 0xFF;       p[23] = (fat_time >> 8) & 0xFF;
    p[24] = fat_date & 0xFF;       p[25] = (fat_date >> 8) & 0xFF;

    p[20] = (fc >> 16) & 0xFF;     p[21] = (fc >> 24) & 0xFF;
    p[26] = fc & 0xFF;             p[27] = (fc >> 8) & 0xFF;
    p[28] = len & 0xFF;            p[29] = (len >> 8) & 0xFF;
    p[30] = (len >> 16) & 0xFF;    p[31] = (len >> 24) & 0xFF;

    if (!sd_write_sector_raw(fs->sd, slot_sector, fs->sd->sec_buf)) {
        fat_cleanup_failed_write(fs, fc, slot_sector, empty_slot);
        return false;
    }
    if (!fat_verify_file(fs, slot_sector, empty_slot, fc, len)) {
        fat_cleanup_failed_write(fs, fc, slot_sector, empty_slot);
        return false;
    }

    fat_update_fsinfo_after_alloc(fs, fc);
    fat_write_fsinfo(fs);

    ESP_LOGI(TAG, "OK: %.8s.%.3s cluster=%lu size=%u", name8, ext3, fc, len);
    return true;
}

int fat32_list_files(fat32fs_t *fs, const char *ext_filter,
                      char names[][FAT32_MAX_FNAME], int max_count)
{
    fat_refresh_cache(fs, ext_filter);
    int count = (fs->cached_count < max_count) ? fs->cached_count : max_count;
    for (int i = 0; i < count; i++) {
        strncpy(names[i], fs->cached_names[i], FAT32_MAX_FNAME - 1);
        names[i][FAT32_MAX_FNAME - 1] = '\0';
    }
    return count;
}

int fat32_get_file_count(fat32fs_t *fs, const char *ext_filter)
{
    fat_refresh_cache(fs, ext_filter);
    return fs->cached_count;
}

bool fat32_stat_file(fat32fs_t *fs, const char *name8, const char *ext3,
                     uint32_t *out_size)
{
    char pn[8], pe_ext[3];
    pad83(pn, name8);
    pad_ext3(pe_ext, ext3);
    uint32_t cur_clus = fs->root_clus;
    while (cur_clus >= 2 && cur_clus < 0x0FFFFFF8) {
        uint32_t root_sec = fat_cluster_to_sector(fs, cur_clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) return false;
            for (int i = 0; i < 16; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                uint8_t fb = ent[0];
                if (fb == 0x00) return false;
                if (fb == 0xE5) continue;
                if (ent[11] == 0x0F) continue;
                if (memcmp(ent, pn, 8) == 0 && memcmp(ent + 8, pe_ext, 3) == 0) {
                    *out_size = ent[28] | ((uint32_t)ent[29] << 8) |
                                ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                    return true;
                }
            }
        }
        cur_clus = fat_read_entry(fs, cur_clus);
    }
    return false;
}

void fat32_walk_root_dir(fat32fs_t *fs, fat32_dir_callback_t cb, void *user_data)
{
    uint32_t cur_clus = fs->root_clus;
    while (cur_clus >= 2 && cur_clus < 0x0FFFFFF8) {
        uint32_t root_sec = fat_cluster_to_sector(fs, cur_clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (!sd_read_sector(fs->sd, root_sec + s, fs->sd->sec_buf)) return;
            for (int i = 0; i < 16; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                uint8_t fb = ent[0];
                if (fb == 0x00) return;
                if (fb == 0xE5) continue;
                if (ent[11] == 0x0F) continue;
                if (!cb(ent, user_data)) return;
            }
        }
        cur_clus = fat_read_entry(fs, cur_clus);
    }
}
