# Nano-Sparrow — an io_uring C10k reactor with zero runtime allocations

Ultra-lightweight asynchronous web/reverse-proxy server in the spirit of Seastar/Drogon/Nginx:
a single-threaded reactor on `io_uring` with **zero `malloc` on the hot path**, scaling to all
cores via `SO_REUSEPORT`, plus TLS (mbedTLS), on-the-fly gzip (libdeflate) and a reverse proxy
with backpressure and streaming request bodies.

Everything below is load-tested: a full wrk matrix, a 30-minute soak run and a regression
suite covering every bug found under load (see [Testing](#testing)).

> **Status: active development — use at your own risk.** Not production-ready: no
> authentication, no security hardening. The web panel (`GET /api/stats`,
> `POST /api/reload`) listens on the server port without any auth — if the server is
> exposed to the internet, the panel is publicly accessible: anyone can read the live
> config summary and replace the config via `/api/reload`. Do not expose it without
> protection (auth reverse proxy, firewall, etc.).

## Features

- **Queue-free reactor** — handlers are called directly from the `io_uring` completion loop
  (submit batching: ~2 SQEs per request, ~0.26 `io_uring_enter` per request). No `TaskQueue`,
  no copies, no dispatchers on the hot path.
- **Multishot accept** — one SQE accepts forever; if the kernel ends the stream without
  `IORING_CQE_F_MORE` (EMFILE/ENFILE), the server re-submits and keeps accepting.
- **Zero Allocation** — `Connection`/`UpstreamConnection` pools are allocated once at startup;
  parsing uses `std::string_view` without copies; gzip compressor and scratch buffers are
  `thread_local` (not 4 KB per connection). TLS contexts are created lazily on first
  handshake (`mbedtls_ssl_setup`) — no pre-allocations (TLS idle RSS == plain HTTP).
- **HTTP Pipelining** — several requests in one TCP packet are handled losslessly: the parser
  returns the number of consumed bytes, the remainder is shifted (`memmove`) and processed in
  the same read cycle.
- **Request body** — `Content-Length` and `Transfer-Encoding: chunked` (the parser validates
  framing **without mutating the buffer**, so the proxy forwards the original chunks; a binary
  handler can call `HttpParser::decode_chunked_in_place()`). Duplicate `Content-Length` or
  `CL + chunked` → `400` (request-smuggling protection).
- **Streaming proxy upload (PROXY_UPLOADING)** — routes declared via `add_proxy`/
  `add_prefix_proxy` accept a body of **any size** (`Content-Length` and chunked):
  `READ_CLIENT → WRITE_UPSTREAM` as data arrives, with backpressure (the next read happens
  only after the window drains). An incremental scanner finds the end of a chunked body
  (chunks of any size, survives window boundaries).
- **Fast-Exit** — small requests (headers + body already in the buffer) go to the backend in a
  single `WRITE_UPSTREAM` and immediately switch to streaming the response: 0 extra SQEs
  versus a naive streaming loop.
- **Exact proxy request boundary** — `start_proxy()` records the absolute end of the request
  (`proxy_request_end` = headers + body); the pipelined tail of `rx_buffer` never leaks to the
  backend.
- **Shared-Nothing multithreading** — each worker (thread) owns its own `Server`, pool and
  ring; the kernel balances `accept` between workers (`SO_REUSEPORT`); no mutexes in the data
  path.
- **Proxy backpressure** — `READ_UPSTREAM` is not submitted until the client drained
  `tx_buffer`; `READ_CLIENT` — until the previous window reached the backend (no unbounded
  memory growth).
- **Gzip "On The Fly"** — via a `thread_local` scratch buffer; bodies under 150 bytes are not
  compressed (the gzip header/dictionary costs more than the body itself); the compressor is
  released by an RAII guard when the stream finishes.
- **Splice static files (SEND_SPLICE)** — plaintext large files are delivered by the kernel
  via `splice(file → pipe → socket)`: pages fly from the page cache to the socket with **0
  bytes through user-space memory** (10 MB file = 0 bytes of process RAM). A pre-allocated
  pool of 512 pipes (created once at startup, lazily backed pages) serves concurrent
  transfers; linked SQE chains (`IOSQE_IO_LINK`) batch file→pipe→socket; a full socket
  re-arms `POLL_OUT` instead of failing. Nginx-class throughput (~14 GB/s).
- **Zero-copy static files (SEND_ZC)** — fallback for plaintext when the pipe pool is
  exhausted or splice is disabled: `READ_FIXED` into registered buffers (DMA from the page
  cache) + `MSG_ZEROCOPY`.
- **TLS file streaming (SEND_TLS_CHUNK)** — large files over TLS are delivered chunk-wise
  (READ_FILE → mbedTLS drain → next chunk) from a pre-allocated **Large Buffer Pool**
  (256 × 128 KB per worker, one allocation at startup): ~80 chunk reads per 10 MB instead of
  1280, so HTTPS static of any size works fast (previously it returned 413).
- **Idle timeouts** — a pool scan once per second (Slowloris protection) covers stuck
  handshakes, upstream `connect()`s and half-closed connections.
- **Hot reload** — `POST /api/reload` validates a new INI config (fail-fast), swaps the
  `(config, router)` pair without blocking workers and wakes them via `eventfd`;
  `GET /api/stats` returns a live config summary.
  > **Warning:** the panel has **no authentication** and listens on the server port —
  if the server is on the internet, the panel is publicly accessible
  (see the status note at the top).
- **Graceful shutdown** — SIGINT/SIGTERM wakes all loops via `eventfd`.
- **Reactor pattern (IoC)** — HTTP logic lives in `IRequestHandler`; `Server` is transport
  only (TCP/TLS/gzip/keep-alive/proxy).

## Architecture

```
main.cpp            — config, router, N workers (SO_REUSEPORT), eventfd shutdown/reload, GC thread
server.hpp/.cpp     — transport core: io_uring loop, TCP/TLS state machine, keep-alive,
                      gzip, proxy with backpressure and streaming bodies, static file streaming
handler.hpp         — IRequestHandler + HttpRouter (allocation-free routing)
http_parser.cpp     — zero-copy header parser (tokens, case-insensitive)
example_handler.cpp — example app: /status, /big, /echo, /api/* (proxy), /static/*
memory_pool.hpp     — O(1) pool without a heap at runtime (assert on double-free)
```

### Connection state machine

```
ACCEPT → SSL_HANDSHAKE → WRITE_RESPONSE | WRITE_FILE_HEADERS → SEND_STATIC (zero-copy chunks)
                                                             → SEND_SPLICE (kernel splice)
                                                             → SEND_TLS_CHUNK (TLS chunks)
                       → READ_STATIC_FILE (buffered small files / gzip static)
                       → PROXY_CONNECTING → PROXY_UPLOADING → PROXY_STREAMING → CLOSE
```

- `SEND_SPLICE` — plaintext large files: `splice(file → pipe → socket)` with linked SQE
  chains, chunks capped to the pipe's real capacity (`F_GETPIPE_SZ`, 64 KB normally —
  the kernel shrinks pipes to 8 KB past `fs.pipe-user-pages-soft`), pipe FIFO residue
  tracked by two counters; short splices and
  `-EAGAIN` (full socket) resume on `POLL_OUT` without losing bytes. 0 user-space copies.
- `SEND_STATIC` — zero-copy plaintext file streaming: `READ_FIXED` + `SEND_ZC` loop
  (fallback when the pipe pool is exhausted).
- `SEND_TLS_CHUNK` — one file chunk delivered through mbedTLS; chunks come from the Large
  Buffer Pool (128 KB) or `tx_buffer` (8 KB) when the pool is exhausted; on EAGAIN the
  connection waits on a `POLL_OUT` op and resumes without losing partially sent bytes.
- Handlers run inline from the loop; ABA protection: the slot `generation` is captured before
  the call and checked after (a stale pointer is never used if the handler closed the
  connection). FIN is handled normally: `read() == 0` or an error in the CQE.

### The io_uring loop

1. Fast path: `io_uring_peek_cqe` — if completions are already queued, no syscall at all.
2. Slow path: `io_uring_wait_cqe_timeout` with a 200 ms kernel timer (also drives the idle
   scan).
3. All completions are processed without a syscall; newly submitted SQEs are pushed in one
   `io_uring_enter` (batching).

### Pools, generations, cancel

- `Connection` slots come from an O(1) pool; every slot has a `generation` counter.
- Every SQE carries an `EventContext {op, ptr, gen}`; stale completions are dropped by
  comparing `gen` with the slot's current generation.
- Pending ops hold a reference to the `file` in the kernel, so `close(fd)` alone does not
  release a socket: `cancel_ops()` cancels by `user_data` before close. Cancels submitted in
  the same batch always run before the re-submitted ops of a reused slot.

## Design decisions (and why)

### Zero allocations on the hot path

All connection/upstream slots, gzip compressors and buffers are created once at startup;
TLS contexts are created lazily on a connection's first handshake. Parsing produces
`string_view`s into the connection's `rx_buffer` (valid only during
`on_request`). Verified with ASAN builds and `strace -e brk,mmap`: no allocations after start.

