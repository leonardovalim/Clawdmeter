#pragma once
#include <stdint.h>

// Board-agnostic ES8311 + I2S chime engine. A board's sound.cpp fills in a
// ChimeConfig (its I2S pins, codec address, volume, and a power-amp enable
// hook) and calls chime_init() once; chime_play() then streams the embedded
// reset clip (bell_pcm.h) to the codec in a one-shot FreeRTOS task so the LVGL
// loop never blocks. The embedded PCM is 44.1 kHz / 16-bit / stereo, so
// cfg.sample_rate must be 44100 for correct-pitch playback.
//
// amp_enable is the only part that differs structurally between boards: the
// 2.16 toggles a GPIO, the 1.8 drives an XCA9554 expander line. May be null.

typedef void (*amp_enable_fn)(bool on);

struct ChimeConfig {
    int          mclk, bclk, ws, dout, din;  // I2S GPIOs
    int          sample_rate;                // must match the embedded PCM (44100)
    uint8_t      es8311_addr;                // codec I2C address (0x18)
    uint8_t      volume;                     // 0..100
    amp_enable_fn amp_enable;                // power-amp on/off hook (nullable)
};

// Reset-notification cadence: 3 chimes 30s apart (0s/30s/60s) so the user is
// likely to hear one even if they're away from the desk when it fires.
#define CHIME_RESET_COUNT        3
#define CHIME_RESET_INTERVAL_MS  30000

// Bring up I2S + the ES8311 codec. Returns true on success; false leaves the
// engine inert (chime_play becomes a no-op). Wire/I2C must already be up.
bool chime_init(const ChimeConfig& cfg);

// Queue one playback of the embedded clip (non-blocking). No-op if not ready
// or already playing.
void chime_play(void);

// Queue a short ascending arpeggio ("level up" jingle), synthesized on the fly
// instead of embedded as PCM. Celebrates sprint tasks landing in Done. Same
// non-blocking contract as chime_play().
void chime_play_fanfare(void);

// Queue a single short blip acknowledging a touch. Same non-blocking contract;
// skipped if a longer sound is already playing.
void chime_play_click(void);

// Queue `count` playbacks, `interval_ms` apart, driven non-blocking from
// chime_tick(). Restarts the schedule if a sequence is already in progress.
void chime_play_repeated(int count, uint32_t interval_ms);

// Drives the chime_play_repeated() schedule. Call once per main loop.
void chime_tick(void);
