"""Login-autostart toggle for Clawdmeter on macOS — the LaunchAgent analogue of
autostart_windows.py's HKCU\\Run entry.

Manages a per-user LaunchAgent plist at
  ~/Library/LaunchAgents/com.clawdmeter.tray.plist
that launches the tray app at login via launchd. Per-user (no sudo, no
system-wide daemon) — the macOS equivalent of Windows' per-user HKCU\\Run.

Public API mirrors autostart_windows.py exactly so tray_macos.py can call the
same three functions:
  enable(tray_script=None)  -- write the plist and load it into launchd
  disable()                 -- unload and remove the plist; idempotent
  is_enabled()              -- True if the plist file is present
"""

import os
import subprocess
import sys
import time
from pathlib import Path

_LABEL = "com.clawdmeter.tray"
_PLIST_PATH = Path.home() / "Library" / "LaunchAgents" / f"{_LABEL}.plist"


def log(msg: str) -> None:
    """Log in the daemon [HH:MM:SS] style."""
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _plist_xml(tray_script: str) -> str:
    """Build the LaunchAgent plist XML.

    Uses the CURRENT interpreter (`sys.executable`) — inside the venv this is
    the venv's python, which already sees the daemon's deps, so unlike Windows
    (whose venv pythonw redirector pops a console) there's no reason to reach
    for the base interpreter here.

    RunAtLoad=true starts it at login; KeepAlive is intentionally omitted so a
    user Quit from the tray menu stays quit (launchd won't respawn it).

    StandardOutPath/StandardErrorPath capture the daemon's [HH:MM:SS] log and
    any startup traceback. Without them a login-time failure is invisible
    (launchd discards a GUI agent's stdio), so "the tray just isn't there" has
    no evidence trail. Logs land in ~/Library/Logs/ — the conventional per-user
    spot, readable in Console.app.
    """
    python = sys.executable
    script = os.path.abspath(tray_script)
    log_dir = Path.home() / "Library" / "Logs"
    out_log = log_dir / "Clawdmeter-tray.out.log"
    err_log = log_dir / "Clawdmeter-tray.err.log"
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n'
        "<dict>\n"
        "    <key>Label</key>\n"
        f"    <string>{_LABEL}</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        f"        <string>{python}</string>\n"
        f"        <string>{script}</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>ProcessType</key>\n"
        "    <string>Interactive</string>\n"
        "    <key>StandardOutPath</key>\n"
        f"    <string>{out_log}</string>\n"
        "    <key>StandardErrorPath</key>\n"
        f"    <string>{err_log}</string>\n"
        "</dict>\n"
        "</plist>\n"
    )


def enable(tray_script: str | None = None) -> None:
    """Write the LaunchAgent plist and load it into launchd.

    Args:
        tray_script: absolute path to the tray entry script. Defaults to this
                     module's own path, but callers should pass tray_macos.py.
    """
    script = tray_script if tray_script is not None else __file__
    _PLIST_PATH.parent.mkdir(parents=True, exist_ok=True)
    _PLIST_PATH.write_text(_plist_xml(script))
    # Reload so the change takes effect this session, not just next login.
    # Best-effort: unload first (ignore "not loaded"), then load.
    subprocess.run(
        ["launchctl", "unload", str(_PLIST_PATH)],
        capture_output=True,
        check=False,
    )
    subprocess.run(
        ["launchctl", "load", str(_PLIST_PATH)],
        capture_output=True,
        check=False,
    )
    log(f"Autostart enabled: {_PLIST_PATH}")


def disable() -> None:
    """Unload the agent and remove the plist. Idempotent when already absent."""
    if _PLIST_PATH.exists():
        subprocess.run(
            ["launchctl", "unload", str(_PLIST_PATH)],
            capture_output=True,
            check=False,
        )
        _PLIST_PATH.unlink(missing_ok=True)
        log("Autostart disabled")


def is_enabled() -> bool:
    """Return True if the LaunchAgent plist is present.

    File-existence check (not `launchctl list`) so it matches the enable/disable
    lifecycle and stays cheap enough for the tray's per-frame live query.
    """
    return _PLIST_PATH.exists()