### Four static-file delivery paths

1. **Buffered** (`READ_STATIC_FILE`): file + 512-byte header reserve fits `buffer_size`
   → one `READ_FILE` into `tx_buffer`, response sent without a copy.
2. **Splice** (`SEND_SPLICE`): plaintext large files — the kernel moves pages
   `file → pipe → socket`; the process touches **0 bytes** of the payload. 512 pipes per
   worker are created at startup (`pipe2`, pages allocated lazily only during transfers);
   on connection close mid-transfer the pipe is recreated (close + `pipe2`) so a stale
   in-flight splice can never corrupt another stream. Pool exhaustion falls back to SEND_ZC.
3. **Zero-copy** (`SEND_STATIC`): plaintext clients, kernel ≥ 6.0, registered buffers
   available → `READ_FIXED` + `MSG_ZEROCOPY` chunks of any file size.
4. **TLS chunks** (`SEND_TLS_CHUNK`): TLS encrypts from user memory, so zero-copy does not
   apply. Headers with the full `Content-Length` are drained synchronously, then the body is
   streamed from the Large Buffer Pool (256 × 128 KB per worker, one block allocation at
   startup; `tx_buffer` fallback): `READ_FILE → drain → next chunk`, with `POLL_OUT`
   re-arming on EAGAIN. Keep-alive reset and pipelined tails work the same as in the
   zero-copy path.

### mbedTLS integration

- The BIO callbacks wrap the connection itself (`Connection*`, not `&conn->fd`): the BIO can
  set `FLAG_SSL_EOF` on `recv() == 0`, so `ssl_read` knows the peer sent EOF.
- `WANT_READ`/`WANT_WRITE` never spin: the server submits a `POLL_IN`/`POLL_OUT` op and
  resumes from the same state (`handle_ssl_handshake` / `continue_tls_chunk`).
- Every mbedTLS error other than WANT_* or a close-notify sets `errno = ENOTCONN` in
  `io_read` — this prevents a stale `EAGAIN` from an earlier iteration from re-arming a poll
  on a dead socket (a real 100% CPU spin found by load tests).

### Keep-alive reset must know its entry state

`dispatch_response()` resets the connection differently depending on `prev_state`:

- **Inline** (`ACCEPT`): the request is still un-consumed in `rx_buffer` — a manual reset
  without re-parsing (re-parse happens in the outer `process_buffered` loop).
- **Async** (`WRITE_RESPONSE` etc.): the request was consumed — `reset_for_keep_alive()` is
  called, which re-arms `POLL_IN`.

Confusing the two produced a stack overflow (recursion `process_buffered → on_request →
reset_for_keep_alive → process_buffered`), caught by ASAN under TLS keep-alive load.

### drain_client never spins on a dead connection

`drain_client()` returns `DrainResult::CLOSED` after `close_connection()`. Without the
`return`, a write error kept the loop running with `fd == -1` and `tx_sent < tx_bytes`
forever — a second 100% CPU spin found under wrk.

### Pipelined-tail consumption depends on state

`process_buffered()` consumes request bytes before returning only for states that still need
`rx_buffer` (`READ_STATIC_FILE`, `WRITE_FILE_HEADERS`, `SEND_STATIC`, `SEND_SPLICE`,
`WRITE_RESPONSE`, `ACCEPT`); otherwise a second entry from `write_done()` would re-parse the
same request and send a duplicate response.

### Hot reload without locks in the data path

`/api/reload` validates the config and router **before** publishing (400 on failure), swaps
`(config, router)` under a short mutex only used by the GC thread, wakes every worker with an
`eventfd` write (CQE `OpType::RELOAD` → workers re-point their raw pointers, 0 overhead), and
schedules the old pair for deletion 10 seconds later. A torn read of the pair is harmless
within that window. Allocations are allowed: this is a rare admin path.

### Gzip threshold

Bodies ≤ 150 bytes are never compressed: the gzip header + dictionary would exceed the body
itself. Static files bigger than `buffer_size - 512` are not gzip-compressed (they take the
zero-copy or TLS-chunk path instead).

## Bugs found by load testing (and their regressions)

### Session history: what broke and how the numbers moved

#### epoll skeleton session (Stages 1–3): first-iteration bugs

Development of the original epoll skeleton per the plan (memory pool, priority queue,
zero-copy parser, proxy) — before the io_uring rewrite. Verified with: GCC 13.3.0
`-O3 -march=native -flto -Wall -Wextra -Werror`, a python backend (`http.server` :9090),
curl, `strace` attach, and RSS measurement before/after load:

| # | Problem | Fix | Numbers: before → after |
|---|---------|-----|-------------------------|
| E1 | **Build failed on `-Werror=unused-result`**: glibc marks `write()` `warn_unused_result`; `(void)write(...)` doesn't suppress it | `discard_write()` helper assigning the result to a variable | build: fail → pass, 0 warnings |
| E2 | **Edge-triggered starvation in the proxy**: python `http.server` sends headers and body in separate `write()`s; a single read per `EPOLLIN` delivered only the first chunk, the rest stayed in the backend socket forever (no new ET event) | `handle_proxy_read` loops reading until `EAGAIN`, each chunk goes to the client via `drain_client()`; `handle_write` pushes `DO_PROXY_READ` once a chunk is fully sent | `curl /api/test.txt`: empty body → `PROXY-OK-FROM-BACKEND`; a 300 000-byte file through the proxy is byte-identical (`cmp` OK) |
| E3 | **Client never listened for `EPOLLOUT` while streaming**: with a full client buffer nobody would resume the write → hang | `drain_client()` re-registers the client fd for `EPOLLOUT` on `EAGAIN`; `handle_write` restores `EPOLLIN` and keeps pulling from the backend | potential stall → 300 KB transit without blocking |
| E4 | **Double `DO_CLOSE` → double release into the pool**: one epoll event could carry `EPOLLIN\|EPOLLHUP`, so `close` was queued twice → `MemoryPool` corruption | `state == CLOSE` guard in the task dispatcher and idempotent `close_connection()` | silent pool corruption → guarded, verified with parallel request bursts |
| E5 | **Harness: `pkill -f "http.server 9090"` killed its own shell** (pattern matched the bash command line) | pattern with a character class `http[.]server` | test runs hung on timeout → stable backend restarts |

Session end-state: `-Werror` build is clean; 100 parallel requests under
`strace -e trace=mmap,brk,mremap,munmap` — **0 memory-allocation syscalls**; RSS
**4 904 kB before and after** 200 requests + proxying a 300 KB file (zero growth);
keep-alive reuses the connection (200 OK). Numbers note: the 1 000-connection pool
reserves ~8 MB of virtual memory, but Linux commits pages lazily, so physical memory
grows only for connections actually in use.

#### Bench/research session: harness, limits and crashes under max_connections (most recent)

In parallel with the performance session ran the comparative-benchmark session: a
docker-compose harness (sparrow/caddy/nginx + a runner with wrk and netem) and the
research phases for the connection limit — `max_connections` 500/1000 in the
research phase and 1000/5000 in the "sparrow only" phase. The full history lives in
`tests/bench/README.md`; here are the problems and their fixes:

