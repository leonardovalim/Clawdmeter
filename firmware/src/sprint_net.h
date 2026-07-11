#pragma once
#include "data.h"

void sprint_net_init(void);
void sprint_net_tick(void);
void sprint_net_provision(const char* ssid, const char* pw, const char* tok);
bool sprint_net_get(UsageData* out);   // true if a fresh WiFi sprint is available
bool sprint_net_wifi_up(void);         // true se o STA está WL_CONNECTED
void sprint_net_forget(void);          // apaga creds do NVS + desconecta (reset WiFi)
void sprint_net_debug_status(void);    // imprime SSID, WiFi.status(), IP, s_have,
                                        // idade do último fetch — via cmd serial "wifi"
