/*
 * MiniMario - ROM store implementation.
 *
 * See rom_store.h for the two ROM sources and the container format.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "rom_store.h"

static const char *TAG = "rom_store";

#define SD_MOUNT_POINT "/sdcard"
#define SD_PIN_CS 4
#define SD_SPI_HOST SPI2_HOST /* owned/initialised by display_init() */
#define SD_MAX_FREQ_KHZ 20000

#define MMROM_MAGIC "MMROM1"
#define MMROM_MAGIC_LEN 8
#define MMROM_HDR_LEN (MMROM_MAGIC_LEN + 4)
#define MMROM_NAME_LEN 32
#define MMROM_ENTRY_LEN (4 + 4 + MMROM_NAME_LEN)

#define INES_MAGIC "NES\x1a"

static rom_entry_t s_entries[ROM_STORE_MAX_ENTRIES];
static int s_count;
static SemaphoreHandle_t s_lock;

static bool s_sd_mounted;
static sdmmc_card_t *s_card;
static char s_sd_error[80] = "not scanned";

static const esp_partition_t *s_partition;
static bool s_flash_ok;
static char s_flash_error[64] = "not scanned";

/* One loaded image at a time. */
static rom_image_t s_loaded;
static bool s_loaded_heap;
static bool s_loaded_mapped;
static esp_partition_mmap_handle_t s_map_handle;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline void store_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static inline void store_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static void entry_set_name(rom_entry_t *e, const char *name)
{
    memset(e->name, 0, sizeof(e->name));
    if (!name) return;
    /* printable ASCII only, leave room for the NUL */
    int j = 0;
    for (int i = 0; name[i] && j < (int)sizeof(e->name) - 1; i++) {
        unsigned char c = (unsigned char)name[i];
        e->name[j++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    e->name[j] = 0;
}

static void free_entries(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].path);
        s_entries[i].path = NULL;
    }
    s_count = 0;
}

static rom_entry_t *entry_add(void)
{
    if (s_count >= ROM_STORE_MAX_ENTRIES) return NULL;
    rom_entry_t *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    s_count++;
    return e;
}

static size_t ines_image_size(const uint8_t *hdr)
{
    size_t trainer = (hdr[6] & 0x04) ? 512 : 0;
    return 16 + trainer + (size_t)hdr[4] * 16384u + (size_t)hdr[5] * 8192u;
}

static bool has_ines_suffix(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    return strcasecmp(name + n - 4, ".nes") == 0;
}

/* Validate the iNES header of a file on the TF card. */
static bool file_is_ines(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[16];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (got < 16 || memcmp(hdr, INES_MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    fclose(f);
    if (sz < 16) return false;
    if (size_out) *size_out = (size_t)sz;
    return true;
}

static int entry_name_cmp(const void *a, const void *b)
{
    const rom_entry_t *ea = (const rom_entry_t *)a;
    const rom_entry_t *eb = (const rom_entry_t *)b;
    return strcasecmp(ea->name, eb->name);
}

/* ------------------------------------------------------------------ */
/* embedded "roms" flash partition                                     */
/* ------------------------------------------------------------------ */

static const esp_partition_t *find_roms_partition(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                                 ROMS_PARTITION_LABEL);
    if (!part) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROMS_PARTITION_SUBTYPE, NULL);
    }
    return part;
}

