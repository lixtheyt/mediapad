// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "raw_hid.h"
#include <string.h>
#include <stdio.h>

enum custom_keycodes {
    KC_SHUFFLE = SAFE_RANGE,
    KC_REPEAT,
    CYCLE_LAYER,
    RGB_SEL_R,
    RGB_SEL_G,
    RGB_SEL_B,
    RGB_SEL_VAL,
    RGB_SEL_FX
};

#define NUM_FUNC_LAYERS 5

#define NUM_CYCLE_LAYERS 6
#define LAYER_RGB 5
#define RGB_STEP 8

#define RGB_VAL_MAX RGB_MATRIX_MAXIMUM_BRIGHTNESS

static uint8_t rgb_r = 255, rgb_g = 255, rgb_b = 255;
static uint8_t rgb_val = RGB_VAL_MAX;

static const char *system_msg = NULL;
static uint32_t system_msg_timer = 0;
#define SYSTEM_MSG_DURATION 2000

static bool held[3][3] = {0};
static bool combo_fired = false;

#define HOLD_TIME_LONG  5000
#define HOLD_TIME_SHORT 2000

typedef enum { COMBO_NONE, COMBO_BOOTLOADER, COMBO_EEPROM, COMBO_REBOOT, COMBO_DEBUG } combo_id_t;
static combo_id_t active_combo = COMBO_NONE;
static uint32_t combo_start_timer = 0;

#define PICKER_TIMEOUT 3000

static bool     picker_open   = false;
static uint8_t  picker_choice = 0;
static uint32_t picker_timer  = 0;

static bool picker_active(void) {
    return picker_open && timer_elapsed32(picker_timer) < PICKER_TIMEOUT;
}

typedef enum { HELP_OFF, HELP_BROWSE, HELP_SHOWING } help_state_t;
static help_state_t help_state = HELP_OFF;
static uint8_t      help_index = 0;

typedef struct {
    const char *label;
    const char *where;
    uint8_t     row;
    uint8_t     col;
} help_entry_t;

static const help_entry_t help_media[] = {
    {"Play/pause", "top row, 1st of 4",  0, 0},
    {"Rewind",     "top row, 2nd of 4",  0, 1},
    {"Forward",    "top row, 3rd of 4",  0, 2},
    {"Prev track", "middle row, left",   1, 0},
    {"Next track", "middle row, right",  1, 1},
    {"Mute",       "top row, far right", 1, 2},
    {"Repeat",     "bottom row, left",   2, 0},
    {"Shuffle",    "bottom row, right",  2, 1},
    {"Mode dial",  "the encoder",        2, 2},
};

static const help_entry_t help_rgb[] = {
    {"Red",        "top row, 1st of 4",  0, 0},
    {"Green",      "top row, 2nd of 4",  0, 1},
    {"Blue",       "top row, 3rd of 4",  0, 2},
    {"Effect",     "middle row, left",   1, 0},
    {"Brightness", "top row, far right", 1, 2},
    {"Mode dial",  "the encoder",        2, 2},
};

static const help_entry_t *help_table(uint8_t *count) {
    if (get_highest_layer(layer_state) == LAYER_RGB) {
        *count = sizeof(help_rgb) / sizeof(help_rgb[0]);
        return help_rgb;
    }
    *count = sizeof(help_media) / sizeof(help_media[0]);
    return help_media;
}

static const uint8_t help_bracket[3][3][2] = {
    { { 9, 13},          {13, 14},          {14, 15} },
    { {10, 12},          {NO_LED, NO_LED},  {15,  5} },
    { {10, 11},          {11, 19},          {19, 17} },
};

static void oled_row(uint8_t row, const char *text);
static void oled_splash(const char *text);
void apply_rgb(void);
static void rgb_defaults(void);

static bool is_held(uint8_t r, uint8_t c) { return held[r][c]; }

static combo_id_t detect_combo(void) {
    bool rewind = is_held(0,1), ff = is_held(0,2), mute = is_held(1,2);
    bool repeat = is_held(2,0), shuffle = is_held(2,1);

    if (rewind && ff && mute)   return COMBO_BOOTLOADER;
    if (repeat && shuffle && mute) return COMBO_EEPROM;
    if (rewind && ff)           return COMBO_REBOOT;
    if (repeat && shuffle)      return COMBO_DEBUG;
    return COMBO_NONE;
}