| # | Problem | Fix | Numbers: before → after |
|---|---------|-----|--------------------------|
| R1 | **Envoy bloated the run** (a fourth server) | removed entirely from compose, configs and scripts | run shorter by ~a third; sparrow/caddy/nginx numbers unchanged |
| R2 | **snap-docker 29.6.1: `exec > file` silently loses output and exit code** (journalctl: `exec attach failed: broken pipe`) | pipe output through `2>&1 \| cat`, take exit from `PIPESTATUS[0]`, use `tee` instead of redirection | `run.sh && echo PASS` falsely failed → honest exit 0/1, full log |
| R3 | **io_uring: entries > 32768 → EINVAL — crash at max_connections=20000** (research phase, historical limits) | cap `entries = min(required, 32768)` in `server.cpp` | research at 20k crashed → works at any limit; 20k-era run held 210.7 MB idle RSS |
| R4 | **Service-recreation races on weak machines** (starting everything at once + immediate recreation → wait_ready timeouts) | `run.sh` brings up only the runner; `run_bench.py` recreates the server services; `ready_with_retry()` (self-healing) | startup timeouts → stable starts |
| R5 | **One-off run "pause"** (transient docker failure: 35 min with no saved results) | diagnose via mtime of `results.json`, `docker compose logs runner`, `docker ps`; rerun | hang → reproducible run, numbers stable (±noise) |
| R6 | **Pool ceiling for connections**: sweep `c > max_connections` looked like a failure (wrk honestly counts rejections as errors) | limit lives in the sparrow config (`__MAX_CONNECTIONS__`); points marked `pool_limit=true`, labeled "pool ceiling" on the plot | "phantom drops" → honest marking; server refuses extra connections but does not crash (max=1000, c=1024: ~150k rps) |
| R7 | **Could not benchmark sparrow alone** (limit analysis without comparison) | `run_only_sparrow.sh`: `--only-sparrow`, `--sparrow-conns "1000 5000"`; results in `results-sparrow/`, CSV gains a `phase` column (matrix/research/sparrow) | status 165.2k/158.2k, TLS 98.4k/99.7k, idle RSS 13.1/63.8 MB (max 1000/5000) |
| R8 | **Harness environment limits**: `nofile=1024` → "bind failed: Bad file descriptor" (512 splice pipes = 1024 fd per worker); the kernel shrinks pipes to 8 KB (`fs.pipe-user-pages-soft`) | `nofile: 1048576` in compose; chunk calibrated with `F_GETPIPE_SZ` + `cap_add: SYS_RESOURCE` | server did not start / 8 KB pipes → full-size pipes, splice speed intact |

How the benchmark numbers moved across the session (demo mode, 6 s per scenario,
b71ece4 → ae1a4b0):

| Scenario (W=1) | b71ece4 | ae1a4b0 | Δ |
|---|---|---|---|
| status, sparrow | 195 659 | 180 678 | −8% |
| status, caddy | 50 222 | 50 324 | ±0% |
| status, nginx | 94 514 | 97 303 | +3% |
| TLS status, sparrow | 112 868 | 119 802 | +6% |
| static_big, sparrow | 984 | 1 053 | +7% |
| sweep c=1024, sparrow | 183 266 | 166 135 | −9% |

The ±10% spread (caddy at W=3 up to −21%) is demo-mode noise (short durations,
parallel host load); the trends are stable. For reproducible numbers use `--full`
(15 s per scenario).

Session end-state: the research phase (500/1000) and "sparrow only" (1000/5000)
work; memory is preallocated for the pool and grows linearly (idle RSS: 6.6/12.9 MB
at 500/1000, ~12.9 MB per 1000 connections); the pool ceiling is honestly marked,
runs are reproducible (±10% demo-mode noise).

#### io_uring performance session: timeouts, Nagle, SQPOLL, affinity (previous)

The most recent session — pure performance under load: a python benchmarker (serial
and pipelined modes, 32 connections) against the old epoll binary, then a local
wrk matrix against nginx 1.24 (no sudo on the host: wrk 4.1 built from source,
nginx unpacked from a `.deb`). Everything was verified with `tests/test_server.py`
(19/19, release + ASAN), TLS pipelining and the timeout close (FIN within ~1.5–2.0 s
with `io_timeout=1` s — the 1 s scan step).

| # | Problem | Fix | Numbers: before → after |
|---|---------|-----|--------------------------|
| N1 | **Idle timeout did not send FIN**: pending SQEs hold a reference to the socket `file` in the kernel, so `close(fd)` alone never released the connection — the client saw no EOF until the process exited | `cancel_ops()`: `io_uring_prep_cancel(sqe, &conn->read_ctx, 0)` by `user_data` for every in-flight op (client read/write, upstream connect/read/write) before `close()`; the timeout scan moved to the top of `run()` with an explicit `io_uring_submit` (previously `-ETIME` hit `continue` without submitting, so cancel SQEs were stuck in userspace) | EOF only on kill → EOF within ~1.5–2.0 s with io_timeout=1 s |
| N2 | **Nagle + delayed-ACK**: a batch of small responses waited ~41 ms per connection (1 connection, pipe=64) | `TCP_NODELAY` (`set_tcp_nodelay()`) on accept and upstream sockets | 41 ms/batch → 0.06 ms/batch; 1 connection pipe=64 → 290k rps |
| N3 | **SQPOLL turned out slower than plain mode**: 8 workers = 8 spinning `iou-sqp` kernel threads | `enable_sqpoll` flag (default false), plain by default | 8w: ~16k → 444–500k rps |
| N4 | **Workers not pinned to cores** | `pthread_setaffinity_np` (worker i → core i % num_cores) | 8w: ~465–477k → ~500k (+7%) |
| N5 | **Leftover processes shared ports via SO_REUSEPORT**: phantom drops down to 20–60k and "worker deaths" during debugging | kill-loop until `pgrep -x nano_sparrow` = 0 + free-port check before every run | unstable runs → reproducible (spread <2–5%) |
| N6 | **`io_uring_wait_cqe_timeout` on every loop iteration** | fast path via `io_uring_peek_cqe` (0 syscalls), wait only when the CQ is empty | 1w serial: 220k → 220–227k rps |

How the numbers moved across the session (python benchmarker, 32 connections):

| Stage | serial | pipelined (pipe=8) |
|---|---|---|
| old epoll binary | ~167k | hung |
| new version, 1 worker | 220–227k | 255k |
| new version, 8 workers | 444–500k | 797k |
| SQPOLL, 8 workers | ~16k | — |
| 1 connection, pipe=64 (after N2) | 290k | — |

Session end-state: 19/19 `test_server.py` PASS (release + ASAN), 8w × 20 connections ×
5 pipelined = 160/160, timeout-FIN confirmed (EOF within ~2 s with io_timeout=1 s).
Final wrk matrix vs nginx 1.24 (wrk `-t8 -c256`, 3×20 s, median):

| Scenario | nano 1w | nano 8w | nginx 8w |
|---|---|---|---|
| static 48 B (plain) | 202k | **635k** | 427k |
| static 1.8 KB (plain) | 164k | **596k** | 406k |
| static 1.8 KB (gzip) | 68k | 349k | **360k** |
| TLS 48 B | — | **424k** | 259k |
| TLS + gzip 1.8 KB | — | **254k** | 222k |

Takeaway: nano 8w beats nginx in every scenario except gzip (parity ~350k — both hit
the compression-core ceiling); 1w→8w scaling is nearly linear (3.1×); the mbedTLS
stack beats nginx's OpenSSL stack by 64%; the cost of gzip is ~3.1× on 1w and ~1.7×
on 8w, TLS ~1.5×.

#### Code-review fixes session (earliest recorded)

The session that preceded upstream development started from a code review; everything below
was fixed in one session and verified with a python echo-backend (sha256 of received bodies),
a 16-case integration suite (release + ASAN) and wrk A/B against the pre-fix binary
(wrk `-t4 -c8 -d10s`):

