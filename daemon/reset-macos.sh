#!/usr/bin/env bash
# reset-macos.sh — restart the Clawdmeter menu-bar tray daemon on macOS.
#
# Handles the documented "stale old-code process holding the single-instance
# lock" gotcha (see CLAUDE.md → macOS operational gotchas): a bare
# `launchctl kickstart` silently no-ops when an orphaned tray from a previous
# session still owns the flock on ~/Library/Caches/Clawdmeter-tray.lock, so the
# Mac keeps running the OLD daemon code and never picks up a `git pull`. This
# script kills the lock holder FIRST, then (re)starts the LaunchAgent, and
# verifies the new process actually came up.
#
# Usage:  ./daemon/reset-macos.sh        (from anywhere; paths resolve to the repo)
set -u

LABEL="com.clawdmeter.tray"
UID_NUM="$(id -u)"
LOCK="$HOME/Library/Caches/Clawdmeter-tray.lock"
OUT_LOG="$HOME/Library/Logs/Clawdmeter-tray.out.log"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_PY="$REPO_ROOT/.venv/bin/python"
TRAY_PY="$REPO_ROOT/daemon/tray_macos.py"

say() { printf '\033[1;36m[reset]\033[0m %s\n' "$1"; }

# 1. Kill whatever holds the single-instance lock (the stale old-code process).
if [ -e "$LOCK" ]; then
  holder="$(lsof -t "$LOCK" 2>/dev/null | head -1)"
  if [ -n "${holder:-}" ]; then
    say "killing lock holder PID $holder"
    kill "$holder" 2>/dev/null || true
    sleep 1
    if kill -0 "$holder" 2>/dev/null; then
      say "still alive — SIGKILL $holder"
      kill -9 "$holder" 2>/dev/null || true
    fi
  else
    say "lock file present but unheld (clean)"
  fi
fi

# 2. Belt-and-suspenders: reap any lingering tray/daemon processes.
pkill -f "daemon/tray_macos.py" 2>/dev/null || true
pkill -f "daemon/claude_usage_daemon.py" 2>/dev/null || true
sleep 1

# 3. (Re)start via the LaunchAgent when it exists; otherwise fall back.
if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
  say "kickstarting LaunchAgent $LABEL"
  launchctl kickstart -k "gui/$UID_NUM/$LABEL"
elif [ -f "$PLIST" ]; then
  say "bootstrapping LaunchAgent from $PLIST"
  launchctl bootstrap "gui/$UID_NUM" "$PLIST" 2>/dev/null || true
  launchctl kickstart -k "gui/$UID_NUM/$LABEL"
elif [ -x "$VENV_PY" ] && [ -f "$TRAY_PY" ]; then
  say "LaunchAgent not installed — launching tray directly (detached)"
  nohup "$VENV_PY" "$TRAY_PY" >>"$OUT_LOG" 2>&1 &
  disown
else
  say "ERROR: no LaunchAgent and no venv/tray script found."
  say "Run the macOS setup first (see daemon/README-macos.md)."
  exit 1
fi

# 4. Verify the tray reacquired the lock (i.e. the new process is alive).
sleep 4
newpid="$(lsof -t "$LOCK" 2>/dev/null | head -1)"
if [ -n "${newpid:-}" ]; then
  say "tray running (PID $newpid). Recent log:"
  tail -n 6 "$OUT_LOG" 2>/dev/null || true
else
  say "WARNING: tray did not acquire the lock within 4s. Check the log:"
  tail -n 15 "$OUT_LOG" 2>/dev/null || true
  exit 1
fi
