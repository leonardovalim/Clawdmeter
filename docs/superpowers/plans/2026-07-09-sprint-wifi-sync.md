# Sprint-over-WiFi Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the ESP32 fetch Sprint/burndown data directly from the DashboardAsana Vercel API over WiFi, so the Sprint screen stays fresh even when the Mac is off — while BLE keeps serving the three local-sourced screens.

**Architecture:** Hybrid transport. A new slim Vercel endpoint returns the exact same `bd` JSON object the daemon already sends over BLE. The daemon provisions WiFi SSID/password + a device token to the ESP32 once over a new BLE PROV characteristic; the device stores them in NVS and then fetches Sprint over HTTPS on its own 5-minute timer. A source arbiter prefers a fresh WiFi sprint, falling back to the BLE `bd`. WiFi is compiled only into the S3 boards behind `-DBOARD_HAS_WIFI`.

**Tech Stack:** ESP32 Arduino (pioarduino platform), NimBLE-Arduino, ArduinoJson 7, WiFiClientSecure/HTTPClient + Preferences (bundled with core, no lib_deps), LVGL 9; Python daemons (bleak); Node ESM serverless on Vercel.

## Global Constraints

- **The WiFi endpoint and the BLE `bd` payload MUST share one JSON shape:** `{"sn":str,"td":int,"dg":int,"dn":int,"tt":int,"bi":[int...],"ba":[int...]}` — sprint name, todo, doing, done, total, burndown ideal series, burndown actual series (`-1` = future/no data). This is exactly what `daemon/claude_usage_daemon.py::_fetch_sprint` produces and what firmware `parse_json`'s `doc["bd"]` block reads (`firmware/src/main.cpp:152-184`). Do not invent new keys.
- **PROV characteristic UUID:** `4c41555a-4465-7669-6365-000000000005` (slot `...05`, next free after REQ `...04`).
- **Provisioning blob format (written to PROV char):** JSON `{"ssid":str,"pw":str,"tok":str}`.
- **Slim endpoint:** `GET https://asana-dash.vercel.app/api/device/sprint`, auth `Authorization: Bearer <DEVICE_TOKEN>`.
- **`BOARD_HAS_WIFI` MUST be a compiler flag** (`-DBOARD_HAS_WIFI` in the two S3 envs), never a `board.h` `#define` — shared `src/*.cpp` files don't include `board.h`. Mirror `BOARD_HAS_PSRAM`.
- **S3-only for v1:** `waveshare_amoled_216`, `waveshare_amoled_18`. C6 envs must keep building with zero WiFi code linked and keep using the BLE `bd`.
- **Never regress:** the daemon keeps sending `bd` over BLE. WiFi is additive; the arbiter falls back to BLE.
- Commit messages end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## File Structure

**DashboardAsana** (`~/Projetos/DashboardAsana`):
- Create `api/device/sprint.js` — slim device endpoint (DEVICE_TOKEN auth, returns `bd` shape).
- Modify `server/index.js` — register the route for local dev.
- Modify `.env.example` — document `DEVICE_TOKEN`.

**Clawdmeter daemon** (`~/Projetos/Pessoal/Clawdmeter/daemon`):
- Modify `claude_usage_daemon.py` and `claude_usage_daemon_windows.py` — `PROV_CHAR_UUID`, `_read_wifi_creds()`, provision-once-on-connect.
- Modify `config.example` — document `wifi_ssid`/`wifi_password`/`device_token`.
- Create `tests/test_wifi_provision.py` — config-parser tests.

**Clawdmeter firmware** (`~/Projetos/Pessoal/Clawdmeter/firmware`):
- Modify `platformio.ini` — add `-DBOARD_HAS_WIFI` to the two S3 envs.
- Modify `src/ble.cpp`/`src/ble.h` — PROV char + `ProvCallbacks` + a hook to hand creds to sprint_net.
- Create `src/sprint_net.h`/`src/sprint_net.cpp` — WiFi connect, HTTPS GET, JSON→`bd` buffer, NVS creds + last-sprint, arbiter accessor. Whole `.cpp` wrapped in `#ifdef BOARD_HAS_WIFI`.
- Create `src/letsencrypt_ca.h` — embedded ISRG Root X1 PEM for TLS.
- Modify `src/main.cpp` — call `sprint_net_init()`/`sprint_net_tick()` and arbitrate WiFi vs BLE `bd` before `ui_update`.

