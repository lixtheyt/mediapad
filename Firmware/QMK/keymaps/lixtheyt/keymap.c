// Copyright 2026 Lix
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include <string.h>
#include <stdio.h>

enum custom_keycodes {
    KC_SHUFFLE = SAFE_RANGE,
    KC_REPEAT,
    CYCLE_LAYER,
    RGB_MODE_CUSTOM,
    RGB_MODE_SCENE,
    RGB_SEL_R,
    RGB_SEL_G,
    RGB_SEL_B,
    RGB_SEL_VAL,
    RGB_SEL_SCENE,
    RGB_FX_NEXT,
    RGB_FX_PREV
};

#define NUM_FUNC_LAYERS 5
#define LAYER_RGB_CUSTOM 5
#define LAYER_RGB_SCENE 6
#define RGB_STEP 8

/* Must match rgb_matrix.max_brightness in keyboard.json. 20 SK6812s at full
 * white would pull well over an amp, so brightness is capped here too and the
 * OLED reports the same number the driver actually uses. */
#define RGB_VAL_MAX 150

static uint8_t rgb_r = 255, rgb_g = 255, rgb_b = 255;
static uint8_t rgb_val = RGB_VAL_MAX;

static uint32_t oled_message_timer = 0;
#define OLED_MESSAGE_DURATION 3000

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

static bool is_held(uint8_t r, uint8_t c) { return held[r][c]; }

static combo_id_t detect_combo(void) {
    bool rewind = is_held(0,0), ff = is_held(0,2), mute = is_held(1,2);
    bool repeat = is_held(2,0), shuffle = is_held(2,1);

    if (rewind && ff && mute)   return COMBO_BOOTLOADER;
    if (repeat && shuffle && mute) return COMBO_EEPROM;
    if (rewind && ff)           return COMBO_REBOOT;
    if (repeat && shuffle)      return COMBO_DEBUG;
    return COMBO_NONE;
}

static void update_hidden_combos(void) {
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
            oled_clear();
            oled_write_ln("BOOTLOADER", false);
            oled_render();
            wait_ms(300);
            bootloader_jump();
            break;
        case COMBO_EEPROM:
            eeconfig_init();
            system_msg = "EEPROM CLEARED";
            system_msg_timer = timer_read32();
            break;
        case COMBO_REBOOT:
            oled_clear();
            oled_write_ln("REBOOTING", false);
            oled_render();
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

static char np_title[13]  = {0};
static char np_artist[12] = {0};
static uint16_t np_pos_sec = 0;
static uint16_t np_dur_sec = 0;
static bool np_playing = false;
static uint32_t np_last_update = 0;
static bool np_has_data = false;

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 32) return;
    if (data[0] != 0x01) return;

    np_playing = data[1] != 0;
    np_pos_sec = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    np_dur_sec = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    uint8_t title_len = data[6];
    if (title_len > 12) title_len = 12;
    memset(np_title, 0, sizeof(np_title));
    memcpy(np_title, &data[7], title_len);

    uint8_t artist_len = data[19];
    if (artist_len > 11) artist_len = 11;
    memset(np_artist, 0, sizeof(np_artist));
    memcpy(np_artist, &data[20], artist_len);

    np_has_data = true;
    np_last_update = timer_read32();
}

static bool nowplaying_is_stale(void) {
    if (!np_has_data) return true;
    return timer_elapsed32(np_last_update) > NOWPLAYING_STALE_MS;
}

/* What the encoder is currently pointed at. Picking a target is a separate
 * press from turning it, so one encoder can drive every RGB parameter. */
enum rgb_channel { CH_NONE, CH_RED, CH_GREEN, CH_BLUE, CH_BRIGHTNESS, CH_SCENE };
static enum rgb_channel active_channel = CH_NONE;

typedef struct { uint8_t mode; const char *name; } rgb_preset_t;

