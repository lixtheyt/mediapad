// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* SSD1306 OLED sits on the XIAO RP2040's I2C1 bus (D4/D5 on the silkscreen).
 * Without these, QMK falls back to the Pro Micro RP2040 board defaults
 * (GP2/GP3), which on this PCB are the encoder's B channel and matrix row 2. */
#define I2C_DRIVER I2CD1
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7

/* Panel geometry, stated outright rather than inherited from the QMK default,
 * so a wrong-sized framebuffer can be ruled out by reading this file. J1 in the
 * BOM is a 4-pin 0.91" SSD1306, which is 128x32 with sequential COM pins.
 *
 * If a 0.96" module ever goes on the board instead, this becomes
 *   #define OLED_DISPLAY_128X64
 * and the two lines below come out. Getting this wrong does not blank the
 * screen, it leaves the untransmitted half of the panel showing whatever
 * happened to be in GDDRAM at power-on - which looks like speckle or dots. */
#define OLED_DISPLAY_WIDTH 128
#define OLED_DISPLAY_HEIGHT 32

/* Needed by the reactive effects (solid_reactive*, splash, solid_splash):
 * without it QMK never records which key was hit, so nothing propagates. */
#define RGB_MATRIX_KEYPRESSES

/* Room in EEPROM for the lighting the user dialled in (rgb_saved_t in the
 * lixtheyt keymap), so it survives unplugging. */
#define EECONFIG_USER_DATA_SIZE 6