static void update_hidden_combos(void) {
    if (help_state != HELP_OFF) {
        active_combo = COMBO_NONE;
        combo_fired  = false;
        return;
    }

    if (get_highest_layer(layer_state) >= NUM_FUNC_LAYERS) {
        active_combo = COMBO_NONE;
        combo_fired  = false;
        return;
    }

    combo_id_t detected = detect_combo();

    if (detected == COMBO_NONE) {
        active_combo = COMBO_NONE;
        combo_fired = false;
        return;
    }

    if (detected != active_combo) {
        active_combo = detected;
        combo_start_timer = timer_read32();
        combo_fired = false;
        return;
    }

    if (combo_fired) return;

    uint32_t needed = (detected == COMBO_REBOOT) ? HOLD_TIME_SHORT : HOLD_TIME_LONG;
    if (timer_elapsed32(combo_start_timer) < needed) return;

    combo_fired = true;
    switch (detected) {
        case COMBO_BOOTLOADER:
            oled_splash("BOOTLOADER");
            wait_ms(300);
            bootloader_jump();
            break;
        case COMBO_EEPROM:
            eeconfig_init();
            rgb_matrix_enable_noeeprom();
            rgb_defaults();
            apply_rgb();
            system_msg = "EEPROM CLEARED";
            system_msg_timer = timer_read32();
            break;
        case COMBO_REBOOT:
            oled_splash("REBOOTING");
            wait_ms(300);
            soft_reset_keyboard();
            break;
        case COMBO_DEBUG:
            debug_enable ^= 1;
            system_msg = debug_enable ? "DEBUG ON" : "DEBUG OFF";
            system_msg_timer = timer_read32();
            break;
        case COMBO_NONE:
            break;
    }
}

#define NOWPLAYING_STALE_MS 4000

#define NP_TEXT_BYTES 24

static char np_title[21]  = {0};
static char np_artist[13] = {0};
static uint16_t np_dur_sec = 0;
static bool    np_playing = false;
static bool    np_shuffle = false;
static uint8_t np_repeat  = 0;
static uint32_t np_last_update = 0;
static bool np_has_data = false;

#define NOWPLAYING_SEEK_TOLERANCE 2

static uint16_t np_base_pos      = 0;
static uint32_t np_base_time     = 0;
static uint16_t np_reported_prev = 0;

static uint16_t nowplaying_position(void) {
    uint32_t pos = np_base_pos;
    if (np_playing) {
        pos += timer_elapsed32(np_base_time) / 1000;
    }
    if (np_dur_sec > 0 && pos > np_dur_sec) {
        pos = np_dur_sec;
    }
    return (uint16_t)(pos > UINT16_MAX ? UINT16_MAX : pos);
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 32) return;
    if (data[0] != 0x01) return;

    const uint8_t  flags    = data[1];
    const bool     playing  = (flags & 0x01) != 0;
    const uint16_t reported = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    const uint16_t running  = nowplaying_position();

    const bool     host_moved = reported != np_reported_prev;
    const bool     behind     = running > reported;
    const uint16_t gap        = behind ? running - reported : reported - running;

    np_dur_sec = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    uint8_t title_len  = data[6];
    uint8_t artist_len = data[7];
    if (title_len > NP_TEXT_BYTES) title_len = NP_TEXT_BYTES;
    if ((uint16_t)title_len + artist_len > NP_TEXT_BYTES) {
        artist_len = (uint8_t)(NP_TEXT_BYTES - title_len);
    }

    uint8_t title_keep  = title_len;
    uint8_t artist_keep = artist_len;
    if (title_keep > sizeof(np_title) - 1)   title_keep  = sizeof(np_title) - 1;
    if (artist_keep > sizeof(np_artist) - 1) artist_keep = sizeof(np_artist) - 1;

    char title[sizeof(np_title)];
    memset(title, 0, sizeof(title));
    memcpy(title, &data[8], title_keep);
    const bool track_changed = strcmp(title, np_title) != 0;
    memcpy(np_title, title, sizeof(np_title));

    memset(np_artist, 0, sizeof(np_artist));
    memcpy(np_artist, &data[8 + title_len], artist_keep);

    if (!np_has_data || track_changed || playing != np_playing ||
        (host_moved && (!behind || gap > NOWPLAYING_SEEK_TOLERANCE))) {
        np_base_pos  = reported;
        np_base_time = timer_read32();
    }
    np_reported_prev = reported;

    if (flags & 0x10) {
        system_msg = "not supported here";
        system_msg_timer = timer_read32();
    }

    np_playing  = playing;
    np_shuffle  = (flags & 0x02) != 0;
    np_repeat   = (uint8_t)((flags >> 2) & 0x03);
    np_has_data = true;
    np_last_update = timer_read32();

    if (np_playing) {
        oled_on();
    }
}

#define PROTO_COMMAND      0x02
#define CMD_TOGGLE_SHUFFLE 0x01
#define CMD_CYCLE_REPEAT   0x02

static void send_command(uint8_t cmd) {
    uint8_t pkt[32] = {0};
    pkt[0] = PROTO_COMMAND;
    pkt[1] = cmd;
    raw_hid_send(pkt, sizeof(pkt));
}

static bool nowplaying_is_stale(void) {
    if (!np_has_data) return true;
    return timer_elapsed32(np_last_update) > NOWPLAYING_STALE_MS;
}

/* What the encoder is currently pointed at. Picking a target is a separate
 * press from turning it, so one encoder can drive every RGB parameter. */
enum rgb_channel { CH_NONE, CH_RED, CH_GREEN, CH_BLUE, CH_BRIGHTNESS, CH_EFFECT };
static enum rgb_channel active_channel = CH_NONE;

typedef struct { uint8_t mode; const char *name; bool tinted; } rgb_preset_t;