---

## Phase 1 — DashboardAsana slim endpoint (independent, curl-verifiable)

### Task 1.1: Device endpoint with DEVICE_TOKEN auth returning the `bd` shape

**Files:**
- Create: `api/device/sprint.js`
- Reference (do not modify): `api/mcp.js:14-25` (auth), `api/sprint.js:10-46` (aggregate reuse), `lib/sprint-aggregator.js:149,229-271`, `lib/asana-tasks.js:120-125`

**Interfaces:**
- Produces: HTTP `GET /api/device/sprint` → `200 {sn,td,dg,dn,tt,bi,ba}`; `401` on bad/missing token; `500` if `DEVICE_TOKEN` unset.

- [ ] **Step 1: Write the endpoint**

```js
// api/device/sprint.js
import crypto from 'node:crypto';
import { fetchEnrichedTasks } from '../../lib/asana-tasks.js';
import { getSprintDates, aggregate } from '../../lib/sprint-aggregator.js';

const SPRINT_START_DATE = process.env.SPRINT_START_DATE;
const PROJECT_GID = process.env.ASANA_PROJECT_GID;
const ASANA_TOKEN = process.env.ASANA_PERSONAL_ACCESS_TOKEN;
const TEAM = process.env.TEAM_NAME || 'Produto';

// Fail-closed bearer check, mirrors api/mcp.js:14-25 but against DEVICE_TOKEN.
function checkAuth(req) {
  const expected = process.env.DEVICE_TOKEN;
  if (!expected) return { ok: false, status: 500, message: 'DEVICE_TOKEN not configured' };
  const m = /^Bearer (.+)$/.exec(req.headers['authorization'] || '');
  if (!m) return { ok: false, status: 401, message: 'Unauthorized' };
  const given = Buffer.from(m[1]);
  const want = Buffer.from(expected);
  if (given.length !== want.length || !crypto.timingSafeEqual(given, want)) {
    return { ok: false, status: 401, message: 'Unauthorized' };
  }
  return { ok: true };
}

export default async function handler(req, res) {
  const auth = checkAuth(req);
  if (!auth.ok) return res.status(auth.status).json({ error: auth.message });
  try {
    const tasks = await fetchEnrichedTasks(ASANA_TOKEN, PROJECT_GID);
    const sprint = getSprintDates(SPRINT_START_DATE, 0);
    const d = aggregate(tasks, { ...sprint, team: TEAM, projectGid: PROJECT_GID }, {});
    const bi = Array.isArray(d.burndown_ideal) ? d.burndown_ideal.map(n => Math.max(0, Math.round(n))) : [];
    const ba = Array.isArray(d.burndown_actual)
      ? d.burndown_actual.map(n => (n == null || n < 0 ? -1 : Math.round(n))) : [];
    res.json({
      sn: String(d.sprint || '').slice(0, 15),
      td: d.todo | 0, dg: d.doing | 0, dn: d.done | 0,
      tt: (d.todo | 0) + (d.doing | 0) + (d.done | 0),
      bi, ba,
    });
  } catch (e) {
    console.error('[asana-dashboard] erro em /api/device/sprint:', e.message);
    res.status(500).json({ error: 'internal error' });
  }
}
```

Note: confirm the env var names during implementation by reading `api/sprint.js:10-20` — if `api/sprint.js` reads the token/project/start-date from different env names, copy those exact names here so both endpoints agree.

- [ ] **Step 2: Register for local dev**

In `server/index.js`, near the existing sprint route (`server/index.js:12-13` imports, `:218` mount) add:

```js
import deviceSprintHandler from '../api/device/sprint.js';   // with the other imports (~:13)
// ...
app.get('/api/device/sprint', deviceSprintHandler);          // near app.get('/api/sprint', ...) (~:218)
```

- [ ] **Step 3: Add DEVICE_TOKEN to .env.example and generate a real one**

