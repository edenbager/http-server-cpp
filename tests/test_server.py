"""End-to-end tests for the web server.

Start the server first (from the repo root):  ./webserver www
Then run:                                      python3 tests/test_server.py
"""
import os
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 8080
ROOT = os.path.join(os.path.dirname(__file__), "..", "www")


def request(raw: bytes, read_until_close=False, timeout=3.0) -> bytes:
    """Send raw bytes and return one full response (headers + Content-Length body)."""
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.sendall(raw)
    data = b""
    while True:
        if not read_until_close and b"\r\n\r\n" in data:
            head, _, body = data.partition(b"\r\n\r\n")
            length = 0
            for line in head.split(b"\r\n")[1:]:
                k, _, v = line.partition(b":")
                if k.strip().lower() == b"content-length":
                    length = int(v.strip())
            if raw.startswith(b"HEAD") or len(body) >= length:
                break
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    return data


def status(resp: bytes) -> int:
    return int(resp.split(b" ", 2)[1])


def body(resp: bytes) -> bytes:
    return resp.partition(b"\r\n\r\n")[2]


passed = failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print(f"PASS  {name}")
    else:
        failed += 1
        print(f"FAIL  {name}")


r = request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
check("GET / serves index.html", status(r) == 200 and b"Welcome to the Web Server" in body(r))

for lang, text in [(b"he", "ברוכים הבאים".encode()), (b"en", b"English version"), (b"fr", b"Bienvenue")]:
    r = request(b"GET /index.html?lang=" + lang + b" HTTP/1.1\r\nHost: x\r\n\r\n")
    check(f"GET ?lang={lang.decode()} serves the {lang.decode()} page", status(r) == 200 and text in body(r))

r = request(b"GET /index.html?lang=de HTTP/1.1\r\nHost: x\r\n\r\n")
check("unknown lang falls back to the default page", status(r) == 200 and b"Welcome to the Web Server" in body(r))

r = request(b"GET /missing.html HTTP/1.1\r\nHost: x\r\n\r\n")
check("GET missing file -> 404", status(r) == 404)

r = request(b"HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
check("HEAD returns headers only", status(r) == 200 and body(r) == b"" and b"Content-Length: " in r)

r = request(b"HEAD /missing.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", read_until_close=True)
check("HEAD on missing file has no body", status(r) == 404 and body(r) == b"")

r = request(b"OPTIONS / HTTP/1.1\r\nHost: x\r\n\r\n")
check("OPTIONS lists methods", status(r) == 200 and b"Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, TRACE" in r)

r = request(b"TRACE / HTTP/1.1\r\nHost: x\r\nX-Test: 1\r\n\r\n")
check("TRACE echoes the request", status(r) == 200 and b"X-Test: 1" in body(r))

msg = b"<script>alert(1)</script>"
r = request(b"POST / HTTP/1.1\r\nHost: x\r\ncontent-length: %d\r\n\r\n" % len(msg) + msg)
check("POST with lowercase content-length is read", status(r) == 200 and b"&lt;script&gt;" in body(r))
check("POST body is HTML-escaped", b"<script>" not in body(r))

page = b"<h1>put test</h1>"
r = request(b"PUT /put_test.html HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(page) + page)
check("PUT new file -> 201", status(r) == 201)
r = request(b"PUT /put_test.html HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(page) + page)
check("PUT existing file -> 200", status(r) == 200)
r = request(b"GET /put_test.html HTTP/1.1\r\nHost: x\r\n\r\n")
check("GET returns the PUT content", body(r) == page)
r = request(b"DELETE /put_test.html HTTP/1.1\r\nHost: x\r\n\r\n")
check("DELETE -> 200", status(r) == 200)
r = request(b"DELETE /put_test.html HTTP/1.1\r\nHost: x\r\n\r\n")
check("DELETE again -> 404", status(r) == 404)

r = request(b"GET /../WebServer.cpp HTTP/1.1\r\nHost: x\r\n\r\n")
check("path traversal on GET is rejected", status(r) == 400)
r = request(b"DELETE /../WebServer.cpp HTTP/1.1\r\nHost: x\r\n\r\n")
check("path traversal on DELETE is rejected", status(r) == 400)
r = request(b"GET /index.html?lang=/../../WebServer HTTP/1.1\r\nHost: x\r\n\r\n")
check("lang can't be used for traversal", b"#include" not in body(r))

big = b"x" * 200_000
with open(os.path.join(ROOT, "big_test.txt"), "wb") as f:
    f.write(big)
r = request(b"GET /big_test.txt HTTP/1.1\r\nHost: x\r\n\r\n", timeout=10)
os.remove(os.path.join(ROOT, "big_test.txt"))
check("200 KB file is sent in full", status(r) == 200 and body(r) == big)

open(os.path.join(ROOT, "empty_test.txt"), "wb").close()
r = request(b"GET /empty_test.txt HTTP/1.1\r\nHost: x\r\n\r\n")
os.remove(os.path.join(ROOT, "empty_test.txt"))
check("empty file -> 200, not 404", status(r) == 200)

r = request(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\nGET /index.html?lang=fr HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            read_until_close=True)
check("two pipelined requests get two responses", r.count(b"HTTP/1.1 200 OK") == 2 and b"Bienvenue" in r)

huge = b"x" * 100_000
r = request(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(huge), read_until_close=True)
check("oversized body -> 413", status(r) == 413)

r = request(b"BREW / HTTP/1.1\r\nHost: x\r\n\r\n")
check("unknown method -> 501", status(r) == 501)

# Many clients at once: open 20 connections, then send on all of them
conns = [socket.create_connection((HOST, PORT), timeout=3) for _ in range(20)]
for c in conns:
    c.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
ok = 0
for c in conns:
    data = b""
    while True:
        chunk = c.recv(65536)
        if not chunk:
            break
        data += chunk
    ok += data.startswith(b"HTTP/1.1 200 OK")
    c.close()
check("20 concurrent connections all served", ok == 20)

# A slow client must not block others
slow = socket.create_connection((HOST, PORT), timeout=3)
slow.sendall(b"GET / HTTP/1.1\r\nHo")   # never finishes
t = time.time()
r = request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
check("a stalled client doesn't block others", status(r) == 200 and time.time() - t < 1)
slow.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
