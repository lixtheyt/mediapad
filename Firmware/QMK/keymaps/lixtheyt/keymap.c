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
    RGB_SEL_VAL
};

#define NUM_FUNC_LAYERS 5
#define LAYER_RGB_CUSTOM 5
#define LAYER_RGB_SCENE 6
#define RGB_STEP 8

static uint8_t rgb_r = 255, rgb_g = 255, rgb_b = 255;
static uint8_t rgb_val = 255;

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

enum rgb_channel { CH_NONE, CH_RED, CH_GREEN, CH_BLUE, CH_BRIGHTNESS };
static enum rgb_channel active_channel = CH_NONE;

typedef struct { uint8_t r, g, b; } rgb_scene_t;
const rgb_scene_t rgb_scenes[] = {
    {255,   0,   0},  
    {  0, 255,   0},  
    {  0,   0, 255},  
    {255, 255, 255},  
    {255, 128,   0},  
    {128,   0, 255},  
    {  0, 255, 255},  
    {255,   0, 255},  
};
#define NUM_SCENES (sizeof(rgb_scenes) / sizeof(rgb_scenes[0]))
static uint8_t scene_index = 0;

static inline uint8_t clamp_add(uint8_t val, int16_t delta) {
    int16_t r = (int16_t)val + delta;
    if (r < 0) return 0;
    if (r > 255) return 255;
    return (uint8_t)r;
}

void apply_rgb_custom(void) {
    uint8_t r = (uint16_t)rgb_r * rgb_val / 255;
    uint8_t g = (uint16_t)rgb_g * rgb_val / 255;
    uint8_t b = (uint16_t)rgb_b * rgb_val / 255;
    rgblight_setrgb(r, g, b);
}

void apply_rgb_scene(void) {
    uint8_t r = (uint16_t)rgb_scenes[scene_index].r * rgb_val / 255;
    uint8_t g = (uint16_t)rgb_scenes[scene_index].g * rgb_val / 255;
    uint8_t b = (uint16_t)rgb_scenes[scene_index].b * rgb_val / 255;
    rgblight_setrgb(r, g, b);
}

void keyboard_post_init_user(void) {
    rgblight_enable();
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
        _______,           _______,             CYCLE_LAYER
    ),
    [6] = LAYOUT(
        _______,           _______,             _______,
        RGB_MODE_SCENE,    RGB_MODE_CUSTOM,     _______,
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
                active_channel = CH_NONE;
                apply_rgb_custom();
            }
            return false;

        case RGB_MODE_SCENE:
            if (record->event.pressed) {
                if (get_highest_layer(layer_state) == LAYER_RGB_SCENE) {
                    scene_index = (scene_index + 1) % NUM_SCENES;
                } else {
                    layer_move(LAYER_RGB_SCENE);
                }
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
                case CH_BRIGHTNESS: rgb_val = clamp_add(rgb_val, delta); break;
                case CH_NONE: break;
            }
            apply_rgb_custom();
            break;

        case LAYER_RGB_SCENE:  
            rgb_val = clamp_add(rgb_val, delta);
            apply_rgb_scene();
            break;
    }
    return false;
}