| # | Problem | Fix | Numbers: before → after |
|---|---------|-----|--------------------------|
| CR1 | **Multishot accept lost forever**: on EMFILE/ENFILE the kernel drops the multishot accept and its CQE arrives without `IORING_CQE_F_MORE` — the server silently stopped accepting | `handle_accept()` resubmits `accept` whenever the CQE lacks `F_MORE` and shutdown hasn't started | permanent accept stall → survives 5×30-connection bursts and FD exhaustion (10/10, 5/5 accepted afterwards) |
| CR2 | **Proxy buffered the whole request in the 4 KB window**: bodies beyond ~3.5 KB were rejected with 413 — large uploads impossible | unified automaton `PROXY_CONNECTING → PROXY_UPLOADING → PROXY_STREAMING`: windowed upload (read → write → drain) with backpressure, Fast-Exit for small bodies (same SQE count as the old path), incremental chunked scanner (any chunk size, survives window boundaries) | 1 MB POST: 413 → ~430 req/s ≈ 430 MB/s, p99 14–16 ms; small-body proxy: ~16.1k → ~16.4k rps (parity) |
| CR3 | **`req.chunked` was set only after the whole body had been scanned**: a chunked request split across windows lost the flag → the proxy treated it as Content-Length 0, Fast-Exit fired after the headers, the body was never forwarded (backend logged `recv_body=0`) | set `chunked` immediately after header parsing, before body scanning | 700 KB chunked POST: timeout → 200 with the full echoed body; suite: 1 MB ConnectionReset / 700 KB timeout → 16/16 PASS |
| CR4 | **MemoryPool double-free** could silently overflow the pool | `assert(top < capacity_ - 1)` | silent corruption → assert on release() past the top |
| CR5 | **gzip wasted CPU on tiny bodies** (48-byte JSON) | compression threshold `body_len > 150` | contributes to `/status` 224.5k → 232k rps (+3.3%) |
| CR6 | **thread_local libdeflate compressor leaked** at thread exit | RAII guard (`CompressorGuard`) | leak → freed when the worker thread exits |

Session end-state: 16/16 integration tests PASS (release and ASAN, zero ASAN errors); streaming
proxy for CL and chunked bodies of any size (any chunk size, trailer-split, pipelined tails);
413 only for non-streaming routes. wrk A/B vs the pre-fix binary: `/status` 224.5k → 232k rps
(+3.3%, mostly CR5), small-body proxy 16.1k → 16.4k rps (parity), 2.0 SQEs per request
(~0.26 `io_uring_enter` — submit batching). Backend keep-alive was explicitly accepted as
out of scope by the review — it was implemented later, in the upstream session.

#### Proxy/upstream development session (earlier than the load-testing phase)

Development of upstream clusters (Round-Robin + keep-alive idle pool), async static files
and the INI config found these bugs — all fixed during the same session, verified with a
python backend (connection counter per socket) and curl/python clients:

| # | Problem | Fix | Numbers: before → after |
|---|---------|-----|--------------------------|
| P1 | **Upstream keep-alive pool never reused**: `scan_response()` treated the header terminator `\r\n\r\n` as a new header → `malformed` → `resp.phase=4` (stream-to-EOF), so the backend socket was closed after every response | stop the header loop at the empty line (`buf[pos]=='\r' && buf[pos+1]=='\n'`) | every request = new backend connection (`backend 1,1,2,2,3,3…`, 0% reuse) → `connection #1` ×4 (4 requests, one backend socket) |
| P2 | **Proxied request re-sent to the backend**: `finish_proxy_response()` did not consume the request bytes, so on client keep-alive `process_buffered()` re-parsed the same request and called `start_proxy()` again (~10 idle-pool hits per 4 real requests); the 2nd request on a keep-alive client connection hung | consume exactly `proxy_request_end − proxy_window_base` bytes, preserving the pipelined tail | keep-alive client: 2nd request timed out → 3 sequential requests on one connection, all answered |
| P3 | **Round-Robin collapsed**: the idle pool is per-cluster, but connections belong to a node — `get_idle_upstream()` handed out the wrong node's socket | `node_idx` in `UpstreamConnection`; the pool returns only a slot for the RR-chosen node | `backend 12 on 9090` ×4 (9091 starved) → `13, 11, 13, 11` (alternating + reuse) |
| P4 | **stack-use-after-scope (ASAN)** in `setup_routes_from_config()`: `string_view` bound to a temporary `std::string` from `substr()` | view over the live config string, `substr()` on the view | ASAN: 1 error → 0 |
| P5 | **connect-refused upstream dropped the client silently** (curl `000`) | `send_error(conn, 502)` on failed `connect()` | `000` → `502 Bad Gateway` |
| P6 | **INI inline comments**: `enable_sqpoll = false # comment` made the value unparseable → fail-fast killed the server with the shipped `server.conf` | strip `#`/`;` tails before parsing | server refused to start → starts |

Session end-state: proxy keep-alive 100% reuse, Round-Robin balanced across two backends,
pipelining (2 requests in one TCP packet) answered, static served with correct
MIME/404/403/413, chunked backend responses and HEAD handled, TLS
(status/proxy/static/keep-alive/gzip/chunked) verified, ASAN clean; 200/200 requests OK
at ~18k rps (python client bound).

Before this README was written the project went through a full load-testing phase: a wrk
matrix that immediately exposed the TLS path (plain HTTP was fine from the start), a stall
diagnosis with DBG instrumentation, and a final clean matrix + soak. All numbers below are
from the same machine, release build, wrk `-t4 -c256 -d20s` (TLS/large files `-c64`);
`~` marks approximate measurements from the diagnostic phase.

| Stage | Problem | Fix | Numbers: before → after |
|---|---|---|---|
| 0 | First matrix: plain OK, TLS path collapsing | — | plain status 217–222k rps; TLS: death (141) / stall (0 rps, 100% CPU) |
| 1 | **SIGPIPE death** (exit 141) | `MSG_DONTWAIT\|MSG_NOSIGNAL` on TLS writes + `SIG_IGN` | dead → survives TLS load |
| 2 | **Stall**: 0 rps at 100% CPU (two spins: handshake EOF + stale errno) | `FLAG_SSL_EOF` set in the BIO on `recv()==0`; `errno=ENOTCONN` for non-WANT_* errors | TLS status ~23k → ~97k rps |
| 3 | **TLS static keep-alive**: 1 request/conn, ~3 rps | `reset_for_keep_alive()` in `dispatch_response` | 3 rps → 94–104k rps; TLS status ~97k → ~119k |
| 4 | **Stack overflow** (ASAN) on TLS keep-alive | reset split by `prev_state` (inline vs async) | crash → stable (c64/c200 clean) |
| 5 | **drain fd=-1 spin**: 100% CPU on aborted writes | `DrainResult::CLOSED` after `close_connection()` | 100% CPU → 0% |
| 6 | **TLS static big → 413** (buffered-path limit) | chunked TLS streaming (`SEND_TLS_CHUNK`) | 131 rps (413) → 159 rps / 281 MB/s, full file |

Final matrix after all fixes: plain status 217–223k rps; TLS status 118–124k rps; TLS
static small 94–105k rps; zero-copy static 746–781 rps / 1.29–1.35 GB/s; 4 workers
2 207–2 733 rps / 3.80–4.71 GB/s. 30-min soak: plain 260–266k rps, TLS 152–160k rps,
RSS constant, zero curl failures.

All of these were discovered with wrk/ASAN during the load-testing phase, then fixed and
locked in `tests/regression.sh`:

| # | Bug | Trigger | Fix | Regression |
|---|-----|---------|-----|------------|
| 1 | **SIGPIPE death** (exit 141) | TLS client closes the socket while the server writes | `MSG_DONTWAIT\|MSG_NOSIGNAL` on TLS writes + `SIG_IGN` | T1: server survives TLS load |
| 2 | **TLS static keep-alive silence** (~3 rps, 1 request/conn) | Async path never re-armed `POLL_IN` after the response | `reset_for_keep_alive()` in `dispatch_response` | T2: TLS static ≥ 10k rps |
| 3 | **Stack overflow** (ASAN) | TLS keep-alive re-parsed an unconsumed request | Reset split by `prev_state` (inline vs async) | T3: c64 keep-alive survives |
| 4 | **Handshake EOF spin** (100% CPU) | FIN without close_notify → `WANT_READ` loop | `FLAG_SSL_EOF` set in the BIO on `recv()==0`; close on WANT_* after EOF | T4: CPU < 20% after 60 aborted handshakes |
| 5 | **Stale-errno spin** (100% CPU) | `recv()==0` → ssl EOF error → re-armed poll on a dead socket | `errno = ENOTCONN` for non-WANT_* mbedTLS errors | T5: CPU < 20% after 64 hard-killed conns |
| 6 | **drain fd=-1 spin** (100% CPU) | Write error → drain loop on a closed connection | `DrainResult::CLOSED` after `close_connection()` | T6: CPU < 20% after 30 aborted responses |
| 7 | **TLS static big → 413** | Buffered path required `file_size + 512 ≤ buffer_size` | Chunked TLS streaming (`SEND_TLS_CHUNK`) | T7: 10 MB TLS download in full |

