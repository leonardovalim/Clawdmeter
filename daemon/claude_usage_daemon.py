#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unicodedata
from pathlib import Path

import httpx
from bleak import BleakClient
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
PROV_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"

POLL_INTERVAL = 60
TICK = 5
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

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

# --- Extra screens (media / stats / burndown) ---
HEATMAP_FILE = Path.home() / ".config" / "claude-usage-monitor" / "heatmap.json"

# One JSON-lines record per poll — the raw dataset for the usage report
# (tools/usage_report.py). Append-only; each line is self-describing so new
# payload fields get captured automatically.
USAGE_LOG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "usage_log.jsonl"

ASANA_MCP_URL   = "https://asana-dash.vercel.app/api/mcp"
ASANA_POLL_SECS = 300           # poll sprint data every 5 min (changes slowly)
_asana_cache:     dict | None = None
_asana_last_poll: float       = 0.0


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


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
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_cached_address(addr: str) -> None:
    """Persist the resolved peripheral address/UUID for proactive reconnect.

    On macOS this is the CoreBluetooth UUID, which lets a later run ask the OS
    to reconnect the bonded device by identifier (see request_reconnect_macos)
    instead of waiting for the user to nudge it.
    """
    try:
        if load_cached_address() == addr:
            return
        SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
        SAVED_ADDR_FILE.write_text(addr + "\n")
        log(f"Cached peripheral address {addr}")
    except OSError as e:
        log(f"Could not cache address: {e}")


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    # 3. Not currently held by the OS — but if we cached this device's UUID on a
    #    prior connect, retrieve the bonded peripheral by identifier and hand it
    #    to BleakClient, which connects it directly over CoreBluetooth (no scan,
    #    no advertising needed). This is what lets the daemon reconnect on its
    #    own each session instead of waiting for a button press / manual Connect.
    addr = load_cached_address()
    if addr and not (skip_addr and addr == skip_addr):
        from Foundation import NSUUID

        uuid = NSUUID.alloc().initWithUUIDString_(addr)
        if uuid is not None:
            try:
                known = list(cm.retrievePeripheralsWithIdentifiers_([uuid]) or [])
            except Exception as e:
                log(f"retrievePeripheralsWithIdentifiers failed: {e}")
                known = []
            if known:
                log(f"Bonded peripheral known by UUID [{addr}]; connecting directly")
                return _wrap(known[0])

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held and no bonded UUID cached; waiting "
                "(connect once to seed the cache)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
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
    otherwise. All other lines (comments, `clock`, etc.) are preserved verbatim
    so the tray toggle never clobbers hand-edited settings. The daemon re-reads
    CONFIG_FILE every poll, so the change takes effect within ~60s with no
    restart (see read_chime_setting). Mirrors the Windows daemon's writer so the
    macOS tray's "Play chime on reset" checkbox behaves identically.
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

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
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
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
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
        cache_read   = int(u.get("cache_read_input_tokens",     0) or 0)
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


# AppleScript that returns "title\nartist\nstate" for Spotify (preferred) or
# Music, or "" when nothing is playing. Avoids the private MediaRemote framework
# (locked down in macOS 15.4) — the two scriptable players cover the ask.
# Emits 6 lines: app, title, artist, state, position(s), duration(s).
# A playing app wins over a merely-loaded one, mirroring the Windows daemon's
# "prefer the session that is actually playing" rule. Spotify reports the track
# duration in milliseconds, Music in seconds — normalized to seconds here.
_MEDIA_APPLESCRIPT = r'''
set out to ""
set lf to linefeed
tell application "System Events"
    set spotOn to (exists (processes where name is "Spotify"))
    set musicOn to (exists (processes where name is "Music"))
end tell

-- Pass 1: whichever app is actually playing wins.
if spotOn then
    try
        tell application "Spotify"
            if player state is playing then
                set out to "Spotify" & lf & (name of current track) & lf & ¬
                    (artist of current track) & lf & "playing" & lf & ¬
                    ((player position) as text) & lf & ¬
                    (((duration of current track) / 1000) as text)
            end if
        end tell
    end try
end if
if out is "" and musicOn then
    try
        tell application "Music"
            if player state is playing then
                set out to "Music" & lf & (name of current track) & lf & ¬
                    (artist of current track) & lf & "playing" & lf & ¬
                    ((player position) as text) & lf & ¬
                    ((duration of current track) as text)
            end if
        end tell
    end try
end if

-- Pass 2: nothing playing — fall back to a loaded-but-paused track.
if out is "" and spotOn then
    try
        tell application "Spotify"
            if player state is not stopped then
                set out to "Spotify" & lf & (name of current track) & lf & ¬
                    (artist of current track) & lf & (player state as text) & lf & ¬
                    ((player position) as text) & lf & ¬
                    (((duration of current track) / 1000) as text)
            end if
        end tell
    end try
end if
if out is "" and musicOn then
    try
        tell application "Music"
            if player state is not stopped then
                set out to "Music" & lf & (name of current track) & lf & ¬
                    (artist of current track) & lf & (player state as text) & lf & ¬
                    ((player position) as text) & lf & ¬
                    ((duration of current track) as text)
            end if
        end tell
    end try
end if
return out
'''

