#!/usr/bin/env python3
"""Cross-platform tray helpers shared by the Windows and macOS tray apps.

Holds the two pieces that are pure and identical on both platforms:

  TrayState   — thread-safe scalar bridge (daemon loop writes, tray reads)
  header_text — pure helper producing the status-header string

Everything platform-specific (icons, autostart, the pystray backend, the
single-instance guard) lives in tray_windows.py / tray_macos.py.
"""

import time


class TrayState:
    """Shared state object bridging the daemon asyncio loop to the tray.

    The daemon loop writes state via the set_* methods; the tray reads the
    scalar attributes. No lock is needed — writes are atomic attribute
    assignments of simple Python scalars, and the tray only ever reads them.

    The loop populates `loop` and `stop_event` at startup (inside the daemon's
    main()) so the tray's Quit handler can route through
    loop.call_soon_threadsafe (asyncio.Event is NOT thread-safe).
    """

    def __init__(self) -> None:
        self.state: str = "scanning"          # "connected" | "scanning" | "error"
        self.reason: str = ""                 # error reason string
        self.last_sync: float | None = None   # time.time() of last successful write

        # Populated by the daemon main() at startup:
        self.loop = None        # asyncio running loop (for call_soon_threadsafe)
        self.stop_event = None  # asyncio.Event (the existing clean-shutdown hook)

    def set_connected(self, ts: float) -> None:
        """Called after write_payload returns True. ts = time.time()."""
        self.state = "connected"
        self.reason = ""
        self.last_sync = ts

    def set_scanning(self) -> None:
        """Called in scan/reconnect branches. BLE churn stays Scanning."""
        self.state = "scanning"
        self.reason = ""

    def set_error(self, why: str) -> None:
        """Called on token-expired / API auth failure (Error = actionable only)."""
        self.state = "error"
        self.reason = why


def header_text(ts: TrayState) -> str:
    """Return the menu status-header string for the current TrayState.

    Shapes:
      "Connected · last update HH:MM"  (ts.last_sync is a float)
      "Connected · last update never"  (ts.last_sync is None)
      "Scanning…"
      "Error: {reason}"
    """
    if ts.state == "connected":
        if ts.last_sync is not None:
            when = time.strftime("%H:%M", time.localtime(ts.last_sync))
        else:
            when = "never"
        return f"Connected · last update {when}"
    if ts.state == "scanning":
        return "Scanning…"
    return f"Error: {ts.reason}"
