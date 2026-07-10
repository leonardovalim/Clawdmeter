#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "hal/sound_hal.h"
#include "ble.h"
#include "version.h"
#include "ble.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Pace screen
    int16_t pace_title_y;     // Ritmo tem título subido: Tiempos 56 acaba em
                              // ~y=86 e o arco fica logo abaixo — colide se
                              // o título usar o title_y padrão (30).
    int16_t pace_arc_size;
    int16_t pace_arc_y;
    int16_t pace_status_y;
    int16_t pace_stat_lbl_y;
    int16_t pace_stat_val_y;
    int16_t pace_stat_dx;     // horizontal offset of each stat column from center
    int16_t pace_hm_y;
    int16_t pace_hm_h;
    int16_t pace_axis_y;
    const lv_font_t* pace_pct_font;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        // Ritmo: respiração + inversão value/label. Título sobe 10px, arco
        // volta a 200 e desce 16 pra abrir 12px acima dele (Tiempos 56 tem
        // descender longo — antes colava no topo do arco).
        L.pace_title_y    = 20;
        L.pace_arc_size   = 200;
        L.pace_arc_y      = 88;
        L.pace_status_y   = 308;
        L.pace_stat_val_y = 342;
        L.pace_stat_lbl_y = 370;
        L.pace_stat_dx    = 100;
        L.pace_hm_y       = 400;
        L.pace_hm_h       = 48;
        L.pace_axis_y     = 456;
        L.pace_pct_font   = &font_styrene_48;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        // Espelha a Proposta A em escala compacta (não é alvo de QA visual,
        // mas segue o mesmo esqueleto: value → label invertido, mais folga).
        L.pace_title_y    = 16;
        L.pace_arc_size   = 156;
        L.pace_arc_y      = 76;
        L.pace_status_y   = 252;
        L.pace_stat_val_y = 288;
        L.pace_stat_lbl_y = 314;
        L.pace_stat_dx    = 78;
        L.pace_hm_y       = 344;
        L.pace_hm_h       = 46;
        L.pace_axis_y     = 394;
        L.pace_pct_font   = &font_styrene_28;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- 24-hour activity heatmap (shared by SCREEN_STATS and SCREEN_PACE) ----
// One instance per screen: the bars are real LVGL objects parented to their own
// container, so the two screens can't share a single set.
struct Heatmap {
    lv_obj_t* fill[24];
    int       bar_max_h;
};
static Heatmap hm_stats = {};

// ---- Stats screen (SCREEN_STATS) ----
static lv_obj_t* stats_container    = nullptr;
static lv_obj_t* arc_cache          = nullptr;
static lv_obj_t* lbl_cache_pct      = nullptr;
static lv_obj_t* lbl_cache_sub      = nullptr;

// ---- Pace screen (SCREEN_PACE) ----
static lv_obj_t* pace_container     = nullptr;
static lv_obj_t* arc_pace           = nullptr;
static lv_obj_t* lbl_pace_pct       = nullptr;
static lv_obj_t* lbl_pace_sub       = nullptr;
static lv_obj_t* lbl_pace_status    = nullptr;
static lv_obj_t* lbl_pace_now_val   = nullptr;
static lv_obj_t* lbl_pace_peak_val  = nullptr;
static Heatmap   hm_pace = {};
// %/hour from usage_rate_per_hour(), pushed by main.cpp. -1 = still warming up.
static float     pace_rate = -1.0f;

// ---- Media screen (SCREEN_MEDIA) ----
static lv_obj_t* media_container    = nullptr;
static lv_obj_t* lbl_media_nothing  = nullptr;
static lv_obj_t* lbl_media_artist   = nullptr;
static lv_obj_t* lbl_media_title    = nullptr;
static lv_obj_t* lbl_media_status   = nullptr;
static lv_obj_t* bar_media          = nullptr;  // track progress bar
static lv_obj_t* lbl_media_time     = nullptr;  // "1:23 / 3:16" under the bar
static lv_obj_t* img_media_art      = nullptr;  // album cover (RGB565 from BLE)
static lv_obj_t* lbl_zone_prev      = nullptr;  // dim hint marking the tap zones:
static lv_obj_t* lbl_zone_play      = nullptr;  //  left third = prev, center =
static lv_obj_t* lbl_zone_next      = nullptr;  //  play/pause, right third = next
static lv_image_dsc_t media_art_dsc;
// Track progress interpolation: the daemon refreshes position every ~5s; between
// updates we advance it locally while playing (same base+tick pattern as the clock).
static int      media_base_pos  = -1;  // seconds at last payload (-1 = no timeline)
static int      media_dur_s     = 0;
static bool     media_playing_s = false;
static uint32_t media_base_ms   = 0;   // lv_tick when media_base_pos landed
static int      media_last_shown = -1; // last rendered second; avoids redraw spam

// ---- Burndown screen (SCREEN_BURNDOWN) ----
static lv_obj_t* burndown_container = nullptr;
static lv_obj_t* lbl_bd_todo        = nullptr;
static lv_obj_t* lbl_bd_doing       = nullptr;
static lv_obj_t* lbl_bd_done        = nullptr;
static lv_obj_t* lbl_bd_nothing     = nullptr;
static lv_obj_t* lbl_bd_sprint      = nullptr;  // "Sprint 32" subtitle
static lv_obj_t* chart_bd           = nullptr;  // burndown line chart
static lv_chart_series_t* ser_bd_ideal  = nullptr;
static lv_chart_series_t* ser_bd_actual = nullptr;
static lv_obj_t* lbl_bd_legend      = nullptr;

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_obj_t* lbl_version;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void media_tap_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, &font_styrene_16, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);
    // lbl_anim is created at the screen level in ui_init() so it floats
    // above all content screens (except splash).
}

