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

/* How far the hidden combos reach. The RGB layers are excluded: there the same
 * switches are colour selectors. */
#define NUM_FUNC_LAYERS 5

/* How far the mode picker walks: the five media modes plus the one RGB mode. */
#define NUM_CYCLE_LAYERS 6
#define LAYER_RGB 5
#define RGB_STEP 8

/* Taken from the driver's own ceiling (rgb_matrix.max_brightness, set in the
 * revision's keyboard.json) rather than restated, so the number the OLED
 * reports and the number the encoder clamps to cannot drift away from the one
 * actually in force. The ceiling exists because a full chain of SK6812s at
 * white pulls well over an amp. */
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

/* The mode picker previews; it does not switch.
 *
 * Clicking opens it on the current mode, turning walks the list and shows the
 * name, and nothing actually changes until a second click confirms. Let it
 * lapse and you are exactly where you started - which is what stops a stray
 * turn dropping you into the RGB mode and re-applying its effect behind your
 * back. */
#define PICKER_TIMEOUT 3000

static bool     picker_open   = false;
static uint8_t  picker_choice = 0;
static uint32_t picker_timer  = 0;

static bool picker_active(void) {
    return picker_open && timer_elapsed32(picker_timer) < PICKER_TIMEOUT;
}

/* Help browser.
 *
 * Play/pause + rewind together opens it, turning the encoder walks the list,
 * and clicking the encoder answers the question: everything goes dark except
 * the LED under the key that does the job. Any switch backs out.
 *
 * The list follows whichever layer you were on, because that is where the
 * question came up - the RGB layers are exactly where the keys stop being
 * self-explanatory. */
typedef enum { HELP_OFF, HELP_BROWSE, HELP_SHOWING } help_state_t;
static help_state_t help_state = HELP_OFF;
static uint8_t      help_index = 0;

/* Labels are capped at BIG_MAX_CHARS (ten) so they fit the double-size font.
 * `where` is the same answer in words, capped at one OLED row (21). It carries
 * the whole answer on its own, which is what makes this work for SW5 and SW8 -
 * they have no LED of their own and never will. */
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

/* The two LEDs flanking a key, left then right, or NO_LED where the board has
 * nothing on that side.
 *
 * The LED grid sits about half a key pitch right of the switch grid, so the LED
 * that "belongs" to a key is physically between that key and the next one along.
 * Lighting it read exactly that way on the board: at Red, both R and G looked
 * lit. No assignment fixes it - solving for the largest margin across all nine
 * keys still leaves a worst case of -4.3 mm, meaning some key would always be
 * pointed at by an LED nearer a different key. It is a property of the layout,
 * not of the choice.
 *
 * So the key is bracketed rather than pointed at: the LED either side comes up
 * and the key is the dark gap between them. Position stops being a point to
 * resolve and becomes a gap to look into. Brightness always read cleanly for
 * this same reason - D6 sits past SW6 with no key beyond it to be confused
 * with.
 *
 * Indices are into rgb_matrix.layout. Anything at or past RGB_MATRIX_LED_COUNT
 * simply does not light, which is how rev16 degrades: it has no D17-D20, so the
 * encoder gets one side of its bracket instead of two. SW5 has no usable pair
 * at all - the nearest LED to its right is most of the board away - so it
 * relies on the words alone. */
static const uint8_t help_bracket[3][3][2] = {
    { { 9, 13},          {13, 14},          {14, 15} },
    { {10, 12},          {NO_LED, NO_LED},  {15,  5} },
    { {10, 11},          {11, 19},          {19, 17} },
};

/* Defined with the rest of the display and RGB code further down; the hidden
 * combos run before that point. */
static void oled_row(uint8_t row, const char *text);
static void oled_splash(const char *text);
void apply_rgb(void);

static bool is_held(uint8_t r, uint8_t c) { return held[r][c]; }