/* Scenes are whole animations, the sort of thing a normal keyboard ships with.
 * Splash also reacts to keypresses, spreading out from the key you hit. */
static const rgb_preset_t rgb_scenes[] = {
    {RGB_MATRIX_CYCLE_PINWHEEL,         "Pinwheel"},
    {RGB_MATRIX_CYCLE_SPIRAL,           "Vortex"},
    {RGB_MATRIX_CYCLE_LEFT_RIGHT,       "Rainbow wave"},
    {RGB_MATRIX_RAINBOW_MOVING_CHEVRON, "Chevron"},
    {RGB_MATRIX_SPLASH,                 "Splash"},
    {RGB_MATRIX_JELLYBEAN_RAINDROPS,    "Raindrops"},
};
#define NUM_SCENES (sizeof(rgb_scenes) / sizeof(rgb_scenes[0]))
static uint8_t scene_index = 0;

/* Custom effects all honour the hue/sat dialled in with RGB_SEL_R/G/B, so the
 * colour you mix is the colour that comes out. */
static const rgb_preset_t rgb_custom_fx[] = {
    {RGB_MATRIX_SOLID_COLOR,           "Solid"},
    {RGB_MATRIX_BREATHING,             "Breathing"},
    {RGB_MATRIX_BAND_VAL,              "Band"},
    {RGB_MATRIX_BAND_PINWHEEL_VAL,     "Band pinwheel"},
    {RGB_MATRIX_BAND_SPIRAL_VAL,       "Band spiral"},
    {RGB_MATRIX_SOLID_REACTIVE_SIMPLE, "React simple"},
    {RGB_MATRIX_SOLID_REACTIVE,        "React hue"},
    {RGB_MATRIX_SOLID_REACTIVE_WIDE,   "React wide"},
    {RGB_MATRIX_SOLID_REACTIVE_CROSS,  "React cross"},
    {RGB_MATRIX_SOLID_SPLASH,          "Splash solid"},
};
#define NUM_CUSTOM_FX (sizeof(rgb_custom_fx) / sizeof(rgb_custom_fx[0]))
static uint8_t custom_fx_index = 0;

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

void apply_rgb_custom(void) {
    uint8_t h, s;
    rgb_to_hs(rgb_r, rgb_g, rgb_b, &h, &s);
    rgb_matrix_mode_noeeprom(rgb_custom_fx[custom_fx_index].mode);
    rgb_matrix_sethsv_noeeprom(h, s, rgb_val);
}

void apply_rgb_scene(void) {
    rgb_matrix_mode_noeeprom(rgb_scenes[scene_index].mode);
    /* Scenes generate their own hue; brightness is the only part we set. */
    rgb_matrix_sethsv_noeeprom(0, 255, rgb_val);
}

void keyboard_post_init_user(void) {
    rgb_matrix_enable_noeeprom();
    apply_rgb_scene();
}

