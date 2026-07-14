"""End-to-end machine↔hub WebSocket flow via FastAPI's TestClient.

Exercises the real register → ack → roster/assignments push → heartbeat →
reconcile path against the actual app, not the modules in isolation. The app is
driven WITHOUT its lifespan (the conftest already builds the schema), so the
background maintenance loop never starts.
"""

from __future__ import annotations

import time

from fastapi.testclient import TestClient

from app import assignments, operators
from app.main import app


def _register(ws, mid="m1", token=""):
    ws.send_json({"type": "register", "token": token,
                  "machine": {"machine_id": mid, "name": "Cell-A"}})


def test_connect_handshake_pushes_roster_and_assignments():
    operators.create_operator("Alice", "1234")
    a = assignments.create("m1", "Fold towel", "vary grip", 50)

    client = TestClient(app)
    with client.websocket_connect("/ws/machine") as ws:
        _register(ws)
        assert ws.receive_json()["type"] == "ack"
        roster = ws.receive_json()
        assert roster["type"] == "roster"
        assert roster["operators"][0]["name"] == "Alice"
        assert roster["operators"][0]["pin_hash"] == operators.hash_pin("1234")
        pushed = ws.receive_json()
        assert pushed["type"] == "assignments"
        assert [x["title"] for x in pushed["assignments"]] == ["Fold towel"]
        assert pushed["assignments"][0]["id"] == a.id


def test_bad_token_is_rejected(monkeypatch):
    monkeypatch.setenv("HUB_TOKEN", "s3cret")
    client = TestClient(app)
    with client.websocket_connect("/ws/machine") as ws:
        _register(ws, token="wrong")
        # The server closes the socket; the next receive raises.
        try:
            ws.receive_json()
            raised = False
        except Exception:
            raised = True
    assert raised


def test_heartbeat_reconciles_downtime_and_assignment_status():
    a = assignments.create("m1", "T", "", None)

    client = TestClient(app)
    with client.websocket_connect("/ws/machine") as ws:
        _register(ws)
        for _ in range(3):  # drain ack + roster + assignments
            ws.receive_json()
        ws.send_json({
            "type": "heartbeat",
            "machine_id": "m1",
            "faults": [{"key": "k1", "reason": "arm down", "since": "2026-07-11T00:00:00+00:00"}],
            "assignments": [{"id": a.id, "status": "done"}],
        })

        # The handler processes the heartbeat asynchronously; poll briefly.
        from app import downtime

        def _settled():
            open_events = downtime.events()["open"]
            done = any(x["id"] == a.id and x["status"] == "done"
                       for x in assignments.all_assignments())
            return len(open_events) == 1 and done

        for _ in range(50):
            if _settled():
                break
            time.sleep(0.02)
        assert _settled()