static const rgb_preset_t rgb_effects[] = {
    {RGB_MATRIX_CYCLE_PINWHEEL,         "Pinwheel",      false},
    {RGB_MATRIX_CYCLE_SPIRAL,           "Vortex",        false},
    {RGB_MATRIX_CYCLE_LEFT_RIGHT,       "Rainbow wave",  false},
    {RGB_MATRIX_RAINBOW_MOVING_CHEVRON, "Chevron",       false},
    {RGB_MATRIX_SPLASH,                 "Splash",        false},
    {RGB_MATRIX_JELLYBEAN_RAINDROPS,    "Raindrops",     false},
    {RGB_MATRIX_SOLID_COLOR,            "Solid",         true},
    {RGB_MATRIX_BREATHING,              "Breathing",     true},
    {RGB_MATRIX_BAND_VAL,               "Band",          true},
    {RGB_MATRIX_BAND_PINWHEEL_VAL,      "Band pinwheel", true},
    {RGB_MATRIX_BAND_SPIRAL_VAL,        "Band spiral",   true},
    {RGB_MATRIX_SOLID_REACTIVE_SIMPLE,  "React simple",  true},
    {RGB_MATRIX_SOLID_REACTIVE,         "React hue",     true},
    {RGB_MATRIX_SOLID_REACTIVE_WIDE,    "React wide",    true},
    {RGB_MATRIX_SOLID_REACTIVE_CROSS,   "React cross",   true},
    {RGB_MATRIX_SOLID_SPLASH,           "Splash solid",  true},
};
#define NUM_EFFECTS (sizeof(rgb_effects) / sizeof(rgb_effects[0]))
static uint8_t effect_index = 0;

static inline uint8_t clamp_add(uint8_t val, int16_t delta) {
    int16_t r = (int16_t)val + delta;
    if (r < 0) return 0;
    if (r > 255) return 255;
    return (uint8_t)r;
}

static inline uint8_t clamp_val(uint8_t val, int16_t delta) {
    int16_t r = (int16_t)val + delta;
    if (r < 0) return 0;
    if (r > RGB_VAL_MAX) return RGB_VAL_MAX;
    return (uint8_t)r;
}

/* QMK ships hsv_to_rgb but no inverse, and rgb_matrix is driven by HSV.
 * Value is dropped on purpose: brightness is its own control (rgb_val). */
static void rgb_to_hs(uint8_t r, uint8_t g, uint8_t b, uint8_t *out_h, uint8_t *out_s) {
    uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    uint8_t chroma = max - min;

    if (chroma == 0 || max == 0) {
        *out_h = 0;
        *out_s = 0;
        return;
    }

    *out_s = (uint8_t)(((uint16_t)chroma * 255) / max);

    int16_t deg;
    if (max == r) {
        deg = (int16_t)(((int32_t)((int16_t)g - (int16_t)b) * 60) / chroma);
    } else if (max == g) {
        deg = (int16_t)(((int32_t)((int16_t)b - (int16_t)r) * 60) / chroma) + 120;
    } else {
        deg = (int16_t)(((int32_t)((int16_t)r - (int16_t)g) * 60) / chroma) + 240;
    }
    if (deg < 0) deg += 360;
    *out_h = (uint8_t)(((uint32_t)deg * 255) / 360);
}

/* The lighting is kept in EEPROM so it survives unplugging. It is written a
 * few seconds after the last turn of the encoder rather than on every step,
 * because one sweep of the dial is dozens of changes and flash wears. */
typedef struct { uint8_t magic, r, g, b, val, effect; } rgb_saved_t;

#define RGB_SAVED_MAGIC   0x4D
#define RGB_SAVE_DELAY_MS 3000

static bool     rgb_unsaved       = false;
static uint32_t rgb_changed_timer = 0;

static void rgb_defaults(void) {
    rgb_r = rgb_g = rgb_b = 255;
    rgb_val      = RGB_VAL_MAX;
    effect_index = 0;
    rgb_unsaved  = false;
}

static void rgb_load(void) {
    rgb_saved_t saved = {0};
    eeconfig_read_user_datablock(&saved, 0, sizeof(saved));

    if (saved.magic != RGB_SAVED_MAGIC || saved.effect >= NUM_EFFECTS || saved.val > RGB_VAL_MAX) {
        return;
    }

    rgb_r        = saved.r;
    rgb_g        = saved.g;
    rgb_b        = saved.b;
    rgb_val      = saved.val;
    effect_index = saved.effect;
}

static void rgb_save_tick(void) {
    if (!rgb_unsaved || timer_elapsed32(rgb_changed_timer) < RGB_SAVE_DELAY_MS) return;

    rgb_saved_t saved = {RGB_SAVED_MAGIC, rgb_r, rgb_g, rgb_b, rgb_val, effect_index};
    eeconfig_update_user_datablock(&saved, 0, sizeof(saved));
    rgb_unsaved = false;
}

void apply_rgb(void) {
    const rgb_preset_t *fx = &rgb_effects[effect_index];
    rgb_matrix_mode_noeeprom(fx->mode);
    if (fx->tinted) {
        uint8_t h, s;
        rgb_to_hs(rgb_r, rgb_g, rgb_b, &h, &s);
        rgb_matrix_sethsv_noeeprom(h, s, rgb_val);
    } else {
        rgb_matrix_sethsv_noeeprom(0, 255, rgb_val);
    }
}

