#!/usr/bin/env python3
"""macOS menu-bar (NSStatusItem) entry and state bridge for Clawdmeter.

The macOS counterpart of tray_windows.py. Same shape, same shared helpers:

  TrayState   — thread-safe scalar bridge (re-exported from tray_common)
  header_text — pure D-05 status-header helper (re-exported from tray_common)
  main()      — tray entry: builds per-state icons, runs the daemon loop in a
                bg thread, and runs pystray.Icon on the main thread

The daemon loop (claude_usage_daemon.main) is UNCHANGED in logic; the tray only
reads the additive TrayState set_* calls the daemon already makes.

pystray uses the macOS NSStatusItem backend here (Cocoa), so this module must
run on the process main thread — same constraint as Windows. Menu items,
autostart, and the chime toggle mirror the Windows tray one-for-one.

Usage::

    python tray_macos.py

Run: python -m pytest daemon/tests/test_windows_tray.py -x -q  (shared helpers)
"""

import os
import sys
import threading
import time

# Repo root = the directory that CONTAINS the `daemon` package (this file is
# <repo>/daemon/tray_macos.py). Resolve it from __file__ so the package imports
# below and the brand-logo asset load work no matter what the current working
# directory is — critical for the LaunchAgent, which starts with cwd = "/".
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

# The LaunchAgent launches us with the venv's python (sys.executable at enable
# time), which already sees the venv's site-packages — unlike Windows, whose
# base-interpreter autostart needs a manual site.addsitedir. Still add the venv
# defensively for the case where the agent was written by a different
# interpreter than the one now importing us. No-op if the dir is absent.
_VENV_SITE = os.path.join(_REPO_ROOT, ".venv", "lib")
if os.path.isdir(_VENV_SITE):
    import glob
    import site
    # macOS venvs nest under lib/pythonX.Y/site-packages.
    for _sp in glob.glob(os.path.join(_VENV_SITE, "python*", "site-packages")):
        site.addsitedir(_sp)

# ---------------------------------------------------------------------------
# TrayState + header_text — pure, cross-platform; shared with tray_windows.py.
# Re-exported here so daemon.tray_macos.TrayState / header_text also resolve.
# ---------------------------------------------------------------------------

from daemon.tray_common import TrayState, header_text  # noqa: E402,F401


# ---------------------------------------------------------------------------
# single-instance guard (POSIX advisory file lock — no stale-lock problem)
# ---------------------------------------------------------------------------

# A flock on this file is held for the process lifetime; the kernel drops it
# automatically when the process dies, so there is no stale-lock cleanup (unlike
# a pidfile). This catches the same duplicate-launch collision the Windows
# named-mutex guards against: the LaunchAgent firing at login while a manually
# started tray is already running would otherwise have two trays fighting over
# the one BLE peripheral.
_SINGLETON_LOCK_PATH = os.path.join(
    os.path.expanduser("~"), "Library", "Caches", "Clawdmeter-tray.lock"
)


