# macOS Setup and Run Guide (menu-bar tray)

This guide covers the **menu-bar tray** version of the Clawdmeter daemon on macOS —
the counterpart of `README-windows.md`. It is written so a Claude Code session on
the Mac can follow it end-to-end and install everything itself.

> **Not yet verified on macOS hardware.** The tray code (`tray_macos.py`,
> `autostart_macos.py`) is logic- and syntax-validated on a non-Mac dev box only.
> The two things most likely to need a real-hardware fix are called out in
> [Troubleshooting](#troubleshooting) — read them before assuming a bug is elsewhere.

There are **two** ways to run on macOS. Pick one; do not run both (they would fight
over the single BLE peripheral):

| Option | What runs | Autostart mechanism | Use when |
|--------|-----------|---------------------|----------|
| **A — Tray** (this guide) | `tray_macos.py` (menu-bar icon + daemon in a bg thread) | LaunchAgent `com.clawdmeter.tray` via the menu's "Start at login" | You want the status icon + toggles, like Windows |
| **B — Headless daemon** (`install-mac.sh`) | `claude_usage_daemon.py` directly, no UI | LaunchAgent `com.user.claude-usage-daemon` | You want a pure background service, no menu bar |

If you previously ran `install-mac.sh`, **unload its service before using the tray**
(see [Switching from the headless daemon](#switching-from-the-headless-daemon)).

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **Python 3.10+** | The daemon uses `X \| None` syntax. `brew install python` if the system `python3` is older. |
| **Claude Code signed in** | The token lives in the macOS Keychain (service `Claude Code-credentials`). Sign in via Claude Code first. |
| **Clawdmeter powered on** | Must be on and in range before the tray starts. |
| **Paired via macOS Bluetooth** | Pair once in **System Settings → Bluetooth** (bonded BLE HID keyboard). Once paired, macOS holds the connection and the daemon connects to that held peripheral — it never scans by name. |

---

## Install (one time)

Run these from the **repository root** (the folder that contains `daemon/`).

**1. Create a virtualenv at the repo root** (matches the tray's venv auto-discovery):

```bash
python3 -m venv .venv
```

**2. Install dependencies:**

```bash
.venv/bin/pip install --upgrade pip
.venv/bin/pip install -r daemon/requirements-macos.txt
```

This installs `bleak` (CoreBluetooth), `httpx`, `pystray` + `Pillow` (the icon), and
the `pyobjc-framework-Cocoa`/`Quartz` bridges that pystray's NSStatusItem backend needs.
Media "now playing" uses the built-in `osascript` against Spotify/Music — no extra dep.

---

## Run

```bash
.venv/bin/python daemon/tray_macos.py
```

- On **first run**, macOS shows a **Bluetooth permission prompt** — grant it. macOS only
  prompts foreground processes, so running the tray by hand once (as above) is what
  registers the permission. After that, login autostart works headlessly.
- A menu-bar icon appears: **amber = scanning**, **green = connected**, **red = error**.
- With a valid token and the device paired + in range, it should turn green and push the
  first usage payload within ~10 s.

### Tray menu (right-click / click the icon)

| Item | Behavior |
|------|----------|
| **Status header** (disabled) | Live status + last sync time, e.g. `Connected · last update 14:32`. |
| **Version** (disabled) | The daemon version tag, confirms which checkout is running. |
| **Start at login** (checkable) | Writes/removes `~/Library/LaunchAgents/com.clawdmeter.tray.plist` and (un)loads it via `launchctl`. Live-queried each time the menu opens. |
| **Play chime on reset** (checkable) | Toggles `chime = on/off` in `~/.config/claude-usage-monitor/config`. The daemon re-reads it every ~60 s — no restart. |
| **Quit** | Signals the daemon loop to stop, waits for a clean GATT disconnect (≤6 s), then exits. Does **not** drop the macOS Bluetooth pairing — the device keeps showing your last-synced usage. |

### Stopping

Use **Quit** in the menu, or `Ctrl+C` in the terminal.

---

## Switching from the headless daemon

If `install-mac.sh` was run before, its launchd service is still polling the device and
will collide with the tray. Unload it:

```bash
launchctl unload -w "$HOME/Library/LaunchAgents/com.user.claude-usage-daemon.plist"
```

To go back to headless-only later, unload the tray agent and reload that one:

```bash
launchctl unload -w "$HOME/Library/LaunchAgents/com.clawdmeter.tray.plist"
launchctl load  -w "$HOME/Library/LaunchAgents/com.user.claude-usage-daemon.plist"
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| **Icon stays amber; connects but no data ever reaches the device** | The one macOS-specific risk: `bleak`/CoreBluetooth runs in the daemon **background thread** while `pystray` owns the main-thread Cocoa run loop. If CoreBluetooth callbacks aren't being pumped, GATT writes stall. | First confirm it's not a pairing/token issue. If genuinely stuck here, the daemon-in-bg-thread + tray-on-main-thread split is the suspect — this is the part that couldn't be tested without a Mac. |
| No Bluetooth permission prompt / never connects | Tray was first launched by launchd (background), which macOS won't prompt | Run `.venv/bin/python daemon/tray_macos.py` by hand once and grant Bluetooth when asked |
| `Device not held by OS; waiting` | Not paired, powered off, or out of range | Pair once in System Settings → Bluetooth; power on; bring in range |
| Red icon: `Error: token expired — run claude login` | Keychain token expired | Re-run `claude login`; the daemon recovers on the next poll |
| Two icons / erratic reconnects | The headless `install-mac.sh` service is also running | Unload it (see [above](#switching-from-the-headless-daemon)) |
| `ModuleNotFoundError: pystray` (only under login autostart) | The LaunchAgent's interpreter differs from the venv | Re-toggle "Start at login" from a tray launched with the venv's python, so the plist records the venv `python` in `sys.executable` |

---

## What is NOT covered here

- `.app` bundle packaging (py2app) — future work.
- The headless-daemon setup — see `install-mac.sh` and the top-level install docs.