void keyboard_post_init_user(void) {
    rgb_matrix_enable_noeeprom();
    rgb_load();
    apply_rgb();
}

#define ZOOM_CTRL_HOLD_MS 400

static bool     zoom_ctrl_held  = false;
static uint32_t zoom_ctrl_timer = 0;

static void zoom_ctrl_release(void) {
    if (zoom_ctrl_held) {
        unregister_code(KC_LCTL);
        zoom_ctrl_held = false;
    }
}

void matrix_scan_user(void) {
    if (zoom_ctrl_held && timer_elapsed32(zoom_ctrl_timer) > ZOOM_CTRL_HOLD_MS) {
        zoom_ctrl_release();
    }
    update_hidden_combos();
    rgb_save_tick();
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_MEDIA_PLAY_PAUSE,  KC_MEDIA_REWIND,      KC_MEDIA_FAST_FORWARD,
        KC_MEDIA_PREV_TRACK,  KC_MEDIA_NEXT_TRACK,  KC_AUDIO_MUTE,
        KC_REPEAT,            KC_SHUFFLE,           CYCLE_LAYER
    ),
    [1] = LAYOUT(
        _______,              _______,              _______,
        _______,              _______,              _______,
        _______,              _______,              CYCLE_LAYER
    ),
    [2] = LAYOUT(
        _______,              _______,              _______,
        _______,              _______,              _______,
        _______,              _______,              CYCLE_LAYER
    ),
    [3] = LAYOUT(
        _______,              _______,              _______,
        _______,              _______,              _______,
        _______,              _______,              CYCLE_LAYER
    ),
    [4] = LAYOUT(
        _______,              _______,              _______,
        _______,              _______,              _______,
        _______,              _______,              CYCLE_LAYER
    ),
    [5] = LAYOUT(
        RGB_SEL_R,            RGB_SEL_G,            RGB_SEL_B,
        RGB_SEL_FX,           KC_NO,                RGB_SEL_VAL,
        KC_NO,                KC_NO,                CYCLE_LAYER
    )
};

static void enter_layer(uint8_t layer) {
    zoom_ctrl_release();
    layer_move(layer);

    if (layer == LAYER_RGB) {
        active_channel = CH_EFFECT;
        apply_rgb();
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    uint8_t r = record->event.key.row;
    uint8_t c = record->event.key.col;
    if (r < 3 && c < 3) {
        held[r][c] = record->event.pressed;
    }

    if (record->event.pressed && help_state == HELP_OFF &&
        ((r == 0 && c == 0 && held[0][1]) || (r == 0 && c == 1 && held[0][0]))) {
        help_state = HELP_BROWSE;
        help_index = 0;
        picker_open = false;
        return false;
    }

    if (help_state != HELP_OFF) {
        if (record->event.pressed) {
            const bool encoder = (r == 2 && c == 2);
            help_state = (help_state == HELP_BROWSE && encoder) ? HELP_SHOWING : HELP_OFF;
        }
        return false;
    }

    switch (keycode) {
        case KC_SHUFFLE:
            if (record->event.pressed) {
                if (nowplaying_is_stale()) tap_code16(LCTL(KC_S));
                else send_command(CMD_TOGGLE_SHUFFLE);
            }
            return false;

        case KC_REPEAT:
            if (record->event.pressed) {
                if (nowplaying_is_stale()) tap_code16(LCTL(KC_R));
                else send_command(CMD_CYCLE_REPEAT);
            }
            return false;

        case CYCLE_LAYER:
            if (record->event.pressed) {
                if (picker_active()) {
                    picker_open = false;
                    enter_layer(picker_choice);
                } else {
                    picker_open   = true;
                    picker_choice = get_highest_layer(layer_state);
                    if (picker_choice >= NUM_CYCLE_LAYERS) picker_choice = 0;
                    picker_timer = timer_read32();
                }
            }
            return false;

        case RGB_SEL_R:
            if (record->event.pressed) { active_channel = CH_RED; }
            return false;
        case RGB_SEL_G:
            if (record->event.pressed) { active_channel = CH_GREEN; }
            return false;
        case RGB_SEL_B:
            if (record->event.pressed) { active_channel = CH_BLUE; }
            return false;
        case RGB_SEL_VAL:
            if (record->event.pressed) { active_channel = CH_BRIGHTNESS; }
            return false;
        case RGB_SEL_FX:
            if (record->event.pressed) { active_channel = CH_EFFECT; }
            return false;

    }
    return true;
}

static const char *layer_name(uint8_t layer) {
    switch (layer) {
        case 0: return "Volume";
        case 1: return "Track skip";
        case 2: return "Arrows";
        case 3: return "Scroll";
        case 4: return "Zoom";
        case LAYER_RGB: return "RGB";
        default: return "?";
    }
}

oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_180;
}

#define OLED_MAX_COLS (OLED_DISPLAY_WIDTH / OLED_FONT_WIDTH)