Append to `.env.example` (near the `MCP_AUTH_TOKEN` block ~`.env.example:40-41`):

```
# Bearer token the Clawdmeter device uses for GET /api/device/sprint.
# Generate with: openssl rand -hex 32
DEVICE_TOKEN=
```

Generate a value and add it to the real `.env` (do not commit `.env`):

```bash
cd ~/Projetos/DashboardAsana && printf 'DEVICE_TOKEN=%s\n' "$(openssl rand -hex 32)" >> .env
```

- [ ] **Step 4: Run the endpoint locally and verify shape**

```bash
cd ~/Projetos/DashboardAsana && npm run dev &   # or `node server/index.js`
TOK=$(grep '^DEVICE_TOKEN=' .env | cut -d= -f2)
curl -s -H "Authorization: Bearer $TOK" http://localhost:3000/api/device/sprint | node -e 'const d=JSON.parse(require("fs").readFileSync(0));console.log(Object.keys(d).sort().join(","));'
```
Expected: `ba,bi,dg,dn,sn,td,tt`. Also verify `curl` with no token → `401`.

- [ ] **Step 5: Commit**

```bash
cd ~/Projetos/DashboardAsana
git checkout -b device-sprint-endpoint
git add api/device/sprint.js server/index.js .env.example
git commit -m "feat(api): slim /api/device/sprint endpoint for the Clawdmeter device

Returns the compact {sn,td,dg,dn,tt,bi,ba} burndown object (same shape the
daemon puts on BLE), gated by a dedicated DEVICE_TOKEN so the device never
carries the team MCP token.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 6: Deploy to Vercel & set the prod env**

Set `DEVICE_TOKEN` in the Vercel project `asana-dash` (dashboard → Settings → Environment Variables, same value as local `.env`), then deploy (push the branch / `vercel --prod` if the CLI is configured). Verify:

```bash
curl -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer $TOK" https://asana-dash.vercel.app/api/device/sprint
```
Expected: `200`. **This step needs the user's Vercel access — flag it if the CLI isn't authenticated.**

---

## Phase 2 — Daemon WiFi provisioning (macOS + Windows, pytest-verifiable)

### Task 2.1: Config parser `_read_wifi_creds()` + tests

**Files:**
- Modify: `daemon/claude_usage_daemon.py` (after `_read_asana_token` ~`:757-773`)
- Modify: `daemon/claude_usage_daemon_windows.py` (after `_read_asana_token` ~`:488-504`)
- Create: `daemon/tests/test_wifi_provision.py`

**Interfaces:**
- Produces: `_read_wifi_creds() -> dict | None` returning `{"ssid":..,"pw":..,"tok":..}` when all three present, else `None`. Also module const `PROV_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"`.

- [ ] **Step 1: Write the failing test**

```python
# daemon/tests/test_wifi_provision.py
import daemon.claude_usage_daemon as mac
import daemon.claude_usage_daemon_windows as win
import pytest

@pytest.mark.parametrize("mod", [mac, win])
def test_wifi_creds_none_when_incomplete(tmp_path, monkeypatch, mod):
    cfg = tmp_path / "config"
    cfg.write_text("wifi_ssid = MyNet\n")  # missing pw + tok
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    monkeypatch.delenv("WIFI_SSID", raising=False)
    assert mod._read_wifi_creds() is None

@pytest.mark.parametrize("mod", [mac, win])
def test_wifi_creds_full_and_hash_in_password(tmp_path, monkeypatch, mod):
    cfg = tmp_path / "config"
    cfg.write_text("wifi_ssid = MyNet\nwifi_password = pa#ss w0rd\ndevice_token = abc123\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    creds = mod._read_wifi_creds()
    assert creds == {"ssid": "MyNet", "pw": "pa#ss w0rd", "tok": "abc123"}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `cd ~/Projetos/Pessoal/Clawdmeter && python -m pytest daemon/tests/test_wifi_provision.py -q`
Expected: FAIL (`_read_wifi_creds` not defined).

- [ ] **Step 3: Implement `_read_wifi_creds` in both daemons**

Add to BOTH files (identical), after `_read_asana_token`. Use the `_read_asana_token` style (whole-line strip, NO split on `#`) so `#` in passwords survives:

