# http-server-cpp

[![tests](https://github.com/edenbager/http-server-cpp/actions/workflows/tests.yml/badge.svg)](https://github.com/edenbager/http-server-cpp/actions/workflows/tests.yml)

A small HTTP/1.1 web server in C++, written from scratch on raw sockets. It serves many clients at once from a **single thread**, using **non-blocking sockets** and `select()`. Built for the Introduction to Computer Networks course at The Academic College of Tel Aviv–Yaffo.

**Stack:** C++17 · Berkeley sockets / Winsock · `select()` · Python (tests)

## What it does

- **Methods:** `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `OPTIONS`, `TRACE`
- **Languages:** `GET /index.html?lang=he|en|fr` serves `index_he.html` etc., and falls back to the default page if that version doesn't exist
- **Many clients, one thread:** up to 60 open connections, no threads, no blocking calls
- **Keep-alive and pipelining:** several requests on one connection are answered in order
- **Idle timeout:** a connection that sends nothing for 2 minutes is closed
- `POST` prints the body to the server console. `PUT` creates or overwrites a file, `DELETE` removes it

The course rules: no multithreading and no blocking sockets.

## Build and run

macOS / Linux:

```
g++ -std=c++17 -O2 WebServer.cpp -o webserver
./webserver www
```

Windows (Visual Studio Developer Command Prompt):

```
cl /std:c++17 /EHsc WebServer.cpp /Fe:webserver.exe
webserver.exe www
```

Then open http://localhost:8080 or http://localhost:8080/index.html?lang=fr.
The argument is the folder to serve (default `www`).

## How it works

```
             select(): who is ready to read / write?
                       │
     ┌─────────────────┼──────────────────┐
  listen socket    readable client     writable client
     │                 │                  │
  accept()         recv() into its     send() the next chunk
  set non-blocking buffer, parse when  of its response; when done,
                   a full request is   parse the next pipelined
                   there, queue reply  request, if any
```

Every connection has its own state (receive buffer, pending response, last activity time), so a slow client never blocks the others. One loop over `select()` drives everything; it also wakes every 5 seconds to close idle connections.

## Tests

`tests/test_server.py` is an end-to-end test that talks to the running server over TCP: all methods, language fallback, 404s, path traversal, a 200 KB file, an empty file, pipelined requests, an oversized body, 20 concurrent clients, and a stalled client that must not block the others.

```
./webserver www &
python3 tests/test_server.py      # 27 passed, 0 failed
```

`docs/` has Wireshark captures of the GET and POST exchanges from the original submission.

## Fixes after submission

The first commit is the code as submitted. Going back to it, I fixed:

- **Path traversal.** `GET /../anything` read files outside the served folder, and `PUT` / `DELETE` could write or delete them. Paths with `..` are now rejected, and `lang` only accepts `he`, `en` or `fr`.
- **Truncated responses.** Responses were copied into a fixed 32 KB buffer, so a bigger file was cut off while `Content-Length` still promised the full size. The send buffer now grows to fit.
- **Pipelining bug.** A second request arriving while the first response was still being sent overwrote that response. Requests now wait their turn.
- **Header parsing.** Header names were matched case-sensitively and the value was assumed to follow `": "`, so `content-length: 5` was ignored. Names are now case-insensitive and values trimmed.
- **HTTP details.** `HEAD` on a missing file returned a body, an empty file returned 404, `PUT` always answered 201 even when overwriting, oversized requests weren't handled, and the `POST` body was echoed into HTML unescaped.
- **Cross-platform build.** The code used Winsock only. A small compatibility layer lets the same file build with POSIX sockets on macOS and Linux.

## What I'd improve

- **Use `poll()` / `epoll` / `kqueue`** instead of `select()`, which is capped by `FD_SETSIZE` and scans every socket on each call.
- **Stream large files** in chunks instead of loading the whole file into memory.
- **URL decoding** (`%20` and friends) and `Content-Type` detection by content, not only by extension.
- **Split the file** into connection handling, HTTP parsing and request handlers, and unit-test the parser on its own.
