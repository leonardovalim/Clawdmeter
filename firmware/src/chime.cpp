#include "chime.h"
#include <Arduino.h>
#include <math.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "bell_pcm.h"   // const uint8_t bell_pcm[] / bell_pcm_len — 44.1 kHz 16-bit stereo

// Shared ES8311 chime engine. See chime.h. Adapted from the original 2.16
// sound.cpp so the 2.16, 1.8 (and any future ES8311 board) share one copy of
// the codec setup, the embedded PCM, and the non-blocking playback task.

static I2SClass      i2s;
static ChimeConfig   cfg;
static bool          ready   = false;
static volatile bool playing = false;

// chime_play_repeated() state, advanced from chime_tick() on the main loop
// (not the playback task) so it stays a plain non-reentrant counter.
static int      repeat_remaining  = 0;
static uint32_t repeat_interval   = 0;
static uint32_t next_repeat_at_ms = 0;

static bool es8311_setup(void) {
    es8311_handle_t es = es8311_create(0, cfg.es8311_addr);   // I2C port 0 (shared Wire bus)
    if (!es) return false;
    // mclk_inverted, sclk_inverted, mclk_from_mclk_pin, mclk_frequency, sample_frequency
    const es8311_clock_config_t clk = {
        false, false, true, cfg.sample_rate * 256, cfg.sample_rate
    };
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(es, false);

    // The shared I2C bus also carries AXP2101/QMI8658 traffic during boot; a
    // transient bus hiccup here (seen in the field as i2c_master_transmit
    // ESP_ERR_INVALID_STATE) can silently leave this write unacknowledged,
    // so the codec stays at its post-reset default volume instead of
    // cfg.volume — chime_init() still reports success, but playback comes
    // out near-silent. Retry until the write is confirmed to land.
    for (int attempt = 1; attempt <= 5; attempt++) {
        if (es8311_voice_volume_set(es, cfg.volume, NULL) == ESP_OK) break;
        Serial.printf("chime: volume set attempt %d failed, retrying\n", attempt);
        delay(5);
    }
    return true;
}

// ---- Fanfare: a synthesized "level up" arpeggio -----------------------------
// C5-E5-G5-C6 plus a held C6. Each note is a sine blended with a square (the
// square is what gives it the chiptune bite) under an exponential decay
// envelope. Rendered one note at a time into a ~20 KB scratch buffer rather
// than one big clip, so we never need a 100 KB allocation.
static const uint16_t FANFARE_HZ[]  = {523, 659, 784, 1047, 1047};
static const uint16_t FANFARE_MS[]  = { 90,  90,  90,   90,  240};
static const int      FANFARE_NOTES = 5;

// A single short blip acknowledging a button tap.
static const uint16_t CLICK_HZ[]  = {1568};   // G6
static const uint16_t CLICK_MS[]  = {35};
static const int      CLICK_NOTES = 1;

// What the currently-queued task should play. Read by tone_task, written by the
// chime_play_* entry points under the `playing` latch, so it can't race.
static const uint16_t* tone_hz    = nullptr;
static const uint16_t* tone_ms    = nullptr;
static int             tone_count = 0;
static float           tone_amp   = 9000.0f;

// Render each note into a scratch buffer and stream it. One note at a time so
// we never need a ~100 KB allocation for the whole clip.
static void play_tones_blocking(const uint16_t* hz, const uint16_t* ms, int n, float amp) {
    for (int k = 0; k < n; k++) {
        const int frames = (cfg.sample_rate * ms[k]) / 1000;
        int16_t* buf = (int16_t*)malloc((size_t)frames * 2 * sizeof(int16_t));
        if (!buf) return;                       // out of heap: stay silent
        const float step = 2.0f * (float)M_PI * hz[k] / cfg.sample_rate;
        float phase = 0.0f;
        for (int i = 0; i < frames; i++) {
            const float env  = expf(-3.2f * (float)i / (float)frames);
            const float sine = sinf(phase);
            const float sq   = sine >= 0.0f ? 1.0f : -1.0f;
            const int16_t v  = (int16_t)((0.45f * sq + 0.55f * sine) * env * amp);
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
            phase += step;
        }
        i2s.write((uint8_t*)buf, (size_t)frames * 4);
        free(buf);
    }
    // i2s.write() returns once the data is queued into DMA, not once it has
    // been clocked out — hold the amp open for the tail (same trap as the bell).
    delay(ms[n - 1] + 60);
}

