/*
 * sdcard_vfs.c - ESP-IDF VFS adapter for Stardust SD card.
 *
 * Translates virtual POSIX paths to FAT32FS 8.3 names.
 *
 * Path mapping:
 *   /sdcard/diaries/2026-05-21.md  → 20260521.MD
 *   /sdcard/memory/index.dat       → MEM_INDX.DAT
 *   /sdcard/memory/records.dat     → MEM_RECS.DAT
 *   /sdcard/memory/digest.dat      → MEM_DIGS.DAT
 *   /sdcard/ANSWERS.TXT            → ANSWERS.TXT
 *   /sdcard/device_manifest.json   → DEVICE.MNF
 *   /sdcard/wallet.dat             → WALLET.DAT
 */
#include "sdcard_vfs.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sdcard_vfs";

/* ---- State ---- */
static sd_driver_t g_sd;
static fat32fs_t  g_fs;
static char        g_mount_point[32];
static SemaphoreHandle_t g_mutex = NULL;

/* ---- Open-file context ---- */
typedef struct {
    char     name8[9];
    char     ext3[4];
    char    *buf;
    uint16_t size;   /* buffer capacity */
    uint16_t pos;    /* current r/w offset */
    bool     written;
} sdcard_file_t;

/* ---- Helpers ---- */

static void set_83(char name8[8], char ext3[3],
                     const char *n8, const char *e3)
{
    memset(name8, ' ', 8); memset(ext3, ' ', 3);
    for (int i = 0; n8[i] && i < 8;  i++) name8[i] = n8[i];
    for (int i = 0; e3[i] && i < 3;  i++) ext3[i] = e3[i];
}

/* ---- Path translation ---- */

typedef struct { const char *rel; const char *n8; const char *e3; } fx_t;
static const fx_t fixed[] = {
    {"/memory/index.dat",    "MEM_INDX", "DAT"},
    {"/memory/records.dat",  "MEM_RECS", "DAT"},
    {"/memory/digest.dat",   "MEM_DIGS", "DAT"},
    {"/ANSWERS.TXT",         "ANSWERS",  "TXT"},
    {"/device_manifest.json","DEVICE",   "MNF"},
    {"/wallet.dat",          "WALLET",   "DAT"},
};

/* Translate virtual path → fill name8[9] + ext3[4].
 * Returns true on success.
 */
static bool vfs_translate(const char *vpath,
                           char name8[9], char ext3[4])
{
    size_t mp_len = strlen(g_mount_point);
    const char *rel = vpath + mp_len;
    if (*rel == '\0') rel = "/";

    /* 1. fixed paths */
    for (int i = 0; i < (int)(sizeof(fixed)/sizeof(fixed[0])); i++) {
        if (strcmp(rel, fixed[i].rel) == 0) {
            set_83(name8, ext3, fixed[i].n8, fixed[i].e3);
            name8[8] = '\0'; ext3[3] = '\0';
            return true;
        }
    }

    /* 2. diary: /diaries/YYYY-MM-DD.md → YYYYMMDD.MD */
    if (strncmp(vpath, "/diaries/", 9) == 0) {
        const char *fname = vpath + 9;
        if (strlen(fname) == 13 && fname[4] == '-' &&
            fname[7] == '-' && strcmp(fname + 10, ".md") == 0)
        {
            char d8[9];
            snprintf(d8, 9, "%.4s%.2s%.2s", fname, fname+5, fname+8);
            set_83(name8, ext3, d8, "MD");
            name8[8] = '\0'; ext3[3] = '\0';
            return true;
        }
    }
    return false;
}

/* ---- fat32 helper: stat without reading ---- */

static bool fat32_stat_light(fat32fs_t *fs,
                               const char *name8, const char *ext3,
                               uint32_t *out_size)
{
    char pn[8], pe[3];
    set_83(pn, pe, name8, ext3);

    uint32_t cur = fs->root_clus;
    while (cur >= 2 && cur < 0x0FFFFFF8UL) {
        uint32_t sec = fs->abs_data + (cur - 2) * fs->sec_per_clus;
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (!sd_read_sector(fs->sd, sec + s, fs->sd->sec_buf)) return false;
            for (int i = 0; i < 16; i++) {
                uint8_t *ent = &fs->sd->sec_buf[i * 32];
                if (ent[0] == 0x00) return false;
                if (ent[0] == 0xE5) continue;
                if (ent[11] == 0x0F) continue;
                if (memcmp(ent,     pn, 8) == 0 &&
                    memcmp(ent + 8, pe, 3) == 0)
                {
                    *out_size = (uint32_t)ent[28]
                               | ((uint32_t)ent[29] << 8)
                               | ((uint32_t)ent[30] << 16)
                               | ((uint32_t)ent[31] << 24);
                    return true;
                }
            }
        }
        /* follow FAT chain */
        uint32_t off = cur * 4;
        uint32_t fs1 = fs->abs_fat1 + off / 512;
        uint32_t o   = off % 512;
        if (!sd_read_sector(fs->sd, fs1, fs->sd->sec_buf)) return false;
        uint32_t nxt = (fs->sd->sec_buf[o] |
                         (fs->sd->sec_buf[o+1] << 8) |
                         (fs->sd->sec_buf[o+2] << 16) |
                         (fs->sd->sec_buf[o+3] << 24)) & 0x0FFFFFFFUL;
        if (nxt >= 0x0FFFFFF8UL) break;
        cur = nxt;
    }
    return false;
}