```python
def _read_wifi_creds():
    """Return {"ssid","pw","tok"} for WiFi Sprint provisioning, or None if incomplete.

    Reads wifi_ssid / wifi_password / device_token from the config file. Does NOT
    split on '#', so passwords may contain '#'. Env vars WIFI_SSID / WIFI_PASSWORD /
    DEVICE_TOKEN take precedence per-field.
    """
    vals = {"ssid": os.environ.get("WIFI_SSID", "").strip(),
            "pw": os.environ.get("WIFI_PASSWORD", ""),
            "tok": os.environ.get("DEVICE_TOKEN", "").strip()}
    keymap = {"wifi_ssid": "ssid", "wifi_password": "pw", "device_token": "tok"}
    try:
        for line in CONFIG_FILE.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if s.startswith("#") or "=" not in s:
                continue
            key, val = s.split("=", 1)
            slot = keymap.get(key.strip().lower())
            if slot and not vals[slot]:
                vals[slot] = val.strip() if slot != "pw" else val.strip()
    except Exception:
        pass
    if vals["ssid"] and vals["pw"] and vals["tok"]:
        return {"ssid": vals["ssid"], "pw": vals["pw"], "tok": vals["tok"]}
    return None
```

Also add the constant next to `REQ_CHAR_UUID` (macOS `:57`, Windows `:57`):

```python
PROV_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
```

- [ ] **Step 4: Run tests, verify pass**

Run: `python -m pytest daemon/tests/test_wifi_provision.py -q`
Expected: PASS (4 passed).

- [ ] **Step 5: Commit**

```bash
git add daemon/claude_usage_daemon.py daemon/claude_usage_daemon_windows.py daemon/tests/test_wifi_provision.py
git commit -m "feat(daemon): read WiFi Sprint provisioning creds from config

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task 2.2: Provision once per connection over PROV char

**Files:**
- Modify: `daemon/claude_usage_daemon.py` (`connect_and_run`, after `setup_refresh_subscription()` ~`:1204`)
- Modify: `daemon/claude_usage_daemon_windows.py` (after `setup_refresh_subscription()` ~`:990`)

**Interfaces:**
- Consumes: `_read_wifi_creds()`, `PROV_CHAR_UUID`, `Session.client`.

- [ ] **Step 1: Add a provisioning write after the refresh subscription (macOS)**

In `claude_usage_daemon.py`, right after `await session.setup_refresh_subscription()` (~`:1204`):

```python
        creds = _read_wifi_creds()
        if creds is not None:
            try:
                blob = json.dumps(creds, separators=(",", ":")).encode()
                await client.write_gatt_char(PROV_CHAR_UUID, blob, response=False)
                log(f"WiFi provisioning sent (ssid={creds['ssid']})")
            except (BleakError, OSError, ValueError) as e:
                log(f"WiFi provisioning skipped: {e}")
```

- [ ] **Step 2: Same for Windows**

In `claude_usage_daemon_windows.py` (~`:990`), same block but `response=True` (Windows convention) and catch `(BleakError, OSError, ValueError)`.

- [ ] **Step 3: Syntax check both**

Run: `python -m py_compile daemon/claude_usage_daemon.py daemon/claude_usage_daemon_windows.py`
Expected: no output (success).

- [ ] **Step 4: Document config.example**

Append to `daemon/config.example`:

```
# --- WiFi Sprint sync (S3 boards only) ---
# When all three are set, the daemon pushes them to the device once per BLE
# connect; the device then fetches Sprint over WiFi (fresh even when this host
# is off). Leave unset to keep Sprint on BLE only.
# wifi_ssid = YourNetwork
# wifi_password = your-wifi-password
# device_token = <the DEVICE_TOKEN from the DashboardAsana Vercel project>
```

- [ ] **Step 5: Commit**

```bash
git add daemon/claude_usage_daemon.py daemon/claude_usage_daemon_windows.py daemon/config.example
git commit -m "feat(daemon): push WiFi creds to device over PROV characteristic

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