static void oled_row_range(uint8_t row, uint8_t from, uint8_t to, const char *text) {
    if (row >= oled_max_lines()) return;

    const uint8_t cols = oled_max_chars();
    if (to > cols) to = cols;
    if (from >= to) return;

    uint8_t       i   = 0;
    const uint8_t col = from;
    oled_set_cursor(col, row);
    for (; col + i < to && text[i] != '\0'; i++) {
        const uint8_t c = (uint8_t)text[i];
        oled_write_char((c >= ' ' && c <= '~') ? (char)c : ' ', false);
    }
    for (; col + i < to; i++) {
        oled_write_char(' ', false);
    }
}

static void oled_row_from(uint8_t row, uint8_t col, const char *text) {
    oled_row_range(row, col, oled_max_chars(), text);
}

static void oled_row(uint8_t row, const char *text) {
    oled_row_from(row, 0, text);
}

static void oled_row_marked(uint8_t row, const char *text, uint8_t mark, uint8_t len) {
    if (row >= oled_max_lines()) return;

    const uint8_t cols = oled_max_chars();
    uint8_t       i    = 0;

    oled_set_cursor(0, row);
    for (; i < cols && text[i] != '\0'; i++) {
        const uint8_t c = (uint8_t)text[i];
        oled_write_char((c >= ' ' && c <= '~') ? (char)c : ' ',
                        len != 0 && i >= mark && i < (uint8_t)(mark + len));
    }
    for (; i < cols; i++) {
        oled_write_char(' ', false);
    }
}

/* The panel goes dark after this long with no key, no turn of the encoder and
 * nothing playing, so a pad left plugged in overnight does not burn the idle
 * screen into the OLED. Any input or a playing track lights it again. */
#define OLED_IDLE_MS 300000

#define OLED_RESYNC_MS 3000
static uint32_t oled_resync_timer = 0;

static void oled_resync_tick(void) {
    if (timer_elapsed32(oled_resync_timer) < OLED_RESYNC_MS) return;
    oled_resync_timer = timer_read32();
    oled_clear();
}

static void oled_rows_blank_from(uint8_t row) {
    for (uint8_t r = row; r < oled_max_lines(); r++) {
        oled_row(r, "");
    }
}

static const uint8_t nibble_doubled[16] = {
    0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
    0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF,
};

#define BIG_CHAR_WIDTH (OLED_FONT_WIDTH * 2)
#define BIG_MAX_CHARS  (OLED_DISPLAY_WIDTH / BIG_CHAR_WIDTH)

static char    big_text[BIG_MAX_CHARS + 1]                 = {0};
static uint8_t big_glyphs[BIG_MAX_CHARS * OLED_FONT_WIDTH] = {0};
static uint8_t big_len                                     = 0;

static void big_rasterise(const char *text) {
    big_len = 0;
    while (big_len < BIG_MAX_CHARS && text[big_len] != '\0') {
        big_text[big_len] = text[big_len];
        big_len++;
    }
    big_text[big_len] = '\0';
    if (big_len == 0) return;

    oled_set_cursor(0, 0);
    for (uint8_t i = 0; i < big_len; i++) {
        const char c = big_text[i];
        oled_write_char((c >= ' ' && c <= '~') ? c : ' ', false);
    }

    oled_buffer_reader_t reader = oled_read_raw(0);
    memcpy(big_glyphs, reader.current_element, (uint16_t)big_len * OLED_FONT_WIDTH);
}

static void oled_big_centered(const char *header, const char *text, const char *footer) {
    if (oled_max_lines() < 2) return;

    if (strncmp(big_text, text, BIG_MAX_CHARS) != 0) {
        big_rasterise(text);
    }
    if (big_len == 0) return;

    const uint8_t page = (uint8_t)((oled_max_lines() - 2) / 2);
    const uint8_t span = (uint8_t)(big_len * BIG_CHAR_WIDTH);
    const uint8_t x0   = (uint8_t)((OLED_DISPLAY_WIDTH - span) / 2);
    const uint8_t cols = (uint8_t)(oled_max_chars() * OLED_FONT_WIDTH);

    const uint8_t last = (uint8_t)(oled_max_lines() - 1);
    for (uint8_t r = 0; r < oled_max_lines(); r++) {
        if (r == page || r == page + 1) continue;
        if (r == 0) {
            oled_row(r, header != NULL ? header : "");
        } else if (r == last) {
            oled_row(r, footer != NULL ? footer : "");
        } else {
            oled_row(r, "");
        }
    }

    for (uint8_t x = 0; x < cols; x++) {
        uint8_t top = 0x00;
        uint8_t bot = 0x00;

        if (x >= x0 && x < x0 + span) {
            const uint8_t off = (uint8_t)(x - x0);
            const uint8_t src = big_glyphs[(off / BIG_CHAR_WIDTH) * OLED_FONT_WIDTH
                                           + (off % BIG_CHAR_WIDTH) / 2];
            top = nibble_doubled[src & 0x0F];
            bot = nibble_doubled[src >> 4];
        }

        oled_write_raw_byte((char)top, page * OLED_DISPLAY_WIDTH + x);
        oled_write_raw_byte((char)bot, (page + 1) * OLED_DISPLAY_WIDTH + x);
    }
}

static void oled_splash(const char *text) {
    for (uint8_t r = 0; r < oled_max_lines(); r++) {
        oled_row(r, r == 1 ? text : "");
    }
    oled_render_dirty(true);
}