void matrix_scan_user(void) {
    update_hidden_combos();
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_MEDIA_REWIND,   KC_MEDIA_PLAY_PAUSE, KC_MEDIA_FAST_FORWARD,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     KC_AUDIO_MUTE,
        KC_REPEAT,         KC_SHUFFLE,          CYCLE_LAYER
    ),
    [1] = LAYOUT(
        _______,           _______,             _______,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     _______,
        _______,           _______,             CYCLE_LAYER
    ),
    [2] = LAYOUT(
        _______,           _______,             _______,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     _______,
        _______,           _______,             CYCLE_LAYER
    ),
    [3] = LAYOUT(
        _______,           _______,             _______,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     _______,
        _______,           _______,             CYCLE_LAYER
    ),
    [4] = LAYOUT(
        _______,           _______,             _______,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     _______,
        _______,           _______,             CYCLE_LAYER
    ),
    [5] = LAYOUT(
        RGB_SEL_R,         RGB_SEL_G,           RGB_SEL_B,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     RGB_SEL_VAL,
        RGB_FX_PREV,       RGB_FX_NEXT,         CYCLE_LAYER
    ),
    [6] = LAYOUT(
        _______,           _______,             _______,
        RGB_SEL_SCENE,     RGB_MODE_CUSTOM,     RGB_SEL_VAL,
        _______,           _______,             CYCLE_LAYER
    )
};

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    uint8_t r = record->event.key.row;
    uint8_t c = record->event.key.col;
    if (r < 3 && c < 3) {
        held[r][c] = record->event.pressed;
    }

    switch (keycode) {
        case KC_SHUFFLE:
            if (record->event.pressed) { tap_code16(LCTL(KC_S)); }
            return false;

        case KC_REPEAT:
            if (record->event.pressed) { tap_code16(LCTL(KC_R)); }
            return false;

        case CYCLE_LAYER:
            if (record->event.pressed) {
                uint8_t current = get_highest_layer(layer_state);
                if (current < NUM_FUNC_LAYERS) {
                    layer_move((current + 1) % NUM_FUNC_LAYERS);
                } else {
                    layer_move(0);
                }
                oled_message_timer = timer_read32();
            }
            return false;

        case RGB_MODE_CUSTOM:
            if (record->event.pressed) {
                layer_move(LAYER_RGB_CUSTOM);
                /* Point the encoder at something straight away so it is never
                 * dead on arrival; R/G/B repoint it in one press. */
                active_channel = CH_BRIGHTNESS;
                apply_rgb_custom();
            }
            return false;

        case RGB_MODE_SCENE:
            if (record->event.pressed) {
                layer_move(LAYER_RGB_SCENE);
                active_channel = CH_SCENE;
                apply_rgb_scene();
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
        case RGB_SEL_SCENE:
            if (record->event.pressed) { active_channel = CH_SCENE; }
            return false;

        case RGB_FX_NEXT:
            if (record->event.pressed) {
                custom_fx_index = (custom_fx_index + 1) % NUM_CUSTOM_FX;
                apply_rgb_custom();
            }
            return false;
        case RGB_FX_PREV:
            if (record->event.pressed) {
                custom_fx_index = (uint8_t)((custom_fx_index + NUM_CUSTOM_FX - 1) % NUM_CUSTOM_FX);
                apply_rgb_custom();
            }
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
        case LAYER_RGB_CUSTOM: return "RGB Custom";
        case LAYER_RGB_SCENE:  return "RGB Scene";
        default: return "?";
    }
}

static const char *channel_name(void) {
    switch (active_channel) {
        case CH_RED:        return "R";
        case CH_GREEN:      return "G";
        case CH_BLUE:       return "B";
        case CH_BRIGHTNESS: return "Bright";
        case CH_SCENE:      return "Scene";
        default:            return "-";
    }
}

oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_0;
}

#define MARQUEE_WIDTH 20
#define MARQUEE_STEP_MS 300

static char marquee_buf[13 + 3 + 12 + 4] = {0};
static uint16_t marquee_len = 0;
static uint16_t marquee_offset = 0;
static uint32_t marquee_timer = 0;

static void marquee_rebuild(void) {
    marquee_buf[0] = '\0';
    if (np_artist[0] != '\0') {
        strncat(marquee_buf, np_artist, sizeof(marquee_buf) - 1);
        strncat(marquee_buf, " - ", sizeof(marquee_buf) - strlen(marquee_buf) - 1);
    }
    strncat(marquee_buf, np_title, sizeof(marquee_buf) - strlen(marquee_buf) - 1);
    strncat(marquee_buf, "     ", sizeof(marquee_buf) - strlen(marquee_buf) - 1);
    marquee_len = strlen(marquee_buf);
    marquee_offset = 0;
}