/* Positions, not keycodes - these are read straight off the matrix, so they
 * have to track wherever the key physically sits. Rewind moved to SW2 when it
 * and play/pause swapped places; the combos are still "rewind + fast-forward"
 * to the fingers, which is what the readme documents. */
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
    /* Nothing hidden fires while the help browser owns the keys. */
    if (help_state != HELP_OFF) {
        active_combo = COMBO_NONE;
        combo_fired  = false;
        return;
    }

    /* Only armed on the media layers. On the RGB layers these same physical
     * keys are the colour selectors, so holding R and B while mixing a colour
     * lands on rewind+fast-forward and would read as reboot-the-keyboard. */
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
            /* eeconfig_init() drops the RGB driver back to its stored defaults,
             * which leaves the LEDs and the OLED disagreeing about what is
             * running. Put the live scene back. */
            rgb_matrix_enable_noeeprom();
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

/* Title and artist share one 24-byte run in the packet, so a short artist buys
 * the title room. At the old fixed 12 bytes "Beds are Burning" arrived as
 * "Beds are Bur". */
#define NP_TEXT_BYTES 24

static char np_title[21]  = {0};
static char np_artist[13] = {0};
static uint16_t np_dur_sec = 0;
static bool    np_playing = false;
static bool    np_shuffle = false;
static uint8_t np_repeat  = 0;   /* 0 none, 1 track, 2 list */
static uint32_t np_last_update = 0;
static bool np_has_data = false;

/* Playback clock, run locally rather than echoed.
 *
 * Windows refreshes a session's timeline position lazily: several seconds pass
 * with the same figure and then it moves in one step, which is what made the
 * display sit still and jump. This ticks from the last sync instead, and only
 * takes the host's figure when ours has become untrustworthy - otherwise a late
 * but accurate packet would drag the clock backwards. */
/* How far behind our own clock the host's figure may fall before it is read as
 * a real seek rather than a refresh that simply arrived late. */
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

    /* A figure the host has not refreshed says nothing about where playback
     * actually is, so it must never pull the clock back - re-syncing on the
     * gap alone made the display count 0 1 2 and then drop to 0 again. Only a
     * figure that has changed since the last packet counts, and even then only
     * when it is ahead of us or far enough behind to be a real seek. */
    const bool     host_moved = reported != np_reported_prev;
    const bool     behind     = running > reported;
    const uint16_t gap        = behind ? running - reported : reported - running;

    np_dur_sec = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    /* The two lengths locate the split inside the shared run, so they are
     * clamped against the run first and only then against our own buffers -
     * the artist's offset depends on how much title was actually sent, not on
     * how much of it we are willing to keep. */
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

    /* Re-sync on anything that makes our own clock untrustworthy: the first
     * packet, a new track, a play/pause edge, or a host figure worth believing. */
    if (!np_has_data || track_changed || playing != np_playing ||
        (host_moved && (!behind || gap > NOWPLAYING_SEEK_TOLERANCE))) {
        np_base_pos  = reported;
        np_base_time = timer_read32();
    }
    np_reported_prev = reported;

    /* The session took the command but would not act on it - browsers expose
     * playback over SMTC without implementing shuffle or repeat at all. Saying
     * so beats a key that silently does nothing. */
    if (flags & 0x10) {
        system_msg = "not supported here";
        system_msg_timer = timer_read32();
    }

    np_playing  = playing;
    np_shuffle  = (flags & 0x02) != 0;
    np_repeat   = (uint8_t)((flags >> 2) & 0x03);
    np_has_data = true;
    np_last_update = timer_read32();

    /* OLED_TIMEOUT (60 s by default) is only reset by key and encoder events,
     * so a track played without touching the pad used to blank the panel a
     * minute in. Incoming metadata counts as activity while something is
     * actually playing; paused, the panel is still allowed to sleep. */
    if (np_playing) {
        oled_on();
    }
}

/* Commands back to the host. Shuffle and repeat then act on whatever Windows
 * says is playing, instead of on one application's keyboard shortcuts. */
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

/* `tinted` says whether an effect follows the mixed colour. The cycling ones
 * generate their own hue and ignore it; the rest come out in whatever R/G/B is
 * dialled in. Carrying it per entry is what lets scenes and custom colours live
 * in one list - the controls do not have to know which half they are in. */
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

