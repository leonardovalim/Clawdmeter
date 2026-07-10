#pragma once
#include "data.h"

void sprint_net_init(void);
void sprint_net_tick(void);
void sprint_net_provision(const char* ssid, const char* pw, const char* tok);
bool sprint_net_get(UsageData* out);   // true if a fresh WiFi sprint is available
void sprint_net_debug_status(void);    // imprime SSID, WiFi.status(), IP, s_have,
                                        // idade do último fetch — via cmd serial "wifi"