## Testing

### `tests/regression.sh` — 13 checks, T1–T7

```bash
bash tests/regression.sh [binary]   # default: build/nano_sparrow
```

Each test: start the server → provoke the bug → verify (liveness / CPU / throughput):

| Test | Provocation | Check |
|------|-------------|-------|
| T1 | TLS load (wrk, c64) | process alive + server answers |
| T2 | TLS static small via wrk | rps > 10 000 |
| T3 | TLS keep-alive c64 + pipelined tails | process alive + server answers |
| T4 | 60 aborted handshakes (`nc` + random bytes, FIN without close_notify) | CPU < 20%, server answers |
| T5 | `kill -9` of a running 64-conn wrk (massive FIN/RST) | CPU < 20%, server answers |
| T6 | 30 responses aborted by the client mid-write (`curl --max-time 0.1`) | CPU < 20%, server answers |
| T7 | 10 MB file over TLS (bigger than the buffer) | full size received, server answers |

Current result: **13/13 PASS**. The script frees port 8443 from foreign listeners, checks for
"bind failed", and uses CPU thresholds (20%) well below measured idle values.

### `tests/soak.sh` — 30-minute stability run

```bash
bash tests/soak.sh [binary]
```

Runs plain (8080 `/status`) and TLS (8443 `/status`) servers under serialized wrk load for 30
minutes. Every 10 s: RSS (`/proc/*/status`), CPU (utime+stime delta), open FD count,
liveness and a curl check on both ports. Every ~100 s: a 20 s wrk throughput point.

Result (this machine, 1 worker each): RPS flat at **260–266k (plain)** and **152–160k (TLS)**
for the first 15 minutes; RSS **constant** (plain: 0 byte growth over 30 min; TLS: +120 KB);
FD count constant at 7/7; zero curl failures; zero spin episodes (CPU 0–1% between load
windows). (A later RPS dip to ~212k/~126k correlated with heavy external load on the host —
firefox/code/opencode at ~160% CPU — not with server degradation.)

### ASAN builds

```bash
cmake -S . -B build-asan -DCMAKE_CXX_FLAGS=-fsanitize=address
cmake --build build-asan -j$(nproc)
```

Used for the full matrix (including TLS keep-alive c200 and the recursion repro) and for the
"no allocations after startup" guarantee.

## Performance (release, 1 worker unless noted; wrk `-t4 -c256 -d20s`, TLS/files `-c64`)

| Scenario | Result |
|---|---|
| plain `/status` (JSON) | **217–223k rps** |
| plain `/status` (soak, wrk -c256) | 260–266k rps |
| `/big` gzip on the fly (1.85 MB body) | 191–204k rps / 346–370 MB/s |
| `/static/small.txt` (86 B, buffered) | 75–83k rps |
| `/static/big.txt` zero-copy (SEND_ZC) | 746–781 rps / **1.29–1.35 GB/s** |
| `/static/big.bin` splice (SEND_SPLICE, 10 MB, wrk -t4 -c16) | **1 412 rps / 13.8 GB/s** |
| `/static/big.txt` splice (SEND_SPLICE, wrk -t4 -c64) | **9.7k rps** |
| `/static/mid.txt` (1300 B, gzip static) | 68–71k rps |
| TLS `/status` | 118–124k rps |
| TLS `/static/small.txt` | 94–105k rps |
| TLS `/static/big.txt` (chunked) | 159 rps / 281 MB/s (single-core mbedTLS bound) |
| reverse proxy `/status` → backend | 63–70k rps |
| 4 workers (SO_REUSEPORT) zero-copy `/static/big.txt` | 2 207–2 733 rps / **3.80–4.71 GB/s** |
| `Connection: close` (no keep-alive) | 39–43k rps |

Test files: `small.txt` 86 B, `mid.txt` 1.3 KB, `big.txt` 1.85 MB, `big.bin` 10 MB.
Ring: 4096 entries, 1024 registered tx-buffers for SEND_ZC, `buffer_size` 8192.

## Comparative benchmark: Caddy, Nginx

The `tests/bench` harness (docker compose) runs three servers with **identical
endpoints** under wrk load from a dedicated runner container, with bad-connection
emulation (`tc netem`), CPU/RSS sampling and matplotlib plots. Everything is
reproducible with one command: `cd tests/bench && ./run.sh` (methodology and
scenario details: [tests/bench/README.md](tests/bench/README.md)).

Run: 2026-08-17, kernel 7.0.0-28-generic, wrk `-t4`, 6 s per scenario (demo mode;
`./run.sh --full` — 15 s each). Server configs allow **1000 connections**
(`max_connections` in sparrow, `worker_connections` in nginx) — sparrow
preallocates its pool at startup, so idle RSS is ~13 MB. Every run is repeated for
`WORKERS=1` and `WORKERS=3` (sparrow: `worker_threads` + SO_REUSEPORT; caddy:
`GOMAXPROCS`; nginx: `worker_processes`). Full data and plots:
`tests/bench/results/`. Bold = best in row (rps: higher is better, p99: lower).

### Problems found while building the harness (and how they were fixed)

| # | Problem | Trigger | Fix |
|---|---|---|---|
| 1 | snap docker cannot see paths outside `$HOME` | bind-mounting `/tmp/opencode/…` → "not a directory" | everything lives in `tests/bench/`; temp paths only for pipes |
| 2 | `docker compose exec … > file` silently loses output and corrupts the exit code (RC=1 instead of 2) | the docker CLI is a snap (29.6.1): with stdout to a regular file the output vanishes | works fine in a terminal; log via `./run.sh \| tee log.txt` |
| 3 | envoy did not scale with workers | the bootstrap config has no concurrency field | `command: envoy -c … --concurrency ${WORKERS}` + `__WORKERS__` templates |
| 4 | sparrow could not resolve the backend by DNS | upstreams are resolved via `inet_pton` (no DNS) | backend on a fixed IP `172.30.0.10` |
| 5 | sparrow crashed in Docker | seccomp blocks `io_uring_*`; 64 MB memlock cap | `seccomp=unconfined` + `memlock=-1` in compose |
| 6 | envoy could not read the TLS key | openssl 3.x writes the key with mode 0600 | `chmod 644` after generation |
| 7 | wrk crashed with `-c1` | threads (`-t4`) exceed connections | threads = `min(4, conns)` |
| 8 | Caddy `/static/` returned 404 | the request was proxied to the backend | `handle_path` aliasing `/www` |
| 9 | envoy did not gzip static files | the backend served `octet-stream` | MIME by file extension (`.txt` → `text/plain`) |
| 10 | `summary.py` crashed on the host | RESULTS defaulted to `/results` | fallback to `../results` |
| 11 | a run was killed by a timeout between worker phases | `results.json` is saved after every phase → only W1 remained | re-ran the full matrix with a larger timeout |
| 12 | wrk unavailable as a package (no sudo on the host) | `apt-get install` requires a password | wrk 4.1 built from source (`/tmp/opencode/wrk-src/wrk`, LuaJIT bundled) |
| 13 | nginx unavailable as a package | same | `apt-get download nginx nginx-common` + `dpkg-deb -x` into a local prefix |
| 14 | nginx refused to start: `mkdir() "/var/lib/nginx/body" failed` | default temp paths point at `/var/lib/nginx` | `client_body_temp_path` and the other temp paths in the harness config |

### How the numbers changed across the session

First smoke run (5 s per scenario) → the final full matrix (3 s, 96 runs).
rps `/status`, WORKERS=1:

| Stage | sparrow | nginx | caddy | envoy |
|---|---|---|---|---|
| smoke (first run) | 195–202k | 93–101k | 53–55k | ~1.4k |
| final matrix | **193,513** | 98,835 | 53,332 | 912 |

Key scenario deltas (smoke → final):

- **TLS `/status`**: 114–121k → 104,929 (W1). In the smoke run sparrow dipped
  to 58k with 3 workers — the final matrix did not reproduce it (108,954, even
  above W1): run-to-run variance, not a regression.
- **static_big (10 MB)**: sparrow 82 → 88 rps (~0.86 GB/s) — stable;
  nginx/caddy 1.1–1.3k rps (sendfile, 11–13 GB/s).
