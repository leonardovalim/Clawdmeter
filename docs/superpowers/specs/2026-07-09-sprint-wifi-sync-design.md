# Sprint via WiFi — hybrid transport design

**Date:** 2026-07-09
**Status:** Approved (design), pending implementation plan
**Scope:** Add a WiFi/HTTPS data path for the Sprint (burndown) screen, keeping BLE
for the other three screens. First release targets ESP32-S3 boards only.

## Problem & motivation

Today every screen's data reaches the device over BLE from the local host daemon.
Three of the four data screens are inherently tied to the local machine:

| Screen | Data source | Location | WiFi-viable? |
|--------|-------------|----------|--------------|
| Usage | Anthropic API | needs the local Claude Code OAuth token (Mac keychain) | No |
| Stats | derived from usage (cache hits, session) | same as Usage | No |
| Now Playing | local Mac player (Spotify/Music via AppleScript) | inherently local | No |
| **Sprint / Burndown** | **DashboardAsana on Vercel** (hosted API + bearer token) | **internet** | **Yes** |

The Sprint screen is the only one whose source is already a hosted internet API, so
the ESP32 (which has WiFi) can fetch it directly.

**Primary drivers** (from brainstorming):
1. **Mac-independence** — Sprint stays live/fresh even when the Mac is off, asleep,
   or out of BLE range. The device becomes an ambient sprint dashboard.
   (Accepted caveat: with the Mac off, Usage/Stats/Now Playing still go stale —
   only Sprint survives.)
2. **Own refresh cadence** — Sprint syncs on its own timer, independent of the 60s
   BLE poll loop.
3. Exploration of the trade-offs (this doc).

## Architecture

Transport becomes **hybrid, driven by data origin**:

- **BLE (local daemon)** → Usage, Stats, Now Playing (local sources).
- **WiFi / HTTPS (device, autonomous)** → Sprint (source is already on the internet).

WiFi sits behind a new compile-time capability flag **`BOARD_HAS_WIFI`**, enabled
**only on the S3 boards** (`waveshare_amoled_216`, `waveshare_amoled_18`). C6 boards
and any board without the flag behave exactly as today, via the BLE fallback — no
regression. This follows the project's capability-flag pattern (no `#ifdef BOARD_*`
in shared code; see `docs/porting/capability-flags.md`).

```
                    ┌───────────────────────────┐
   Mac ON:  daemon ─┤ BLE: usage/stats/media/bd  ├─► ESP32 ─► screens
                    └───────────────────────────┘        ▲
   always:  DashboardAsana (Vercel) ── WiFi/HTTPS ───────┘  (Sprint)
            GET /api/device/sprint  {sn,td,dg,dn,tt,bi,ba}
```

### Why S3-only for v1

WiFi STA + NimBLE peripheral coexistence is well supported and comfortable on the
S3 (dual-core, has PSRAM). On the C6 it is tight: single-core RISC-V, **no PSRAM**,
and TLS buffers alone want ~40 KB+ of RAM on top of LVGL + NimBLE. Given the fallback
design makes C6 lose nothing, deferring C6 is the pragmatic YAGNI call.

## Components

### 1. WiFi provisioning (BLE → NVS, one-time)

- New BLE characteristic **PROV (`4c41555a-...0005`, write)** on the existing service.
- The daemon writes a provisioning blob containing `wifi_ssid`, `wifi_password`, and
  `device_token`.
- The firmware stores these in **NVS** and is autonomous thereafter — it reconnects
  WiFi on every boot without the Mac.
- Daemon source: three new config lines (`wifi_ssid`, `wifi_password`, `device_token`)
  read from the same config file as `asana_token`, pushed over PROV on connect when
  present and not already acknowledged by the device.

### 2. Vercel slim endpoint

- New `GET /api/device/sprint` on DashboardAsana.
- Auth: bearer **`DEVICE_TOKEN`** — a dedicated env var, separate from the team's
  `MCP_AUTH_TOKEN`, so the device never carries the team secret.
