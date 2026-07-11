// WiFi Sprint fetch path (S3-only, -DBOARD_HAS_WIFI). Stores BLE-provisioned
// WiFi creds + device token in NVS, polls the slim /api/device/sprint
// endpoint over TLS every FETCH_INTERVAL_MS, and hands the parsed bd_*
// fields back to main.cpp when fresh. On boards without WiFi (the C6 ports)
// this whole file compiles to empty stubs so shared callers still link.
#ifdef BOARD_HAS_WIFI
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>
#include "letsencrypt_ca.h"
#include "sprint_net.h"

// This build ships CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, so mbedTLS allocates the
// ~32KB of TLS handshake buffers from internal DRAM. At fetch time LVGL + NimBLE
// + the WiFi stack leave under ~30KB contiguous internal, so those allocations
// fail (MBEDTLS_ERR_SSL_ALLOC_FAILED) and the fetch never completes. Redirect
// mbedTLS's allocator to PREFER PSRAM (8MB free), falling back to internal — the
// same policy as CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, but flipped on at runtime via
// the MBEDTLS_PLATFORM_MEMORY hook (defined in ESP-IDF's mbedtls esp_config.h).
static void* tls_calloc_psram(size_t n, size_t size) {
    return heap_caps_calloc_prefer(n, size, 2,
                                   MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tls_free_psram(void* p) { heap_caps_free(p); }

static const char* SPRINT_URL = "https://asana-dash.vercel.app/api/device/sprint";
static const uint32_t FETCH_INTERVAL_MS = 300000UL;   // 5 min between fetches
static const uint32_t SPRINT_FRESH_MS   = 900000UL;   // 15 min freshness window

static Preferences s_prefs;
static char s_ssid[33] = "", s_pw[64] = "", s_tok[80] = "";
static UsageData s_bd{};           // only bd_* fields are populated
static uint32_t s_last_ok = 0;
static bool s_have = false;
static uint32_t s_last_fetch = 0;

static void load_creds() {
    s_prefs.begin("clawdwifi", true);
    s_prefs.getString("ssid", s_ssid, sizeof(s_ssid));
    s_prefs.getString("pw",   s_pw,   sizeof(s_pw));
    s_prefs.getString("tok",  s_tok,  sizeof(s_tok));
    s_prefs.end();
}

void sprint_net_provision(const char* ssid, const char* pw, const char* tok) {
    // Daemon reescreve o blob a cada reconexão BLE (~60s). Se as credenciais
    // são as mesmas de sempre, derrubar WiFi.disconnect() + WiFi.begin() a
    // cada vez impede o fetch de terminar — thrash. Só re-associa se algum
    // dos três campos efetivamente mudou.
    const bool net_changed = (strcmp(ssid, s_ssid) != 0) || (strcmp(pw, s_pw) != 0);
    const bool tok_changed = (strcmp(tok, s_tok) != 0);
    if (!net_changed && !tok_changed) {
        return;  // idempotente, sem log — evita spam
    }
    s_prefs.begin("clawdwifi", false);
    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pw",   pw);
    s_prefs.putString("tok",  tok);
    s_prefs.end();
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pw,   pw,   sizeof(s_pw));
    strlcpy(s_tok,  tok,  sizeof(s_tok));
    if (net_changed && s_ssid[0]) {
        WiFi.disconnect();
        WiFi.begin(s_ssid, s_pw);
    }
    if (tok_changed) {
        s_last_fetch = 0;   // força fetch imediato com o novo token
    }
    Serial.printf("sprint_net: provisioned ssid=%s (net_changed=%d tok_changed=%d)\n",
                  s_ssid, net_changed, tok_changed);
}

// Parse the {sn,td,dg,dn,tt,bi,ba} object into s_bd.bd_* — mirrors main.cpp:152-184.
static bool parse_bd(const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject bd = doc.as<JsonObject>();
    if (bd.isNull()) return false;
    s_bd.bd_todo  = bd["td"] | 0;
    s_bd.bd_doing = bd["dg"] | 0;
    s_bd.bd_done  = bd["dn"] | 0;
    s_bd.bd_total = bd["tt"] | 0;
    strlcpy(s_bd.bd_name, bd["sn"] | "", sizeof(s_bd.bd_name));
    // bd_max é o teto do eixo Y do burndown. O caminho BLE (main.cpp:167-175)
    // calcula max sobre ideal E actual — se o "actual" ultrapassa o "ideal"
    // (scope creep no sprint), o gráfico ainda cabe. Espelha essa lógica aqui
    // pra não divergir.
    JsonArray bi = bd["bi"].as<JsonArray>();
    JsonArray ba = bd["ba"].as<JsonArray>();
    uint8_t n = 0;
    uint8_t mx = 0;
    for (JsonVariant v : bi) {
        if (n >= BD_MAX_DAYS) break;
        int iv = v | 0;
        if (iv < 0) iv = 0;
        s_bd.bd_ideal[n] = (uint8_t)iv;
        if (iv > mx) mx = (uint8_t)iv;
        int av = n < ba.size() ? (ba[n] | -1) : -1;
        s_bd.bd_actual[n] = (int16_t)av;
        if (av > mx) mx = (uint8_t)av;
        n++;
    }
    s_bd.bd_days = n;
    s_bd.bd_max  = mx;
    s_bd.has_burndown = (n > 0);
    return s_bd.has_burndown;
}

static void fetch_now() {
    if (WiFi.status() != WL_CONNECTED || !s_tok[0]) return;

    // Route mbedTLS's allocations to PSRAM (see tls_calloc_psram). Global +
    // idempotent, so install once on the first fetch.
    static bool tls_alloc_installed = false;
    if (!tls_alloc_installed) {
        mbedtls_platform_set_calloc_free(tls_calloc_psram, tls_free_psram);
        tls_alloc_installed = true;
    }

    // Heap check: TLS handshake precisa de ~40KB. Se sobrar menos, dá
    // "SSL - Memory allocation failed" e nunca termina. Log pra ter chão
    // se voltar a acontecer.
    size_t free_before = ESP.getFreeHeap();
    size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    Serial.printf("sprint_net: fetch pre-heap free=%u largest_int=%u\n",
                  (unsigned)free_before, (unsigned)largest_free);

    WiFiClientSecure client;
    // TLS insecure: skip cert verification, poupa ~10-15KB de heap (sem parse
    // do CA nem cadeia de trust). Necessário porque no S3 com LVGL+NimBLE
    // ativos o setCACert dava MBEDTLS_ERR_SSL_ALLOC_FAILED. Trade-off: um MITM
    // na rede local pode interceptar o Bearer token; o endpoint é read-only
    // (só devolve dados do sprint) então o pior caso é o atacante espelhar o
    // sprint. Follow-up: voltar pra setCACert quando dimensionar mbedTLS
    // pra alocar em PSRAM.
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, SPRINT_URL)) return;
    http.addHeader("Authorization", String("Bearer ") + s_tok);
    http.setTimeout(8000);
    int code = http.GET();
    Serial.printf("sprint_net: fetch post-heap free=%u (code=%d)\n",
                  (unsigned)ESP.getFreeHeap(), code);
    if (code == 200) {
        if (parse_bd(http.getString())) {
            s_have = true;
            s_last_ok = millis();
            Serial.printf("sprint_net: fetch 200 OK (%s, %u/%u/%u/%u)\n",
                s_bd.bd_name, s_bd.bd_todo, s_bd.bd_doing, s_bd.bd_done, s_bd.bd_total);
        } else {
            Serial.println("sprint_net: fetch 200 mas parse_bd falhou");
        }
    } else {
        Serial.printf("sprint_net: HTTP %d\n", code);
    }
    http.end();
}

