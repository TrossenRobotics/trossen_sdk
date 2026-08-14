"""The Lite MJPEG feed has to survive a viewer that stops listening.

Two things are pinned here, both of them things the browser cannot do for us.

`/cameras` has to report a per-camera frame counter. A viewer cannot tell a
stalled connection from an idle camera by looking at the picture — the stream
keepalive re-sends the *same* frame every 5s, so both look frozen. The counter
is what separates them, and the frontend watchdog only reconnects when the
server produced frames the viewer never received.

And a stream write has to time out. A client that goes away without closing
(screen asleep, Wi-Fi gone, machine off) leaves the handler blocked in write()
forever once the socket buffer fills, holding a thread and spending bandwidth
on frames nobody is reading. rivet-01 was seen doing exactly that for a viewer
that was 100% unreachable.
"""

from __future__ import annotations

import json
import socket
import threading
import urllib.request

import pytest

# Same rationale as test_termination: recorder_runner imports the compiled SDK
# at module scope, which is not built everywhere this suite runs.
pytest.importorskip("trossen_sdk")
pytest.importorskip("numpy")
pytest.importorskip("rerun")

from http.server import ThreadingHTTPServer  # noqa: E402

from app import recorder_runner as rr  # noqa: E402


@pytest.fixture()
def feed(monkeypatch):
    """Run the real _MJPEGHandler on an ephemeral port with two fake cameras.

    The send timeout is shortened so the disconnect test does not sit for the
    production 15s.
    """
    monkeypatch.setattr(rr, "_MJPEG_SEND_TIMEOUT_S", 1.0)
    monkeypatch.setattr(rr, "_latest_jpeg", {"cam_a": b"\xff\xd8frame", "cam_b": b"\xff\xd8f"})
    monkeypatch.setattr(rr, "_mjpeg_seq", {"cam_a": 7, "cam_b": 3})
    monkeypatch.setattr(rr, "_mjpeg_cond", threading.Condition())

    server = ThreadingHTTPServer(("127.0.0.1", 0), rr._MJPEGHandler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()


def _get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.loads(r.read())


def test_cameras_reports_frame_counters(feed):
    body = _get_json(f"{feed}/cameras")
    assert body["cameras"] == ["cam_a", "cam_b"]
    # Without these a viewer cannot tell "my stream died" from "this camera has
    # nothing new", and would reconnect an idle camera forever.
    assert body["seq"] == {"cam_a": 7, "cam_b": 3}


def test_health_reports_the_same_state(feed):
    body = _get_json(f"{feed}/health")
    assert body["status"] == "ok"
    assert body["seq"] == {"cam_a": 7, "cam_b": 3}


def test_camera_list_survives_a_camera_with_no_counter(feed, monkeypatch):
    """A camera whose first frame landed between the two dict writes."""
    monkeypatch.setattr(rr, "_latest_jpeg", {"cam_a": b"x", "cam_new": b"y"})
    monkeypatch.setattr(rr, "_mjpeg_seq", {"cam_a": 7})
    body = _get_json(f"{feed}/cameras")
    assert body["cameras"] == ["cam_a", "cam_new"]
    # Absent rather than zero: the viewer treats a missing counter as "no
    # frames seen yet", which is the truth and is not a stall.
    assert body["seq"] == {"cam_a": 7}


def test_stream_drops_a_client_that_stops_reading(feed):
    """The abandoned-viewer case: connect, read nothing, never close.

    The handler thread must give up on its own. Before the send timeout it
    stayed blocked in write() indefinitely.
    """
    host, port = feed.rsplit(":", 1)
    sock = socket.create_connection(("127.0.0.1", int(port)), timeout=5)
    try:
        sock.sendall(
            b"GET /stream/cam_a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        # Shrink the receive window to nothing so the server's writes block as
        # soon as the buffers fill, which is what an unreachable client looks
        # like from this side.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)

        # Keep the camera producing so the handler keeps trying to write.
        stop = threading.Event()

        def produce():
            n = 7
            big = b"\xff\xd8" + b"\x00" * 200_000
            while not stop.is_set():
                with rr._mjpeg_cond:
                    n += 1
                    rr._mjpeg_seq["cam_a"] = n
                    rr._latest_jpeg["cam_a"] = big
                    rr._mjpeg_cond.notify_all()
                stop.wait(0.02)

        t = threading.Thread(target=produce, daemon=True)
        t.start()
        try:
            # Read nothing. The server should hit its 1s send timeout and close;
            # a recv on a closed connection returns b"" rather than hanging.
            sock.settimeout(20)
            deadline_hit = False
            try:
                while True:
                    if sock.recv(65536) == b"":
                        break
            except socket.timeout:
                deadline_hit = True
            assert not deadline_hit, "server never dropped the silent client"
        finally:
            stop.set()
            t.join(timeout=5)
    finally:
        sock.close()
