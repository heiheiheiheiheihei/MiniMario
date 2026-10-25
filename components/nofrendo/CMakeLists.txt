# nofrendo NES emulation core (GPLv2, (c) 1998-2000 Matthew Conte).
#
# Copied from Espressif's esp32-nesemu (components/nofrendo) and adapted for
# ESP-IDF 5.3 / ESP32-C6.  The platform specific OSD layer lives in
# components/nes_port, not here.

file(GLOB NOFRENDO_MAPPERS "${CMAKE_CURRENT_LIST_DIR}/mappers/*.c")

idf_component_register(
    SRCS
        "bitmap.c"
        "config.c"
        "event.c"
        "gui.c"
        "gui_elem.c"
        "intro.c"
        "log.c"
        "memguard.c"
        "nofrendo.c"
        "pcx.c"
        "vid_drv.c"
        "cpu/dis6502.c"
        "cpu/nes6502.c"
        "libsnss/libsnss.c"
        "nes/nes.c"
        "nes/nesinput.c"
        "nes/nes_mmc.c"
        "nes/nes_pal.c"
        "nes/nes_ppu.c"
        "nes/nes_rom.c"
        "nes/mmclist.c"
        "nes/nesstate.c"
        "sndhrdw/fds_snd.c"
        "sndhrdw/mmc5_snd.c"
        "sndhrdw/nes_apu.c"
        "sndhrdw/vrcvisnd.c"
        ${NOFRENDO_MAPPERS}
    INCLUDE_DIRS
        "."
        "cpu"
        "libsnss"
        "nes"
        "sndhrdw"
)

# Old 1998-era code: keep it at -O2 (nofrendo uses aliasing tricks and lots of
# int arithmetic), silence the noisiest warnings, never turn them into errors.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fno-strict-aliasing
    -Wno-error
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-char-subscripts
    -Wno-pointer-sign
    # IDF builds with -Werror=all; old nofrendo code trips a few classes.
    -Wno-error=stringop-truncation
    -Wno-error=stringop-overflow
    -Wno-error=stringop-overread
    -Wno-error=array-bounds
    -Wno-error=maybe-uninitialized
    -Wno-error=uninitialized
    -Wno-error=format-truncation
    -Wno-error=format-overflow
    -Wno-error=implicit-fallthrough
    -Wno-error=unused-result
    -Wno-error=type-limits
    -Wno-error=restrict
    -Wno-error=misleading-indentation
    -Wno-error=parentheses
    -Wno-error=address
)
