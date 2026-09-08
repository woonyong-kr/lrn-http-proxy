import concurrent.futures
from collections import Counter
from contextlib import contextmanager
import http.server
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
COUNTS = Counter()
LOCK = threading.Lock()


class Origin(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with LOCK:
            COUNTS[self.path] += 1
        if self.path == "/slow":
            time.sleep(0.8)
        if self.path == "/chunked":
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
            )
            return
        body = (
            b"x" * 150000
            if self.path == "/large"
            else b"x" * 100000
            if self.path.startswith("/big-cache")
            else self.path.encode() + b":payload"
        )
        self.send_response(200)
        self.send_header(
            "Content-Length", str(len(body) + (10 if self.path == "/broken" else 0))
        )
        if self.path != "/unmarked":
            self.send_header(
                "Cache-Control",
                (
                    "public, max-age=60"
                    if self.path.startswith("/big-cache")
                    else "public, max-age=2"
                )
                if self.path != "/private"
                else "private, max-age=100",
            )
        if self.path == "/stale":
            self.send_header("Age", "999")
        if self.path == "/cookie":
            self.send_header("Set-Cookie", "a=b")
        self.end_headers()
        try:
            for i in range(0, len(body), 1024):
                self.wfile.write(body[i : i + 1024])
        except (BrokenPipeError, ConnectionResetError):
            pass


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_ready(proc, port, diagnostics):
    # Startup can be delayed on a busy machine; readiness has its own deadline.
    # This is only startup readiness; request timeout tests still use 300 ms.
    deadline = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError as error:
            status = proc.poll()
            if status is not None or time.monotonic() > deadline:
                diagnostics.seek(0)
                raise RuntimeError(
                    f"Server not ready: exit={status}, stderr={diagnostics.read()!r}"
                ) from error
            time.sleep(0.01)


@contextmanager
def servers():
    COUNTS.clear()
    origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    thread = threading.Thread(target=origin.serve_forever, daemon=True)
    thread.start()
    port = free_port()
    diagnostics = tempfile.TemporaryFile(mode="w+")
    proc = subprocess.Popen(
        [str(ROOT / "webproxy-lab/proxy"), str(port)],
        env={**os.environ, "PROXY_TIMEOUT_MS": "300", "PROXY_WORKERS": "4"},
        stdout=subprocess.DEVNULL,
        stderr=diagnostics,
    )
    try:
        wait_ready(proc, port, diagnostics)
        yield port, origin.server_port
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        diagnostics.close()
        origin.shutdown()
        origin.server_close()
        thread.join()


def request(proxy, origin, path, headers="", raw=None, fragmented=False):
    data = (
        raw
        or f"GET http://127.0.0.1:{origin}{path} HTTP/1.1\r\nHost: 127.0.0.1:{origin}\r\n{headers}\r\n".encode()
    )
    with socket.create_connection(("127.0.0.1", proxy), timeout=3) as s:
        if fragmented:
            for i in range(0, len(data), 7):
                s.sendall(data[i : i + 7])
        else:
            s.sendall(data)
        chunks = []
        while True:
            try:
                b = s.recv(65536)
            except ConnectionResetError:
                break
            if not b:
                break
            chunks.append(b)
    return b"".join(chunks)


class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.ctx = servers()
        self.proxy, self.origin = self.ctx.__enter__()

    def tearDown(self):
        self.ctx.__exit__(None, None, None)

    def req(self, path, **kw):
        return request(self.proxy, self.origin, path, **kw)

    def test_hit_miss_expiry(self):
        a = self.req("/fresh", fragmented=True)
        b = self.req("/fresh")
        self.assertEqual(a.split(b"\r\n\r\n", 1)[1], b"/fresh:payload")
        self.assertEqual(b.split(b"\r\n\r\n", 1)[1], b"/fresh:payload")
        self.assertEqual(COUNTS["/fresh"], 1)
        time.sleep(2.1)  # max-age is the behavior under test
        self.req("/fresh")
        self.assertEqual(COUNTS["/fresh"], 2)

    def test_private_cookie_unmarked_large_are_not_cached(self):
        for path in ["/private", "/cookie", "/unmarked", "/large", "/stale"]:
            with self.subTest(path=path):
                a = self.req(path)
                b = self.req(path)
                self.assertEqual(COUNTS[path], 2)
                self.assertEqual(a.split(b"\r\n\r\n", 1)[1], b.split(b"\r\n\r\n", 1)[1])

    def test_auth_and_cookie_requests_bypass_existing_cache(self):
        self.req("/fresh")
        self.req("/fresh", headers="Authorization: Bearer test\r\n")
        self.req("/fresh", headers="Cookie: a=b\r\n")
        self.assertEqual(COUNTS["/fresh"], 3)

    def test_broken_origin_is_not_cached(self):
        self.req("/broken")
        self.req("/broken")
        self.assertEqual(COUNTS["/broken"], 2)

    def test_timeout_and_unsupported_framing(self):
        self.assertIn(b"504", self.req("/slow").split(b"\r\n", 1)[0])
        self.assertIn(b"502", self.req("/chunked").split(b"\r\n", 1)[0])
        self.assertIn(b"400", self.req("/", headers="Content-Length: 10\r\n"))
        self.assertIn(b"501", self.req("/", raw=b"CONNECT a:443 HTTP/1.1\r\n\r\n"))

    def test_cache_capacity_and_recently_used_entry(self):
        for i in range(10):
            self.req(f"/big-cache{i}")
        self.req("/big-cache0")
        self.assertEqual(COUNTS["/big-cache0"], 1)
        self.req("/big-cache10")
        self.req("/big-cache0")
        self.assertEqual(COUNTS["/big-cache0"], 1)
        self.req("/big-cache1")
        self.assertEqual(COUNTS["/big-cache1"], 2)

    def test_tiny_server_static_file(self):
        port = free_port()
        diagnostics = tempfile.TemporaryFile(mode="w+")
        proc = subprocess.Popen(
            [str(ROOT / "webproxy-lab/tiny/tiny"), str(port)],
            cwd=ROOT / "webproxy-lab/tiny",
            stdout=subprocess.DEVNULL,
            stderr=diagnostics,
        )
        try:
            wait_ready(proc, port, diagnostics)
            expected = (ROOT / "webproxy-lab/tiny/home.html").read_bytes()
            for _ in range(2):
                response = request(self.proxy, port, "/home.html")
                self.assertEqual(response.split(b"\r\n\r\n", 1)[1], expected)
        finally:
            proc.terminate()
            proc.wait(timeout=3)
            diagnostics.close()

    def test_parallel_responses_and_repeated_disconnects(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            replies = list(pool.map(lambda i: self.req(f"/p{i % 4}"), range(48)))
        for i, r in enumerate(replies):
            self.assertEqual(r.split(b"\r\n\r\n", 1)[1], f"/p{i % 4}:payload".encode())
        for _ in range(40):
            with socket.create_connection(("127.0.0.1", self.proxy), timeout=1):
                pass
        self.assertIn(b"200", self.req("/alive").split(b"\r\n", 1)[0])


if __name__ == "__main__":
    unittest.main()
