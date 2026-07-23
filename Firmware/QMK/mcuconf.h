// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include_next <mcuconf.h>

/* The SSD1306 OLED hangs off I2C1 (GP6/GP7). ChibiOS leaves both RP2040 I2C
 * peripherals disabled by default, so I2CD1 has to be turned on here. */
#undef RP_I2C_USE_I2C0
#define RP_I2C_USE_I2C0 FALSE

#undef RP_I2C_USE_I2C1
#define RP_I2C_USE_I2C1 TRUE
