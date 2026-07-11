#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_MEDIA,       // landscape right  (quadrant 1): now playing
    SCREEN_STATS,       // cache ring + heatmap (compiled, no longer mapped)
    SCREEN_BURNDOWN,    // landscape left   (quadrant 3): Asana sprint board
    SCREEN_PACE,        // upside-down      (quadrant 2): projection + heatmap
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
// Consumption rate in %/hour for the Pace screen's projection; -1 while the
// rate window is still warming up. Fed from main.cpp, which owns the sampler.
void ui_set_usage_rate(float pct_per_hour);
// Show/hide the WiFi-setup overlay (QR) on the Sprint screen. Driven from the
// main loop off wifi_portal_active(). No-op on boards without WiFi.
void ui_set_wifi_setup(bool active);
// WiFi-reset countdown overlay (gesto dos 2 botões laterais). secs>=0 mostra o
// número; secs<0 esconde.
void ui_wifi_reset_countdown(int secs);
// Album cover for the media screen: w*h RGB565 (little-endian) pixels.
// nullptr clears/hides the cover. The buffer must stay valid until replaced.
void ui_set_album_art(const uint8_t* rgb565, int w, int h);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
