// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* SSD1306 OLED sits on the XIAO RP2040's I2C1 bus (D4/D5 on the silkscreen).
 * Without these, QMK falls back to the Pro Micro RP2040 board defaults
 * (GP2/GP3), which on this PCB are the encoder's B channel and matrix row 2.
 * Display size is left at the QMK default of 128x32. */
#define I2C_DRIVER I2CD1
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7

/* Needed by the reactive effects (solid_reactive*, splash, solid_splash):
 * without it QMK never records which key was hit, so nothing propagates. */
#define RGB_MATRIX_KEYPRESSES