void apply_rgb(void) {
    const rgb_preset_t *fx = &rgb_effects[effect_index];
    rgb_matrix_mode_noeeprom(fx->mode);
    if (fx->tinted) {
        uint8_t h, s;
        rgb_to_hs(rgb_r, rgb_g, rgb_b, &h, &s);
        rgb_matrix_sethsv_noeeprom(h, s, rgb_val);
    } else {
        /* Generates its own hue; brightness is the only part that is ours. */
        rgb_matrix_sethsv_noeeprom(0, 255, rgb_val);
    }
}

void keyboard_post_init_user(void) {
    rgb_matrix_enable_noeeprom();
    apply_rgb();
}

/* Ctrl is held across the whole zoom gesture rather than wrapped around each
 * detent. Registering it and letting go again on every step raced the wheel
 * report - the host frequently saw the wheel with Ctrl already released, which
 * is just a scroll, so the zoom only caught occasionally. It is dropped once
 * the turning stops. */
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
}

/* Layers 0-4 are the media modes. The keys are the same on all of them - only
 * what the encoder does changes - so 1-4 are transparent and fall through to
 * layer 0 rather than restating it five times.
 *
 * Layers 5 and 6 are the exception: there the keys really do become something
 * else, and nothing falls through to the media set. Nothing outside those two
 * layers touches the LEDs, and nothing inside them touches playback. */
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
    /* One RGB mode covering both the animations and the mixed colours. The keys
     * only aim the dial - stepping the effect list is the dial's job too, so
     * there is one way to change a thing rather than two. The unused keys are
     * dead rather than transparent: left transparent they would fall through to
     * media keys, and this mode is meant to be free of those. */
    [5] = LAYOUT(
        RGB_SEL_R,            RGB_SEL_G,            RGB_SEL_B,
        RGB_SEL_FX,           KC_NO,                RGB_SEL_VAL,
        KC_NO,                KC_NO,                CYCLE_LAYER
    )
};

/* Everything that has to happen when a mode becomes current. The RGB mode needs
 * the dial pointed at something and its effect re-applied, or arriving there
 * leaves the knob doing nothing at all. */