Note: the write silently no-ops if the firmware lacks the PROV char (older firmware) — bleak raises, we log and continue. Full round-trip verification happens in Phase 4 after the new firmware is flashed.

---

## Phase 3 — Firmware (S3 WiFi path; build-verifiable, hardware-verified in Phase 4)

### Task 3.1: Add `-DBOARD_HAS_WIFI` to the two S3 envs

**Files:**
- Modify: `firmware/platformio.ini` (env `waveshare_amoled_216` `build_flags` ~`:19`; env `waveshare_amoled_18` ~`:79`)

- [ ] **Step 1: Add the flag next to `-DBOARD_HAS_PSRAM` in BOTH S3 envs**

Add `-DBOARD_HAS_WIFI` on its own line in the `build_flags` list of `[env:waveshare_amoled_216]` and `[env:waveshare_amoled_18]` (do NOT add to either `_c6` env).

- [ ] **Step 2: Verify the flag reaches only S3**

Run: `cd firmware && grep -n "BOARD_HAS_WIFI" platformio.ini`
Expected: exactly two matches, both in S3 env blocks.

- [ ] **Step 3: Commit** (fold into Task 3.4's commit if executed together, else commit standalone)

### Task 3.2: Embedded Let's Encrypt root CA

**Files:**
- Create: `firmware/src/letsencrypt_ca.h`

- [ ] **Step 1: Create the CA header with the ISRG Root X1 PEM**

Create `src/letsencrypt_ca.h` exposing `extern const char* LETSENCRYPT_ROOT_CA;` defined as the public **ISRG Root X1** certificate PEM (the Let's Encrypt root Vercel chains to). Fetch the canonical PEM at implementation time:

```bash
curl -s https://letsencrypt.org/certs/isrgrootx1.pem
```

Paste it verbatim as a raw string literal:

```cpp
#pragma once
// ISRG Root X1 (Let's Encrypt root). Vercel's TLS chain terminates here.
// Source: https://letsencrypt.org/certs/isrgrootx1.pem
static const char LETSENCRYPT_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
...full ISRG Root X1 PEM here...
-----END CERTIFICATE-----
)EOF";
```

- [ ] **Step 2: Commit** (fold into Task 3.4).

### Task 3.3: `sprint_net` module — NVS, WiFi, HTTPS, JSON→`bd` buffer

**Files:**
- Create: `firmware/src/sprint_net.h`, `firmware/src/sprint_net.cpp`
- Reference: `src/data.h:34-44` (`bd_*`), `src/main.cpp:152-184` (`bd` parse to mirror), `src/brightness.cpp:14-35` (Preferences pattern)

**Interfaces:**
- Produces (header, always visible; stubbed when no WiFi):
  - `void sprint_net_init(void);`
  - `void sprint_net_tick(void);`
  - `void sprint_net_provision(const char* ssid, const char* pw, const char* tok);` — called from BLE PROV callback; persists to NVS and (re)connects.
  - `bool sprint_net_get(UsageData* out);` — if a fresh WiFi sprint exists (last OK within `SPRINT_FRESH_MS`), copies `bd_*` into `out` and returns true; else false.

- [ ] **Step 1: Header with unconditional prototypes**

```cpp
// src/sprint_net.h
#pragma once
#include "data.h"
void sprint_net_init(void);
void sprint_net_tick(void);
void sprint_net_provision(const char* ssid, const char* pw, const char* tok);
bool sprint_net_get(UsageData* out);   // true if a fresh WiFi sprint is available
```

- [ ] **Step 2: Implementation, whole body gated by `#ifdef BOARD_HAS_WIFI`**

Create `src/sprint_net.cpp`. Structure:
- `#ifdef BOARD_HAS_WIFI` … `#else` (empty stubs so linking works on C6) … `#endif`.
- Includes: `<WiFi.h>`, `<WiFiClientSecure.h>`, `<HTTPClient.h>`, `<Preferences.h>`, `<ArduinoJson.h>`, `"letsencrypt_ca.h"`, `"sprint_net.h"`.
- Constants: `SPRINT_URL "https://asana-dash.vercel.app/api/device/sprint"`, `FETCH_INTERVAL_MS 300000UL`, `SPRINT_FRESH_MS 900000UL`, NVS namespace `"clawdwifi"` keys `ssid`/`pw`/`tok`, plus last-sprint keys.
- State: `static UsageData s_bd{};` (only `bd_*` fields used), `static uint32_t s_last_ok=0;`, `static bool s_have=false;`, cred buffers.

Core functions (real code):

```cpp
#ifdef BOARD_HAS_WIFI
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "letsencrypt_ca.h"
#include "sprint_net.h"

static Preferences s_prefs;
static char s_ssid[33]="", s_pw[64]="", s_tok[80]="";
static UsageData s_bd{};           // only bd_* fields are populated
static uint32_t s_last_ok = 0;
static bool s_have = false;
static uint32_t s_last_fetch = 0;
static const uint32_t FETCH_INTERVAL_MS = 300000UL;
static const uint32_t SPRINT_FRESH_MS   = 900000UL;

static void load_creds() {
  s_prefs.begin("clawdwifi", true);
  s_prefs.getString("ssid", s_ssid, sizeof(s_ssid));
  s_prefs.getString("pw",   s_pw,   sizeof(s_pw));
  s_prefs.getString("tok",  s_tok,  sizeof(s_tok));
  s_prefs.end();
}

void sprint_net_provision(const char* ssid, const char* pw, const char* tok) {
  s_prefs.begin("clawdwifi", false);
  s_prefs.putString("ssid", ssid);
  s_prefs.putString("pw",   pw);
  s_prefs.putString("tok",  tok);
  s_prefs.end();
  strlcpy(s_ssid, ssid, sizeof(s_ssid));
  strlcpy(s_pw,   pw,   sizeof(s_pw));
  strlcpy(s_tok,  tok,  sizeof(s_tok));
  if (s_ssid[0]) { WiFi.disconnect(); WiFi.begin(s_ssid, s_pw); }
  s_last_fetch = 0;   // fetch ASAP once connected
  Serial.printf("sprint_net: provisioned ssid=%s\n", s_ssid);
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
  JsonArray bi = bd["bi"].as<JsonArray>();
  JsonArray ba = bd["ba"].as<JsonArray>();
  uint8_t n = 0;
  for (JsonVariant v : bi) { if (n >= BD_MAX_DAYS) break; s_bd.bd_ideal[n++] = v.as<int>(); }
  s_bd.bd_days = n;
  s_bd.bd_max  = (n > 0) ? s_bd.bd_ideal[0] : 0;
  uint8_t m = 0;
  for (JsonVariant v : ba) { if (m >= BD_MAX_DAYS) break; s_bd.bd_actual[m++] = v.as<int>(); }
  s_bd.has_burndown = (n > 0);
  return s_bd.has_burndown;
}

static void fetch_now() {
  if (WiFi.status() != WL_CONNECTED || !s_tok[0]) return;
  WiFiClientSecure client;
  client.setCACert(LETSENCRYPT_ROOT_CA);
  HTTPClient http;
  if (!http.begin(client, "https://asana-dash.vercel.app/api/device/sprint")) return;
  http.addHeader("Authorization", String("Bearer ") + s_tok);
  http.setTimeout(8000);
  int code = http.GET();
  if (code == 200) {
    if (parse_bd(http.getString())) { s_have = true; s_last_ok = millis(); }
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
  return true;
}
#else   // no WiFi on this board — empty stubs so shared callers link
#include "sprint_net.h"
void sprint_net_init(void) {}
void sprint_net_tick(void) {}
void sprint_net_provision(const char*, const char*, const char*) {}
bool sprint_net_get(UsageData*) { return false; }
#endif
```

Confirm field names/types against `src/data.h:34-44` while implementing (e.g. `bd_actual` is `int16_t`, `bd_ideal` is `uint8_t`).

- [ ] **Step 3: Commit** (fold with 3.1/3.2 — see 3.5 build gate).

### Task 3.4: PROV characteristic in BLE, wired to `sprint_net_provision`

**Files:**
- Modify: `src/ble.cpp` (UUID ~`:10-13`; ptr ~`:89-91`; new `ProvCallbacks` modeled on `RxCallbacks` `:258-319`; create char in `ble_init()` after `:393`, before `svc->start()` `:395`)
- Reference: `src/main.cpp` includes for how to reach `sprint_net_provision`

**Interfaces:**
- Consumes: `sprint_net_provision(ssid,pw,tok)` from `sprint_net.h`.

- [ ] **Step 1: Add UUID, pointer, include**

At `ble.cpp:13` add `#define PROV_CHAR_UUID "4c41555a-4465-7669-6365-000000000005"`. Near `:91` add `static NimBLECharacteristic* prov_char = nullptr;`. Add `#include "sprint_net.h"` with the other includes near `ble.cpp:5`.

- [ ] **Step 2: Add `ProvCallbacks` (parse JSON blob, call sprint_net_provision)**

Model on `RxCallbacks` (`:258-319`), keeping the same `info.isEncrypted()`/owner gate:

```cpp
class ProvCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) return;                 // same gate as RxCallbacks
    std::string v = chr->getValue();
    JsonDocument doc;
    if (deserializeJson(doc, v)) return;
    const char* ssid = doc["ssid"] | "";
    const char* pw   = doc["pw"]   | "";
    const char* tok  = doc["tok"]  | "";
    if (ssid[0] && tok[0]) {
      sprint_net_provision(ssid, pw, tok);
      Serial.println("BLE: WiFi provisioning received");
    }
  }
};
static ProvCallbacks provCb;
```

(`ArduinoJson.h` is already included in the firmware; add the include to `ble.cpp` if not present.)

- [ ] **Step 3: Create the characteristic in `ble_init()`**

After the REQ char block (`ble.cpp:393`), before `svc->start()` (`:395`):

```cpp
    prov_char = svc->createCharacteristic(
        PROV_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    prov_char->setCallbacks(&provCb);
```

- [ ] **Step 4: (defer build to 3.6)**

### Task 3.5: Arbitration in `main.cpp` (WiFi fresh wins, else BLE)

**Files:**
- Modify: `src/main.cpp` (include; `setup()` after `ble_init()` ~`:291`; `loop()` tick near `ble_tick()` ~`:371`; data merge before `ui_update` ~`:458-480`)

- [ ] **Step 1: Include + init + tick**

Add `#include "sprint_net.h"` near the top includes. In `setup()` after `ble_init();` (`:291`) add `sprint_net_init();`. In `loop()` near `ble_tick();` (`:371`) add `sprint_net_tick();`. (The stubs make these safe on C6.)

- [ ] **Step 2: Arbitrate before rendering**

In the data-handling block (`:458-480`), after `parse_json(...)` fills `usage` from BLE and before `ui_update(&usage)` (`:475`), override with a fresh WiFi sprint:

```cpp
            // WiFi Sprint wins when fresh; otherwise the BLE bd already in `usage`
            // stays (fallback). No-op stub on non-WiFi boards.
            sprint_net_get(&usage);
```

Because `sprint_net_get` only writes the `bd_*` fields and only when fresh, the BLE-sourced `bd` remains untouched when WiFi is stale/absent. On boards without WiFi it returns false immediately.

Also apply the same call on the periodic no-BLE-data path if the UI is refreshed without a new BLE payload — confirm during implementation whether `ui_update` is only called on new BLE data (`:475`). If Sprint must update between BLE payloads, add a lightweight path: when `sprint_net_get` reports a *newer* `s_last_ok` than last render, call `ui_update(&usage)` from `loop()` with the last-known `usage`. Keep a `static UsageData last_usage;` cached at the `:475` call site for this.

- [ ] **Step 3: Build all four envs**

```bash
cd firmware
pio run -e waveshare_amoled_216
pio run -e waveshare_amoled_18
pio run -e waveshare_amoled_216_c6
pio run -e waveshare_amoled_18_c6
```
Expected: all four succeed. The two C6 builds must link with the empty `sprint_net` stubs (no WiFi symbols). If a C6 build pulls in WiFi, the `#ifdef BOARD_HAS_WIFI` gate or the flag placement is wrong — fix before proceeding.

- [ ] **Step 4: Commit the firmware WiFi path**

```bash
git add firmware/platformio.ini firmware/src/sprint_net.h firmware/src/sprint_net.cpp \
        firmware/src/letsencrypt_ca.h firmware/src/ble.cpp firmware/src/main.cpp
git commit -m "feat(firmware): fetch Sprint over WiFi on S3 (PROV char + HTTPS + arbiter)

New sprint_net module (S3-only, -DBOARD_HAS_WIFI) stores BLE-provisioned WiFi
creds + device token in NVS, fetches the slim /api/device/sprint every 5 min
over TLS, and overrides the BLE bd when fresh. C6 links empty stubs.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 4 — End-to-end integration & hardware verification (user-assisted)

### Task 4.1: Flash S3 and verify Mac-independent Sprint

- [ ] **Step 1: Flash the user's 2.16 S3**

```bash
cd firmware && pio run -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101
```
(Adjust port. Needs the device on USB — user-assisted.)

- [ ] **Step 2: Provision via daemon**

Add `wifi_ssid`, `wifi_password`, `device_token` to `~/.config/claude-usage-monitor/config`, restart the tray (`launchctl kickstart -k gui/$(id -u)/com.clawdmeter.tray`), and confirm the daemon log shows `WiFi provisioning sent (...)`. Confirm the firmware serial log shows `BLE: WiFi provisioning received`.

- [ ] **Step 3: Verify direct WiFi fetch**

In the firmware serial monitor, confirm no `sprint_net: HTTP <non-200>` errors and that the Sprint screen shows the current sprint. Then **quit the tray / turn off BLE** (or move out of range) and confirm the Sprint screen keeps updating after ~5 min (other screens go stale — expected).

- [ ] **Step 4: Verify fallback & C6 non-regression**

- Unprovision (blank the config lines) + reflash-less: with stale/no WiFi, confirm the BLE `bd` still paints Sprint while the daemon runs.
- Build+flash a C6 board (or at least build) and confirm Sprint still shows via BLE with no WiFi code.

- [ ] **Step 5: Final docs update**

Update `CLAUDE.md` (daemon/firmware sections) to document the WiFi Sprint path, `BOARD_HAS_WIFI`, the PROV char `...05`, and the `/api/device/sprint` contract. Commit.

---

## Self-Review

**Spec coverage:**
- Hybrid transport (BLE local / WiFi Sprint) → Tasks 1.1, 3.3, 3.5. ✓
- `BOARD_HAS_WIFI` S3-only compiler flag → Task 3.1, gating in 3.3/3.5, C6 build check 3.5.3. ✓
- Provisioning BLE→NVS one-time → Tasks 2.2 (daemon), 3.4 (PROV char), 3.3 (NVS). ✓
- Slim endpoint + DEVICE_TOKEN → Task 1.1. ✓
- Fallback WiFi-prefers/BLE-fallback → Task 3.5.2 (freshness gate). ✓
- Root CA TLS → Tasks 3.2, 3.3 (`setCACert`). ✓
- 5-min cadence / 15-min freshness → Task 3.3 constants. ✓
- Testing (curl, pytest, build, Mac-off, fallback, C6) → Tasks 1.1.4, 2.1, 3.5.3, Phase 4. ✓

**Placeholder scan:** The ISRG Root X1 PEM (Task 3.2) and the exact env-var names in Task 1.1 are fetched at implementation time from named canonical sources — concrete, not open-ended. No "TBD"/"handle errors appropriately" left.

**Type consistency:** `sprint_net_get(UsageData*)`/`sprint_net_provision(const char*,const char*,const char*)`/`sprint_net_init`/`sprint_net_tick` names match between `sprint_net.h` (Task 3.3.1), the stubs (3.3.2), the BLE caller (3.4.2), and main.cpp (3.5). The `bd`/blob/`{sn,td,dg,dn,tt,bi,ba}` JSON shapes match across endpoint (1.1), daemon contract, and firmware parse (3.3.2). PROV UUID `...05` identical in daemon (2.1) and firmware (3.4.1).
