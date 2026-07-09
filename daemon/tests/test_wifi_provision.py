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