/* ---- VFS callbacks ---- */

static int cb_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx; (void)mode;
    char n8[9], e3[4];
    if (!vfs_translate(path, n8, e3)) { errno = ENOENT; return -1; }

    xSemaphoreTake(g_mutex, portMAX_DELAY);

    sdcard_file_t *f = calloc(1, sizeof(*f));
    if (!f) { xSemaphoreGive(g_mutex); errno = ENOMEM; return -1; }
    memcpy(f->name8, n8, 8); f->name8[8] = '\0';
    memcpy(f->ext3,  e3, 3); f->ext3[3]  = '\0';
    f->pos   = 0;
    f->written = false;

    /* if O_CREAT and file does NOT exist → create empty buffer */
    bool exists = fat32_file_exists(&g_fs, n8, e3);
    if ((flags & O_CREAT) && !exists) {
        f->buf = calloc(1, 512);
        f->size = 0;
        xSemaphoreGive(g_mutex);
        return (int)(intptr_t)f;
    }

    /* existing file → read into buffer */
    if (!exists) { free(f); xSemaphoreGive(g_mutex); errno = ENOENT; return -1; }

    uint32_t fsize = 0;
    fat32_stat_light(&g_fs, n8, e3, &fsize);
    uint16_t cap = (fsize < 512) ? 512 : (uint16_t)fsize + 256;
    f->buf = malloc(cap);
    if (!f->buf) { free(f); xSemaphoreGive(g_mutex); errno = ENOMEM; return -1; }
    f->size = fat32_read_file(&g_fs, n8, e3, (char *)f->buf, cap);
    f->pos  = 0;
    xSemaphoreGive(g_mutex);
    return (int)(intptr_t)f;
}

static ssize_t cb_write(void *ctx, int fd, const void *src, size_t sz)
{
    (void)ctx;
    sdcard_file_t *f = (sdcard_file_t *)(intptr_t)fd;
    if (!f || !src) { errno = EBADF; return -1; }

    uint16_t need = f->pos + (uint16_t)sz;
    if (need > f->size) {
        uint16_t nsz = need + 256;
        char *nb = realloc(f->buf, nsz);
        if (!nb) { errno = ENOMEM; return -1; }
        f->buf = nb;
        f->size = nsz;
    }
    memcpy(f->buf + f->pos, src, sz);
    f->pos += (uint16_t)sz;
    f->written = true;
    return (ssize_t)sz;
}

static ssize_t cb_read(void *ctx, int fd, void *dst, size_t sz)
{
    (void)ctx;
    sdcard_file_t *f = (sdcard_file_t *)(intptr_t)fd;
    if (!f || !dst) { errno = EBADF; return -1; }
    uint16_t rem = f->size - f->pos;
    if (f->pos >= f->size) return 0;  /* EOF */
    uint16_t n = (sz > rem) ? rem : (uint16_t)sz;
    memcpy(dst, f->buf + f->pos, n);
    f->pos += n;
    return (ssize_t)n;
}

static int cb_close(void *ctx, int fd)
{
    (void)ctx;
    sdcard_file_t *f = (sdcard_file_t *)(intptr_t)fd;
    if (!f) { errno = EBADF; return -1; }

    if (f->written && f->buf && f->pos > 0) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        fat32_write_file(&g_fs, f->name8, f->ext3, f->buf, f->pos);
        xSemaphoreGive(g_mutex);
    }
    free(f->buf);
    free(f);
    return 0;
}

static off_t cb_lseek(void *ctx, int fd, off_t off, int whence)
{
    (void)ctx;
    sdcard_file_t *f = (sdcard_file_t *)(intptr_t)fd;
    if (!f) { errno = EBADF; return (off_t)-1; }
    off_t np;
    switch (whence) {
    case SEEK_SET: np = off; break;
    case SEEK_CUR: np = (off_t)f->pos + off; break;
    case SEEK_END: np = (off_t)f->size + off; break;
    default: errno = EINVAL; return (off_t)-1;
    }
    if (np < 0 || np > (off_t)f->size) { errno = EINVAL; return (off_t)-1; }
    f->pos = (uint16_t)np;
    return np;
}

static int cb_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    sdcard_file_t *f = (sdcard_file_t *)(intptr_t)fd;
    if (!f || !st) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = f->size;
    st->st_mode = S_IFREG | 0666;
    return 0;
}

