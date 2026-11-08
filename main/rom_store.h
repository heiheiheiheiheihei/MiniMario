/*
 * MiniMario - ROM store.
 *
 * Two ROM sources, merged into one list:
 *   1. TF card (FAT, mounted at /sdcard).  The card slot shares the LCD SPI2
 *      bus, so the card is attached with sdspi_host_init_device() after
 *      display_init() has created the bus.  Files are copied to the internal
 *      heap and capped at ROM_STORE_MAX_SD_BYTES (a hard cap; the practical
 *      limit is the largest free contiguous block, see docs/app.md).
 *   2. The "roms" data partition (type 0x40/subtype 0x01, offset 0x1F0000).
 *      Preferred layout is the MMROM1 container (see tools/pack_roms.py):
 *          magic  "MMROM1\0\0"                       8 bytes
 *          u32    count
 *          count * { u32 offset, u32 size, char name[32] }   (offset from start)
 *          ROM blobs
 *      A partition that starts with "NES\x1a" is treated as one legacy raw
 *      iNES image.  Flashed ROMs are memory-mapped (no RAM cost) so they can
 *      be bigger than the TF-card heap limit.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Must match the packed container / menu capacity. */
#define ROM_STORE_MAX_ENTRIES 64

/* TF-card images are read into the heap while the UI canvas is released. */
#define ROM_STORE_MAX_SD_BYTES (256 * 1024)

/* "roms" data partition (partitions.csv: "roms, data, 0x40" -> type=data,
 * subtype=0x40).  It is located by label first, subtype as a fallback. */
#define ROMS_PARTITION_LABEL "roms"
#define ROMS_PARTITION_SUBTYPE 0x40

typedef enum {
    ROM_SRC_FLASH = 0, /* embedded in the "roms" flash partition */
    ROM_SRC_SD = 1,    /* TF card heap load */
} rom_source_t;

typedef struct {
    char name[40];   /* display name, NUL terminated */
    uint32_t size;   /* image size in bytes */
    uint32_t offset; /* offset inside the "roms" partition (flash only) */
    char *path;      /* heap allocated full path (TF card only) */
    uint8_t source;  /* rom_source_t */
    bool too_large;  /* TF-card file over ROM_STORE_MAX_SD_BYTES */
} rom_entry_t;

/* A loaded ROM image; valid until rom_store_unload(). */
typedef struct {
    const uint8_t *data;
    size_t len;
} rom_image_t;

/* Mount the TF card and scan both sources.  Never fails because of a missing
 * card (inspect rom_store_sd_mounted() / rom_store_sd_error()). */
esp_err_t rom_store_init(void);

/* Re-scan both sources (unmounts/remounts the card so new files show up). */
esp_err_t rom_store_rescan(void);

int rom_store_count(void);
const rom_entry_t *rom_store_get(int index);
bool rom_store_sd_mounted(void);
const char *rom_store_sd_error(void);
bool rom_store_has_flash_roms(void);
int rom_store_count_source(rom_source_t source);
/* Why the embedded partition has no ROMs (empty string when it worked). */
const char *rom_store_flash_error(void);

/* Load entry <index>; call rom_store_unload() when done with it. */
esp_err_t rom_store_load(int index, rom_image_t *out);

/* Load an explicit TF-card path (absolute or relative to /sdcard). */
esp_err_t rom_store_load_path(const char *path, rom_image_t *out);

void rom_store_unload(void);

/* Static human readable explanation for an error from load(). */
const char *rom_store_error_str(esp_err_t err);
