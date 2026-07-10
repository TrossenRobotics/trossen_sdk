"""Admin login + machine registration auth, stdlib-only.

Two independent trust checks, both tuned for a trusted LAN:

  - **Admin humans** authenticate once with a shared password
    (`HUB_ADMIN_PASSWORD`) and get an HMAC-signed, expiring session cookie.
    No user table in Phase 1 — one console password is enough to keep the
    dashboard off casual eyes on the network.
  - **Machines** present a pre-shared `HUB_TOKEN` in their register frame so
    a random device can't join the fleet and inject fake status.

Signing uses `hmac`/`hashlib` with a server secret (`HUB_SECRET`), so there
is no crypto dependency to add. If `HUB_SECRET` is unset a random one is
generated per boot — fine for a single-process hub (it just means existing
cookies stop validating across a restart, forcing a re-login).
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import logging
import os
import secrets
import time

logger = logging.getLogger("app.auth")

COOKIE_NAME = "hub_session"

# Session lifetime. A collection shift is long; a full day means the admin
# isn't re-typing the password constantly, and it's a LAN console anyway.
_SESSION_TTL_S = 24 * 60 * 60

# Per-boot fallback secret when HUB_SECRET is unset. Module-level so every
# sign/verify in this process shares it.
_SECRET = os.environ.get("HUB_SECRET") or secrets.token_hex(32)
if not os.environ.get("HUB_SECRET"):
    logger.warning("HUB_SECRET unset — using a random per-boot secret; sessions drop on restart")


def _sign(payload: str) -> str:
    """Return `payload.sig`, sig = base64url(HMAC-SHA256(secret, payload))."""
    mac = hmac.new(_SECRET.encode(), payload.encode(), hashlib.sha256).digest()
    sig = base64.urlsafe_b64encode(mac).decode().rstrip("=")
    return f"{payload}.{sig}"


def password_ok(candidate: str) -> bool:
    """Constant-time compare against `HUB_ADMIN_PASSWORD` (default 'admin').

    Defaulting to 'admin' keeps first-run frictionless; the startup banner
    warns when the default is in effect so it isn't left that way in the
    field.
    """
    expected = os.environ.get("HUB_ADMIN_PASSWORD", "admin")
    return hmac.compare_digest(candidate or "", expected)


def issue_session() -> str:
    """Mint a signed session cookie value carrying its own expiry."""
    expiry = int(time.time()) + _SESSION_TTL_S
    return _sign(str(expiry))


def session_valid(cookie_value: str | None) -> bool:
    """Verify a session cookie: intact signature and not expired."""
    if not cookie_value or "." not in cookie_value:
        return False
    payload, _, _sig = cookie_value.rpartition(".")
    # Re-sign the payload and constant-time compare the whole token; this
    # both checks the signature and rejects tampering in one step.
    if not hmac.compare_digest(_sign(payload), cookie_value):
        return False
    try:
        return int(payload) > int(time.time())
    except ValueError:
        return False


def machine_token_ok(candidate: str | None) -> bool:
    """Check a machine's register token against `HUB_TOKEN`.

    When `HUB_TOKEN` is unset the check is skipped (any machine may join) to
    keep local bring-up easy; a warning is logged so it isn't relied on in
    production.
    """
    expected = os.environ.get("HUB_TOKEN", "")
    if not expected:
        logger.warning("HUB_TOKEN unset — accepting any machine registration")
        return True
    return hmac.compare_digest(candidate or "", expected)