static void marquee_draw(void) {
    char window[MARQUEE_WIDTH + 1];
    if (marquee_len == 0) {
        oled_write_ln("", false);
        return;
    }
    if (marquee_len <= MARQUEE_WIDTH) {
        oled_write_ln(marquee_buf, false);
        return;
    }
    for (uint8_t i = 0; i < MARQUEE_WIDTH; i++) {
        window[i] = marquee_buf[(marquee_offset + i) % marquee_len];
    }
    window[MARQUEE_WIDTH] = '\0';
    oled_write_ln(window, false);

    if (timer_elapsed32(marquee_timer) > MARQUEE_STEP_MS) {
        marquee_offset = (marquee_offset + 1) % marquee_len;
        marquee_timer = timer_read32();
    }
}

static void draw_progress_bar(uint16_t pos, uint16_t dur) {
    #define BAR_WIDTH 18
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
    oled_write_ln(bar, false);
}

static void draw_time_line(uint16_t pos, uint16_t dur) {
    char line[MARQUEE_WIDTH + 1];
    snprintf(line, sizeof(line), "%2u:%02u / %2u:%02u",
             pos / 60, pos % 60, dur / 60, dur % 60);
    oled_write_ln(line, false);
}

/* On the RGB layers the display becomes the control panel: which effect is
 * live, the mixed colour, and what the encoder is about to change. */
static void draw_rgb_panel(uint8_t layer) {
    char line[MARQUEE_WIDTH + 2];
    oled_clear();
    if (layer == LAYER_RGB_SCENE) {
        oled_write_ln("RGB Scene", false);
        oled_write_ln(rgb_scenes[scene_index].name, false);
        snprintf(line, sizeof(line), "Val %3u", rgb_val);
        oled_write_ln(line, false);
        snprintf(line, sizeof(line), "Enc: %s", channel_name());
        oled_write_ln(line, false);
    } else {
        oled_write_ln("RGB Custom", false);
        oled_write_ln(rgb_custom_fx[custom_fx_index].name, false);
        snprintf(line, sizeof(line), "R%3u G%3u B%3u", rgb_r, rgb_g, rgb_b);
        oled_write_ln(line, false);
        snprintf(line, sizeof(line), "V%3u  Enc:%s", rgb_val, channel_name());
        oled_write_ln(line, false);
    }
}

static char last_title_for_marquee[13] = {0};

bool oled_task_user(void) {
    if (system_msg != NULL && timer_elapsed32(system_msg_timer) < SYSTEM_MSG_DURATION) {
        oled_clear();
        oled_set_cursor(0, 1);
        oled_write_ln(system_msg, false);
        return false;
    } else if (system_msg != NULL) {
        system_msg = NULL;
    }

    if (oled_message_timer != 0 && timer_elapsed32(oled_message_timer) < OLED_MESSAGE_DURATION) {
        oled_clear();
        oled_set_cursor(0, 1);
        oled_write_ln("Mode:", false);
        oled_write_ln(layer_name(get_highest_layer(layer_state)), false);
        return false;
    }
    oled_message_timer = 0;

    uint8_t layer = get_highest_layer(layer_state);
    if (layer == LAYER_RGB_CUSTOM || layer == LAYER_RGB_SCENE) {
        draw_rgb_panel(layer);
        return false;
    }

    if (!nowplaying_is_stale() && np_title[0] != '\0') {
        if (strncmp(last_title_for_marquee, np_title, sizeof(last_title_for_marquee)) != 0) {
            strncpy(last_title_for_marquee, np_title, sizeof(last_title_for_marquee) - 1);
            marquee_rebuild();
        }
        oled_clear();
        oled_set_cursor(0, 0);
        marquee_draw();
        oled_write_ln("", false);
        draw_progress_bar(np_pos_sec, np_dur_sec);
        draw_time_line(np_pos_sec, np_dur_sec);
    } else {
        oled_clear();
        oled_write_ln("Hackpad", false);
    }
    return false;
}

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (index != 0) return false;

    uint8_t layer = get_highest_layer(layer_state);
    int16_t delta = clockwise ? RGB_STEP : -RGB_STEP;

    switch (layer) {
        case 0:
            if (clockwise) { tap_code(KC_KB_VOLUME_UP); }
            else { tap_code(KC_KB_VOLUME_DOWN); }
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
            if (clockwise) { tap_code16(LCTL(KC_EQUAL)); }
            else { tap_code16(LCTL(KC_MINUS)); }
            break;

        case LAYER_RGB_CUSTOM:
            switch (active_channel) {
                case CH_RED:        rgb_r   = clamp_add(rgb_r, delta);   break;
                case CH_GREEN:      rgb_g   = clamp_add(rgb_g, delta);   break;
                case CH_BLUE:       rgb_b   = clamp_add(rgb_b, delta);   break;
                case CH_BRIGHTNESS: rgb_val = clamp_val(rgb_val, delta); break;
                default: break;
            }
            apply_rgb_custom();
            break;

        case LAYER_RGB_SCENE:
            if (active_channel == CH_SCENE) {
                scene_index = clockwise
                    ? (uint8_t)((scene_index + 1) % NUM_SCENES)
                    : (uint8_t)((scene_index + NUM_SCENES - 1) % NUM_SCENES);
            } else {
                rgb_val = clamp_val(rgb_val, delta);
            }
            apply_rgb_scene();
            break;
    }
    return false;
}