#define MARQUEE_STEP_MS 300

#define MARQUEE_GAP "     "

static char marquee_buf[sizeof(np_artist) + 3 + sizeof(np_title) + sizeof(MARQUEE_GAP)];
static uint16_t marquee_len = 0;
static uint16_t marquee_offset = 0;
static uint32_t marquee_timer = 0;

static void marquee_rebuild(void) {
    if (np_artist[0] != '\0') {
        snprintf(marquee_buf, sizeof(marquee_buf), "%s - %s" MARQUEE_GAP, np_artist, np_title);
    } else {
        snprintf(marquee_buf, sizeof(marquee_buf), "%s" MARQUEE_GAP, np_title);
    }
    marquee_len = strlen(marquee_buf);
    marquee_offset = 0;
    marquee_timer = timer_read32();
}

static void marquee_draw(uint8_t row) {
    char window[OLED_MAX_COLS + 1];
    uint8_t width = oled_max_chars();
    if (width > OLED_MAX_COLS) width = OLED_MAX_COLS;

    if (marquee_len == 0) {
        oled_row(row, "");
        return;
    }
    if (marquee_len <= width) {
        oled_row(row, marquee_buf);
        return;
    }
    for (uint8_t i = 0; i < width; i++) {
        window[i] = marquee_buf[(marquee_offset + i) % marquee_len];
    }
    window[width] = '\0';
    oled_row(row, window);

    if (timer_elapsed32(marquee_timer) > MARQUEE_STEP_MS) {
        marquee_offset = (marquee_offset + 1) % marquee_len;
        marquee_timer = timer_read32();
    }
}

#define BAR_WIDTH 18

static void draw_progress_bar(uint8_t row, uint16_t pos, uint16_t dur) {
    char bar[BAR_WIDTH + 3] = {0};
    bar[0] = '[';
    uint8_t filled = 0;
    if (dur > 0) {
        filled = (uint32_t)pos * BAR_WIDTH / dur;
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;
    }
    for (uint8_t i = 0; i < BAR_WIDTH; i++) {
        bar[1 + i] = (i < filled) ? '#' : '-';
    }
    bar[1 + BAR_WIDTH] = ']';
    bar[2 + BAR_WIDTH] = '\0';
    oled_row(row, bar);
}

static const uint8_t mark_play[OLED_FONT_WIDTH]  = {0x7F, 0x7F, 0x3E, 0x3E, 0x1C, 0x08};
static const uint8_t mark_pause[OLED_FONT_WIDTH] = {0x7F, 0x7F, 0x00, 0x7F, 0x7F, 0x00};

#define MARK_WIDE (OLED_FONT_WIDTH * 2)

