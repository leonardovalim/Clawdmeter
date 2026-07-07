#!/usr/bin/env python3
"""Unit tests for the Windows daemon's chime config read/write round-trip.

Covers read_chime_setting / write_chime_setting, backing the tray's
"Play chime on reset" toggle (tray_windows._on_toggle_chime).

Run: python -m pytest daemon/tests/test_windows_config.py -x -q
"""
import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import read_chime_setting, write_chime_setting


def test_chime_defaults_off_when_file_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_chime_setting() == "off"


def test_write_chime_creates_file_and_dir(tmp_path, monkeypatch):
    cfg = tmp_path / "nested" / "config"  # parent dir doesn't exist yet
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    write_chime_setting("on")
    assert read_chime_setting() == "on"


def test_write_chime_toggles_existing_value(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("chime = off\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    write_chime_setting("on")
    assert read_chime_setting() == "on"
    write_chime_setting("off")
    assert read_chime_setting() == "off"


def test_write_chime_preserves_other_lines(tmp_path, monkeypatch):
    """Toggling chime must not disturb comments or the clock setting."""
    cfg = tmp_path / "config"
    cfg.write_text("# my notes\nclock = 24\nchime = off\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    write_chime_setting("on")
    text = cfg.read_text()
    assert "# my notes" in text
    assert "clock = 24" in text
    assert "chime = on" in text


def test_write_chime_appends_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    write_chime_setting("on")
    assert "clock = auto" in cfg.read_text()
    assert read_chime_setting() == "on"
