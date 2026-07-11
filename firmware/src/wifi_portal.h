#pragma once
// Captive-portal WiFi provisioning (S3-only, BOARD_HAS_WIFI). Brings up a SoftAP
// + DNS catch-all + a tiny HTTP form so a phone can set SSID / password / Asana
// token with no PC and no daemon — the "smart plug" onboarding flow. Writes the
// same NVS keys sprint_net reads ("clawdwifi": ssid/pw/tok), then reboots so
// sprint_net_init() picks them up. On boards without WiFi (C6) these are stubs.
void        wifi_portal_start(void);   // sobe AP + DNS + HTTP server
void        wifi_portal_stop(void);    // derruba tudo, volta pra STA
bool        wifi_portal_active(void);
void        wifi_portal_tick(void);    // processa DNS + HTTP; chamar todo loop
const char* wifi_portal_ap_name(void); // SSID do AP de setup (pra UI)
const char* wifi_portal_ap_ip(void);   // IP do portal, ex "192.168.4.1" (pra UI)
