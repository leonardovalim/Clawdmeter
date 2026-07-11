// Captive-portal WiFi provisioning — see wifi_portal.h. AP-only setup flow: no
// TLS fetch runs while the portal is up (main.cpp gates sprint_net_tick on
// wifi_portal_active()), so the ~26KB largest internal block that the TLS
// handshake needs isn't contended here. Creds land in NVS and a reboot applies
// them via sprint_net_init().
#ifdef BOARD_HAS_WIFI
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include "wifi_portal.h"

static const char* AP_NAME  = "Clawdmeter-Setup";
static const uint16_t DNS_PORT = 53;

static DNSServer  s_dns;
static WebServer  s_server(80);
static Preferences s_prefs;
static bool     s_active = false;
static bool     s_manual = false;
static bool     s_restart_pending = false;
static uint32_t s_restart_at = 0;
static IPAddress s_ap_ip;
static char     s_ip_str[16] = "192.168.4.1";
static String   s_scan_opts;   // cached <option> list from the boot-time scan
static int      s_scan_count = 0;

// HTML-escape into a <select>/attribute-safe form. SSIDs can contain & < > " '.
static String esc(const String& in) {
    String o; o.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&': o += "&amp;";  break;
            case '<': o += "&lt;";   break;
            case '>': o += "&gt;";   break;
            case '"': o += "&quot;"; break;
            case '\'':o += "&#39;";  break;
            default:  o += c;        break;
        }
    }
    return o;
}

// Scan once at start (AP not yet serving clients) and cache the <option> list —
// re-scanning on every GET would block the handler for seconds.
static void scan_networks(void) {
    s_scan_opts = "";
    s_scan_count = 0;
    int n = WiFi.scanNetworks();
    // Mesh APs broadcast the same SSID several times — a raw list shows
    // "Lilo_Rick_Morty" ×3 interleaved with "..._IoT" ×3, and the user taps the
    // wrong near-identical row. Collapse to one <option> per unique SSID.
    for (int i = 0; i < n; i++) {
        String ss = WiFi.SSID(i);
        if (ss.length() == 0) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (WiFi.SSID(j) == ss) { dup = true; break; }
        }
        if (dup) continue;
        s_scan_opts += "<option value=\"" + esc(ss) + "\">" + esc(ss) + "</option>";
        s_scan_count++;
        Serial.printf("wifi_portal: scan '%s' %ddBm\n", ss.c_str(), WiFi.RSSI(i));
        if (s_scan_count >= 24) break;
    }
    WiFi.scanDelete();
}

static String build_page(void) {
    String p;
    p.reserve(1800 + s_scan_opts.length());
    p += "<!doctype html><html><head><meta charset=utf-8>"
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
         "<title>Clawdmeter WiFi</title><style>"
         "body{font-family:system-ui,-apple-system,sans-serif;background:#191917;"
         "color:#eee;margin:0;padding:24px;max-width:440px}"
         "h1{font-size:20px;font-weight:600}"
         "label{display:block;margin:18px 0 5px;font-size:13px;color:#b8b0a8}"
         "input,select{width:100%;box-sizing:border-box;padding:11px;font-size:16px;"
         "border-radius:9px;border:1px solid #46413c;background:#242220;color:#eee}"
         "button{margin-top:26px;width:100%;padding:14px;font-size:16px;font-weight:600;"
         "background:#d97757;color:#fff;border:0;border-radius:9px}"
         "small{display:block;margin-top:14px;color:#8a827a}</style></head><body>"
         "<h1>Clawdmeter · WiFi</h1>"
         "<form method=POST action=/save>"
         "<label>Rede</label><select name=ssid_pick>"
         "<option value=\"\">— escolher —</option>";
    p += s_scan_opts;
    p += "</select>"
         "<label>ou digite o nome da rede (oculta)</label>"
         "<input name=ssid_manual autocomplete=off placeholder=\"SSID\">"
         "<label>Senha do WiFi</label>"
         "<input name=pw type=password autocomplete=off>"
         "<label>Token do Asana</label>"
         "<input name=tok autocomplete=off placeholder=\"device token\">"
         "<button type=submit>Salvar e conectar</button></form>"
         "<small>Campos em branco mantêm o valor atual. A placa reinicia e "
         "conecta na rede escolhida.</small></body></html>";
    return p;
}