- Returns only the `bd` object: `{sn, td, dg, dn, tt, bi, ba}` (sprint name, todo,
  doing, done, total, burndown ideal series, burndown actual series). Tiny payload,
  trivial to parse on-device. Reuses the internal `get_sprint` logic.

### 3. Firmware: `sprint_net` module + source arbitration

- Shared module **`firmware/src/sprint_net.cpp`**, compiled only when `BOARD_HAS_WIFI`
  is set. Responsibilities: connect/maintain WiFi STA, HTTPS GET every **5 minutes**
  (matches the daemon's current `ASANA_POLL_SECS`), parse the slim JSON into the
  `bd_*` fields of `data.h`.
- **Source arbitration:** keep two sprint buffers (BLE-sourced and WiFi-sourced) with
  timestamps. The UI prefers the **WiFi buffer when fresh** (last successful fetch
  within ~15 min); otherwise it falls back to the BLE `bd`. The last good sprint is
  persisted in NVS so the screen paints immediately on boot, before the first fetch.

### 4. Networking, TLS & errors

- WiFi STA stays connected (desk device, USB-powered); reconnect with backoff on drop.
- TLS: embed the **Let's Encrypt root CA (ISRG Root X1)** to validate Vercel — more
  correct than `setInsecure()`, and the chain is stable for years.
- Failure handling (WiFi down, HTTP ≠ 200, invalid JSON): keep the last value and fall
  back to the BLE `bd` after the freshness threshold. Never blocks or crashes the UI.

## Data flow

1. On boot, firmware loads NVS: WiFi creds, device token, last-known sprint (painted
   immediately if present).
2. If `BOARD_HAS_WIFI` and creds exist → connect WiFi, start the 5-min fetch timer.
3. Each fetch: HTTPS GET `/api/device/sprint` with the device token → parse → update
   the WiFi sprint buffer + timestamp → persist to NVS.
4. BLE continues delivering usage/stats/media and (still) `bd` as the fallback source.
5. The Sprint screen renders from the arbiter: fresh WiFi wins, else BLE `bd`.

## Error handling summary

| Failure | Behavior |
|---------|----------|
| WiFi not provisioned | Sprint uses BLE `bd` (today's behavior). |
| WiFi connect fails | Retry with backoff; Sprint uses BLE `bd`. |
| HTTPS non-200 / timeout | Keep last WiFi value until stale, then BLE `bd`. |
| JSON parse error | Ignore response, keep last value. |
| Mac off (no BLE) | Sprint stays fresh via WiFi; other 3 screens go stale. |

## Testing strategy

- **Endpoint:** `curl` `/api/device/sprint` with the device token → confirm slim JSON.
- **Mac-independence:** stop the daemon / disable BLE and confirm Sprint keeps updating
  over WiFi (screenshot on S3 via the `screenshot` serial command).
- **Persistence:** reboot the device with no daemon → it connects WiFi on its own (NVS).
- **Fallback:** unprovision or set wrong WiFi → confirm BLE `bd` repaints the screen.
- **No regression on C6:** build a C6 env, confirm `BOARD_HAS_WIFI` is off and Sprint
  still shows via BLE `bd`.

## Chosen defaults (approved)

1. Fetch cadence: **5 minutes**.
2. TLS: **embedded root CA** (not `setInsecure()`).
3. Provisioning UX: **config-file lines** in the daemon (not a tray dialog) for v1.
4. Device auth: **separate `DEVICE_TOKEN`**, not the team `MCP_AUTH_TOKEN`.

## Out of scope (v1)

- C6 boards (WiFi+BLE coexistence / no-PSRAM RAM budget) — deferred.
- Moving Usage/Stats/Now Playing off BLE (their sources are local; would require a
  cloud relay + hosting the OAuth token — much larger architecture).
- Tray UI for provisioning (config-file lines suffice for v1).
- Cert pinning beyond the bundled root CA.

## Repos touched

- **Clawdmeter** — firmware (`sprint_net.cpp`, `BOARD_HAS_WIFI`, PROV characteristic,
  NVS, arbiter) + daemon (push provisioning blob over PROV).
- **DashboardAsana** — new `/api/device/sprint` endpoint + `DEVICE_TOKEN` env.
