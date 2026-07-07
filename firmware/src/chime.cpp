#include "chime.h"
#include <Arduino.h>
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