def _acquire_single_instance():
    """Acquire the process-wide single-instance lock.

    Returns a truthy handle to keep alive for the process lifetime if this is
    the first/only tray, or None if another Clawdmeter tray already owns the
    lock (the caller must then exit immediately, before touching BLE).

    Uses fcntl.flock(LOCK_EX | LOCK_NB) on a lock file kept open for the process
    lifetime. We never close the file handle — the OS releases the lock at
    process exit, which is precisely the lock lifetime we want.

    Off-macOS (Linux dev box / unit tests) this is still a POSIX flock and works
    the same; only the tray backend is macOS-specific. Fails OPEN on any
    unexpected OS error so a filesystem quirk never blocks tray startup —
    single-instance is best-effort hardening.
    """
    try:
        import fcntl
    except ImportError:
        return object()  # no fcntl (e.g. Windows) — no-op sentinel

    try:
        os.makedirs(os.path.dirname(_SINGLETON_LOCK_PATH), exist_ok=True)
        # Keep the fd open for the whole process — closing it drops the lock.
        fd = open(_SINGLETON_LOCK_PATH, "w")
        try:
            fcntl.flock(fd.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fd.close()
            return None  # another instance already holds it
        return fd
    except OSError:
        # Any other filesystem error: fail open so the tray always starts.
        return object()


# ---------------------------------------------------------------------------
# main() — tray entry (pystray on main thread, daemon loop in bg thread)
# ---------------------------------------------------------------------------

def main() -> None:
    """Tray entry point: build icons, start daemon bg thread, run pystray.

    `import pystray` is intentionally INSIDE this function (not at module top)
    so the module can be imported on a headless Linux dev box for unit tests of
    the pure helpers (TrayState, header_text) without pystray's Cocoa backend
    failing to load.
    """
    # Single-instance guard FIRST — before icons, the daemon thread, or any BLE
    # work. If another tray already owns the lock (e.g. the LaunchAgent fired
    # while a manual tray was running), exit silently.
    _instance_lock = _acquire_single_instance()
    if _instance_lock is None:
        return

    import asyncio as _asyncio
    import pystray
    from pystray import Menu, MenuItem

    import daemon.autostart_macos as autostart
    from daemon.claude_usage_daemon import (
        main as daemon_main,
        log as daemon_log,
        read_chime_setting,
        write_chime_setting,
    )
    from daemon.icon_assets import load_logo_rgba, build_state_icons
    from daemon.version import DAEMON_VERSION

    # Build per-state icons once at startup; swap icon.icon per tick (never recomposite).
    base = load_logo_rgba(os.path.join(_REPO_ROOT, "firmware", "src", "logo.h"))
    images = build_state_icons(base)

    ts = TrayState()
    icon = pystray.Icon("Clawdmeter", images["scanning"], "Clawdmeter")

    # --- background thread: asyncio loop ---
    def _run_daemon() -> None:
        # daemon=True thread: an unhandled exception here would vanish silently
        # and freeze the tray on its last state forever. Surface it instead —
        # log the traceback and flip the tray to an actionable error state.
        try:
            _asyncio.run(daemon_main(tray_state=ts))
        except Exception as e:  # last-resort thread guard
            import traceback
            daemon_log(f"Daemon thread crashed: {e!r}")
            daemon_log(traceback.format_exc())
            ts.set_error(f"daemon crashed: {type(e).__name__}")

    daemon_thread = threading.Thread(target=_run_daemon, daemon=True)
    daemon_thread.start()

    # --- menu ---
    def _on_quit(icon_ref, _item) -> None:
        # NEVER call ts.stop_event.set() directly from the tray thread;
        # asyncio.Event is NOT thread-safe. Route through the daemon loop, then
        # WAIT for the daemon thread to finish its graceful shutdown (the loop's
        # finally: client.disconnect()) before stopping the icon — without the
        # join the daemon=True thread is killed mid-flight and the peer never
        # gets a clean GATT disconnect. The timeout caps the block so Quit can
        # never hang if a disconnect wedges; we exit anyway as a fallback.
        if ts.loop is not None and ts.stop_event is not None:
            ts.loop.call_soon_threadsafe(ts.stop_event.set)
            daemon_thread.join(timeout=6.0)
        icon_ref.stop()

    def _on_toggle(_icon_ref, _item) -> None:
        if autostart.is_enabled():
            autostart.disable()
        else:
            # Pass THIS file explicitly so the LaunchAgent points at the tray
            # entry point, not at autostart_macos.py (which starts nothing).
            autostart.enable(tray_script=os.path.abspath(__file__))
        icon.update_menu()

    def _on_toggle_chime(_icon_ref, _item) -> None:
        write_chime_setting("off" if read_chime_setting() == "on" else "on")
        icon.update_menu()

    version_text = f"Version: {DAEMON_VERSION}"

    icon.menu = Menu(
        # Non-clickable status header; text updates via update_menu() on state change.
        MenuItem(lambda _item: header_text(ts), None, enabled=False),
        # Non-clickable version line — confirms which checkout is running.
        MenuItem(version_text, None, enabled=False),
        # Start-at-login toggle: checked= is a CALLABLE for live query.
        MenuItem("Start at login", _on_toggle, checked=lambda _item: autostart.is_enabled()),
        # Chime toggle: same live-query pattern. The daemon re-reads CONFIG_FILE
        # every poll (~60s), so this takes effect without restarting the daemon.
        MenuItem("Play chime on reset", _on_toggle_chime, checked=lambda _item: read_chime_setting() == "on"),
        MenuItem("Quit", _on_quit),
    )

    # --- setup callback (runs in pystray's setup thread, 1s poll) ---
    prev_state: dict = {"state": None, "last_sync": None}

    def _refresh(_icon: pystray.Icon) -> None:
        _icon.visible = True
        while _icon._running:  # type: ignore[attr-defined]
            current = ts.state
            last_sync = ts.last_sync
            state_changed = current != prev_state["state"]
            # Refresh the tooltip/menu when last_sync advances too — not only on
            # state change. A healthy "connected" daemon polling a flat usage
            # value never changes state, so a transition-only refresh would
            # freeze the "last update HH:MM" tooltip and read as a dead daemon.
            if state_changed or last_sync != prev_state["last_sync"]:
                if state_changed:
                    _icon.icon = images[current]  # icon image depends on state only
                _icon.title = header_text(ts)
                # Toast ONLY on transition INTO error, not on every error tick.
                if current == "error" and prev_state["state"] != "error":
                    _icon.notify(ts.reason or "Clawdmeter error", "Clawdmeter")
                prev_state["state"] = current
                prev_state["last_sync"] = last_sync
                _icon.update_menu()
            time.sleep(1.0)

    # Blocks the main thread until icon.stop() is called from _on_quit.
    icon.run(setup=_refresh)


if __name__ == "__main__":
    main()