// ======== 24-hour heatmap (shared) ========

// Build the panel + 24 stacked bars into `parent` at (L.margin, y) sized
// (L.content_w, h). Geometry is derived from h so both screens can place it
// wherever their layout wants.
static void heatmap_build(Heatmap* hm, lv_obj_t* parent, int y, int h) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, h);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    const int bar_w = (L.content_w - 16) / 24;
    hm->bar_max_h = h - 16;

    for (int i = 0; i < 24; i++) {
        lv_obj_t* bg = lv_obj_create(panel);
        lv_obj_set_size(bg, bar_w - 2, hm->bar_max_h);
        lv_obj_set_pos(bg, i * bar_w, 0);
        lv_obj_set_style_bg_color(bg, COL_BAR_BG, 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_radius(bg, 2, 0);
        lv_obj_set_style_pad_all(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

        hm->fill[i] = lv_obj_create(bg);
        lv_obj_set_size(hm->fill[i], bar_w - 2, 0);
        lv_obj_set_pos(hm->fill[i], 0, hm->bar_max_h);
        lv_obj_set_style_bg_color(hm->fill[i], COL_DIM, 0);
        lv_obj_set_style_bg_opa(hm->fill[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hm->fill[i], 0, 0);
        lv_obj_set_style_radius(hm->fill[i], 2, 0);
        lv_obj_clear_flag(hm->fill[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

// 0 → track color; 1-49 green; 50-79 amber; >= 80 red.
static void heatmap_update(Heatmap* hm, const UsageData* data) {
    if (!hm->fill[0] || hm->bar_max_h == 0) return;
    for (int i = 0; i < 24; i++) {
        int val      = data->hourly[i];
        int fill_h   = (hm->bar_max_h * val) / 100;
        lv_obj_set_height(hm->fill[i], fill_h);
        lv_obj_set_y(hm->fill[i], hm->bar_max_h - fill_h);
        lv_color_t col = (val == 0) ? COL_BAR_BG :
                         (val >= 80) ? COL_RED    :
                         (val >= 50) ? COL_AMBER  : COL_GREEN;
        lv_obj_set_style_bg_color(hm->fill[i], col, 0);
    }
}

// ======== Stats screen ========

static void init_stats_screen(lv_obj_t* scr) {
    stats_container = lv_obj_create(scr);
    lv_obj_set_size(stats_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(stats_container, 0, 0);
    lv_obj_set_style_bg_color(stats_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(stats_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stats_container, 0, 0);
    lv_obj_set_style_pad_all(stats_container, 0, 0);
    lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(stats_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(stats_container);
    lv_label_set_text(title, "Stats");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    // Cache hit ring — ¾ circle, fills clockwise from bottom-left
    arc_cache = lv_arc_create(stats_container);
    lv_obj_set_size(arc_cache, 180, 180);
    lv_obj_align(arc_cache, LV_ALIGN_TOP_MID, 0, 90);
    lv_arc_set_rotation(arc_cache, 135);
    lv_arc_set_bg_angles(arc_cache, 0, 270);
    lv_arc_set_range(arc_cache, 0, 100);
    lv_arc_set_value(arc_cache, 0);
    lv_obj_set_style_arc_color(arc_cache, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_cache, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_cache, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_cache, 16, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc_cache, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc_cache, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc_cache, LV_OBJ_FLAG_CLICKABLE);

    lbl_cache_pct = lv_label_create(stats_container);
    lv_label_set_text(lbl_cache_pct, "--");
    lv_obj_set_style_text_font(lbl_cache_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_cache_pct, COL_TEXT, 0);
    lv_obj_align_to(lbl_cache_pct, arc_cache, LV_ALIGN_CENTER, 0, -8);

    lbl_cache_sub = lv_label_create(stats_container);
    lv_label_set_text(lbl_cache_sub, "cache hits");
    lv_obj_set_style_text_font(lbl_cache_sub, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_cache_sub, COL_DIM, 0);
    lv_obj_align_to(lbl_cache_sub, arc_cache, LV_ALIGN_CENTER, 0, 28);

    // 24-hour activity heatmap
    heatmap_build(&hm_stats, stats_container, 295, 110);
}

// ======== Pace screen ========

void ui_set_usage_rate(float pct_per_hour) { pace_rate = pct_per_hour; }

// Absolute local wall-clock as "14:32" or "2:32 PM", per the daemon's clock_fmt.
static void fmt_clock(long epoch, char* buf, size_t len) {
    time_t t = (time_t)epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);   // epoch is already local wall-clock → gmtime keeps it
    if (clock_fmt == 12) {
        int h12 = tmv.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, len, "%d:%02d %s", h12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, len, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
}

// "3h42" / "42min" — used when the daemon supplies no wall clock (epoch == 0).
static void fmt_rel(int mins, char* buf, size_t len) {
    if (mins < 60) snprintf(buf, len, "%dmin", mins);
    else           snprintf(buf, len, "%dh%02d", mins / 60, mins % 60);
}

static void init_pace_screen(lv_obj_t* scr) {
    pace_container = lv_obj_create(scr);
    lv_obj_set_size(pace_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(pace_container, 0, 0);
    lv_obj_set_style_bg_color(pace_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(pace_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pace_container, 0, 0);
    lv_obj_set_style_pad_all(pace_container, 0, 0);
    lv_obj_clear_flag(pace_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pace_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(pace_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(pace_container);
    lv_label_set_text(title, "Ritmo");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.pace_title_y);

    // Projection gauge — same ¾-circle geometry as the Stats cache ring.
    arc_pace = lv_arc_create(pace_container);
    lv_obj_set_size(arc_pace, L.pace_arc_size, L.pace_arc_size);
    lv_obj_align(arc_pace, LV_ALIGN_TOP_MID, 0, L.pace_arc_y);
    lv_arc_set_rotation(arc_pace, 135);
    lv_arc_set_bg_angles(arc_pace, 0, 270);
    lv_arc_set_range(arc_pace, 0, 100);
    lv_arc_set_value(arc_pace, 0);
    lv_obj_set_style_arc_color(arc_pace, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_pace, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_pace, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_pace, 16, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc_pace, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc_pace, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc_pace, LV_OBJ_FLAG_CLICKABLE);

    lbl_pace_pct = lv_label_create(pace_container);
    lv_label_set_text(lbl_pace_pct, "--");
    lv_obj_set_style_text_font(lbl_pace_pct, L.pace_pct_font, 0);
    lv_obj_set_style_text_color(lbl_pace_pct, COL_TEXT, 0);
    lv_obj_align_to(lbl_pace_pct, arc_pace, LV_ALIGN_CENTER, 0, -8);

    lbl_pace_sub = lv_label_create(pace_container);
    lv_label_set_text(lbl_pace_sub, "até o reset");
    lv_obj_set_style_text_font(lbl_pace_sub, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_pace_sub, COL_DIM, 0);
    lv_obj_align_to(lbl_pace_sub, arc_pace, LV_ALIGN_CENTER, 0, 28);

    lbl_pace_status = lv_label_create(pace_container);
    lv_label_set_text(lbl_pace_status, "Sem dados");
    lv_obj_set_style_text_font(lbl_pace_status, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_pace_status, COL_DIM, 0);
    lv_obj_align(lbl_pace_status, LV_ALIGN_TOP_MID, 0, L.pace_status_y);

    // Two stat columns: "agora" (current rate) and "pico hoje" (busiest hour).
    // Valor acima (styrene_24 TEXT, primeira coisa que o olho pega), rótulo
    // abaixo (styrene_14 DIM, contexto). Inverte a ordem original pra ficar
    // mais próximo do padrão dashboard/relógio.
    lv_obj_t* now_lbl = lv_label_create(pace_container);
    lv_label_set_text(now_lbl, "agora");
    lv_obj_set_style_text_font(now_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(now_lbl, COL_DIM, 0);
    lv_obj_align(now_lbl, LV_ALIGN_TOP_MID, -L.pace_stat_dx, L.pace_stat_lbl_y);

    lbl_pace_now_val = lv_label_create(pace_container);
    lv_label_set_text(lbl_pace_now_val, "--");
    lv_obj_set_style_text_font(lbl_pace_now_val, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_pace_now_val, COL_TEXT, 0);
    lv_obj_align(lbl_pace_now_val, LV_ALIGN_TOP_MID, -L.pace_stat_dx, L.pace_stat_val_y);

    lv_obj_t* peak_lbl = lv_label_create(pace_container);
    lv_label_set_text(peak_lbl, "pico hoje");
    lv_obj_set_style_text_font(peak_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(peak_lbl, COL_DIM, 0);
    lv_obj_align(peak_lbl, LV_ALIGN_TOP_MID, L.pace_stat_dx, L.pace_stat_lbl_y);

    lbl_pace_peak_val = lv_label_create(pace_container);
    lv_label_set_text(lbl_pace_peak_val, "--");
    lv_obj_set_style_text_font(lbl_pace_peak_val, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_pace_peak_val, COL_TEXT, 0);
    lv_obj_align(lbl_pace_peak_val, LV_ALIGN_TOP_MID, L.pace_stat_dx, L.pace_stat_val_y);

    heatmap_build(&hm_pace, pace_container, L.pace_hm_y, L.pace_hm_h);

    struct { const char* txt; lv_align_t al; int dx; } axis[3] = {
        { "0h",  LV_ALIGN_TOP_LEFT,  L.margin },
        { "12h", LV_ALIGN_TOP_MID,   0        },
        { "23h", LV_ALIGN_TOP_RIGHT, -L.margin },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* t = lv_label_create(pace_container);
        lv_label_set_text(t, axis[i].txt);
        lv_obj_set_style_text_font(t, &font_styrene_12, 0);
        lv_obj_set_style_text_color(t, COL_DIM, 0);
        lv_obj_align(t, axis[i].al, axis[i].dx, L.pace_axis_y);
    }
}

static void update_pace_screen(const UsageData* data) {
    if (!pace_container) return;

    // Busiest hour today; strict > keeps the earliest on a tie. All-zero (the
    // bash daemon, which sends no `hm`) reads as "no data".
    int peak_h = -1, peak_v = 0;
    for (int i = 0; i < 24; i++) {
        if (data->hourly[i] > peak_v) { peak_v = data->hourly[i]; peak_h = i; }
    }
    if (peak_h < 0) lv_label_set_text(lbl_pace_peak_val, "--");
    else            lv_label_set_text_fmt(lbl_pace_peak_val, "%dh", peak_h);

    const bool warming = (pace_rate < 0.0f);
    if (warming) lv_label_set_text(lbl_pace_now_val, "--");
    else         lv_label_set_text_fmt(lbl_pace_now_val, "%d%%/h", (int)(pace_rate + 0.5f));

    // "reset 16:10" (wall clock) or "reset em 3h42" (daemon without a clock).
    // Empty when there's no reset value at all (session_reset_mins == -1).
    char reset[24];
    const int mins = data->session_reset_mins;
    if (mins < 0)                    reset[0] = '\0';
    else if (data->clock_epoch > 0) { char t[12]; fmt_clock(data->clock_epoch + (long)mins * 60, t, sizeof(t));
                                      snprintf(reset, sizeof(reset), " · reset %s", t); }
    else                            { char r[12]; fmt_rel(mins, r, sizeof(r));
                                      snprintf(reset, sizeof(reset), " · reset em %s", r); }

    char status[56];
    lv_color_t col = COL_ACCENT;
    int gauge;

    if (data->enterprise) {
        // Monthly spending, not a 5h window — the projection model doesn't apply.
        gauge = (int)(data->session_pct + 0.5f);
        snprintf(status, sizeof(status), "Reset %s", data->reset_date);
        col = COL_DIM;
    } else if (warming || mins < 0) {
        gauge = (int)(data->session_pct + 0.5f);
        snprintf(status, sizeof(status), "%s%s",
                 warming ? "Medindo ritmo…" : "Sem projeção", reset);
        col = COL_DIM;
    } else {
        float proj = data->session_pct + pace_rate * (float)mins / 60.0f;
        if (proj < 0.0f)   proj = 0.0f;
        if (proj > 100.0f) proj = 100.0f;
        gauge = (int)(proj + 0.5f);

        if (gauge >= 100 && pace_rate > 0.0f) {
            col = COL_RED;
            // Hits 100% this many hours from now, at the current rate.
            float hrs = (100.0f - data->session_pct) / pace_rate;
            if (hrs < 0.0f) hrs = 0.0f;   // already at/over 100
            int   out_mins = (int)(hrs * 60.0f + 0.5f);
            if (data->clock_epoch > 0) {
                char t[12];
                fmt_clock(data->clock_epoch + (long)out_mins * 60, t, sizeof(t));
                snprintf(status, sizeof(status), "Esgota ~%s%s", t, reset);
            } else {
                char r[12];
                fmt_rel(out_mins, r, sizeof(r));
                snprintf(status, sizeof(status), "Esgota em ~%s%s", r, reset);
            }
        } else if (gauge >= 100) {
            // At/over 100 with a non-positive rate (session_pct itself already
            // maxed): red, but no ETA to compute.
            col = COL_RED;
            snprintf(status, sizeof(status), "No limite%s", reset);
        } else if (gauge >= 85) {
            col = COL_AMBER;
            snprintf(status, sizeof(status), "No limite%s", reset);
        } else {
            col = COL_GREEN;
            snprintf(status, sizeof(status), "No ritmo%s", reset);
        }
    }

    lv_arc_set_value(arc_pace, gauge);
    lv_obj_set_style_arc_color(arc_pace, col, LV_PART_INDICATOR);
    lv_label_set_text_fmt(lbl_pace_pct, "%d%%", gauge);
    lv_label_set_text(lbl_pace_status, status);
    lv_obj_set_style_text_color(lbl_pace_status, col, 0);
    lv_obj_align(lbl_pace_status, LV_ALIGN_TOP_MID, 0, L.pace_status_y);

    heatmap_update(&hm_pace, data);
}

// ======== Media screen ========

static void init_media_screen(lv_obj_t* scr) {
    media_container = lv_obj_create(scr);
    lv_obj_set_size(media_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(media_container, 0, 0);
    lv_obj_set_style_bg_color(media_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(media_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(media_container, 0, 0);
    lv_obj_set_style_pad_all(media_container, 0, 0);
    lv_obj_clear_flag(media_container, LV_OBJ_FLAG_SCROLLABLE);
    // Deliberately NO global_click_cb here: on this screen a tap means play/pause
    // (wired below), not navigate-to-splash. Splash stays reachable from the
    // other screens and from the PWR button.
    lv_obj_add_flag(media_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(media_container);
    lv_label_set_text(title, "Now Playing");
    lv_obj_set_style_text_font(title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    lbl_media_nothing = lv_label_create(media_container);
    lv_label_set_text(lbl_media_nothing, "Nothing playing");
    lv_obj_set_style_text_font(lbl_media_nothing, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_media_nothing, COL_DIM, 0);
    lv_obj_align(lbl_media_nothing, LV_ALIGN_CENTER, 0, 0);

    lbl_media_artist = lv_label_create(media_container);
    lv_label_set_text(lbl_media_artist, "");
    lv_obj_set_style_text_font(lbl_media_artist, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_media_artist, COL_TEXT, 0);
    lv_label_set_long_mode(lbl_media_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_media_artist, L.content_w);
    lv_obj_align(lbl_media_artist, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_add_flag(lbl_media_artist, LV_OBJ_FLAG_HIDDEN);

    lbl_media_title = lv_label_create(media_container);
    lv_label_set_text(lbl_media_title, "");
    lv_obj_set_style_text_font(lbl_media_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_media_title, COL_DIM, 0);
    lv_label_set_long_mode(lbl_media_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_media_title, L.content_w);
    lv_obj_align(lbl_media_title, LV_ALIGN_TOP_MID, 0, 235);
    lv_obj_add_flag(lbl_media_title, LV_OBJ_FLAG_HIDDEN);

    lbl_media_status = lv_label_create(media_container);
    lv_label_set_text(lbl_media_status, "");
    lv_obj_set_style_text_font(lbl_media_status, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_media_status, COL_ACCENT, 0);
    lv_obj_align(lbl_media_status, LV_ALIGN_TOP_MID, 0, 305);
    lv_obj_add_flag(lbl_media_status, LV_OBJ_FLAG_HIDDEN);

    bar_media = make_bar(media_container, (L.scr_w - L.content_w) / 2, 370,
                         L.content_w, 8);
    lv_bar_set_range(bar_media, 0, 1000);   // per-mille — smooth on long tracks
    lv_obj_set_style_bg_color(bar_media, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(bar_media, LV_OBJ_FLAG_HIDDEN);

    lbl_media_time = lv_label_create(media_container);
    lv_label_set_text(lbl_media_time, "");
    lv_obj_set_style_text_font(lbl_media_time, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_media_time, COL_DIM, 0);
    lv_obj_align(lbl_media_time, LV_ALIGN_TOP_MID, 0, 392);
    lv_obj_add_flag(lbl_media_time, LV_OBJ_FLAG_HIDDEN);

    // Dim hints marking the three tap zones (left=prev, center=play/pause,
    // right=next). Purely visual — the hit area is the full third of the screen,
    // so an imprecise tap still lands, unlike the old small buttons.
    lbl_zone_prev = lv_label_create(media_container);
    lv_label_set_text(lbl_zone_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(lbl_zone_prev, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_zone_prev, COL_DIM, 0);
    lv_obj_align(lbl_zone_prev, LV_ALIGN_TOP_MID, -(L.scr_w / 3), 300);
    lv_obj_add_flag(lbl_zone_prev, LV_OBJ_FLAG_HIDDEN);

    lbl_zone_play = lv_label_create(media_container);
    lv_label_set_text(lbl_zone_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lbl_zone_play, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_zone_play, COL_DIM, 0);
    lv_obj_align(lbl_zone_play, LV_ALIGN_TOP_MID, 0, 300);
    lv_obj_add_flag(lbl_zone_play, LV_OBJ_FLAG_HIDDEN);

    lbl_zone_next = lv_label_create(media_container);
    lv_label_set_text(lbl_zone_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(lbl_zone_next, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_zone_next, COL_DIM, 0);
    lv_obj_align(lbl_zone_next, LV_ALIGN_TOP_MID, L.scr_w / 3, 300);
    lv_obj_add_flag(lbl_zone_next, LV_OBJ_FLAG_HIDDEN);

    // Touch zones: the whole screen is the target (see media_tap_cb). Leaving the
    // media screen is done by tilting back to portrait / the PWR button.
    lv_obj_add_flag(media_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(media_container, media_tap_cb, LV_EVENT_CLICKED, NULL);

    // Album cover, streamed by the daemon over BLE (see ble_take_album_art).
    // Takes the top-LEFT slot where the Claude logo normally sits (the logo is
    // hidden on this screen — see ui_show_screen). Source is 96x96, scaled
    // ~0.7x with the pivot at the top-left corner so the scaled top-left lands
    // exactly on the alignment position (matching the logo's old spot).
    img_media_art = lv_image_create(media_container);
    lv_image_set_pivot(img_media_art, 0, 0);
    lv_image_set_scale(img_media_art, 180);   // 96 * 180/256 ≈ 67px
    lv_obj_align(img_media_art, LV_ALIGN_TOP_LEFT, L.margin, L.title_y - 10);
    lv_obj_set_style_radius(img_media_art, 10, 0);
    lv_obj_set_style_clip_corner(img_media_art, true, 0);
    lv_obj_add_flag(img_media_art, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_album_art(const uint8_t* rgb565, int w, int h) {
    if (!img_media_art) return;
    if (!rgb565 || w <= 0 || h <= 0) {
        lv_obj_add_flag(img_media_art, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    memset(&media_art_dsc, 0, sizeof(media_art_dsc));
    media_art_dsc.header.w = w;
    media_art_dsc.header.h = h;
    media_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    media_art_dsc.header.stride = w * 2;
    media_art_dsc.data = rgb565;
    media_art_dsc.data_size = (uint32_t)w * h * 2;
    // The BLE layer reuses one buffer for every cover; image caching is off
    // (LV_CACHE_DEF_SIZE 0), so re-setting the src + invalidating repaints
    // straight from the updated pixels.
    lv_image_set_src(img_media_art, &media_art_dsc);
    lv_obj_clear_flag(img_media_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(img_media_art);
}

// Render the media progress bar + time label for a given position (seconds).
static void render_media_progress(int pos) {
    if (media_dur_s <= 0) return;
    if (pos < 0) pos = 0;
    if (pos > media_dur_s) pos = media_dur_s;
    if (pos == media_last_shown) return;
    media_last_shown = pos;
    lv_bar_set_value(bar_media, (int)((int64_t)pos * 1000 / media_dur_s), LV_ANIM_OFF);
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%d:%02d / %d:%02d",
             pos / 60, pos % 60, media_dur_s / 60, media_dur_s % 60);
    lv_label_set_text(lbl_media_time, tbuf);
}

// ======== Burndown screen ========

static void init_burndown_screen(lv_obj_t* scr) {
    burndown_container = lv_obj_create(scr);
    lv_obj_set_size(burndown_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(burndown_container, 0, 0);
    lv_obj_set_style_bg_color(burndown_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(burndown_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(burndown_container, 0, 0);
    lv_obj_set_style_pad_all(burndown_container, 0, 0);
    lv_obj_clear_flag(burndown_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(burndown_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(burndown_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(burndown_container);
    lv_label_set_text(title, "Sprint");
    lv_obj_set_style_text_font(title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.title_y);

    lbl_bd_sprint = lv_label_create(burndown_container);
    lv_label_set_text(lbl_bd_sprint, "");
    lv_obj_set_style_text_font(lbl_bd_sprint, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_bd_sprint, COL_DIM, 0);
    lv_obj_align(lbl_bd_sprint, LV_ALIGN_TOP_MID, 0, L.title_y + 42);

    lbl_bd_nothing = lv_label_create(burndown_container);
    lv_label_set_text(lbl_bd_nothing, "No sprint data");
    lv_obj_set_style_text_font(lbl_bd_nothing, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_bd_nothing, COL_DIM, 0);
    lv_obj_align(lbl_bd_nothing, LV_ALIGN_CENTER, 0, 0);

    // Burndown line chart: ideal (dim) vs actual (accent).
    chart_bd = lv_chart_create(burndown_container);
    lv_obj_set_size(chart_bd, L.content_w, 210);
    lv_obj_align(chart_bd, LV_ALIGN_TOP_MID, 0, 120);
    lv_chart_set_type(chart_bd, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart_bd, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart_bd, 4, 0);
    lv_obj_set_style_bg_color(chart_bd, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(chart_bd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart_bd, 0, 0);
    lv_obj_set_style_radius(chart_bd, 10, 0);
    lv_obj_set_style_pad_all(chart_bd, 8, 0);
    lv_obj_set_style_line_color(chart_bd, COL_DIM, LV_PART_MAIN);   // grid lines
    lv_obj_set_style_line_opa(chart_bd, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_size(chart_bd, 4, 4, LV_PART_INDICATOR);        // point radius
    ser_bd_ideal  = lv_chart_add_series(chart_bd, COL_DIM,    LV_CHART_AXIS_PRIMARY_Y);
    ser_bd_actual = lv_chart_add_series(chart_bd, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    // Compact To Do / Doing / Done row under the chart.
    const int col_w  = (L.content_w - 8) / 3;
    const int col_y  = 345;
    const int col_h  = 100;
    const char* labels[3] = {"To Do", "Doing", "Done"};
    lv_color_t  colors[3] = {COL_DIM, COL_AMBER, COL_GREEN};
    lv_obj_t**  ptrs[3]   = {&lbl_bd_todo, &lbl_bd_doing, &lbl_bd_done};

    for (int i = 0; i < 3; i++) {
        lv_obj_t* panel = make_panel(burndown_container,
                                     L.margin + i * (col_w + 4), col_y, col_w, col_h);

        lv_obj_t* count = lv_label_create(panel);
        lv_label_set_text(count, "--");
        lv_obj_set_style_text_font(count, &font_tiempos_34, 0);
        lv_obj_set_style_text_color(count, colors[i], 0);
        lv_obj_align(count, LV_ALIGN_TOP_MID, 0, 12);
        *ptrs[i] = count;

        lv_obj_t* lbl = lv_label_create(panel);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &font_styrene_16, 0);
        lv_obj_set_style_text_color(lbl, COL_DIM, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    lv_obj_add_flag(chart_bd,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_bd_sprint, LV_OBJ_FLAG_HIDDEN);
}

// ======== Confetti (sprint celebration) ========
//
// A burst of falling rectangles over the burndown screen, fired when tasks land
// in Done between two refreshes. The particles are allocated once and reused —
// each burst just re-randomizes and restarts their animations, so a celebration
// costs no allocation and leaves nothing to free.

#define CONFETTI_N 28

static lv_obj_t* confetti[CONFETTI_N];
static bool      confetti_built = false;

static void confetti_set_y(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }
static void confetti_set_x(void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); }

static void confetti_hide(lv_anim_t* a) {
    lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void confetti_build(void) {
    if (confetti_built || !burndown_container) return;
    for (int i = 0; i < CONFETTI_N; i++) {
        lv_obj_t* p = lv_obj_create(burndown_container);
        lv_obj_set_size(p, 8, 14);
        lv_obj_set_style_radius(p, 2, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        confetti[i] = p;
    }
    confetti_built = true;
}

static void confetti_burst(void) {
    confetti_build();
    if (!confetti_built) return;

    static const lv_color_t palette[] = {
        COL_ACCENT, COL_GREEN, COL_TEXT,
    };
    const int npal = sizeof(palette) / sizeof(palette[0]);

    for (int i = 0; i < CONFETTI_N; i++) {
        lv_obj_t* p = confetti[i];
        const int x0   = rand() % (L.scr_w - 8);
        const int drift = (rand() % 81) - 40;      // -40..+40 px sideways
        const int y0   = -20 - (rand() % 60);
        const int dur  = 1100 + (rand() % 900);
        const int wait = rand() % 400;

        lv_obj_set_style_bg_color(p, palette[rand() % npal], 0);
        lv_obj_set_pos(p, x0, y0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, p);
        lv_anim_set_exec_cb(&ay, confetti_set_y);
        lv_anim_set_values(&ay, y0, L.scr_h + 30);
        lv_anim_set_time(&ay, dur);
        lv_anim_set_delay(&ay, wait);
        lv_anim_set_ready_cb(&ay, confetti_hide);   // park it once it exits frame
        lv_anim_start(&ay);

        lv_anim_t ax;
        lv_anim_init(&ax);
        lv_anim_set_var(&ax, p);
        lv_anim_set_exec_cb(&ax, confetti_set_x);
        lv_anim_set_values(&ax, x0, x0 + drift);
        lv_anim_set_time(&ax, dur);
        lv_anim_set_delay(&ax, wait);
        lv_anim_start(&ax);
    }
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_stats_screen(scr);
    init_pace_screen(scr);
    init_media_screen(scr);
    init_burndown_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

    // Version label, splash-only — a discreet on-device confirmation that a
    // flash actually landed, with no serial monitor needed. Created last so
    // it renders on top of the splash animation; visibility toggled in
    // ui_show_screen() alongside logo_img/battery_img.
    lbl_version = lv_label_create(scr);
    lv_label_set_text(lbl_version, FIRMWARE_VERSION);
    lv_obj_set_style_text_font(lbl_version, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_version, COL_DIM, 0);
    // -L.margin, not a few px: the panel's corners are physically rounded
    // and hidden by the bezel, so anything hugging the literal corner never
    // reaches the eye even though it's in the framebuffer (see CLAUDE.md).
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_RIGHT, -L.margin, -L.margin);

    // Animated status line — lives at the screen level so it floats on top of
    // all content screens. Hidden on SCREEN_SPLASH, shown everywhere else.
    lbl_anim = lv_label_create(scr);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_48, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    // ---- Stats screen ----
    if (arc_cache) {
        if (data->cache_hit_pct >= 0) {
            lv_arc_set_value(arc_cache, data->cache_hit_pct);
            lv_color_t col = (data->cache_hit_pct >= 70) ? COL_GREEN :
                             (data->cache_hit_pct >= 40) ? COL_AMBER : COL_RED;
            lv_obj_set_style_arc_color(arc_cache, col, LV_PART_INDICATOR);
            snprintf(buf, sizeof(buf), "%d%%", data->cache_hit_pct);
            lv_label_set_text(lbl_cache_pct, buf);
        } else {
            lv_arc_set_value(arc_cache, 0);
            lv_label_set_text(lbl_cache_pct, "--");
        }
    }
    heatmap_update(&hm_stats, data);

    // ---- Pace screen ----
    update_pace_screen(data);

    // ---- Media screen ----
    if (media_container) {
        if (data->has_media) {
            lv_obj_add_flag(lbl_media_nothing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_media_artist, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_media_title,  LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_media_artist, data->media_artist);
            lv_label_set_text(lbl_media_title,  data->media_title);
            // Zone hints on: center glyph mirrors play state (▶ when paused).
            lv_obj_clear_flag(lbl_zone_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_zone_play, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_zone_next, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_zone_play,
                              data->media_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            media_base_pos  = data->media_pos;
            media_dur_s     = data->media_dur;
            media_playing_s = data->media_playing;
            media_base_ms   = last_data_ms;
            media_last_shown = -1;   // force a redraw with the fresh position
            if (media_dur_s > 0 && media_base_pos >= 0) {
                lv_obj_clear_flag(bar_media,       LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_media_time,  LV_OBJ_FLAG_HIDDEN);
                render_media_progress(media_base_pos);
            } else {
                lv_obj_add_flag(bar_media,       LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lbl_media_time,  LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_clear_flag(lbl_media_nothing, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_media_artist,    LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_media_title,     LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_media_status,    LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(bar_media,           LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_media_time,      LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(img_media_art,       LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_zone_prev,       LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_zone_play,       LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_zone_next,       LV_OBJ_FLAG_HIDDEN);
            media_base_pos = -1;
            media_dur_s    = 0;
        }
    }

    // ---- Burndown screen ----
    if (burndown_container) {
        if (data->has_burndown) {
            // Celebrate tasks that crossed into Done since the last payload.
            // -1 means "no baseline yet", so a fresh boot or a reconnect never
            // fires. A sprint rollover re-baselines instead of celebrating the
            // new sprint's inherited Done count.
            static int  last_bd_done = -1;
            static char last_bd_name[sizeof(data->bd_name)] = {0};
            const bool same_sprint = strcmp(last_bd_name, data->bd_name) == 0;
            if (same_sprint && last_bd_done >= 0 && data->bd_done > last_bd_done) {
                confetti_burst();
                sound_hal_play_celebrate();
            }
            last_bd_done = data->bd_done;
            snprintf(last_bd_name, sizeof(last_bd_name), "%s", data->bd_name);

            lv_obj_add_flag(lbl_bd_nothing, LV_OBJ_FLAG_HIDDEN);
            snprintf(buf, sizeof(buf), "%d", data->bd_todo);
            lv_label_set_text(lbl_bd_todo,  buf);
            snprintf(buf, sizeof(buf), "%d", data->bd_doing);
            lv_label_set_text(lbl_bd_doing, buf);
            snprintf(buf, sizeof(buf), "%d", data->bd_done);
            lv_label_set_text(lbl_bd_done,  buf);

            if (data->bd_name[0]) {
                lv_label_set_text(lbl_bd_sprint, data->bd_name);
                lv_obj_clear_flag(lbl_bd_sprint, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(lbl_bd_sprint, LV_OBJ_FLAG_HIDDEN);
            }

            // Burndown chart: plot ideal + actual; future actual points are gaps.
            if (data->bd_days > 0 && data->bd_max > 0 && chart_bd) {
                lv_chart_set_point_count(chart_bd, data->bd_days);
                lv_chart_set_range(chart_bd, LV_CHART_AXIS_PRIMARY_Y, 0, data->bd_max);
                for (int i = 0; i < data->bd_days; i++) {
                    lv_chart_set_value_by_id(chart_bd, ser_bd_ideal, i,
                                             data->bd_ideal[i]);
                    lv_chart_set_value_by_id(chart_bd, ser_bd_actual, i,
                        (data->bd_actual[i] < 0) ? LV_CHART_POINT_NONE
                                                 : (int32_t)data->bd_actual[i]);
                }
                lv_chart_refresh(chart_bd);
                lv_obj_clear_flag(chart_bd, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(chart_bd, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_clear_flag(lbl_bd_nothing, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_bd_todo,  "--");
            lv_label_set_text(lbl_bd_doing, "--");
            lv_label_set_text(lbl_bd_done,  "--");
            lv_obj_add_flag(chart_bd,      LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_bd_sprint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    // Usage-specific sub-screen management (pair hint / idle / live panels)
    if (current_screen == SCREEN_USAGE) {
        update_view_state();
        if (view_state == 1) splash_mini_tick();
    }
    // No animated status line on the splash screen.
    if (current_screen == SCREEN_SPLASH) return;

    uint32_t now = lv_tick_get();

    // Title clock only applies to the usage screen (has its own lbl_title).
    if (current_screen == SCREEN_USAGE && clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    // Track progress on the media screen: advance locally while playing so the
    // bar moves every second between the daemon's ~5s refreshes.
    if (current_screen == SCREEN_MEDIA && media_playing_s &&
        media_dur_s > 0 && media_base_pos >= 0) {
        render_media_progress(media_base_pos + (int)((now - media_base_ms) / 1000));
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";
    } else if (current_screen == SCREEN_USAGE && view_state == 1) {
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

// Tap zones: play/pause is the big, forgiving center (~56% of the width) so an
// imprecise tap never skips a track by accident; only a deliberate tap in the
// outer ~22% edge skips (left=prev, right=next). Uses the release point in LVGL
// space (touch is already unrotated in my_touch_cb, so X maps to what the user
// sees as left/right). The whole zone is the target — glyphs are only hints.
static void media_tap_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    lv_point_t p = { L.scr_w / 2, 0 };   // default to center if no indev
    if (indev) lv_indev_get_point(indev, &p);
    uint8_t cmd;
    if      (p.x < L.scr_w * 22 / 100)   cmd = MEDIA_CMD_PREV;
    else if (p.x > L.scr_w * 78 / 100)   cmd = MEDIA_CMD_NEXT;
    else                                 cmd = MEDIA_CMD_PLAYPAUSE;
    sound_hal_play_click();
    ble_send_media_cmd(cmd);
    // Optimistic feedback: flip the center glyph on this frame so play/pause
    // feels instant even though the daemon round-trip confirms a beat later.
    if (cmd == MEDIA_CMD_PLAYPAUSE && lbl_zone_play) {
        media_playing_s = !media_playing_s;
        lv_label_set_text(lbl_zone_play,
                          media_playing_s ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

void ui_show_screen(screen_t screen) {
    // Hide all content containers
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (stats_container)    lv_obj_add_flag(stats_container,    LV_OBJ_FLAG_HIDDEN);
    if (pace_container)     lv_obj_add_flag(pace_container,     LV_OBJ_FLAG_HIDDEN);
    if (media_container)    lv_obj_add_flag(media_container,    LV_OBJ_FLAG_HIDDEN);
    if (burndown_container) lv_obj_add_flag(burndown_container, LV_OBJ_FLAG_HIDDEN);
    if (lbl_anim)           lv_obj_add_flag(lbl_anim,           LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:
        splash_show();
        break;
    case SCREEN_USAGE:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_STATS:
        if (stats_container)    lv_obj_clear_flag(stats_container,    LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_PACE:
        if (pace_container)     lv_obj_clear_flag(pace_container,     LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_MEDIA:
        if (media_container)    lv_obj_clear_flag(media_container,    LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_BURNDOWN:
        if (burndown_container) lv_obj_clear_flag(burndown_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }

    // Whimsical status line lives on the usage screen only — the other screens
    // (media, stats, sprint) have their own content and shouldn't waste the
    // bottom band on it.
    if (screen == SCREEN_USAGE && lbl_anim)
        lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);

    if (logo_img) {
        // Media screen reuses the top-left slot for the album cover, so the
        // logo steps aside there.
        if (screen == SCREEN_SPLASH || screen == SCREEN_MEDIA)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (lbl_version) {
        if (screen == SCREEN_SPLASH) lv_obj_clear_flag(lbl_version, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_add_flag(lbl_version, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