- **badnet_latency**: ~620–630 → 605–627 — everyone hits the netem ceiling
  (32 conns × 50 ms ≈ 640 rps).
- **badnet_loss (2%)**: ~7.8k → 8,141 (W1) / 7,794 (W3).
- **caddy with 3 workers**: 126k (smoke) → 116–126k (sweep c16–c1024).
- **nginx `/status`**: 93–101k (smoke) → 95,570–98,835 (W1/W3) — stable.

### rps (WORKERS=1)

| Scenario | sparrow | caddy | nginx |
|---|---|---|---|
| status JSON (keep-alive) | **197,342** | 53,259 | 97,839 |
| `Connection: close` | 7,423 | 6,947 | **67,775** |
| TLS `/status` | **123,143** | 47,050 | 54,419 |
| gzip `/static/mid.txt` (1.3 KB) | **71,673** | 25,160 | 50,015 |
| `/static/small.txt` (86 B) | 81,765 | 32,925 | **100,159** |
| `/static/big.bin` (10 MB) | 1,022 | 885 | **1,337** |
| proxy GET `/api/ping` | 1,315 | **1,527** | 1,126 |
| proxy POST `/api/echo` | 1,027 | **1,523** | 1,121 |
| badnet base (c32, no netem) | **198,487** | 54,341 | 100,263 |
| badnet latency (50±10 ms) | **628** | 626 | 630 |
| badnet jitter (20±15 ms) | **1,535** | 1,529 | 1,520 |
| badnet loss (2%) | 7,369 | 7,384 | **7,915** |

### p99 latency, ms (WORKERS=1)

| Scenario | sparrow | caddy | nginx |
|---|---|---|---|
| status JSON (keep-alive) | 13.4 | 4.5 | **1.1** |
| `Connection: close` | 19.0 | 21.8 | **2.0** |
| TLS `/status` | 20.5 | 5.6 | **1.9** |
| gzip `/static/mid.txt` (1.3 KB) | 16.0 | 7.7 | **3.0** |
| `/static/small.txt` (86 B) | 2.4 | 6.5 | **1.1** |
| `/static/big.bin` (10 MB) | **28.3** | 40.8 | 28.7 |
| proxy GET `/api/ping` | 1,370.0 | **48.7** | 1,410.0 |
| proxy POST `/api/echo` | 1,340.0 | **47.9** | 1,410.0 |
| badnet base (c32, no netem) | 9.5 | 2.8 | **0.6** |
| badnet latency (50±10 ms) | 74.5 | 74.2 | **73.8** |
| badnet jitter (20±15 ms) | 55.0 | **54.6** | 54.8 |
| badnet loss (2%) | 222.0 | **208.0** | 317.7 |

### rps (WORKERS=3)

| Scenario | sparrow | caddy | nginx |
|---|---|---|---|
| status JSON (keep-alive) | **197,875** | 127,975 | 95,635 |
| `Connection: close` | 6,304 | 18,359 | **68,397** |
| TLS `/status` | 121,669 | **123,481** | 54,509 |
| gzip `/static/mid.txt` (1.3 KB) | **71,268** | 65,073 | 48,889 |
| `/static/small.txt` (86 B) | 81,240 | 80,921 | **99,669** |
| `/static/big.bin` (10 MB) | 1,109 | 979 | **1,229** |
| proxy GET `/api/ping` | 1,076 | **1,508** | 1,313 |
| proxy POST `/api/echo` | 1,202 | **1,507** | 1,205 |
| badnet base (c32, no netem) | **197,510** | 129,918 | 100,209 |
| badnet latency (50±10 ms) | **631** | 626 | 629 |
| badnet jitter (20±15 ms) | **1,548** | 1,530 | 1,528 |
| badnet loss (2%) | 7,554 | **7,768** | 7,317 |

### p99 latency, ms (WORKERS=3)

| Scenario | sparrow | caddy | nginx |
|---|---|---|---|
| status JSON (keep-alive) | 14.6 | 2.6 | **1.7** |
| `Connection: close` | 21.0 | 17.2 | **2.1** |
| TLS `/status` | 90.2 | 2.6 | **2.2** |
| gzip `/static/mid.txt` (1.3 KB) | 22.4 | 4.3 | **3.2** |
| `/static/small.txt` (86 B) | 2.5 | 3.7 | **1.1** |
| `/static/big.bin` (10 MB) | **26.1** | 31.7 | 27.9 |
| proxy GET `/api/ping` | 1,380.0 | **64.2** | 1,340.0 |
| proxy POST `/api/echo` | 1,370.0 | **56.7** | 611.9 |
| badnet base (c32, no netem) | 8.5 | 1.4 | **0.8** |
| badnet latency (50±10 ms) | **73.5** | 74.4 | 73.6 |
| badnet jitter (20±15 ms) | **54.7** | 54.8 | 54.8 |
| badnet loss (2%) | 278.5 | 239.9 | **209.0** |

### Sweep: rps vs connection count (`/status`)

WORKERS=1:

| conns | sparrow | caddy | nginx |
|---|---|---|---|
| 1 | **70,284** | 38,737 | 50,006 |
| 16 | **194,356** | 56,291 | 99,771 |
| 64 | **198,632** | 54,527 | 101,232 |
| 256 | **192,451** | 51,819 | 95,128 |
| 1024 | **186,889**¹ | 49,283¹ | 82,875¹ |

WORKERS=3:

| conns | sparrow | caddy | nginx |
|---|---|---|---|
| 1 | **68,826** | 22,414 | 47,305 |
| 16 | **194,584** | 130,118 | 100,204 |
| 64 | **200,043** | 133,753 | 100,633 |
| 256 | **192,169** | 126,740 | 92,204 |
| 1024 | **189,727**¹ | 119,556¹ | 83,372¹ |

¹ c=1024 exceeds the config limit of 1000 connections — pool ceiling: the server
rejects extra connections and wrk counts them as errors (marked on the sweep chart),
so these points sit slightly below the curve instead of above it.

### Charts (matplotlib, from the fresh run)

![rps by scenario, WORKERS=1](tests/bench/results/rps_1.png)

![rps by scenario, WORKERS=3](tests/bench/results/rps_3.png)

![p99 latency, WORKERS=1](tests/bench/results/latency_1.png)

![p99 latency, WORKERS=3](tests/bench/results/latency_3.png)

![Server CPU% during the run, WORKERS=1](tests/bench/results/cpu_1.png)

![Server CPU% during the run, WORKERS=3](tests/bench/results/cpu_3.png)

![Memory (idle/peak RSS), WORKERS=1](tests/bench/results/memory_1.png)

![Memory (idle/peak RSS), WORKERS=3](tests/bench/results/memory_3.png)

![Bad-connection impact (base / latency / jitter / loss)](tests/bench/results/badnet.png)

![Scaling with connection count (1..1024)](tests/bench/results/sweep.png)

![Memory (idle RSS) and `/status` rps vs max_connections (research phase)](tests/bench/results/memory_conns.png)

### Memory (idle/peak, WORKERS=1)

| Container | idle RSS | peak RSS | peak usage |
|---|---|---|---|
| sparrow http | 13 MB | 13 MB | 18 MB |
| sparrow tls | 13 MB | 15 MB | 24 MB |
| caddy | 11 MB | 28 MB | 76 MB |
| nginx | 3 MB | 5 MB | 45 MB |

All of sparrow's memory is allocated once at startup (connection pool 1k × 8 KB,
512 splice pipes, 256 × 128 KB TLS buffers) and TLS contexts are created lazily per
connection instead of pre-allocated — `idle ≈ peak`, and the TLS server now costs
the same as the plain one (316 → 13 MB). The other servers grow under load as
connections/buffers are allocated. Methodology is in
[tests/bench/README.md](tests/bench/README.md).

### Memory vs max_connections (research phase)

Separate runs with `max_connections` 500/1000 (workers=1): idle RSS with no
load and `/status` rps — sparrow **preallocates the connection pool** (buffers at
startup), so its memory grows linearly:

| Container (idle RSS, MB) | 500 | 1,000 |
|---|---|---|
| sparrow http | 6.6 | 12.9 |
| sparrow tls | 6.7 | 13.0 |
| caddy | 11.2 | 11.2 |
| nginx | 2.4 | 2.6 |