static void scan_flash_locked(void)
{
    s_flash_ok = false;
    s_flash_error[0] = 0;
    s_partition = find_roms_partition();
    if (!s_partition) {
        snprintf(s_flash_error, sizeof(s_flash_error), "no roms partition");
        return;
    }

    uint8_t hdr[16];
    esp_err_t err = esp_partition_read(s_partition, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        snprintf(s_flash_error, sizeof(s_flash_error), "read failed: %s", esp_err_to_name(err));
        return;
    }

    /* Legacy: a single raw iNES image at the start of the partition. */
    if (memcmp(hdr, INES_MAGIC, 4) == 0) {
        size_t size = ines_image_size(hdr);
        if (size < 16) {
            snprintf(s_flash_error, sizeof(s_flash_error), "invalid iNES header");
            return;
        }
        if (size > s_partition->size) size = s_partition->size;
        rom_entry_t *e = entry_add();
        if (!e) {
            snprintf(s_flash_error, sizeof(s_flash_error), "too many ROMs");
            return;
        }
        entry_set_name(e, "embedded.nes");
        e->size = (uint32_t)size;
        e->offset = 0;
        e->source = ROM_SRC_FLASH;
        s_flash_ok = true;
        ESP_LOGI(TAG, "flash: legacy raw iNES, %u bytes", (unsigned)size);
        return;
    }

    if (memcmp(hdr, MMROM_MAGIC, MMROM_MAGIC_LEN) != 0) {
        snprintf(s_flash_error, sizeof(s_flash_error), "empty or unknown container");
        return;
    }

    uint32_t count = 0;
    memcpy(&count, hdr + MMROM_MAGIC_LEN, sizeof(count));
    if (count == 0 || count > ROM_STORE_MAX_ENTRIES ||
        (size_t)MMROM_HDR_LEN + (size_t)count * MMROM_ENTRY_LEN > s_partition->size) {
        snprintf(s_flash_error, sizeof(s_flash_error), "bad container header (count=%u)",
                 (unsigned)count);
        return;
    }

    uint8_t *table = malloc((size_t)count * MMROM_ENTRY_LEN);
    if (!table) {
        snprintf(s_flash_error, sizeof(s_flash_error), "no memory for ROM table");
        return;
    }
    err = esp_partition_read(s_partition, MMROM_HDR_LEN, table, (size_t)count * MMROM_ENTRY_LEN);
    if (err != ESP_OK) {
        free(table);
        snprintf(s_flash_error, sizeof(s_flash_error), "read table failed: %s", esp_err_to_name(err));
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = table + (size_t)i * MMROM_ENTRY_LEN;
        uint32_t off, size;
        memcpy(&off, p, 4);
        memcpy(&size, p + 4, 4);
        const char *name = (const char *)(p + 8);
        if (size < 16 || off < MMROM_HDR_LEN || off > s_partition->size ||
            size > s_partition->size - off) {
            ESP_LOGW(TAG, "flash: skipping bad entry %u (off=%u size=%u)", (unsigned)i,
                     (unsigned)off, (unsigned)size);
            continue;
        }
        rom_entry_t *e = entry_add();
        if (!e) break;
        char safe[MMROM_NAME_LEN + 1];
        memcpy(safe, name, MMROM_NAME_LEN);
        safe[MMROM_NAME_LEN] = 0;
        entry_set_name(e, safe[0] ? safe : "embedded.nes");
        e->size = size;
        e->offset = off;
        e->source = ROM_SRC_FLASH;
        ESP_LOGI(TAG, "flash ROM: '%s' %u bytes @+0x%05x", e->name, (unsigned)size,
                 (unsigned)off);
    }
    free(table);
    s_flash_ok = true;
}

/* ------------------------------------------------------------------ */
/* TF card                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t sd_mount_locked(void)
{
    if (s_sd_mounted) return ESP_OK;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_MAX_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = (gpio_num_t)SD_PIN_CS;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    s_card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_sd_mounted = false;
        s_card = NULL;
        snprintf(s_sd_error, sizeof(s_sd_error), "mount failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "TF card: %s", s_sd_error);
        return err;
    }
    s_sd_mounted = true;
    s_sd_error[0] = 0;
    ESP_LOGI(TAG, "TF card: '%s' %uMB mounted at %s", s_card->cid.name ? (const char *)s_card->cid.name : "?",
             (unsigned)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024)),
             SD_MOUNT_POINT);
    return ESP_OK;
}

static void sd_unmount_locked(void)
{
    if (s_sd_mounted && s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
    }
    s_sd_mounted = false;
    s_card = NULL;
}

static void scan_sd_locked(void)
{
    if (!s_sd_mounted) return;

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        snprintf(s_sd_error, sizeof(s_sd_error), "cannot open %s", SD_MOUNT_POINT);
        return;
    }

    int sd_first = s_count;
    struct dirent *de;
    int skipped = 0;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue; /* ".", "..", hidden files */
        if (!has_ines_suffix(name)) continue;

        char path[300];
        int n = snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
        if (n < 0 || n >= (int)sizeof(path)) {
            skipped++;
            continue;
        }
        size_t size = 0;
        if (!file_is_ines(path, &size)) {
            ESP_LOGW(TAG, "skipping '%s': not a valid iNES file", name);
            skipped++;
            continue;
        }
        if (s_count >= ROM_STORE_MAX_ENTRIES) {
            ESP_LOGW(TAG, "ROM list full (%d entries)", ROM_STORE_MAX_ENTRIES);
            skipped++;
            break;
        }
        char *path_dup = strdup(path);
        if (!path_dup) {
            skipped++;
            break;
        }
        rom_entry_t *e = entry_add();
        if (!e) {
            free(path_dup);
            break;
        }
        entry_set_name(e, name); /* display name without /sdcard/ */
        e->size = (uint32_t)size;
        e->offset = 0;
        e->path = path_dup;
        e->source = ROM_SRC_SD;
        e->too_large = size > ROM_STORE_MAX_SD_BYTES;
    }
    closedir(dir);

    int sd_count = s_count - sd_first;
    if (sd_count > 1) {
        qsort(&s_entries[sd_first], (size_t)sd_count, sizeof(rom_entry_t), entry_name_cmp);
    }
    ESP_LOGI(TAG, "TF card: %d ROM(s) found%s", sd_count,
             skipped ? " (some entries skipped, see log)" : "");
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t rom_store_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_loaded, 0, sizeof(s_loaded));
    s_loaded_heap = false;
    s_loaded_mapped = false;

    store_lock();
    free_entries();
    scan_flash_locked();
    sd_mount_locked();
    scan_sd_locked();
    int count = s_count;
    store_unlock();

    ESP_LOGI(TAG, "ROM store: %d ROM(s) (%d flash, sd=%s)", count,
             rom_store_count_source(ROM_SRC_FLASH), s_sd_mounted ? "mounted" : "absent");
    return ESP_OK;
}