static void handle_root(void) {
    s_server.send(200, "text/html; charset=utf-8", build_page());
}

// Any unknown host → bounce to the portal root. This is what makes the phone's
// "sign in to network" captive-portal popup appear.
static void handle_not_found(void) {
    s_server.sendHeader("Location", String("http://") + s_ip_str + "/", true);
    s_server.send(302, "text/plain", "");
}

static void handle_save(void) {
    String ssid = s_server.arg("ssid_pick");
    if (ssid.length() == 0) ssid = s_server.arg("ssid_manual");
    ssid.trim();
    String pw  = s_server.arg("pw");
    String tok = s_server.arg("tok");
    tok.trim();

    // Empty fields preserve the current NVS value, so a user changing only the
    // network doesn't wipe the token (and vice-versa).
    s_prefs.begin("clawdwifi", false);
    if (ssid.length()) s_prefs.putString("ssid", ssid);
    if (pw.length())   s_prefs.putString("pw",   pw);
    if (tok.length())  s_prefs.putString("tok",  tok);
    s_prefs.end();

    Serial.printf("wifi_portal: saved ssid='%s' (pw=%d tok=%d) — rebooting\n",
                  ssid.c_str(), pw.length() > 0, tok.length() > 0);

    s_server.send(200, "text/html; charset=utf-8",
        "<!doctype html><meta charset=utf-8><body style=\"font-family:system-ui;"
        "background:#191917;color:#eee;padding:32px;text-align:center\">"
        "<h2>Salvo ✓</h2><p>A placa vai reiniciar e conectar em<br><b>" + esc(ssid) +
        "</b>.</p><p style=\"color:#8a827a\">Pode fechar esta página.</p></body>");

    // Defer the restart so the HTTP response actually flushes to the phone.
    s_restart_pending = true;
    s_restart_at = millis() + 1500;
}

void wifi_portal_start(bool manual) {
    if (s_active) return;
    s_manual = manual;
    WiFi.mode(WIFI_AP_STA);            // AP_STA so scan_networks() can see nearby APs
    WiFi.softAP(AP_NAME);              // open network — short-lived setup window
    delay(120);
    s_ap_ip = WiFi.softAPIP();
    strlcpy(s_ip_str, s_ap_ip.toString().c_str(), sizeof(s_ip_str));
    scan_networks();

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(DNS_PORT, "*", s_ap_ip);

    s_server.on("/", handle_root);
    s_server.on("/save", HTTP_POST, handle_save);
    s_server.onNotFound(handle_not_found);
    s_server.begin();

    s_active = true;
    s_restart_pending = false;
    Serial.printf("wifi_portal: AP '%s' @ %s (%d redes)\n",
                  AP_NAME, s_ip_str, s_scan_count);
}

void wifi_portal_stop(void) {
    if (!s_active) return;
    s_server.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_active = false;
    Serial.println("wifi_portal: stopped");
}

void wifi_portal_tick(void) {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_server.handleClient();
    if (s_restart_pending && (int32_t)(millis() - s_restart_at) >= 0) {
        Serial.println("wifi_portal: restart to apply creds");
        ESP.restart();
    }
}

bool        wifi_portal_active(void)  { return s_active; }
bool        wifi_portal_is_manual(void) { return s_manual; }
const char* wifi_portal_ap_name(void) { return AP_NAME; }
const char* wifi_portal_ap_ip(void)   { return s_ip_str; }

#else   // no WiFi on this board — empty stubs so shared callers link
#include "wifi_portal.h"
void        wifi_portal_start(bool)   {}
void        wifi_portal_stop(void)    {}
bool        wifi_portal_active(void)  { return false; }
bool        wifi_portal_is_manual(void) { return false; }
void        wifi_portal_tick(void)    {}
const char* wifi_portal_ap_name(void) { return ""; }
const char* wifi_portal_ap_ip(void)   { return ""; }
#endif
