#!/usr/bin/env python3
"""Unit tests for the Windows daemon's silent OAuth token refresh.

The credentials file Claude Code writes carries a refreshToken next to the
accessToken, so an expired token can be renewed with a plain HTTP call — no
browser, no `claude login`. These tests pin the two things that can go badly
wrong: losing a rotated refresh token, and clobbering a different account's
credentials after the user runs `claude login` mid-refresh.

No network: httpx.AsyncClient is stubbed. No real credentials: every test runs
against a temp file via the CLAUDE_CREDENTIALS_PATH override.

Run: python -m pytest daemon/tests/test_windows_token_refresh.py -x -q
"""
import asyncio
import json
import time

import pytest

import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import (
    _load_credentials,
    refresh_access_token,
    token_needs_refresh,
)

OLD_ACCESS = "sk-ant-oat01-OLD"
OLD_REFRESH = "sk-ant-ort01-OLD"
SCOPES = ["user:inference", "user:profile"]


def _doc(expires_at_ms: int) -> dict:
    """A credentials doc shaped like the real one, with sibling keys to protect."""
    return {
        "claudeAiOauth": {
            "accessToken": OLD_ACCESS,
            "refreshToken": OLD_REFRESH,
            "expiresAt": expires_at_ms,
            "scopes": list(SCOPES),
            "subscriptionType": "pro",
            "rateLimitTier": "default_claude_ai",
        },
        "trustedDeviceToken": "trusted-abc",
        "mcpOAuth": {"some:server|hash": {"accessToken": "mcp-token"}},
    }


@pytest.fixture
def creds(tmp_path, monkeypatch):
    """Point the daemon's credential lookup at a throwaway file."""
    path = tmp_path / ".credentials.json"
    path.write_text(json.dumps(_doc(int((time.time() + 3600) * 1000))), encoding="utf-8")
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(path))
    return path


class _FakeResp:
    def __init__(self, status_code, payload):
        self.status_code = status_code
        self._payload = payload

    def json(self):
        return self._payload


def _stub_httpx(monkeypatch, status_code=200, payload=None, on_post=None):
    """Replace httpx.AsyncClient so no request leaves the machine.

    `on_post` runs while the request is "in flight", letting a test emulate a
    concurrent `claude login` landing between the read and the write.
    """
    captured = {}

    class _FakeClient:
        def __init__(self, *a, **k):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return False

        async def post(self, url, json=None, **k):
            captured["url"] = url
            captured["body"] = json
            if on_post:
                on_post()
            return _FakeResp(status_code, payload or {})

    monkeypatch.setattr(mod.httpx, "AsyncClient", _FakeClient)
    return captured


def test_needs_refresh_false_when_far_from_expiry(creds):
    assert token_needs_refresh() is False


def test_needs_refresh_true_when_expired(creds):
    creds.write_text(json.dumps(_doc(int((time.time() - 60) * 1000))), encoding="utf-8")
    assert token_needs_refresh() is True


def test_needs_refresh_true_inside_margin(creds):
    """Renew before the boundary so a poll never races expiry."""
    soon = int((time.time() + mod.TOKEN_REFRESH_MARGIN_S - 30) * 1000)
    creds.write_text(json.dumps(_doc(soon)), encoding="utf-8")
    assert token_needs_refresh() is True


def test_refresh_sends_the_cli_request_shape(creds, monkeypatch):
    captured = _stub_httpx(monkeypatch, payload={"access_token": "new", "expires_in": 60})
    asyncio.run(refresh_access_token())
    assert captured["url"] == mod.OAUTH_TOKEN_URL
    body = captured["body"]
    assert body["grant_type"] == "refresh_token"
    assert body["client_id"] == mod.OAUTH_CLIENT_ID
    assert body["refresh_token"] == OLD_REFRESH
    # Scopes come from disk, not a hardcoded list: a work account and a personal
    # account can hold different ones.
    assert body["scope"] == " ".join(SCOPES)


def test_refresh_persists_rotated_token_and_keeps_siblings(creds, monkeypatch):
    _stub_httpx(monkeypatch, payload={
        "access_token": "sk-ant-oat01-NEW",
        "refresh_token": "sk-ant-ort01-NEW",
        "expires_in": 28800,
    })
    assert asyncio.run(refresh_access_token()) == "sk-ant-oat01-NEW"

    after = json.loads(creds.read_text(encoding="utf-8"))
    oauth = after["claudeAiOauth"]
    assert oauth["accessToken"] == "sk-ant-oat01-NEW"
    assert oauth["refreshToken"] == "sk-ant-ort01-NEW"   # rotation must be saved
    assert oauth["expiresAt"] / 1000 > time.time() + 28000
    assert oauth["scopes"] == SCOPES
    assert oauth["subscriptionType"] == "pro"
    assert after["trustedDeviceToken"] == "trusted-abc"  # sibling keys survive
    assert after["mcpOAuth"] == {"some:server|hash": {"accessToken": "mcp-token"}}
    assert not creds.with_name(creds.name + ".clawdmeter.tmp").exists()


def test_refresh_keeps_old_token_when_server_omits_rotation(creds, monkeypatch):
    _stub_httpx(monkeypatch, payload={"access_token": "new-a", "expires_in": 60})
    asyncio.run(refresh_access_token())
    oauth = json.loads(creds.read_text(encoding="utf-8"))["claudeAiOauth"]
    assert oauth["refreshToken"] == OLD_REFRESH


def test_refresh_aborts_when_account_switched_mid_flight(creds, monkeypatch):
    """`claude login` between our read and our write must win.

    Persisting here would overwrite the newly-logged-in account's credentials
    with the previous account's — the whole reason for the compare-and-swap.
    """
    def simulate_login():
        other = _doc(int((time.time() + 3600) * 1000))
        other["claudeAiOauth"]["accessToken"] = "sk-ant-oat01-PERSONAL"
        other["claudeAiOauth"]["refreshToken"] = "sk-ant-ort01-PERSONAL"
        creds.write_text(json.dumps(other), encoding="utf-8")

    _stub_httpx(monkeypatch, payload={
        "access_token": "sk-ant-oat01-WORK",
        "refresh_token": "sk-ant-ort01-WORK",
        "expires_in": 28800,
    }, on_post=simulate_login)

    assert asyncio.run(refresh_access_token()) is None
    oauth = json.loads(creds.read_text(encoding="utf-8"))["claudeAiOauth"]
    assert oauth["accessToken"] == "sk-ant-oat01-PERSONAL"
    assert oauth["refreshToken"] == "sk-ant-ort01-PERSONAL"


def test_refresh_returns_none_on_http_error_and_leaves_file_alone(creds, monkeypatch):
    _stub_httpx(monkeypatch, status_code=401, payload={})
    assert asyncio.run(refresh_access_token()) is None
    oauth = json.loads(creds.read_text(encoding="utf-8"))["claudeAiOauth"]
    assert oauth["refreshToken"] == OLD_REFRESH
    assert oauth["accessToken"] == OLD_ACCESS


def test_refresh_returns_none_without_access_token_in_response(creds, monkeypatch):
    _stub_httpx(monkeypatch, payload={"expires_in": 60})
    assert asyncio.run(refresh_access_token()) is None


def test_refresh_skips_raw_token_credentials(tmp_path, monkeypatch):
    """A bare-token blob has no refreshToken; refresh must no-op, not crash."""
    path = tmp_path / ".credentials.json"
    path.write_text("sk-ant-oat01-rawtokenvalue0123456789", encoding="utf-8")
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(path))
    assert _load_credentials() is None
    assert token_needs_refresh() is False
    assert asyncio.run(refresh_access_token()) is None