esp_err_t rom_store_rescan(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    store_lock();
    if (s_loaded_mapped || s_loaded_heap) {
        store_unlock();
        return ESP_ERR_INVALID_STATE; /* never rescan with a ROM mapped */
    }
    free_entries();
    scan_flash_locked();
    if (s_sd_mounted) sd_unmount_locked();
    sd_mount_locked();
    scan_sd_locked();
    int count = s_count;
    store_unlock();

    ESP_LOGI(TAG, "rescan: %d ROM(s) (sd=%s)", count, s_sd_mounted ? "mounted" : "absent");
    return ESP_OK;
}

int rom_store_count(void)
{
    store_lock();
    int n = s_count;
    store_unlock();
    return n;
}

const rom_entry_t *rom_store_get(int index)
{
    const rom_entry_t *e = NULL;
    store_lock();
    if (index >= 0 && index < s_count) e = &s_entries[index];
    store_unlock();
    return e;
}

bool rom_store_sd_mounted(void)
{
    return s_sd_mounted;
}

const char *rom_store_sd_error(void)
{
    return s_sd_error;
}

const char *rom_store_flash_error(void)
{
    return s_flash_error;
}

bool rom_store_has_flash_roms(void)
{
    return rom_store_count_source(ROM_SRC_FLASH) > 0;
}

int rom_store_count_source(rom_source_t source)
{
    int n = 0;
    store_lock();
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].source == (uint8_t)source) n++;
    }
    store_unlock();
    return n;
}

static esp_err_t load_flash_locked(const rom_entry_t *e, rom_image_t *out)
{
    if (!s_partition) {
        s_partition = find_roms_partition();
        if (!s_partition) return ESP_ERR_NOT_FOUND;
    }
    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(s_partition, e->offset, e->size, ESP_PARTITION_MMAP_DATA,
                                       &ptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap '%s' failed: %s", e->name, esp_err_to_name(err));
        return err;
    }
    s_loaded_mapped = true;
    s_map_handle = handle;
    s_loaded.data = (const uint8_t *)ptr;
    s_loaded.len = e->size;
    *out = s_loaded;
    return ESP_OK;
}

static esp_err_t load_file_locked(const char *path, rom_image_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 16) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)sz > ROM_STORE_MAX_SD_BYTES) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf) buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    if (memcmp(buf, INES_MAGIC, 4) != 0) {
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_loaded_heap = true;
    s_loaded.data = buf;
    s_loaded.len = (size_t)sz;
    *out = s_loaded;
    return ESP_OK;
}

esp_err_t rom_store_load(int index, rom_image_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    store_lock();
    if (s_loaded_heap || s_loaded_mapped) {
        store_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || s_count <= index) {
        store_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const rom_entry_t *e = &s_entries[index];
    esp_err_t err;
    if (e->source == ROM_SRC_FLASH) {
        err = load_flash_locked(e, out);
    } else {
        if (e->too_large) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = load_file_locked(e->path, out);
        }
    }
    store_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded '%s' (%u bytes, %s)", e->name, (unsigned)out->len,
                 e->source == ROM_SRC_FLASH ? "flash mmap" : "TF card heap");
    }
    return err;
}

esp_err_t rom_store_load_path(const char *path, rom_image_t *out)
{
    if (!out || !path || !*path) return ESP_ERR_INVALID_ARG;
    char full[300];
    if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "%s/%s", SD_MOUNT_POINT, path);
    }
    store_lock();
    if (s_loaded_heap || s_loaded_mapped) {
        store_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = load_file_locked(full, out);
    store_unlock();
    if (err == ESP_OK) ESP_LOGI(TAG, "loaded '%s' (%u bytes)", full, (unsigned)out->len);
    return err;
}

void rom_store_unload(void)
{
    store_lock();
    if (s_loaded_mapped) {
        esp_partition_munmap(s_map_handle);
        s_loaded_mapped = false;
        s_map_handle = 0;
    }
    if (s_loaded_heap && s_loaded.data) {
        free((void *)s_loaded.data);
        s_loaded_heap = false;
    }
    memset(&s_loaded, 0, sizeof(s_loaded));
    store_unlock();
}

const char *rom_store_error_str(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ok";
    case ESP_ERR_INVALID_SIZE:
        return "TF ROM >256 KB - flash it";
    case ESP_ERR_NOT_FOUND:
        return "ROM file not found (no TF card?)";
    case ESP_ERR_NO_MEM:
        return "OOM - wpad off, reboot";
    case ESP_ERR_INVALID_RESPONSE:
        return "not a valid iNES ROM (missing NES\\x1a header)";
    case ESP_ERR_INVALID_STATE:
        return "a ROM is already loaded";
    case ESP_ERR_INVALID_ARG:
        return "invalid ROM index";
    default:
        return esp_err_to_name(err);
    }
}
