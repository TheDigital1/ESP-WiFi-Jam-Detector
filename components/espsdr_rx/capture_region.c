/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Capture-SRAM reservation for the ESP32-C5 receiver.
 * Copied from ESPARGOS esp-sdr @f86da81, main/targets/esp32c5/chip.h
 * (Copyright ESPARGOS contributors, GPL-3.0-or-later).
 *
 * The C5 RF dump engine owns the whole 128 KiB bank 0x40820000-0x40840000
 * while capturing (SRAM_OWNER_REG bit 1). Keeping it out of the heap here,
 * together with upstream/targets/esp32c5/sram_guard.ld for static sections,
 * means nothing else can ever land in it. Linked from M1 on, before any
 * radio code, so a growing image fails the link early.
 */
#include "heap_memory_layout.h"

/* C5 ownership bit 1 covers an entire 128 KiB bank. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c5_rf_dump);