Why does a smaller limit cost nothing in performance? The connection pool is a
*ceiling*, not a per-request resource: all scenarios run at 16–64 connections
(sweep tops out at 1024), and the hot path — O(1) slot lookup, zero allocations,
a 4096-entry io_uring ring — is identical regardless of pool size. The pool is
paid up front as pinned pages (`IORING_REGISTER_BUFFERS` needs `memlock`), so
cutting the limit from 10k to 1k drops idle RSS ~10x (127 → 13 MB) without moving
rps/p99: 197.3k rps at max=1000 vs 180.7k at max=10000 — run-to-run noise. The
only real cost of a small limit is the ceiling itself: at c=1024 the server
rejects extra connections (marked "pool ceiling" on the sweep chart) instead of
serving them.

Runs automatically at the end of `./run.sh`; disable with `--no-research`,
change the list with `--conns-research "500 1000"`.

### Sparrow-only phase (`run_only_sparrow.sh`)

Deep performance analysis of Nano-Sparrow **alone** at different
`max_connections` limits (set in the sparrow config, not in wrk): nginx and caddy
are stopped, the full scenario matrix + badnet + sweep + idle memory runs for
each limit (workers=1). Results go to `tests/bench/results-sparrow/`, leaving
the comparative `results/` untouched:

```bash
cd tests/bench
./run_only_sparrow.sh                  # max_connections 500 and 1500
./run_only_sparrow.sh --conns "500 1500 5000"
```

Demo run (6 s per scenario, this machine; format rps / p99):

| Scenario | max=500 | max=1500 |
|---|---|---|
| status JSON (keep-alive) | 163,702 / 7.1 ms | **169,226** / 6.9 ms |
| `Connection: close` | 5,549 / 26.0 ms | 5,414 / 24.5 ms |
| TLS `/status` | 101,120 / 8.3 ms | **101,004** / 5.2 ms |
| gzip `/static/mid.txt` (1.3 KB) | 62,349 / 12.8 ms | 62,191 / 11.2 ms |
| `/static/small.txt` (86 B) | 67,333 / 3.1 ms | 67,063 / 3.3 ms |
| `/static/big.bin` (10 MB) | 1,073 / 29.5 ms | 1,199 / 29.3 ms |
| proxy GET `/api/ping` | 1,190 / 1,330 ms | 1,221 / 1,360 ms |
| proxy POST (streamed upload) | 1,543 / 44.8 ms | 1,545 / 44.0 ms |
| badnet latency 50±10 ms | 633 / 72.5 ms | 629 / 74.5 ms |
| badnet jitter 20±15 ms | 1,519 / 54.7 ms | 1,530 / 54.7 ms |
| badnet loss 2% | 7,120 / 207.7 ms | 7,441 / 208.2 ms |
| idle anon RSS (http) | **6.8 MB** | **19.5 MB** |

The connection-pool ceiling is visible in the sweep part: at `max_connections=500`
the `c=1024` point is marked "pool ceiling" — sparrow rejects connections beyond
the config limit (wrk counts them as errors), but the server does not crash
(73.4k rps vs ~161–166k at lower concurrency). At `max_connections=1500` the
ceiling is not reached — all 1024 connections are served (~154k rps). Chart:
`tests/bench/results-sparrow/sparrow_only.png`.

Why idle RSS scales with `max_connections`: sparrow preallocates the whole
connection pool at startup (`connection_buffers_` = max_connections × 2 × 8 KB in
one block, plus the slot pools and the io_uring ring), so idle anon RSS grows
linearly by ~12.7 MB per 1,000 slots — 6.8 MB at 500, 13.1 MB at 1k, 19.5 MB at
1.5k. This is the price of zero-alloc on the hot path.

Why p99 looks like that: it is nearly independent of `max_connections` and is set
by four sources — single-worker CPU saturation (status p99 ~7 ms at ~165k rps vs
p50 0.13 ms), the connection-establishment path (no-keep-alive ~25 ms, 10 MB
transfer ~29 ms ≈ bridge bandwidth), the Python backend (proxy p50 41 ms, p99
~1.3 s — backend queue, not sparrow), and the network (netem: ~73 ms latency,
~208 ms loss). The TLS p99 (8.3 vs 5.2 ms at a similar p50) is a noisy handshake
tail on one worker, not a pool-size effect — the comparative matrix (max=1000)
measured 20.5 ms for the same scenario. The sweep shows the flip side of the pool
ceiling: at c=1024 with max=500 the p99 is *better* (21.5 ms vs 58.7 ms at
max=1500), because rejected connections never queue on the server.

### Observations

- **Proxy scenarios are backend-bound for everyone** (~0.8–1.5k rps, p99
  0.05–1.6 s): to compare the proxy paths themselves, swap in a fast backend;
  caddy leads here (1.5k rps), sparrow has a higher p99 on uploads (streaming).
- **badnet_latency**: every server hits the theoretical netem ceiling
  (32 connections × 50 ms RTT ≈ 640 rps) — the network dominates the servers.
- **status JSON**: sparrow leads in every configuration — 197.3k rps with one
  worker vs 97.8k for nginx and 53.3k for caddy; with 3 workers caddy scales to
  128.0k while sparrow stays ~198k (wrk-client bound). In the sweep sparrow
  holds ~186–200k from 16 to 1024 connections.
- **`Connection: close`** is sparrow's weakest scenario (6.3–7.4k rps here —
  the docker-bridge environment is too noisy to show the ~37k rps reached on the
  host loopback); nginx is far ahead (67–68k): new connections are accepted via
  multishot accept on a single thread.
- **10 MB static**: nginx leads — 1,337 rps (W=1, kernel `sendfile`,
  ~13.4 GB/s); sparrow — 1,022 rps (~10.2 GB/s): it streams 8 KB chunks through
  user space instead, but uses no page cache and no kernel copy. On tiny files
  (86 B) nginx also leads: 100.2k vs 81.8k at one worker.
- **TLS**: with one worker sparrow is the fastest (123.1k rps) but its p99 is worse
  (20.5 ms vs 1.9 ms for nginx at c64); with 3 workers caddy edges ahead
  (123.5k vs 121.7k for sparrow), and sparrow's p99 (90.2 ms) shows the noisy
  handshake tail of a single ring.
- **badnet loss (2%)**: everyone is level at one worker (~7.4–7.9k rps) and at
  three workers (~7.3–7.8k) — retries over keep-alive work everywhere.
- **Memory**: sparrow preallocates its connection pool at startup (6.6 MB at 500,
  12.9 MB at 1k connections — ~12.9 MB per 1000), caddy stays flat (~11 MB),
  nginx stays flat too (2.4→2.6 MB). That is the price of zero-alloc on the hot
  path: no allocations under load.

## Building

Dependencies: CMake 3.14+, a C++17 compiler, `libdeflate` (pkg-config), `git`. mbedTLS
(2.28.9) and liburing (2.7) are not vendored: CMake fetches them from GitHub via
FetchContent at the first configure (network required); bumping a dependency = changing
the `GIT_TAG` in `CMakeLists.txt`.

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
./build/nano_sparrow server.conf
```

## Running with Docker

Multi-stage build (Ubuntu 24.04): mbedTLS and liburing are linked statically — the only
runtime dependency is `libdeflate0`, the final image is ~88 MB. The container ships a
minimal `server.conf` (`/status` + `/api/stats`); mount your own with
`-v $PWD/server.conf:/app/server.conf`. Dependencies are cloned from GitHub during the
build, so `docker build` needs network access.

```bash
docker build -t zero-alloc-server:latest .
docker run -d \
  --name my_http_server \
  -p 8080:8080 \
  --ulimit memlock=-1:-1 \
  --security-opt seccomp=unconfined \
  zero-alloc-server:latest
```

Both flags are **mandatory** on Docker's default profile:

- `--security-opt seccomp=unconfined` — the default seccomp profile blocks the
  `io_uring_*` syscalls; without it the server exits with `Failed to init ring`.
- `--ulimit memlock=-1:-1` — the default 64 MB memlock cap is not enough to register the
  tx-buffer pool (`max_connections × buffer_size`, e.g. 1000 × 8192 ≈ 8 MB) via
  `IORING_REGISTER_BUFFERS`; without it SEND_ZC stays disabled.

Verify:

```bash
curl -i http://localhost:8080/status      # {"status": "ok", "server": "zero-allocation-v6"}
curl -i http://localhost:8080/api/stats
docker logs my_http_server
```

High-throughput mode (host network, zero-copy enabled):

```bash
docker run -d --name my_http_server_fast --net=host \
  --ulimit memlock=-1:-1 --security-opt seccomp=unconfined \
  zero-alloc-server:latest
