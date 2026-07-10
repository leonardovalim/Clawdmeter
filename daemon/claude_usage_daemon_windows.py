#!/usr/bin/env python3
"""Claude Usage Tracker Daemon — Windows (Phase 2).

Reads the Claude OAuth token from the native-Windows credentials path and
polls the Anthropic API for rate-limit utilization data. BLE glue added in
later plans.
"""

import asyncio
import calendar
import datetime
import json
import logging
import logging.handlers
import os
import re
import signal
import subprocess
import sys
import threading
import time
import unicodedata
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

def _to_display_ascii(s: str) -> str:
    """Flatten accented/Unicode text to the plain ASCII the on-device bitmap
    fonts actually ship. Those fonts were generated ASCII-only, so 'ã', 'ç',
    'é'… render as missing-glyph boxes. NFKD splits each letter from its
    combining accent; we drop the accents (and any other non-ASCII leftover)
    so "Paixão" -> "Paixao" instead of "Paix[]o"."""
    if not s:
        return s
    s = s.replace("–", "-").replace("—", "-")  # en/em dash -> hyphen
    s = unicodedata.normalize("NFKD", s)
    return s.encode("ascii", "ignore").decode("ascii")


def _sanitize_display_strings(obj):
    """Recursively apply _to_display_ascii to every string in a payload."""
    if isinstance(obj, str):
        return _to_display_ascii(obj)
    if isinstance(obj, dict):
        return {k: _sanitize_display_strings(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_sanitize_display_strings(v) for v in obj]
    return obj


DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
CONNECT_RETRIES = 3        # D-01: attempts before giving up on a device
CONNECT_RETRY_DELAY = 2.0  # D-01: seconds between failed connect attempts
ZOMBIE_BREAK_LIMIT = 1     # D-03: consecutive write failures before abandoning a half-open link
                           # N=1: breaks at T=60s, leaves ~60s headroom for reconnect+poll inside 120s SLA
                           # N=2 would bust the 120s budget before reconnect even begins
RECONNECT_BACKOFF_CAP = 8  # D-05: fast-reconnect cap (seconds); keeps stacked retries inside 120s SLA
                           # ~5–10s band per CONTEXT.md Claude's Discretion; 8 chosen as middle ground

# Optional reset chime.
# Optional clock display. 
# Config lives under the same Clawdmeter dir as daemon.log.
CONFIG_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter" / "config"

ASANA_MCP_URL    = "https://asana-dash.vercel.app/api/mcp"
ASANA_POLL_SECS  = 300          # poll sprint data every 5 min (changes slowly)
_asana_cache:     dict | None = None
_asana_last_poll: float       = 0.0

# OAuth token refresh. The credentials file Claude Code writes carries a
# refreshToken next to the accessToken, so an expired token can be renewed with
# a silent HTTP call — no browser, no `claude login`. Endpoint and client id are
# the ones the Claude Code CLI itself uses.
OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
# Renew this long before expiry so a poll never races the expiry boundary.
TOKEN_REFRESH_MARGIN_S = 300

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "system": [
        {
            "type": "text",
            "text": "Usage monitor probe.",
            "cache_control": {"type": "ephemeral"},
        }
    ],
    "messages": [{"role": "user", "content": "hi"}],
}

HEATMAP_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter" / "heatmap.json"


