// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    /*
     * Eight MX switches plus the encoder's push switch. The encoder sits to
     * the right of the lower two rows, so it is the last key in LAYOUT.
     *
     * ┌───┬───┬───┬───┐
     * │Rwd│Ply│FFd│Mut│
     * ├───┼───┼───┴───┘
     * │Prv│Nxt│
     * ├───┼───┤ ┌───┐
     * │Vl-│Vl+│ │Stp│  <- encoder push
     * └───┴───┘ └───┘
     *
     * Turning the encoder is volume, see encoder_update_user() below.
     */
    [0] = LAYOUT(
        KC_MRWD, KC_MPLY, KC_MFFD,
        KC_MPRV, KC_MNXT, KC_MUTE,
        KC_VOLD, KC_VOLU, KC_MSTP
    )
};

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (index == 0) {
        if (clockwise) {
            tap_code(KC_VOLU);
        } else {
            tap_code(KC_VOLD);
        }
    }
    return false;
}