static const uint8_t mark_shuffle[MARK_WIDE] = {
    0x42, 0x42, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x42, 0xE7, 0x42, 0x00,
};
static const uint8_t mark_repeat_all[MARK_WIDE] = {
    0x7E, 0x81, 0x81, 0x81, 0x83, 0x87, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t mark_repeat_one[MARK_WIDE] = {
    0x7E, 0x81, 0x81, 0x81, 0x83, 0x87, 0x7E, 0x00, 0x84, 0xFE, 0x80, 0x00,
};

static void oled_glyph_at(uint8_t row, uint8_t col, const uint8_t *glyph, uint8_t width) {
    const uint16_t base = (uint16_t)row * OLED_DISPLAY_WIDTH
                        + (uint16_t)col * OLED_FONT_WIDTH;
    for (uint8_t i = 0; i < width; i++) {
        oled_write_raw_byte((char)(glyph != NULL ? glyph[i] : 0x00), base + i);
    }
}

static void draw_time_line(uint8_t row, uint16_t pos, uint16_t dur) {
    char line[OLED_MAX_COLS + 1];
    snprintf(line, sizeof(line), " %2u:%02u / %2u:%02u",
             pos / 60, pos % 60, dur / 60, dur % 60);
    oled_row_range(row, 1, 17, line);

    oled_glyph_at(row, 0, np_playing ? mark_play : mark_pause, OLED_FONT_WIDTH);
    oled_glyph_at(row, 17, np_shuffle ? mark_shuffle : NULL, MARK_WIDE);
    oled_glyph_at(row, 19, np_repeat == 1 ? mark_repeat_one
                         : np_repeat == 2 ? mark_repeat_all
                                          : NULL, MARK_WIDE);
}

/* On the RGB layer the display becomes the control panel: which effect is
 * live, the mixed colour, and what the encoder is about to change. */
static void draw_rgb_panel(void) {
    char line[OLED_MAX_COLS + 1];
    const rgb_preset_t *fx = &rgb_effects[effect_index];
    const bool on_colour = (active_channel == CH_RED ||
                            active_channel == CH_GREEN ||
                            active_channel == CH_BLUE);

    snprintf(line, sizeof(line), "%-13s %2u/%u", fx->name,
             (unsigned)(effect_index + 1), (unsigned)NUM_EFFECTS);
    oled_row_marked(0, line, 0, active_channel == CH_EFFECT ? 13 : 0);

    snprintf(line, sizeof(line), "R%3u G%3u B%3u", rgb_r, rgb_g, rgb_b);
    uint8_t mark = 0;
    switch (active_channel) {
        case CH_RED:   mark = 0;  break;
        case CH_GREEN: mark = 5;  break;
        case CH_BLUE:  mark = 10; break;
        default:       break;
    }
    oled_row_marked(1, line, mark, on_colour ? 4 : 0);

    snprintf(line, sizeof(line), "Bright %3u", rgb_val);
    oled_row_marked(2, line, 0, active_channel == CH_BRIGHTNESS ? 10 : 0);

    oled_row(3, (on_colour && !fx->tinted) ? "effect ignores RGB" : "");
    oled_rows_blank_from(4);
}

static char last_np_title[sizeof(np_title)]   = {0};
static char last_np_artist[sizeof(np_artist)] = {0};

bool oled_task_user(void) {
    if (help_state == HELP_OFF && !(np_playing && !nowplaying_is_stale()) &&
        last_input_activity_elapsed() > OLED_IDLE_MS) {
        oled_off();
        return false;
    }
    oled_on();

    oled_resync_tick();

    if (help_state != HELP_OFF) {
        uint8_t             count = 0;
        const help_entry_t *table = help_table(&count);
        if (help_index >= count) help_index = 0;

        char caption[OLED_MAX_COLS + 1];
        if (help_state == HELP_BROWSE) {
            snprintf(caption, sizeof(caption), "Help  %u/%u",
                     (unsigned)(help_index + 1), (unsigned)count);
            oled_big_centered(caption, table[help_index].label, "Click to show");
        } else {
            const help_entry_t *e    = &table[help_index];
            const uint8_t      *pair = help_bracket[e->row][e->col];
            const bool          lit  =
                (pair[0] != NO_LED && pair[0] < RGB_MATRIX_LED_COUNT) ||
                (pair[1] != NO_LED && pair[1] < RGB_MATRIX_LED_COUNT);

            oled_big_centered(lit ? "Showing" : "Showing (text only)",
                              e->label, e->where);
        }
        return false;
    }

    if (system_msg != NULL && timer_elapsed32(system_msg_timer) < SYSTEM_MSG_DURATION) {
        oled_row(0, "");
        oled_row(1, system_msg);
        oled_rows_blank_from(2);
        return false;
    } else if (system_msg != NULL) {
        system_msg = NULL;
    }

    if (picker_active()) {
        oled_big_centered("Select mode", layer_name(picker_choice), "Click to confirm");
        return false;
    }
    picker_open = false;

    uint8_t layer = get_highest_layer(layer_state);
    if (layer == LAYER_RGB) {
        draw_rgb_panel();
        return false;
    }

    if (!nowplaying_is_stale() && np_title[0] != '\0') {
        if (strcmp(last_np_title, np_title) != 0 || strcmp(last_np_artist, np_artist) != 0) {
            memcpy(last_np_title, np_title, sizeof(last_np_title));
            memcpy(last_np_artist, np_artist, sizeof(last_np_artist));
            marquee_rebuild();
        }
        const uint16_t pos = nowplaying_position();
        marquee_draw(0);
        oled_row(1, "");
        draw_progress_bar(2, pos, np_dur_sec);
        draw_time_line(3, pos, np_dur_sec);
        oled_rows_blank_from(4);
    } else {
        char line[OLED_MAX_COLS + 1];
        snprintf(line, sizeof(line), "mediapad   %u LED", (unsigned)RGB_MATRIX_LED_COUNT);
        oled_row(0, line);
        oled_row(1, "");
        oled_row(2, layer_name(layer));
        oled_row(3, nowplaying_is_stale() ? "waiting for app" : "nothing playing");
        oled_rows_blank_from(4);
    }

    return false;
}

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (index != 0) return false;

    if (help_state == HELP_BROWSE) {
        uint8_t count = 0;
        help_table(&count);
        help_index = clockwise ? (uint8_t)((help_index + 1) % count)
                               : (uint8_t)((help_index + count - 1) % count);
        return false;
    }
    if (help_state == HELP_SHOWING) return false;

    if (picker_active()) {
        picker_choice = clockwise
                            ? (uint8_t)((picker_choice + 1) % NUM_CYCLE_LAYERS)
                            : (uint8_t)((picker_choice + NUM_CYCLE_LAYERS - 1) % NUM_CYCLE_LAYERS);
        picker_timer = timer_read32();
        return false;
    }

    uint8_t layer = get_highest_layer(layer_state);
    int16_t delta = clockwise ? RGB_STEP : -RGB_STEP;

    switch (layer) {
        case 0:
            if (clockwise) { tap_code(KC_AUDIO_VOL_UP); }
            else { tap_code(KC_AUDIO_VOL_DOWN); }
            break;
        case 1:
            if (clockwise) { tap_code(KC_MEDIA_NEXT_TRACK); }
            else { tap_code(KC_MEDIA_PREV_TRACK); }
            break;
        case 2:
            if (clockwise) { tap_code(KC_UP); }
            else { tap_code(KC_DOWN); }
            break;
        case 3:
            if (clockwise) { tap_code(MS_WHLU); }
            else { tap_code(MS_WHLD); }
            break;
        case 4:
            if (!zoom_ctrl_held) {
                register_code(KC_LCTL);
                zoom_ctrl_held = true;
                wait_ms(12);
            }
            zoom_ctrl_timer = timer_read32();
            tap_code_delay(clockwise ? MS_WHLU : MS_WHLD, 12);
            break;

        case LAYER_RGB:
            switch (active_channel) {
                case CH_RED:        rgb_r   = clamp_add(rgb_r, delta);   break;
                case CH_GREEN:      rgb_g   = clamp_add(rgb_g, delta);   break;
                case CH_BLUE:       rgb_b   = clamp_add(rgb_b, delta);   break;
                case CH_BRIGHTNESS: rgb_val = clamp_val(rgb_val, delta); break;
                case CH_EFFECT:
                    effect_index = clockwise
                        ? (uint8_t)((effect_index + 1) % NUM_EFFECTS)
                        : (uint8_t)((effect_index + NUM_EFFECTS - 1) % NUM_EFFECTS);
                    break;
                default: break;
            }
            apply_rgb();
            rgb_unsaved       = true;
            rgb_changed_timer = timer_read32();
            break;
    }
    return false;
}

bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    if (help_state != HELP_SHOWING) return true;

    uint8_t             count = 0;
    const help_entry_t *table = help_table(&count);
    if (help_index >= count) return false;

    for (uint8_t i = led_min; i < led_max; i++) {
        rgb_matrix_set_color(i, 0, 0, 0);
    }

    const help_entry_t *entry = &table[help_index];
    const uint8_t      *pair  = help_bracket[entry->row][entry->col];

    for (uint8_t k = 0; k < 2; k++) {
        const uint8_t led = pair[k];
        if (led == NO_LED || led >= RGB_MATRIX_LED_COUNT) continue;
        if (led < led_min || led >= led_max) continue;
        rgb_matrix_set_color(led, RGB_VAL_MAX, RGB_VAL_MAX, RGB_VAL_MAX);
    }
    return false;
}

/*
 * Encoder modes, in the order the picker steps through them.
 *
 * The encoder switch opens a picker that previews rather than switches:
 *
 *   click                   open it, showing the mode you are already in
 *   turn while it is open   walk the list - nothing changes yet
 *   click again             confirm, and only now does the mode change
 *   let it lapse            stay exactly where you were
 *
 * Confirming is what keeps a stray turn from dropping the board into the RGB
 * mode and re-applying its effect behind your back.
 *
 *   #  Layer     OLED label    Turn CW              Turn CCW
 *   -  --------  ------------  -------------------  -------------------
 *   0  base      "Volume"      volume up            volume down
 *   1            "Track skip"  next track           previous track
 *   2            "Arrows"      arrow up             arrow down
 *   3            "Scroll"      mouse wheel up       mouse wheel down
 *   4            "Zoom"        zoom in  (Ctrl +)    zoom out (Ctrl -)
 *   5  LAYER_RGB "RGB"         selected target +    selected target -
 *
 *   Physical layout        SW1 SW2 SW3 SW6
 *                          SW4 SW5
 *                          SW7 SW8   (SW9, the encoder, right of SW8)
 *
 * Layers 0-4 all carry the same keys - only the encoder changes - so 1-4 are
 * transparent and fall through to layer 0:
 *
 *   SW1  play / pause        SW2  rewind         SW3  fast forward
 *   SW4  previous track      SW5  next track     SW6  mute
 *   SW7  repeat (Ctrl+R)     SW8  shuffle (Ctrl+S)
 *   SW9  mode picker (encoder push)
 *
 * Layer 5 is the one place keys change meaning, and nothing there falls through
 * to the media set, so playback cannot be disturbed while dialling colours:
 *
 *   SW1/SW2/SW3  point the dial at red / green / blue
 *   SW4          point the dial at the effect list (default on entry)
 *   SW6          point the dial at brightness
 *   SW5/SW7/SW8  dead - the dial steps the effect list, so there is one way to
 *                change a thing rather than two
 *
 * The effect list is one list of 16, not two. The first six cycle through their
 * own colours and ignore the mix; the rest come out in whatever R/G/B is dialled
 * in. Which is which is carried per entry as `tinted`, so nothing else has to
 * care where the boundary falls.
 *
 * Step size is RGB_STEP (8) per detent. Brightness is capped at RGB_VAL_MAX,
 * which follows rgb_matrix.max_brightness from whichever revision is being
 * built, to keep the SK6812 chain inside a sane USB current budget.
 *
 * The hidden combos are armed on layers 0-4 only - see update_hidden_combos().
 *
 * Help browser: play/pause + rewind together. Turn to walk the list, click the
 * encoder to answer, and any switch backs out. The list follows the mode it was
 * opened from.
 *
 * The answer comes two ways at once: the LEDs either side of the key light up
 * with the key itself as the dark gap between them, and the display spells the
 * position out in words. The words are the part that always works - see
 * help_bracket for why pointing at a single LED cannot.
 *
 * Reactive effects (React*, Splash*) need RGB_MATRIX_KEYPRESSES, set in
 * config.h, plus the per-LED positions in the revision's rgb_matrix.layout.
 */