static void enter_layer(uint8_t layer) {
    /* Never carry a held Ctrl out of the zoom mode. */
    zoom_ctrl_release();
    layer_move(layer);

    if (layer == LAYER_RGB) {
        /* Point the dial at something straight away so it is never dead on
         * arrival. The effect list is the most useful default now that scenes
         * and colours are one list; R/G/B and brightness repoint it in a press. */
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

    /* Play/pause + rewind together opens the help browser. Caught on the second
     * of the two presses and swallowed, so only the first key's media action
     * ever reaches the host. */
    if (record->event.pressed && help_state == HELP_OFF &&
        ((r == 0 && c == 0 && held[0][1]) || (r == 0 && c == 1 && held[0][0]))) {
        help_state = HELP_BROWSE;
        help_index = 0;
        /* Drop any half-open mode picker so backing out of help returns to the
         * screen you were on. Nothing was switched, so there is nothing to undo. */
        picker_open = false;
        return false;
    }

    /* While help is up it owns every switch: the encoder answers the current
     * question, anything else backs out, and nothing reaches the host. Releases
     * fall through to the return below so letting go of the opening pair does
     * not immediately close it again. */
    if (help_state != HELP_OFF) {
        if (record->event.pressed) {
            const bool encoder = (r == 2 && c == 2);
            help_state = (help_state == HELP_BROWSE && encoder) ? HELP_SHOWING : HELP_OFF;
        }
        return false;
    }

    switch (keycode) {
        /* With the companion live these go through Windows and work against
         * whatever is actually playing. Without it they fall back to Spotify's
         * own shortcuts, which is all they ever were. */
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
                    /* The confirmation. Only now does the mode actually change. */
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


/* The panel is mounted with its flex tail pointing at the top of the case, so
 * the controller's origin lands in the corner furthest from the reader and
 * everything comes out inverted without this. */
oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_180;
}

/* 128 / 6 px per glyph = 21 columns. This sizes the line buffers; the live
 * counts come from oled_max_chars()/oled_max_lines(), which stay right even if
 * the rotation above ever becomes a quarter turn. */
#define OLED_MAX_COLS (OLED_DISPLAY_WIDTH / OLED_FONT_WIDTH)

/* Writes exactly one row: positions the cursor itself, emits the text, then
 * pads the rest of the row with spaces. Every screen below writes every row,
 * so nothing from the previous frame survives and no clear is needed.
 *
 * Addressing rows outright is what makes that reliable. oled_write_ln()
 * advances by "whatever is left of the current row", which for a string that
 * exactly fills a row leaves the cursor already on the next one - so it blanks
 * that next row too and consumes two rows for one call. Anything counting rows
 * as it goes then drifts out of step with the panel and its padding lands in
 * the wrong place, which is what left the tail of a longer previous string on
 * screen.
 *
 * A per-frame oled_clear() would also hide that, but it marks the whole
 * framebuffer dirty, so QMK re-pushes all 512 bytes over I2C every render
 * rather than only the blocks that changed. Here the per-character dirty check
 * still applies, so a static screen costs nothing. */
/* Bounded at both ends so a row can be shared: cells outside [from, to) belong
 * to whatever drew them and are not padded over on the next frame. Without that
 * the clock and the corner symbols would overwrite each other every frame and
 * keep the block permanently dirty. */
static void oled_row_range(uint8_t row, uint8_t from, uint8_t to, const char *text) {
    if (row >= oled_max_lines()) return;

    const uint8_t cols = oled_max_chars();
    if (to > cols) to = cols;
    if (from >= to) return;

    uint8_t       i   = 0;
    const uint8_t col = from;
    oled_set_cursor(col, row);
    for (; col + i < to && text[i] != '\0'; i++) {
        /* Only printable ASCII goes through as itself. oled_write_char treats
         * \n and \r as cursor moves, which would break the one-row-per-call
         * guarantee, and the font maps bytes above 0x7F to unrelated symbols -
         * so an accented character in a track title would come out as garbage.
         * A space is the honest rendering of what cannot be shown. */
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

/* Cells left of `col` are left alone, which is what lets the status mark be
 * drawn into the first one by hand without this repainting over it - and
 * without the two of them fighting for the same bytes every frame. */
static void oled_row(uint8_t row, const char *text) {
    oled_row_from(row, 0, text);
}

/* As oled_row, but with `len` characters from `mark` in reverse video. The
 * highlight lands on the value itself rather than on a label pointing at it,
 * which is what makes the RGB panel readable at a glance: whatever is inverted
 * is what the dial is about to change. */
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

/* How long a corrupted panel is allowed to stay corrupted.
 *
 * QMK only re-sends framebuffer blocks whose *contents* changed. That is what
 * keeps the I2C traffic down, but it also means a single dropped or corrupted
 * transfer is permanent: the panel is left showing something the framebuffer
 * does not, the two never disagree again, and the stale pixels sit there until
 * something else happens to alter that block. Half-cleared text and speckle
 * both survive that way.
 *
 * oled_clear() marks every block dirty, and the screen builders below repaint
 * every row in the same pass, so the buffer is refilled before anything is
 * transmitted. The cost is one full screen of traffic every few seconds
 * instead of on every frame. */
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

/* Doubling a glyph. Each source byte is a column of 8 vertical pixels, and
 * each of its nibbles becomes a whole byte with every bit repeated - so 8 rows
 * become 16, spread across two pages. */
static const uint8_t nibble_doubled[16] = {
    0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
    0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF,
};

#define BIG_CHAR_WIDTH (OLED_FONT_WIDTH * 2)
#define BIG_MAX_CHARS  (OLED_DISPLAY_WIDTH / BIG_CHAR_WIDTH)

static char    big_text[BIG_MAX_CHARS + 1]                 = {0};
static uint8_t big_glyphs[BIG_MAX_CHARS * OLED_FONT_WIDTH] = {0};
static uint8_t big_len                                     = 0;

/* Pulls the font bitmap for a string out of the driver and caches it.
 *
 * QMK keeps its font table private, so the only way at a glyph is to have the
 * driver rasterise it into the framebuffer and read the result back out. That
 * is a side-effecting trick - it dirties the blocks under the top row - so it
 * has to happen once per string, never once per frame.
 *
 * Once per frame is what broke it: those blocks came back dirty on every pass,
 * and QMK sends only OLED_UPDATE_PROCESS_LIMIT blocks per render (one, by
 * default) always starting from the lowest dirty block. The renderer spent
 * every frame re-sending block 0 and never reached the pages the doubled text
 * lives in, so all the panel ever showed was the blanked scratch row - an
 * empty patch in the top left. */
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

/* Paints the cached string at double size, centred - 12x16 per character, so
 * ten of them across a 128x32.
 *
 * Every byte of the two text pages is written every frame, glyph or blank, and
 * every other row is blanked, so what ends up on screen depends only on the
 * cached string. Both oled_write_raw_byte and oled_write_char skip bytes that
 * already hold the right value, so repainting an unchanged screen marks
 * nothing dirty and leaves the render loop free to drain.
 *
 * The raw indices assume the framebuffer maps as page * width + column, which
 * holds for OLED_ROTATION_0 and _180 - both are done by the panel's own
 * segment and COM remap, leaving the buffer layout alone. A quarter turn would
 * need this reworked. */
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

    /* The doubled text takes two of the four rows, which leaves the top and
     * bottom for a small caption on either side of it. */
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

/* One-shot message for the combos that are about to take the board away.
 * oled_render() pushes at most OLED_UPDATE_PROCESS_LIMIT blocks per call - one,
 * by default - so the normal render path would get a sixteenth of the screen
 * out before the reset landed, and the message never appeared.
 * oled_render_dirty(true) flushes all of it. */
static void oled_splash(const char *text) {
    for (uint8_t r = 0; r < oled_max_lines(); r++) {
        oled_row(r, r == 1 ? text : "");
    }
    oled_render_dirty(true);
}

#define MARQUEE_STEP_MS 300

/* The run of blanks that separates the tail of the text from its head once it
 * wraps around. */
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
    /* Without this the first step of a new track is however much of the
     * 300 ms was left over from the previous one. */
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

/* The font has no pause glyph. 0x13 is a double exclamation mark, so its bars
 * stop short of the bottom and then carry a stray dot, and there is no play
 * triangle that lines up with it either. Both are drawn as raw columns instead,
 * one cell wide each so the clock beside them never shifts. Bit 0 is the top
 * pixel of a column. */
static const uint8_t mark_play[OLED_FONT_WIDTH]  = {0x7F, 0x7F, 0x3E, 0x3E, 0x1C, 0x08};
static const uint8_t mark_pause[OLED_FONT_WIDTH] = {0x7F, 0x7F, 0x00, 0x7F, 0x7F, 0x00};

/* Two cells wide, because one is not enough to draw either of these as anything
 * but a blob - at six pixels the shuffle came out an X and the repeat an O.
 *
 * Shuffle: two paths that cross and leave to the right under arrowheads.
 * Repeat:  a closed loop the full height of the row, with an arrowhead filling
 *          its top-right corner. The single-track 1 stands *beside* the loop
 *          rather than inside it - at seven pixels across there is no room for
 *          a digit in there that does not just fill the hole. */
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

/* Raw columns straight into the framebuffer, starting at a character cell.
 * NULL clears the space instead, which is how a mode that is off is drawn. */
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
    /* Stops short of the last four cells, which the two wide symbols own. */
    oled_row_range(row, 1, 17, line);

    oled_glyph_at(row, 0, np_playing ? mark_play : mark_pause, OLED_FONT_WIDTH);
    oled_glyph_at(row, 17, np_shuffle ? mark_shuffle : NULL, MARK_WIDE);
    oled_glyph_at(row, 19, np_repeat == 1 ? mark_repeat_one
                         : np_repeat == 2 ? mark_repeat_all
                                          : NULL, MARK_WIDE);
}

/* On the RGB layers the display becomes the control panel: which effect is
 * live, the mixed colour, and what the encoder is about to change. */
/* Four rows, each answering one question: where am I in the list, what is
 * running, what is it set to, and what will the knob change if I turn it now.
 * The old panel packed the same facts into initials and left you guessing. */
/* One row per thing the dial can be pointed at, and the one it is pointed at
 * right now is inverted. Reading the panel becomes "which row is highlighted"
 * rather than matching an abbreviation on the last line against three numbers
 * on the one above it. */
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

    /* Six of the sixteen effects run their own colours. Saying so beats leaving
     * the dial apparently doing nothing when it is turned. */
    oled_row(3, (on_colour && !fx->tinted) ? "effect ignores RGB" : "");
    oled_rows_blank_from(4);
}

/* Both halves are tracked, not just the title: two different tracks can share
 * a title, and the companion app can send the artist a packet later than the
 * title. Watching the title alone left the previous artist on screen. */
static char last_np_title[sizeof(np_title)]   = {0};
static char last_np_artist[sizeof(np_artist)] = {0};

bool oled_task_user(void) {
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
            /* The footer carries the answer in words. The LEDs bracket the key
             * at the same time where the board has them - rev16 has nothing
             * either side of the encoder, so that one says outright that the
             * dark board is the answer, not a fault. */
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
        /* The name shown is the one being previewed, not the one in force:
         * nothing has changed yet, and letting this lapse changes nothing. */
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
            /* Both buffers are the same size as their source and the sources
             * are always terminated, so this cannot truncate. */
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
        /* Nothing on the wire from the companion app. Saying so beats one word
         * on an otherwise empty panel, which reads as a half-dead display. */
        char line[OLED_MAX_COLS + 1];
        /* The LED count is the one visible difference between the revisions, so
         * the idle screen is where it belongs - out of the way, but there when
         * you need to know which build is on the board. */
        snprintf(line, sizeof(line), "mediapad   %u LED", (unsigned)RGB_MATRIX_LED_COUNT);
        oled_row(0, line);
        oled_row(1, "");
        oled_row(2, layer_name(layer));
        /* Two different silences, and they used to look identical. The
         * companion sends a perfectly good packet with an empty title when
         * Windows reports no media session, so "connected, nothing playing"
         * read exactly like "no companion at all" - which is the one thing
         * this line exists to tell you apart. */
        oled_row(3, nowplaying_is_stale() ? "waiting for app" : "nothing playing");
        oled_rows_blank_from(4);
    }

    return false;
}

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (index != 0) return false;

    /* Browsing the help list, and nothing else, while it is open. */
    if (help_state == HELP_BROWSE) {
        uint8_t count = 0;
        help_table(&count);
        help_index = clockwise ? (uint8_t)((help_index + 1) % count)
                               : (uint8_t)((help_index + count - 1) % count);
        return false;
    }
    if (help_state == HELP_SHOWING) return false;

    /* Picker is up: move the preview and keep it up. The layer itself is not
     * touched until a click confirms. */
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
            /* Consumer page, not the keyboard page. KC_KB_VOLUME_UP is HID
             * usage 0x80 on the keyboard page, which nothing in QMK turns into
             * a consumer report and which hosts ignore - it went out as a plain
             * keycode and did nothing. KC_AUDIO_VOL_UP is the one report.h
             * maps to AUDIO_VOL_UP, and the same page KC_AUDIO_MUTE was
             * already using two keys away. */
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
            /* Ctrl + wheel, not Ctrl + '=' / '-'. Those two keycodes are
             * *positions* on a US layout; on a Slovak one - or any other - they
             * land on different characters and the zoom never happens. The
             * wheel carries no layout at all, and more applications honour it.
             *
             * The waits are not padding: the modifier and the wheel go out on
             * different USB endpoints, so Ctrl needs to be settled before the
             * first wheel report, and the mouse report needs a moment between
             * press and release or the mousekey system coalesces it away. */
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
            break;
    }
    return false;
}

/* While an answer is on screen everything goes dark except the key that does
 * the job - one switch has to be unmistakable, which is the whole point of
 * answering with the LEDs rather than more words. */
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