# Which app _get_media_info last read from — album art and transport commands
# must target that same app.
_media_app: str | None = None


async def _osascript(script: str, timeout: float = 5.0):
    try:
        return await asyncio.to_thread(
            subprocess.run,
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return None


def _parse_locale_float(s: str) -> float:
    """Parse a number AppleScript formatted per the system locale.

    `(player position) as text` etc. honour the locale, so on a pt-BR Mac they
    come back as "134,26" / "1.234,56" (comma decimal, dot thousands) rather
    than the "134.26" Python's float() expects. Normalize both conventions so
    position/duration aren't silently dropped by the ValueError handler.
    """
    s = s.strip()
    if "," in s and "." in s:
        s = s.replace(".", "").replace(",", ".")  # "1.234,56" -> "1234.56"
    else:
        s = s.replace(",", ".")                    # "134,26"   -> "134.26"
    return float(s)


async def _get_media_info() -> dict | None:
    """Return {t, a, p[, ps, d]} for the current Spotify/Music track, or None."""
    global _media_app
    if sys.platform != "darwin":
        return None
    _media_app = None
    proc = await _osascript(_MEDIA_APPLESCRIPT)
    if proc is None:
        return None
    parts = proc.stdout.strip().split("\n")
    if len(parts) < 4 or not parts[1].strip():
        return None
    app, title, artist, state = parts[0].strip(), parts[1], parts[2], parts[3].strip().lower()
    _media_app = app
    info = {
        "t": title[:47],
        "a": artist[:31],
        "p": 1 if state == "playing" else 0,
    }
    # Position/duration, when the player reported a timeline. Sent as whole
    # seconds; the firmware interpolates between updates.
    if len(parts) >= 6:
        try:
            pos = int(_parse_locale_float(parts[4]))
            dur = int(_parse_locale_float(parts[5]))
            if dur > 0:
                info["ps"] = max(0, pos)
                info["d"] = dur
        except (TypeError, ValueError):
            pass
    return info


# Device -> host media commands, notified on REQ_CHAR (see ble_send_media_cmd).
MEDIA_CMD_PLAYPAUSE = 0x10
MEDIA_CMD_NEXT      = 0x11
MEDIA_CMD_PREV      = 0x12

_MEDIA_CMD_SCRIPT = {
    MEDIA_CMD_PLAYPAUSE: "playpause",
    MEDIA_CMD_NEXT:      "next track",
    MEDIA_CMD_PREV:      "previous track",
}


async def _media_control(code: int) -> bool:
    """Apply a transport command from the device to the active player."""
    app = _media_app
    if not app:
        log("Media command ignored: no active player")
        return False
    verb = _MEDIA_CMD_SCRIPT.get(code)
    if not verb:
        return False
    proc = await _osascript(f'tell application "{app}" to {verb}')
    if proc is None or proc.returncode != 0:
        err = proc.stderr.strip() if proc else "osascript failed"
        log(f"Media command 0x{code:02x} failed: {err}")
        return False
    return True


# ---- Album art (SCREEN_MEDIA cover) ----
# The firmware expects a fixed 96x96 RGB565 image, streamed in "CA"-tagged
# chunks over the RX characteristic. See ble_take_album_art() in ble.cpp.
ART_W = ART_H = 96

_MUSIC_ART_SCRIPT = r'''
on run argv
    set outPath to item 1 of argv
    tell application "Music"
        set d to raw data of artwork 1 of current track
    end tell
    set f to open for access (POSIX file outPath) with write permission
    set eof f to 0
    write d to f
    close access f
end run
'''


def _rgb565_from_bytes(data: bytes) -> bytes | None:
    """Center-crop, resize to ART_W x ART_H, and pack as little-endian RGB565."""
    import io as _io

    from PIL import Image

    img = Image.open(_io.BytesIO(data)).convert("RGB")
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


async def _fetch_album_art_rgb565() -> bytes | None:
    """Grab the current track's cover from whichever player _get_media_info chose.

    Spotify exposes an artwork *URL* (fetch it); Music only exposes raw bytes
    through AppleScript, so we have it dump them to a temp file.
    """
    app = _media_app
    if not app:
        return None
    try:
        if app == "Spotify":
            proc = await _osascript(
                'tell application "Spotify" to return artwork url of current track'
            )
            url = proc.stdout.strip() if proc else ""
            if not url.startswith("http"):
                return None
            async with httpx.AsyncClient(timeout=10.0) as http:
                resp = await http.get(url)
            if resp.status_code != 200:
                return None
            data = resp.content
        else:
            with tempfile.NamedTemporaryFile(suffix=".art", delete=False) as tmp:
                tmp_path = tmp.name
            try:
                proc = await asyncio.to_thread(
                    subprocess.run,
                    ["osascript", "-e", _MUSIC_ART_SCRIPT, tmp_path],
                    capture_output=True, text=True, timeout=8,
                )
                if proc.returncode != 0:
                    return None
                data = Path(tmp_path).read_bytes()
            finally:
                Path(tmp_path).unlink(missing_ok=True)
        if not data:
            return None
        return _rgb565_from_bytes(data)
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
    """Read asana_token from the config file, or from the ASANA_TOKEN env var."""
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
                vals[slot] = val.strip()
    except Exception:
        pass
    if vals["ssid"] and vals["pw"] and vals["tok"]:
        return {"ssid": vals["ssid"], "pw": vals["pw"], "tok": vals["tok"]}
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


def _log_usage(payload: dict) -> None:
    """Append one JSON-lines record per poll — the raw dataset for the usage
    report. Best-effort: a logging failure must never disturb the poll loop.
    Skips transient/bulky fields (media, chime/clock flags); keeps everything
    that describes usage over time. Mirrors the Windows daemon."""
    try:
        rec = {
            "ts": round(time.time(), 1),
            "iso": datetime.datetime.now().isoformat(timespec="seconds"),
        }
        for k in ("s", "sr", "w", "wr", "st", "acct", "ch", "tp", "pd", "rd"):
            if k in payload:
                rec[k] = payload[k]
        bd = payload.get("bd")
        if isinstance(bd, dict):
            for k in ("sn", "td", "dg", "dn", "tt"):
                if k in bd:
                    rec["bd_" + k] = bd[k]
        USAGE_LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        with USAGE_LOG_FILE.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception as e:
        log(f"usage log write failed: {e}")


async def _enrich_extra_screens(payload: dict) -> None:
    """Add the media/heatmap/burndown fields to the active plan's payload.

    Runs once per cycle on the chosen payload (not per config dir), so the
    heatmap peak and the Asana poll aren't double-counted across plans. `ch`
    (cache hit) is set per-call in poll_api since it's read from that call's
    own response body.
    """
    payload["hm"] = _update_heatmap(int(payload.get("s", 0) or 0))

    mi = await _get_media_info()
    if mi is not None:
        payload["mi"] = mi

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

    _log_usage(payload)   # payload completo (s/w/ch + hm/mi/bd) → log do relatório


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
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
        # Anthropic's utilization header can read slightly over 1.0 once a
        # window is exhausted (observed 1.05) — clamp so the device shows a
        # clean "100%" instead of a confusing "105%".
        try:
            return max(0, min(100, int(round(float(util) * 100))))
        except ValueError:
            return 0

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
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
    payload["ch"] = _cache_hit_from_resp(resp)   # cache hit % from this call's body
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # Reached whenever the 5h utilization header is absent and reset_ts is
        # the "0" default: there is no billing period to report. Stepping a month
        # back from the epoch lands in 1969, which is merely nonsense here but
        # raises OSError on the Windows daemon — guard both the same way.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """Poll every configured config dir and return the active plan's payload.

    Returns None when no dir yields a usable payload this cycle. A single
    configured dir (the default) collapses to exactly the old single-poll path.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        payload = await poll_api(token)
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    active_payload = payloads[active]
    await _enrich_extra_screens(active_payload)   # media / heatmap / burndown
    return active_payload


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
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale or missing bond as either:
    - CBErrorDomain Code=14 "Peer removed pairing information" (ESP32 lost its
      bond keys — e.g. after a firmware reflash or a bond-clear gesture).
    - CBErrorDomain Code=15 "Failed to encrypt the connection..." (macOS holds
      stale keys that the ESP32 no longer accepts).

    Both mean the daemon must unpair the macOS side so the next connect
    re-bonds cleanly. Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=14" in s or "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


BLUEUTIL_PATHS = [
    "/opt/homebrew/bin/blueutil",   # Apple Silicon Homebrew
    "/usr/local/bin/blueutil",      # Intel Homebrew / manual install
    "blueutil",                     # PATH lookup
]

def _find_blueutil() -> str | None:
    """Return the first working blueutil binary path, or None."""
    for p in BLUEUTIL_PATHS:
        if shutil.which(p):
            return p
    return None

def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    exe = _find_blueutil()
    if exe is None:
        log("blueutil not found; install with: brew install blueutil")
        return None
    try:
        return subprocess.run(
            [exe, *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not _find_blueutil():
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


def _any_token_available() -> bool:
    """True if at least one configured config dir currently has an OAuth token.

    Lets the tray distinguish a genuine "no token — run claude login" error from
    a transient API/network failure (which must NOT raise the actionable toast).
    """
    return any(read_token_for(d) for d in read_config_dirs())


async def connect_and_run(target, stop_event: asyncio.Event, tray_state=None) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            if unpair_macos():
                # The bond was removed, so the cached CoreBluetooth UUID
                # now points at a peripheral whose keys no longer match.
                # Drop the cache so the next cycle discovers the device
                # fresh and establishes a new bond.
                SAVED_ADDR_FILE.unlink(missing_ok=True)
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    # Cache the resolved address/UUID so a later run can ask the OS to
    # reconnect this bonded device by identifier instead of waiting passively.
    if not isinstance(target, str) and getattr(target, "address", None):
        save_cached_address(target.address)
    session = Session(client)
    await session.setup_refresh_subscription()

    creds = _read_wifi_creds()
    if creds is not None:
        try:
            blob = json.dumps(creds, separators=(",", ":")).encode()
            await client.write_gatt_char(PROV_CHAR_UUID, blob, response=False)
            log(f"WiFi provisioning sent (ssid={creds['ssid']})")
        except (BleakError, OSError, ValueError) as e:
            log(f"WiFi provisioning skipped: {e}")

    last_poll = 0.0
    used_successfully = False
    cached_payload: dict | None = None
    art_sent_key: tuple | None = None
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                payload = await poll_active_payload()
                if payload is None:
                    log("No usable config dir this cycle")
                    # Only surface the actionable "token expired" error when there
                    # is genuinely no token anywhere — a None payload can also mean
                    # a transient network/API failure, which must not toast.
                    if tray_state is not None and not _any_token_available():
                        tray_state.set_error("token expired — run claude login")
                    elif cached_payload is not None:
                        # The API poll failed this cycle for some reason we may not
                        # have a handler for yet (a 429 without utilization headers,
                        # a non-429 error at full credit/quota exhaustion, a network
                        # blip). Resend the last known-good payload as a keep-alive
                        # so the firmware's 90s freshness clock doesn't expire into
                        # "No data" while we're most likely still sitting at
                        # whatever % we last reported (e.g. 100%, exhausted).
                        if await session.write_payload(cached_payload) and tray_state is not None:
                            tray_state.set_connected(time.time())
                elif await session.write_payload(payload):
                    last_poll = time.time()
                    used_successfully = True
                    cached_payload = payload
                    if tray_state is not None:
                        tray_state.set_connected(time.time())

            # Media moves faster than the 60s usage poll (track changes,
            # play/pause, position). Between polls, re-read the media info every
            # TICK and resend the cached payload whenever it changed — the
            # firmware replaces its whole state per message, so the resend must
            # carry the full usage payload, not just "mi". Mirrors the Windows
            # daemon's media_dirty block.
            elif cached_payload is not None:
                session.media_dirty.clear()
                mi = await _get_media_info()
                if mi != cached_payload.get("mi"):
                    if mi is None:
                        cached_payload.pop("mi", None)
                    else:
                        cached_payload["mi"] = mi
                    if await session.write_payload(cached_payload) and tray_state is not None:
                        tray_state.set_connected(time.time())

            # Album art follows the track: whenever the (title, artist) pair
            # changes, push the new cover (or a clear frame). On failure the key
            # is left unchanged so the next tick retries.
            if cached_payload is not None:
                mi_now = cached_payload.get("mi")
                key = (mi_now.get("t"), mi_now.get("a")) if mi_now else None
                if key != art_sent_key:
                    art = await _fetch_album_art_rgb565() if key is not None else None
                    if await _send_album_art(session.client, art):
                        art_sent_key = key
                        log(f"Album art {'sent' if art else 'cleared'} for {key}")

            # Wake on a refresh request OR a just-applied transport command,
            # whichever comes first, so media state pushes promptly instead of
            # waiting out the full TICK.
            waiters = [
                asyncio.ensure_future(session.refresh_requested.wait()),
                asyncio.ensure_future(session.media_dirty.wait()),
            ]
            try:
                await asyncio.wait(
                    waiters, timeout=TICK, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for w in waiters:
                    w.cancel()
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    # Populate the shared state object so the tray can route Quit through
    # loop.call_soon_threadsafe (asyncio.Event is not thread-safe). Additive —
    # the stop_event/loop lines above are unchanged.
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # OS signal handlers can only be installed from the main thread. Under the
    # tray the loop runs in a background thread (pystray owns the main thread),
    # so skip signal setup there — the tray owns clean shutdown via stop_event.
    import threading
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            if tray_state is not None:
                tray_state.set_scanning()
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event, tray_state)
        if not ok:
            if tray_state is not None:
                tray_state.set_scanning()
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