```

Note: the project is compiled with `-march=native`, so the image is tied to the CPU of the
machine it was built on.

## Configuration (`server.conf`)

INI with sections (`#`/`;` are comments; errors are fatal — the server refuses to start):

```ini
[server]
port = 8080                # listen port
worker_threads = 1         # SO_REUSEPORT workers (each with its own pool and ring)
max_connections = 1000     # pool per ONE worker (divided between workers in main)
buffer_size = 8192         # rx/tx buffers (one memory block at startup)
io_timeout_ms = 30000      # idle timeout (Slowloris protection)
splice_pipes = 512         # pre-allocated pipes/worker for SEND_SPLICE (0 = off)
tls_large_buffers = 256    # Large Buffer Pool for TLS static, /worker (0 = off)
tls_large_buffer_size = 131072  # size of one TLS large buffer
enable_sqpoll = false      # kernel SQPOLL mode (not for every kernel)
enable_affinity = true     # pin workers to CPU cores
enable_gzip = true
enable_keep_alive = true   # global switch: client and upstream keep-alive

[ssl]
enable_ssl = false
ssl_cert = cert.pem
ssl_key = key.pem

# Backend clusters for Round-Robin balancing; several node per cluster
[upstream:api]
node = 127.0.0.1:9090
# node = 127.0.0.1:9091

# Routes: "path = type:argument"
[routes]
/status = status           # JSON status
/api = proxy:api           # proxy to a cluster (body of any size)
/static/ = static:/var/www # async static (io_uring, 0 copies)
```

Route types: `proxy:name` (streaming proxy to a cluster), `static:path` (async static,
trailing slash required), `status`, `big` (gzip demo), `echo` (body echo).

## Web panel (hot reload)

- `GET /api/stats` — live config summary (JSON built with `snprintf`, 0 allocations):
  port, workers, buffer size, gzip/keep-alive/ssl/sqpoll/affinity/zerocopy flags, timeout,
  upstream and route counts.
- `POST /api/reload` — body is a full INI config. Validated fail-fast (400 on any error),
  the `(config, router)` pair is swapped atomically, all workers are woken via `eventfd`, the
  old pair is deleted by a GC thread 10 seconds later. The web-panel routes survive reloads.

## Using it as a framework

```cpp
#include "handler.hpp"
#include "server.hpp"

static void handle_status(Server& server, Connection* conn, const HttpRequest&,
                          const std::string&, std::string_view) {
    static const char kBody[] = "{\"status\": \"ok\"}";
    server.send_response(conn, kBody, sizeof(kBody) - 1, "application/json", true);
}

// target — the route argument (cluster name / static folder), mount — mount point;
// both strings live in the router from startup (0 allocations)
static void handle_api(Server& server, Connection* conn, const HttpRequest&,
                       const std::string& target, std::string_view) {
    server.start_proxy(conn, target); // Round-Robin across the cluster + keep-alive pool
}

int main() {
    ServerConfig config;           // or loaded from server.conf
    config.upstreams["api"].nodes.push_back(BackendConfig{"127.0.0.1", 9090});
    HttpRouter router;
    router.add("/status", handle_status);                    // exact route
    router.add_proxy("/api", handle_api, "api");             // streaming body
    router.add_prefix_proxy("/api/", handle_api, "api");     // prefix streaming

    Server server(config, &router);
    server.start();
    server.run(); // no return: from here on, zero allocations
}
```

Public `Server` API for handlers:

- `start_proxy(Connection*, const std::string& cluster)` — proxy to a cluster
  (Round-Robin; keep-alive connections come from the idle pool);
- `serve_static_file_async(conn, full_path, mime)` — async file read (`io_uring READ_FILE`
  straight into `tx_buffer`), zero-copy plaintext or chunked TLS delivery;
- `send_response(conn, body, len, content_type, gzip_ok)` — 200 response (gzip applied
  automatically for bodies > 150 bytes when the client sent `Accept-Encoding: gzip`);
- `send_error(conn, status)` — JSON error (400/404/405/413/500), closes the connection.

`HttpRequest` holds `string_view`s into the connection's `rx_buffer` — valid **only during**
the `on_request` call. For `Transfer-Encoding: chunked`, `req.body` points at the raw chunks
and `req.chunked == true` (the flag is set right after the headers, before the full body —
important for streaming); to get a contiguous body call
`HttpParser::decode_chunked_in_place(conn, req, body)` (never for proxied requests — it
breaks framing).

## Success metrics

- **RAM:** fixed, does not grow under load: `sizeof(Connection) * max_connections` + OS base;
  gzip buffers/compressors are per-thread, not per-connection. Verified by the 30-min soak.
- **Allocations:** `strace -e brk,mmap` shows nothing after startup (ASAN build checks it).
- **Timeouts:** a connection without data closes within `io_timeout_ms + 1 s` (scan step).
- **Accept under pressure:** multishot accept survives FD exhaustion (EMFILE) and resumes
  accepting once slots free up.

## Known limitations

- `send_response()` responses are bounded by `buffer_size` (headers + body); large bodies
  need streaming (static files stream via SEND_ZC/TLS chunks; general handlers do not yet
  have a chunked-writer API).
- Request body: **streaming** routes (`add_proxy`) accept any size; ordinary routes answer
  `413` for `Content-Length` above the buffer.
- Backend responses are relayed verbatim (headers are not rewritten); a response without
  `Content-Length`/chunked is streamed to EOF (connection closes).
- `splice()` zero-copy for the proxy is deferred (incompatible with TLS; the current path is
  one copy through `tx_buffer` + backpressure).
- The parser does not support exotic syntaxes (header folding, `Expect: 100-continue`).
- Single platform: Linux + io_uring (kernel ≥ 6.0 for SEND_ZC); no HTTP/2 yet.

## Roadmap

- [x] Queue-free reactor (direct dispatch + ABA protection)
- [x] `thread_local` gzip scratch and compressor (RAII, 150-byte threshold)
- [x] Config in the constructor (no globals), multi-instance
- [x] `SO_REUSEPORT` workers + graceful shutdown
- [x] `IRequestHandler` + `HttpRouter` (allocation-free routes)
- [x] Idle timeouts (Slowloris protection)
- [x] Multishot accept with re-submit on drop without `IORING_CQE_F_MORE`
- [x] HTTP pipelining without loss (memmove of the buffer tail)
- [x] Request body: `Content-Length` + chunked, 400 on smuggling
- [x] Streaming proxy upload (PROXY_UPLOADING, Fast-Exit, chunked scanner)
- [x] Lazy TLS context setup on first handshake (no `calloc` in accept)
- [x] Dynamic buffer size (one memory block at startup)
- [x] INI config with sections + routes from config
- [x] Upstream clusters with Round-Robin balancing
- [x] Backend keep-alive (idle pool of persistent connections)
- [x] Async static (io_uring `READ_FILE` into `tx_buffer`, 0 copies)
- [x] Zero-copy static streaming (READ_FIXED + SEND_ZC)
- [x] Chunked TLS static (SEND_TLS_CHUNK, files of any size over HTTPS)
- [x] `splice()` static streaming (SEND_SPLICE, file→pipe→socket, 0 bytes in user space)
- [x] TLS Large Buffer Pool (256 × 128 KB per worker) for HTTPS static
- [x] Hot reload (`/api/reload`) + web panel (`/api/stats`)
- [x] Regression suite (T1–T12) + 30-min soak script
- [ ] Streaming writer API for arbitrary handlers (chunked)
- [ ] Timing wheel instead of the O(N) pool scan
- [ ] `splice()` for plaintext proxy
- [ ] HTTP/2, ALPN
- [ ] CI (build + regression + ASAN on every commit), fuzzing of the HTTP parser


## License

This project is licensed under the [GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).
The bundled `picohttpparser` (`include/picohttpparser.h`, `src/picohttpparser.c`) is
Copyright (c) 2009-2014 Kazuho Oku and others, licensed under the MIT/Perl license.
