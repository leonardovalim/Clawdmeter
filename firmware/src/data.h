#pragma once
#include <Arduino.h>

#define BD_MAX_DAYS 14   // burndown series cap (sprints are ~10 working days)

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Stats screen (SCREEN_STATS)
    int     cache_hit_pct;   // 0-100; -1 = no data yet
    uint8_t hourly[24];      // peak session_pct per hour today (0-100)

    // Media screen (SCREEN_MEDIA)
    char media_title[48];
    char media_artist[32];
    bool media_playing;
    bool has_media;
    int  media_pos;          // seconds into the track (-1 = player has no timeline)
    int  media_dur;          // track length in seconds (0 = player has no timeline)

    // Burndown screen (SCREEN_BURNDOWN)
    uint8_t bd_todo;         // open tasks
    uint8_t bd_doing;        // in-progress tasks
    uint8_t bd_done;         // completed this sprint
    uint8_t bd_total;        // total sprint tasks
    bool    has_burndown;
    char    bd_name[16];     // sprint label e.g. "Sprint 32"
    uint8_t bd_days;         // number of points in the burndown series (<= BD_MAX_DAYS)
    uint8_t bd_ideal[BD_MAX_DAYS];   // ideal remaining per day
    int16_t bd_actual[BD_MAX_DAYS];  // actual remaining per day; -1 = future/no data
    uint8_t bd_max;          // Y-axis top (usually bd_ideal[0]); 0 = no series
    bool    bd_source_wifi;  // true = último bd_* veio do fetch WiFi/Vercel;
                             // false = BLE (fresco ou cache do último payload)
};