static int cb_stat(const char *path, struct stat *st)
{
    char n8[9], e3[4];
    if (!vfs_translate(path, n8, e3)) { errno = ENOENT; return -1; }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t sz = 0;
    bool ok = fat32_stat_light(&g_fs, n8, e3, &sz);
    xSemaphoreGive(g_mutex);
    if (!ok) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = sz;
    st->st_mode = S_IFREG | 0666;
    return 0;
}

/* ---- Public API ---- */

bool sdcard_vfs_mount(const char *mount_point,
                       int spi_host,
                       int cs_pin, int mosi_pin, int miso_pin, int sck_pin)
{
    if (!sd_init(&g_sd, (spi_host_device_t)spi_host,
                 cs_pin, mosi_pin, miso_pin, sck_pin)) {
        ESP_LOGE(TAG, "SD init failed");
        return false;
    }
    if (!fat32_mount(&g_fs, &g_sd)) {
        ESP_LOGE(TAG, "FAT32 mount failed");
        return false;
    }

    /* ---- FAT32 cluster/sector diagnostic ---- */
    ESP_LOGI(TAG, "FAT32 diag: root_clus=%lu sec_per_clus=%u data_start=%lu",
             (unsigned long)g_fs.root_clus, g_fs.sec_per_clus,
             (unsigned long)g_fs.abs_data);
    // cluster → sector: abs_data + (cluster - 2) * sec_per_clus
    uint32_t root_sec = g_fs.abs_data + (g_fs.root_clus - 2) * g_fs.sec_per_clus;
    ESP_LOGI(TAG, "FAT32 diag: root_sec=%lu", (unsigned long)root_sec);
    if (sd_read_sector(&g_sd, root_sec, g_sd.sec_buf)) {
        int entries = 0;
        for (int i = 0; i < 16 && entries < 20; i++) {
            uint8_t fb = g_sd.sec_buf[i * 32];
            if (fb == 0x00) break;
            if (fb == 0xE5) continue;
            char name[13] = {0};
            memcpy(name, &g_sd.sec_buf[i * 32], 8);
            memcpy(name + 8, ".", 1);
            memcpy(name + 9, &g_sd.sec_buf[i * 32 + 8], 3);
            for (int j = 0; j < 12; j++) if (name[j] == ' ') name[j] = 0;
            uint32_t fc = g_sd.sec_buf[i * 32 + 20];
            fc |= ((uint32_t)g_sd.sec_buf[i * 32 + 21] << 8);
            fc |= ((uint32_t)g_sd.sec_buf[i * 32 + 26] << 16);
            fc |= ((uint32_t)g_sd.sec_buf[i * 32 + 27] << 24);
            uint32_t sz = g_sd.sec_buf[i * 32 + 28];
            sz |= ((uint32_t)g_sd.sec_buf[i * 32 + 29] << 8);
            sz |= ((uint32_t)g_sd.sec_buf[i * 32 + 30] << 16);
            sz |= ((uint32_t)g_sd.sec_buf[i * 32 + 31] << 24);
            ESP_LOGI(TAG, "FAT32 diag: [%d] '%s' clus=%lu size=%lu",
                     i, name, (unsigned long)fc, (unsigned long)sz);
            entries++;
        }
        if (entries == 0) {
            ESP_LOGI(TAG, "FAT32 diag: root dir is empty");
        }
    } else {
        ESP_LOGE(TAG, "FAT32 diag: read root sector FAILED");
    }

    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) return false;
    strncpy(g_mount_point, mount_point, sizeof(g_mount_point)-1);
    g_mount_point[sizeof(g_mount_point)-1] = '\0';

    esp_vfs_t vfs = {
        .flags         = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p        = &cb_open,
        .close_p       = &cb_close,
        .read_p        = &cb_read,
        .write_p       = &cb_write,
        .lseek_p       = &cb_lseek,
        .fstat_p       = &cb_fstat,
        .stat          = &cb_stat,
    };
    esp_err_t e = esp_vfs_register(mount_point, &vfs, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "VFS register failed: %d", e);
        vSemaphoreDelete(g_mutex); g_mutex = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Mounted at %s", mount_point);
    return true;
}

bool sdcard_vfs_write_direct(const char *vpath, const char *data, uint16_t len)
{
    if (!g_mutex) {
        ESP_LOGE(TAG, "direct write: SD not mounted");
        return false;
    }
    char n8[9], e3[4];
    if (!vfs_translate(vpath, n8, e3)) {
        ESP_LOGE(TAG, "direct write: translate failed for %s", vpath);
        return false;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool ok = fat32_write_file(&g_fs, n8, e3, data, len);
    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "direct write: %s → %.8s.%.3s (%u bytes) %s",
             vpath, n8, e3, (unsigned)len, ok ? "OK" : "FAIL");
    return ok;
}

void sdcard_vfs_unmount(void)
{
    esp_vfs_unregister(g_mount_point);
    if (g_mutex) { vSemaphoreDelete(g_mutex); g_mutex = NULL; }
    ESP_LOGI(TAG, "Unmounted");
}