static void tone_task(void* arg) {
    if (cfg.amp_enable) cfg.amp_enable(true);
    delay(8);
    play_tones_blocking(tone_hz, tone_ms, tone_count, tone_amp);
    if (cfg.amp_enable) cfg.amp_enable(false);
    playing = false;
    vTaskDelete(nullptr);
}

static void queue_tones(const uint16_t* hz, const uint16_t* ms, int n, float amp,
                        const char* name) {
    if (!ready || playing) return;
    playing = true;
    tone_hz = hz; tone_ms = ms; tone_count = n; tone_amp = amp;
    // 4 KB isn't enough once the float math pulls in its stack frames.
    if (xTaskCreatePinnedToCore(tone_task, name, 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        Serial.printf("chime: %s task create failed\n", name);
        playing = false;
    }
}

void chime_play_fanfare(void) {
    queue_tones(FANFARE_HZ, FANFARE_MS, FANFARE_NOTES, 9000.0f, "fanfare");
}

void chime_play_click(void) {
    // Quieter than the fanfare — this fires on every tap.
    queue_tones(CLICK_HZ, CLICK_MS, CLICK_NOTES, 4200.0f, "click");
}

static void chime_task(void* arg) {
    if (cfg.amp_enable) cfg.amp_enable(true);
    delay(8);                                  // let the amp settle (avoids turn-on pop)
    i2s.write((uint8_t*)bell_pcm, bell_pcm_len);
    // i2s.write() only blocks until the clip is queued into the DMA buffer, not
    // until the codec has actually finished streaming it out — cutting the amp
    // right after write() returns silently truncated almost the whole clip.
    // 16-bit stereo = 4 bytes/frame; hold the amp open for the real duration.
    uint32_t frames  = bell_pcm_len / 4;
    uint32_t play_ms = (frames * 1000UL) / cfg.sample_rate;
    delay(play_ms + 20);                       // + settle margin before muting
    if (cfg.amp_enable) cfg.amp_enable(false);
    playing = false;
    vTaskDelete(nullptr);
}

bool chime_init(const ChimeConfig& c) {
    cfg = c;
    if (cfg.amp_enable) cfg.amp_enable(false);   // amp off until we play

    i2s.setPins(cfg.bclk, cfg.ws, cfg.dout, cfg.din, cfg.mclk);
    if (!i2s.begin(I2S_MODE_STD, cfg.sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("chime: I2S init failed");
        return false;
    }
    if (!es8311_setup()) {
        Serial.println("chime: ES8311 init failed");
        return false;
    }
    ready = true;
    Serial.println("chime: ES8311 ready");
    return true;
}

void chime_play(void) {
    if (!ready || playing) {
        Serial.printf("chime: play skipped (ready=%d playing=%d)\n", ready, playing);
        return;
    }
    playing = true;
    if (xTaskCreatePinnedToCore(chime_task, "chime", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        Serial.println("chime: task create failed");
        playing = false;   // couldn't spawn — stay silent rather than wedge the flag
    }
}

void chime_play_repeated(int count, uint32_t interval_ms) {
    if (count <= 0) return;
    chime_play();
    repeat_remaining  = count - 1;
    repeat_interval   = interval_ms;
    next_repeat_at_ms = millis() + interval_ms;
}

void chime_tick(void) {
    // millis() subtraction wraps safely across the ~49-day rollover.
    if (repeat_remaining > 0 && (int32_t)(millis() - next_repeat_at_ms) >= 0) {
        chime_play();
        repeat_remaining--;
        next_repeat_at_ms += repeat_interval;
    }
}
