/*
 * MiniMario - serial debug console (native USB-Serial-JTAG, /dev/ttyACM0).
 *
 * The port is shared with ESP-IDF log output.  The console installs the
 * usb_serial_jtag driver for input and keeps its polling interval short so it
 * never starves the emulator/UI tasks.  Text responses use printf (VFS);
 * binary `shot` bursts use usb_serial_jtag_write_bytes() and temporarily
 * silence ESP_LOG so the frame dump cannot be interleaved with log lines.
 */
#pragma once

/* Install the USB-Serial-JTAG driver and start the console task. */
void console_init(void);