void sprint_net_init(void) {
    load_creds();
    WiFi.mode(WIFI_STA);
    if (s_ssid[0]) WiFi.begin(s_ssid, s_pw);
}

void sprint_net_tick(void) {
    if (!s_ssid[0]) return;
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED &&
        (s_last_fetch == 0 || now - s_last_fetch >= FETCH_INTERVAL_MS)) {
        s_last_fetch = now;
        fetch_now();
    }
}

bool sprint_net_wifi_up(void) {
    return WiFi.status() == WL_CONNECTED;
}

void sprint_net_forget(void) {
    // Reset de WiFi (gesto dos 2 botões): apaga as credenciais do NVS, zera o
    // estado em RAM e derruba o STA. Depois disso o portal sobe pra reprovisionar.
    s_prefs.begin("clawdwifi", false);
    s_prefs.clear();
    s_prefs.end();
    s_ssid[0] = s_pw[0] = s_tok[0] = '\0';
    s_have = false;
    s_last_ok = 0;
    WiFi.disconnect(true);
    Serial.println("sprint_net: credenciais apagadas (forget)");
}

void sprint_net_debug_status(void) {
    uint32_t now = millis();
    Serial.printf("=== sprint_net status ===\n");
    Serial.printf("  ssid nvs: '%s'\n", s_ssid);
    Serial.printf("  tok len : %u\n", (unsigned)strlen(s_tok));
    Serial.printf("  WiFi status: %d (WL_CONNECTED=3)\n", WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  IP: %s  RSSI: %d\n",
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    Serial.printf("  s_have=%d s_last_ok=%u (idade=%u ms) s_last_fetch=%u\n",
        s_have, s_last_ok, s_last_ok ? now - s_last_ok : 0, s_last_fetch);
    if (s_have) {
        Serial.printf("  bd: sn='%s' td=%u dg=%u dn=%u tt=%u days=%u\n",
            s_bd.bd_name, s_bd.bd_todo, s_bd.bd_doing, s_bd.bd_done,
            s_bd.bd_total, s_bd.bd_days);
    }
}

bool sprint_net_get(UsageData* out) {
    if (!s_have) return false;
    if (millis() - s_last_ok > SPRINT_FRESH_MS) return false;
    out->has_burndown = s_bd.has_burndown;
    out->bd_todo = s_bd.bd_todo; out->bd_doing = s_bd.bd_doing;
    out->bd_done = s_bd.bd_done; out->bd_total = s_bd.bd_total;
    out->bd_days = s_bd.bd_days; out->bd_max = s_bd.bd_max;
    strlcpy(out->bd_name, s_bd.bd_name, sizeof(out->bd_name));
    memcpy(out->bd_ideal,  s_bd.bd_ideal,  sizeof(out->bd_ideal));
    memcpy(out->bd_actual, s_bd.bd_actual, sizeof(out->bd_actual));
    out->bd_source_wifi = true;   // badge do Burndown mostra a fonte
    return true;
}
#else   // no WiFi on this board — empty stubs so shared callers link
#include "sprint_net.h"
void sprint_net_init(void) {}
void sprint_net_tick(void) {}
void sprint_net_provision(const char*, const char*, const char*) {}
bool sprint_net_get(UsageData*) { return false; }
bool sprint_net_wifi_up(void) { return false; }
void sprint_net_forget(void) {}
void sprint_net_debug_status(void) { Serial.println("sprint_net: sem WiFi neste board"); }
#endif