/*
 * Encoder modes, in the order CYCLE_LAYER steps through them.
 *
 * Tapping the encoder (CYCLE_LAYER) walks 0 -> 1 -> 2 -> 3 -> 4 -> 0.
 * The two RGB layers are not part of that cycle: they are entered directly
 * with RGB_MODE_SCENE / RGB_MODE_CUSTOM, and CYCLE_LAYER drops back to 0.
 *
 *   #  Layer            OLED label    Turn CW              Turn CCW
 *   -  ---------------  ------------  -------------------  -------------------
 *   0  base             "Volume"      volume up            volume down
 *   1                   "Track skip"  next track           previous track
 *   2                   "Arrows"      arrow up             arrow down
 *   3                   "Scroll"      mouse wheel up       mouse wheel down
 *   4                   "Zoom"        zoom in  (Ctrl +)    zoom out (Ctrl -)
 *   5  LAYER_RGB_CUSTOM "RGB Custom"  selected target +    selected target -
 *   6  LAYER_RGB_SCENE  "RGB Scene"   selected target +    selected target -
 *
 * On both RGB layers the encoder is a general-purpose dial: you first press a
 * key to say what it should act on, then turn it. The OLED shows the current
 * target on the "Enc:" line, so it is never a guess.
 *
 *   Physical layout        SW1 SW2 SW3 SW6
 *                          SW4 SW5
 *                          SW7 SW8   (encoder)
 *
 *   Layer 6, "RGB Scene" - entered with SW4 from any other layer
 *     SW4  point encoder at the scene list (default on entry)
 *     SW6  point encoder at brightness
 *     SW5  switch over to RGB Custom
 *     rest fall through to layer 0, so the media keys still work here
 *     5 scenes: Pinwheel, Rainbow wave, Chevron, Splash, Raindrops
 *
 *   Layer 5, "RGB Custom" - entered with SW5 from any other layer
 *     SW1/SW2/SW3  point encoder at red / green / blue
 *     SW6          point encoder at brightness (default on entry)
 *     SW7/SW8      previous / next effect
 *     SW4          switch back to RGB Scene
 *     10 effects: Solid, Breathing, Band, Band pinwheel, Band spiral,
 *                 React simple, React hue, React wide, React cross,
 *                 Splash solid
 *
 * Step size is RGB_STEP (8) per detent. Brightness is capped at RGB_VAL_MAX
 * (150) to keep 20 SK6812s inside a sane USB current budget.
 *
 * Reactive effects (React*, Splash*) need RGB_MATRIX_KEYPRESSES, set in
 * config.h, plus the per-LED positions in keyboard.json's rgb_matrix.layout.
 */