def _build_file_logger() -> logging.Logger | None:
    """Create a rotating file logger for field diagnostics, or None.

    Autostart launches the tray under pythonw.exe, which has no console — stdout
    is discarded (and is in fact None, making print() unsafe). A rotating file is
    then the ONLY trail when the daemon stalls in the field. Windows-only: on the
    Linux dev box / CI the console print() suffices, and gating to win32 keeps the
    pure-helper unit tests from writing stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("clawdmeter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import (tray imports this module)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    path = base / "Clawdmeter" / "daemon.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort — logging setup must never stop the daemon
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


_FILE_LOGGER = _build_file_logger()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # Under pythonw sys.stdout is None and print() would raise — guard it so a
    # missing console can never crash the daemon thread (the silent-freeze mode).
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass
    if _FILE_LOGGER is not None:
        _FILE_LOGGER.info(msg)


class AuthError(Exception):
    """Raised by poll_api on a genuine 401/403 — the token really is expired or
    invalid and the user must re-run `claude login`. Distinct from a None return,
    which means a TRANSIENT failure (network/DNS, timeout, rate-limit, 5xx) that
    must NOT be mislabeled as a token problem (SC#5: a boot-time `getaddrinfo
    failed` DNS blip wrongly fired the 'token expired' toast)."""

def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" so the device stays silent until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def write_chime_setting(value: str) -> None:
    """Persist the `chime` option (on/off) to CONFIG_FILE.

    Rewrites the existing `chime = ...` line in place if present, appends it
    otherwise. All other lines (comments, `clock`, etc.) are preserved
    verbatim so the tray toggle never clobbers settings edited by hand. The
    daemon re-reads CONFIG_FILE every poll, so the change takes effect within
    ~60s with no restart (see read_chime_setting).
    """
    value = "on" if value == "on" else "off"
    lines = CONFIG_FILE.read_text().splitlines() if CONFIG_FILE.exists() else []

    for i, line in enumerate(lines):
        key = line.split("#", 1)[0].split("=", 1)[0].strip().lower()
        if key == "chime":
            lines[i] = f"chime = {value}"
            break
    else:
        lines.append(f"chime = {value}")

    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text("\n".join(lines) + "\n")


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" so existing setups keep showing "Usage" until opted in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection on Windows via the registry. Returns 12 or 24."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\International") as k:
            # iTime: "1" = 24-hour, "0" = 12-hour.
            val, _ = winreg.QueryValueEx(k, "iTime")
            return 24 if str(val).strip() == "1" else 12
    except (ImportError, OSError):
        return 24


def add_clock_fields(payload: dict) -> None:
    """Add "t" (local wall-clock epoch) + "tf" (12|24) when the config opts in."""
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


def _cache_hit_from_resp(resp) -> int:
    """Return cache hit % (0-100) from the probe response body, or -1 if unavailable."""
    try:
        u = resp.json().get("usage", {})
        cache_read   = int(u.get("cache_read_input_tokens",   0) or 0)
        cache_create = int(u.get("cache_creation_input_tokens", 0) or 0)
        inp          = int(u.get("input_tokens", 0) or 0)
        total = cache_read + cache_create + inp
        return round(cache_read * 100 / total) if total > 0 else -1
    except Exception:
        return -1


def _load_heatmap() -> list[int]:
    today = datetime.date.today().isoformat()
    try:
        raw = json.loads(HEATMAP_FILE.read_text(encoding="utf-8"))
        h = raw.get("hourly", [])
        if raw.get("date") == today and isinstance(h, list) and len(h) == 24:
            return list(h)
    except Exception:
        pass
    return [0] * 24


def _update_heatmap(session_pct: int) -> list[int]:
    """Track peak session utilization per hour; returns the 24-entry array."""
    hourly = _load_heatmap()
    hour = datetime.datetime.now().hour
    hourly[hour] = max(hourly[hour], max(0, min(100, session_pct)))
    try:
        HEATMAP_FILE.parent.mkdir(parents=True, exist_ok=True)
        HEATMAP_FILE.write_text(
            json.dumps({"date": datetime.date.today().isoformat(), "hourly": hourly}),
            encoding="utf-8",
        )
    except Exception:
        pass
    return hourly


# Some players (notably web players) report Paused to GSMTC while the track is
# audibly advancing. Remember the last observed (track, position, wall-clock) so
# _get_media_info can override "paused" when the position is moving.
_media_probe: dict | None = None

# Media properties of the session _get_media_info most recently chose — kept so
# the album-art fetch reads the thumbnail of the same track it reported.
_media_props = None


async def _current_media_session():
    """Pick the GSMTC session the user actually means.

    get_current_session() is Windows' idea of "current", which is often a paused
    browser tab while the real player is elsewhere — scan all sessions and
    prefer the one actually playing.
    MediaPlaybackStatus: Closed=0, Changing=1, Stopped=2, Playing=3, Paused=4
    """
    if sys.platform != "win32":
        return None
    # winsdk is deprecated and has no wheels for Python >= 3.14; its
    # successor is the per-namespace winrt-* packages (same API surface).
    try:
        from winsdk.windows.media.control import (  # type: ignore[import]
            GlobalSystemMediaTransportControlsSessionManager as GSM,
        )
    except ImportError:
        from winrt.windows.media.control import (  # type: ignore[import]
            GlobalSystemMediaTransportControlsSessionManager as GSM,
        )
    mgr = await GSM.request_async()
    try:
        for s in mgr.get_sessions():
            pb = s.get_playback_info()
            if pb is not None and int(getattr(pb, "playback_status", 0) or 0) == 3:
                return s
    except Exception:
        pass
    return mgr.get_current_session()


# Device -> host media commands, notified on REQ_CHAR (see ble_send_media_cmd).
MEDIA_CMD_PLAYPAUSE = 0x10
MEDIA_CMD_NEXT      = 0x11
MEDIA_CMD_PREV      = 0x12


async def _media_control(code: int) -> bool:
    """Apply a transport command from the device to the active media session."""
    try:
        session = await _current_media_session()
        if not session:
            log("Media command ignored: no active session")
            return False
        if code == MEDIA_CMD_PLAYPAUSE:
            await session.try_toggle_play_pause_async()
        elif code == MEDIA_CMD_NEXT:
            await session.try_skip_next_async()
        elif code == MEDIA_CMD_PREV:
            await session.try_skip_previous_async()
        else:
            return False
        return True
    except Exception as e:
        log(f"Media command 0x{code:02x} failed: {e}")
        return False


async def _get_media_info() -> dict | None:
    """Return {t, a, p[, ps, d]} from Windows GlobalSystemMediaTransportControls, or None."""
    global _media_probe, _media_props
    if sys.platform != "win32":
        return None
    _media_props = None
    try:
        session = await _current_media_session()
        if not session:
            return None
        props = await session.try_get_media_properties_async()
        if not props:
            return None
        _media_props = props
        pb = session.get_playback_info()
        playing = pb is not None and int(getattr(pb, "playback_status", 0) or 0) == 3
        info = {
            "t": (props.title  or "")[:47],
            "a": (props.artist or "")[:31],
            "p": 1 if playing else 0,
        }
        # Track position/duration, when the player reports a timeline. Sent as
        # whole seconds; the firmware interpolates between updates.
        try:
            tl = session.get_timeline_properties()
            dur = int(tl.end_time.total_seconds()) if tl and tl.end_time else 0
            pos = int(tl.position.total_seconds()) if tl and tl.position else 0
            if dur > 0:
                info["ps"] = max(0, min(pos, dur))
                info["d"] = dur
        except Exception:
            pass

        # Paused-but-advancing override: if the same track's position moved
        # forward roughly in step with wall-clock time, it is playing no matter
        # what playback_status claims (some players never leave "Paused").
        now = time.time()
        if info["p"] == 0 and "ps" in info and _media_probe is not None:
            same_track = (_media_probe["t"], _media_probe["a"]) == (info["t"], info["a"])
            elapsed = now - _media_probe["ts"]
            advanced = info["ps"] - _media_probe["ps"]
            if same_track and 0 < elapsed <= 90 and 0 < advanced <= elapsed + 3:
                info["p"] = 1
        if "ps" in info:
            _media_probe = {"t": info["t"], "a": info["a"], "ps": info["ps"], "ts": now}
        else:
            _media_probe = None
        return info
    except ImportError:
        return None
    except Exception:
        return None


# --- Album art --------------------------------------------------------------
# The firmware shows a 96x96 cover on the media screen (PSRAM boards only; the
# others ignore the frames). We read the GSMTC thumbnail, center-crop + resize
# with Pillow, convert to RGB565 little-endian, and stream it over the RX
# characteristic in binary frames: b"CA" + chunk_idx + flags(bit0=last) + data.
# idx 0xFF is "no art for this track" (clears the cover on-device).
ART_W = ART_H = 96


async def _fetch_album_art_rgb565() -> bytes | None:
    props = _media_props
    if props is None or getattr(props, "thumbnail", None) is None:
        return None
    try:
        from winrt.windows.storage.streams import Buffer, DataReader, InputStreamOptions

        stream = await props.thumbnail.open_read_async()
        size = int(stream.size)
        if size <= 0 or size > 4 * 1024 * 1024:
            return None
        buf = Buffer(size)
        await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
        try:
            data = bytes(buf)  # pywinrt IBuffer supports the buffer protocol...
        except TypeError:      # ...but fall back to DataReader if this build doesn't
            reader = DataReader.from_buffer(buf)
            data = bytes(reader.read_bytes(buf.length))

        import io

        from PIL import Image

        img = Image.open(io.BytesIO(data)).convert("RGB")
        w, h = img.size
        side = min(w, h)
        img = img.crop((
            (w - side) // 2, (h - side) // 2,
            (w + side) // 2, (h + side) // 2,
        )).resize((ART_W, ART_H), Image.LANCZOS)
        px = img.tobytes()
        out = bytearray(ART_W * ART_H * 2)
        j = 0
        for i in range(0, len(px), 3):
            v = ((px[i] >> 3) << 11) | ((px[i + 1] >> 2) << 5) | (px[i + 2] >> 3)
            out[j] = v & 0xFF
            out[j + 1] = (v >> 8) & 0xFF
            j += 2
        return bytes(out)
    except Exception as e:
        log(f"Album art fetch failed: {e}")
        return None


async def _send_album_art(client, art: bytes | None) -> bool:
    """Stream a cover (or a clear frame for None) to the device. True on success."""
    try:
        if art is None:
            await client.write_gatt_char(RX_CHAR_UUID, b"CA\xff\x00", response=True)
            return True
        # Chunk to the negotiated MTU (ATT header 3 + our frame header 4). The
        # floor of 96 keeps chunk_idx comfortably inside one byte.
        mtu = int(getattr(client, "mtu_size", 0) or 0) or 185
        chunk = max(96, min(mtu - 7, 480))
        total = (len(art) + chunk - 1) // chunk
        for idx in range(total):
            part = art[idx * chunk:(idx + 1) * chunk]
            flags = 1 if idx == total - 1 else 0
            await client.write_gatt_char(
                RX_CHAR_UUID, bytes((0x43, 0x41, idx, flags)) + part, response=True
            )
        return True
    except (BleakError, OSError, AssertionError) as e:
        log(f"Album art send failed: {e}")
        return False


def _read_asana_token() -> str | None:
    """Read asana_token from the Clawdmeter config file, or from env ASANA_TOKEN."""
    env = os.environ.get("ASANA_TOKEN", "").strip()
    if env:
        return env
    try:
        for line in CONFIG_FILE.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            if key.strip().lower() == "asana_token":
                v = val.strip()
                return v if v else None
    except Exception:
        pass
    return None


def _clamp_series(vals, cap: int) -> list[int]:
    """Turn a burndown array into small non-negative ints; None/negatives -> -1."""
    out = []
    for v in (vals or [])[:cap]:
        if v is None:
            out.append(-1)
        else:
            try:
                out.append(max(-1, min(255, int(round(float(v))))))
            except (TypeError, ValueError):
                out.append(-1)
    return out


async def _fetch_sprint(asana_token: str) -> dict | None:
    """Call the Asana dashboard get_sprint REST tool and return the burndown, or None.

    Returns {sn, td, dg, dn, tt, bi, ba}: sprint name, todo/doing/done counts, and
    the ideal (bi) + actual (ba) burndown series. Future actual days come back as
    -1 so the firmware can draw them as gaps.
    """
    headers = {"Authorization": f"Bearer {asana_token}"}
    params = {"tool": "get_sprint", "escopo": "Time"}
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(ASANA_MCP_URL, headers=headers, params=params)
        if resp.status_code != 200:
            log(f"Asana HTTP {resp.status_code}: {resp.text[:120]}")
            return None
        sc = resp.json()
        td = int(sc.get("todo", 0) or 0)
        dg = int(sc.get("doing", 0) or 0)
        dn = int(sc.get("done", 0) or 0)
        bi = _clamp_series(sc.get("burndown_ideal"), 14)
        ba = _clamp_series(sc.get("burndown_actual"), 14)
        out = {"sn": str(sc.get("sprint", ""))[:15], "td": td, "dg": dg,
               "dn": dn, "tt": td + dg + dn}
        if bi:
            out["bi"] = bi
            out["ba"] = ba
        return out
    except Exception as e:
        log(f"Asana poll error: {e}")
    return None


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        # Network/DNS/timeout — transient. Return None (no toast), retry next tick.
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        # Genuine auth rejection — the ONLY case that warrants the actionable
        # "run claude login" toast.
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        raise AuthError(resp.status_code)
    if resp.status_code >= 400:
        # Other 4xx/5xx (rate-limit, server error) — transient, not a token issue.
        # 429 while rate-limited still carries valid utilization headers —
        # extract them so the device shows 100% instead of going idle.
        if resp.status_code == 429 and (
            resp.headers.get("anthropic-ratelimit-unified-5h-utilization")
            or resp.headers.get("anthropic-ratelimit-unified-overage-utilization")
        ):
            log(f"API HTTP 429 (rate-limited) — extracting headers anyway")
            # fall through to the header-parsing code below
        else:
            log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
            return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in

    payload["ch"] = _cache_hit_from_resp(resp)
    payload["hm"] = _update_heatmap(payload.get("s", 0))

    mi = await _get_media_info()
    if mi is not None:
        payload["mi"] = mi

    # Burndown (Asana sprint) — polled every ASANA_POLL_SECS, cached between polls.
    global _asana_cache, _asana_last_poll
    now_ts = time.time()
    if now_ts - _asana_last_poll >= ASANA_POLL_SECS:
        asana_tok = _read_asana_token()
        if asana_tok:
            bd = await _fetch_sprint(asana_tok)
            if bd is not None:
                _asana_cache = bd
        _asana_last_poll = now_ts
    if _asana_cache is not None:
        payload["bd"] = _asana_cache

    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Monthly window is assumed (headers expose only reset_ts, not period). Per the
    Claude Enterprise Admin API reference, spend-limit period's "only value today
    is monthly" — see the macOS daemon for the full note.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30, "rd": ""}
    if period_end <= 0:
        # The "ent" branch is taken whenever the 5h utilization header is absent
        # — which also happens on an otherwise-fine 200 that simply carries no
        # rate-limit headers. reset_ts is then the "0" default, and stepping one
        # month back from the epoch lands in 1969; datetime.timestamp() raises
        # OSError for pre-1970 dates on Windows, taking the whole poll loop down.
        return {"tp": 0, "pd": 30, "rd": ""}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30, "rd": ""}
    pct_val = (now - period_start) / period_len * 100
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": int(round(period_len / 86400)),
        "rd": f"{dt_end.strftime('%b')} {dt_end.day}",
    }


def _mac_from_pnp_instance_id(instance_id: str) -> str | None:
    """Recover a canonical BLE MAC ("AA:BB:CC:DD:EE:FF") from a PnP instance id.

    Windows encodes a paired BLE device's address in its PnP instance id as a
    12-hex run after a ``DEV_`` token, e.g.::

        BTHLE\\DEV_98A316A5D706\\7&B8081D1&0&98A316A5D706  ->  98:A3:16:A5:D7:06

    Returns None when no ``DEV_<12 hex>`` token is present. Pure — the
    subprocess that produces the instance id lives in discover_bonded_address().
    """
    m = re.search(r"DEV_([0-9A-Fa-f]{12})(?![0-9A-Fa-f])", instance_id)
    if not m:
        return None
    h = m.group(1).upper()
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def discover_bonded_address() -> str | None:
    """Return the BLE address of the bonded Clawdmeter, or None.

    A device that is paired AND connected to Windows stops advertising, so
    BleakScanner can't see it (the steady state once paired — see
    README-windows.md). WinRT can still connect to it directly by address, so
    we recover that address from the OS:

    1. CLAWDMETER_BLE_ADDRESS env override (skips discovery — testing / pinning).
    2. Windows PnP table, filtered to the device's FriendlyName.

    Non-Windows or any failure returns None.
    """
    if override := os.environ.get("CLAWDMETER_BLE_ADDRESS"):
        return override.strip().upper()
    if sys.platform != "win32":
        return None
    command = (
        "Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | "
        f"Where-Object {{ $_.FriendlyName -eq '{DEVICE_NAME}' }} | "
        "Select-Object -ExpandProperty InstanceId"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True,
            text=True,
            timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError) as e:
        log(f"Bonded-address lookup failed: {e}")
        return None
    for line in result.stdout.splitlines():
        if mac := _mac_from_pnp_instance_id(line):
            return mac
    return None


async def acquire_target():
    """Return a connectable handle for the Clawdmeter, or None.

    Targets only the device bonded to THIS machine (via the PnP table /
    CLAWDMETER_BLE_ADDRESS) — it never scans for a nearby device by name, so it
    can't grab a stranger's or the wrong nearby unit. The device must be paired
    with Windows once first (the documented setup). Returns a BLEDevice or None.
    """
    address = discover_bonded_address()
    if not address:
        return None
    log(f"Not advertising; connecting to bonded address {address}")
    # CRITICAL: hand BleakClient a BLEDevice, not the bare address string. WinRT's
    # connect() resolves a bare string via an advertisement scan (find_device_by_address)
    # — which always fails for a bonded device that has stopped advertising, the very
    # case we are handling. A BLEDevice sets _device_info directly, so WinRT connects
    # via from_bluetooth_address_with_bluetooth_address_type_async and skips the scan.
    return BLEDevice(address, DEVICE_NAME, None)


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        # Set after a transport command lands, so the poll loop wakes at once
        # and pushes the new track/state instead of waiting out the 5s TICK.
        self.media_dirty = asyncio.Event()
        self._loop = asyncio.get_running_loop()

    def _on_refresh(self, _char, data: bytearray) -> None:
        # REQ_CHAR carries a one-byte opcode: 0x01 = refresh, 0x1x = transport.
        code = data[0] if data else 0x01
        if code == 0x01:
            log("Refresh requested by device")
            self.refresh_requested.set()
            return
        if code not in (MEDIA_CMD_PLAYPAUSE, MEDIA_CMD_NEXT, MEDIA_CMD_PREV):
            log(f"Unknown device opcode 0x{code:02x}")
            return
        name = {MEDIA_CMD_PLAYPAUSE: "play/pause",
                MEDIA_CMD_NEXT: "next",
                MEDIA_CMD_PREV: "prev"}[code]
        log(f"Media command from device: {name}")
        # Bleak may invoke this off the loop thread; hop back on before spawning.
        self._loop.call_soon_threadsafe(
            lambda: asyncio.ensure_future(self._run_media(code))
        )

    async def _run_media(self, code: int) -> None:
        if await _media_control(code):
            # Give the player a beat to apply it, then let the loop re-read.
            await asyncio.sleep(0.35)
            self.media_dirty.set()

    async def setup_refresh_subscription(self) -> None:
        # The refresh subscription is optional — the 60s poll loop works without it.
        # WinRT's start_notify() CCCD write can raise a raw OSError/WinError (not
        # wrapped as BleakError) when the peer GATT server is transiently unavailable,
        # e.g. a just-power-cycled ESP32 whose server is not yet ready (G-03-01, SC#3).
        # Degrade gracefully instead of crashing the daemon so it stays single-process
        # across a power-cycle reconnect (SC#4, no restart).
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError, OSError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        payload = _sanitize_display_strings(payload)
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=True)
            return True
        except (BleakError, OSError) as e:
            # WinRT can raise a raw OSError/WinError (NOT wrapped as BleakError)
            # when the peer GATT server goes transiently unavailable mid-write —
            # the same failure class setup_refresh_subscription() guards against.
            # Returning False trips the zombie-link break -> clean reconnect,
            # rather than an uncaught exception killing the daemon thread (the
            # silent-freeze failure mode, SC#2 field report).
            log(f"Write failed: {e}")
            return False


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _windows_credential_candidates() -> list[Path]:
    """Return the ordered list of credential file paths to probe (first hit wins).

    Priority:
    1. CLAUDE_CREDENTIALS_PATH env override (D-03, project-specific)
    2. CLAUDE_CONFIG_DIR env override (official Claude override)
    3. D-02 candidate list: home/.claude, LOCALAPPDATA/Claude, APPDATA/Claude
    """
    # Priority 1: project-specific env override (D-03)
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override)]
    # Priority 2: official CLAUDE_CONFIG_DIR env override
    if config_dir := os.environ.get("CLAUDE_CONFIG_DIR"):
        return [Path(config_dir) / ".credentials.json"]
    # Priority 3: D-02 candidate list — first hit wins
    home = Path.home()
    local_appdata = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    return [
        home / ".claude" / ".credentials.json",          # primary (confirmed by docs)
        local_appdata / "Claude" / ".credentials.json",  # fallback 2
        appdata / "Claude" / ".credentials.json",        # fallback 3
    ]


def read_token() -> str | None:
    """Read the Claude OAuth access token from the first available credential file."""
    for path in _windows_credential_candidates():
        try:
            return _extract_access_token(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return None


def _load_credentials() -> tuple[Path, dict] | None:
    """Return (path, parsed doc) for the first credentials file carrying a
    `claudeAiOauth` block. Only that shape can be refreshed — a raw-token blob
    (see _extract_access_token) has no refresh token, so refresh is skipped.
    """
    for path in _windows_credential_candidates():
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(doc, dict) and isinstance(doc.get("claudeAiOauth"), dict):
            return path, doc
    return None


def _write_credentials(path: Path, doc: dict) -> None:
    """Persist the credentials doc atomically.

    Writes a sibling temp file and os.replace()s it over the original, so a
    crash mid-write can never leave a truncated credentials file. The temp file
    lives in the same directory, so it inherits that directory's ACL — which is
    exactly where the real file's permissions come from (all of its ACEs are
    inherited), so the swap does not loosen access.
    """
    tmp = path.with_name(path.name + ".clawdmeter.tmp")
    tmp.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def token_needs_refresh() -> bool:
    """True when the stored access token is expired or about to expire."""
    loaded = _load_credentials()
    if not loaded:
        return False
    expires_ms = loaded[1]["claudeAiOauth"].get("expiresAt")
    if not isinstance(expires_ms, (int, float)):
        return False
    return time.time() + TOKEN_REFRESH_MARGIN_S >= expires_ms / 1000


async def refresh_access_token() -> str | None:
    """Trade the stored refresh token for a fresh access token, persisting both.

    Returns the new access token, or None if the refresh could not be completed
    (no refreshable credentials, network/HTTP failure, or an account switch
    detected mid-flight).

    Account-switch safety: `claude login` rewrites the whole claudeAiOauth block,
    so a refresh token cached in memory can belong to an account the user has
    since left. We therefore (a) read the refresh token straight off disk here,
    never from memory, and (b) re-read the file immediately before writing and
    abort unless the refresh token is still the one we exchanged. Without that
    compare-and-swap a slow refresh could clobber the newly-logged-in account's
    credentials with the previous account's.
    """
    loaded = _load_credentials()
    if not loaded:
        return None
    _, doc = loaded
    oauth = doc["claudeAiOauth"]
    old_refresh = oauth.get("refreshToken")
    if not isinstance(old_refresh, str) or not old_refresh:
        log("Token refresh: credentials carry no refreshToken")
        return None

    body = {
        "grant_type": "refresh_token",
        "refresh_token": old_refresh,
        "client_id": OAUTH_CLIENT_ID,
    }
    # Ask for exactly the scopes this account already holds rather than a
    # hardcoded list — a work account and a personal account can differ.
    scopes = oauth.get("scopes")
    if isinstance(scopes, list) and scopes:
        body["scope"] = " ".join(scopes)

    try:
        async with httpx.AsyncClient(timeout=30.0) as http:
            resp = await http.post(OAUTH_TOKEN_URL, json=body)
    except httpx.HTTPError as e:
        log(f"Token refresh failed (network): {e}")
        return None
    if resp.status_code != 200:
        # A rejected refresh token is the one case that truly needs `claude login`.
        log(f"Token refresh rejected: HTTP {resp.status_code}")
        return None
    try:
        data = resp.json()
    except ValueError:
        log("Token refresh: malformed response body")
        return None

    new_access = data.get("access_token")
    if not isinstance(new_access, str) or not new_access:
        log("Token refresh: response carried no access_token")
        return None
    # Refresh tokens are usually rotated; fall back to the old one if not.
    new_refresh = data.get("refresh_token") or old_refresh
    expires_in = data.get("expires_in")

    latest = _load_credentials()
    if not latest:
        return new_access  # cannot persist, but the token is good for this poll
    latest_path, latest_doc = latest
    if latest_doc["claudeAiOauth"].get("refreshToken") != old_refresh:
        # Someone ran `claude login` while we were in flight. Persisting now
        # would overwrite the new account. Drop ours; next cycle reads theirs.
        log("Token refresh: credentials changed on disk (account switch?) — discarding")
        return None

    block = latest_doc["claudeAiOauth"]
    block["accessToken"] = new_access
    block["refreshToken"] = new_refresh
    if isinstance(expires_in, (int, float)):
        block["expiresAt"] = int((time.time() + expires_in) * 1000)
    try:
        _write_credentials(latest_path, latest_doc)
    except OSError as e:
        log(f"Token refresh: could not persist credentials: {e}")
        return new_access  # still usable in memory for this cycle
    log(f"Token refreshed silently; expires {_read_expiry()}")
    return new_access


def _read_expiry() -> str:
    """Return human-readable expiry from the first-hit credentials file.

    Reads claudeAiOauth.expiresAt (epoch milliseconds — JS convention).
    Divides by 1000 before passing to fromtimestamp (Python expects seconds).
    Returns 'expiry unknown' on any parse failure.
    """
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            data = json.loads(raw)
            oauth = data.get("claudeAiOauth", {})
            expires_ms = oauth.get("expiresAt")
            if expires_ms is None:
                return "expiry unknown"
            # CRITICAL: expiresAt is JS-convention epoch milliseconds; divide by 1000
            # before fromtimestamp (Python expects seconds). Raw value -> year ~57000.
            dt = datetime.datetime.fromtimestamp(
                expires_ms / 1000, tz=datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%d %H:%M UTC")
        except (TypeError, ValueError, OSError, AttributeError, json.JSONDecodeError):
            return "expiry unknown"
    return "expiry unknown"


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any of `events` is set, or after `timeout` seconds.

    Lets the poll loop's TICK wait wake immediately on a stop signal (clean,
    responsive Quit) without losing the refresh-request wakeup — instead of
    waiting only on refresh_requested and re-checking stop_event up to TICK
    later. Cancels and drains the loser tasks so they don't warn.
    """
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def connect_and_run(device, stop_event: asyncio.Event, tray_state=None) -> bool:
    """Connect to device and poll until disconnected or stopped.

    Returns True if at least one successful write occurred.

    `device` is a BLEDevice — either from an advertisement scan or built from the
    bonded address by acquire_target(). The getattr keeps the log line robust if a
    bare address string is ever passed in.
    """
    log(f"Connecting to {getattr(device, 'address', device)}...")
    # D-01: retry wrapper — defeats WinRT post-wake failure modes
    # (Could not get GATT services: Unreachable, stale is_connected).
    # Rebuild a fresh BleakClient each attempt (locked D-05 recipe).
    client = None
    for attempt in range(CONNECT_RETRIES):
        # D-05: pass BLEDevice (not address string), address_type="random" (NimBLE
        # static-random), use_cached_services=False (DIY firmware — WinRT GATT cache
        # may be stale after firmware reflash).
        client = BleakClient(
            device,
            address_type="random",
            use_cached_services=False,
        )
        try:
            await client.connect()
        # AssertionError: bleak's WinRT backend has an internal race in
        # get_descriptors_async that surfaces as a bare assert (killed the
        # daemon on 2026-07-07). OSError: WinRT can also raise raw WinError.
        except (BleakError, asyncio.TimeoutError, AssertionError, OSError) as e:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed: {e}")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        if not client.is_connected:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed (not connected)")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        # Connected successfully
        break
    else:
        log(f"Connection failed after {CONNECT_RETRIES} attempts")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0  # D-03: poll immediately on first connect
    used_successfully = False
    consecutive_failures = 0  # D-03: zombie-link break counter
    cached_payload: dict | None = None  # last usage payload, for media-only refreshes
    art_sent_key = ()  # (title, artist) whose art was pushed; () forces a first push
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                # Renew before expiry so a poll never races the boundary. On an
                # account switch refresh_access_token() bails out, and the
                # read_token() below picks up whoever is logged in now.
                if token_needs_refresh():
                    await refresh_access_token()
                token = read_token()  # D-09: fresh each cycle
                if not token:
                    log("No token; skipping poll")
                    if tray_state:
                        tray_state.set_error("token expired — run claude login")
                else:
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        # Real 401/403. Before nagging the user, try the silent
                        # refresh once — this is the ordinary expiry path and
                        # needs no browser.
                        refreshed = await refresh_access_token()
                        payload = None
                        if refreshed:
                            try:
                                payload = await poll_api(refreshed)
                            except AuthError:
                                refreshed = None
                        if not refreshed:
                            # Refresh token itself is dead — only now is a
                            # human-driven `claude login` actually required.
                            if tray_state:
                                tray_state.set_error("token expired — run claude login")
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True
                            consecutive_failures = 0  # D-03: reset on success
                            cached_payload = payload
                            if tray_state:
                                tray_state.set_connected(time.time())
                        else:
                            consecutive_failures += 1
                            if consecutive_failures >= ZOMBIE_BREAK_LIMIT:
                                log(
                                    f"Zombie link detected ({consecutive_failures} consecutive"
                                    f" write failures); abandoning connection"
                                )
                                break
                    # else: payload is None from a TRANSIENT failure (network/DNS,
                    # timeout, rate-limit, 5xx). poll_api already logged it; do NOT
                    # toast "token expired" — that mislabeled a boot-time DNS blip
                    # as an auth problem (SC#5). Leave tray state unchanged; the next
                    # tick retries and set_connected() recovers it.

            # Media moves faster than the 60s usage poll (track changes,
            # play/pause, position). Between polls, re-read the media info every
            # TICK and resend the cached payload whenever it changed — the
            # firmware replaces its whole state per message, so the resend must
            # carry the full usage payload, not just "mi".
            elif cached_payload is not None:
                session.media_dirty.clear()
                mi = await _get_media_info()
                if mi != cached_payload.get("mi"):
                    if mi is None:
                        cached_payload.pop("mi", None)
                    else:
                        cached_payload["mi"] = mi
                    if not await session.write_payload(cached_payload):
                        consecutive_failures += 1
                        if consecutive_failures >= ZOMBIE_BREAK_LIMIT:
                            log(
                                f"Zombie link detected ({consecutive_failures} consecutive"
                                f" write failures); abandoning connection"
                            )
                            break

            # Album art follows the track: whenever the (title, artist) pair
            # changes, push the new cover (or a clear frame). On failure the key
            # is left unchanged so the next tick retries.
            if cached_payload is not None:
                mi_now = cached_payload.get("mi")
                key = (mi_now.get("t"), mi_now.get("a")) if mi_now else None
                if key != art_sent_key:
                    art = await _fetch_album_art_rgb565() if key is not None else None
                    if await _send_album_art(client, art):
                        art_sent_key = key
                        log(f"Album art {'sent' if art else 'cleared'} for {key}")

            # Wake on a refresh request OR a stop, whichever comes first. Waking
            # promptly on stop_event is what lets the finally below run
            # client.disconnect() before the process exits, so the peer gets a
            # clean GATT disconnect (returns to its waiting screen) instead of
            # being left frozen on stale data after Quit (SC#3 graceful shutdown).
            await _wait_first(session.refresh_requested, session.media_dirty,
                              stop_event, timeout=TICK)
    finally:
        # Clean GATT disconnect on the way out — this is what tells the peripheral
        # the link is gone. WinRT can surface a raw OSError (not BleakError) here,
        # so swallow both; the link tears down regardless once we exit.
        try:
            await client.disconnect()
        except (BleakError, OSError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


def _next_backoff(current: int, cap: int) -> int:
    """D-05: double current backoff value, clamped to cap.

    Pure helper — unit-testable without driving the main loop.
    Used by both slow-search (cap=60) and fast-reconnect (cap=RECONNECT_BACKOFF_CAP) regimes.
    """
    return min(current * 2, cap)


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    # Populate the shared state object so the tray can route Quit through
    # loop.call_soon_threadsafe (RESEARCH Pitfall 2).  Additive — the existing
    # stop_event = asyncio.Event() line above is unchanged.
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # OS signal handlers can only be installed from the main thread, and
    # loop.add_signal_handler is unsupported on Windows. When running under the
    # tray (04-03) the loop lives in a background thread and the tray owns clean
    # shutdown via stop_event (loop.call_soon_threadsafe), so skip silently there.
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                # Windows: add_signal_handler not supported; fall back to signal.signal
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    # Not the main thread of the main interpreter — tray owns shutdown.
                    pass

    log("=== Claude Usage Tracker Daemon (BLE, Windows) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # D-05: two distinct backoff regimes — slow-search (device absent) vs fast-reconnect (link dropped)
    search_backoff = 1     # caps at 60s — gentle, for a device that is genuinely absent/off
    reconnect_backoff = 1  # caps at RECONNECT_BACKOFF_CAP — fast, to clear the 120s SLA after a drop
    while not stop_event.is_set():
        device = await acquire_target()
        if not device:
            # Slow-search regime: device was not found by scan — back off gently
            if tray_state:
                tray_state.set_scanning()
            log(f"Device not found, retrying in {search_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=search_backoff)
            except asyncio.TimeoutError:
                pass
            search_backoff = _next_backoff(search_backoff, 60)
            continue

        # Catch-all: a single flaky BLE exception must never kill the daemon —
        # treat any unexpected error as a failed session and re-enter the
        # reconnect regime.
        try:
            ok = await connect_and_run(device, stop_event, tray_state)
        except Exception as e:
            log(f"Unexpected error in session ({type(e).__name__}): {e}")
            ok = False
        if not ok:
            # Fast-reconnect regime: had/attempted a link that dropped — retry quickly
            if tray_state:
                tray_state.set_scanning()
            log(f"Connection lost, reconnecting in {reconnect_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=reconnect_backoff)
            except asyncio.TimeoutError:
                pass
            reconnect_backoff = _next_backoff(reconnect_backoff, RECONNECT_BACKOFF_CAP)
        else:
            # Successful session — reset reconnect counter to floor; search_backoff also reset
            reconnect_backoff = 1
            search_backoff = 1


if __name__ == "__main__":
    if sys.platform != "win32":
        print(
            "Warning: running under Linux/WSL — WinRT BLE will not be available.",
            file=sys.stderr,
        )
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
