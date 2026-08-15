// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Nano-Sparrow — High-Performance Zero-Allocation HTTP Server
 * Copyright (C) 2024-2026 Максим Питикин <pitikinm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "config.hpp"
#include <mbedtls/ssl.h>

struct Connection; // Forward declaration

// Битовая маска in-flight опов слота. Биты выставляются при подаче SQE и
// Bitmask of the slot's in-flight ops. Bits are set when an SQE is submitted and
// снимаются в process_completion (только при совпадении generation — иначе
// cleared in process_completion (only on a generation match — otherwise
// stale-completion сбросит бит нового владельца, и новый оп останется
// a stale completion would clear the new owner's bit, leaving the new op
// неотменяемым, а fd зависнет в ядре). cancel_ops() отменяет только
// uncancellable and the fd stuck in the kernel). cancel_ops() cancels only
// выставленные биты: холостые IORING_OP_CANCEL (-ENOENT) заставляют ядро
// the set bits: idle IORING_OP_CANCEL (-ENOENT) forces the kernel to
// сканировать все in-flight опы кольца — при Connection: close это был
// scan all in-flight ops of the ring — with Connection: close this used to be
// главный тормоз (5k rps вместо десятков тысяч).
// the main bottleneck (5k rps instead of tens of thousands).
enum : uint8_t {
    INFL_R  = 1 << 0, // read_ctx: READ_CLIENT | POLL_IN
    INFL_W  = 1 << 1, // write_ctx: WRITE_CLIENT | POLL_OUT
    // file_read_ctx: READ_FILE (buffered and READ_FIXED)
    INFL_F  = 1 << 2, // file_read_ctx: READ_FILE (буферный и READ_FIXED)
    INFL_Z  = 1 << 3, // send_zc_ctx: SEND_ZC
    INFL_SI = 1 << 4, // splice_in_ctx: SPLICE_IN
    INFL_SO = 1 << 5, // splice_out_ctx: SPLICE_OUT
};

enum : uint8_t {
    INFL_UC = 1 << 0, // connect_ctx: CONNECT_UPSTREAM
    INFL_UR = 1 << 1, // read_ctx: READ_UPSTREAM
    INFL_UW = 1 << 2, // write_ctx: WRITE_UPSTREAM
};

// Структура соединения с бэкендом
// Backend connection structure
struct UpstreamConnection {
    int fd;
    // Back-reference to the client
    Connection* client_conn; // Обратная ссылка на клиента
    // Slot epoch: ABA protection of in-flight completions
    uint32_t generation;     // Эпоха слота: ABA-защита in-flight completion'ов
    // Cluster index (idle pool, [upstream:...])
    int cluster_idx;         // Индекс кластера (idle-пул, [upstream:...])
    // Node index in the cluster (Round-Robin: the idle pool is
    int node_idx;            // Индекс узла в кластере (Round-Robin: idle-пул
                             // per-кластер, но соединение принадлежит узлу)
    // The connection sits in the idle pool (keep-alive)
    bool is_idle;            // Соединение лежит в idle-пуле (keep-alive)
    // Monotonic time of entering idle (pool timeout)
    uint64_t idle_since_ms;  // Монотонное время ухода в idle (таймаут пула)
    // Mask of in-flight ops (INFL_UC/UR/UW)
    uint8_t inflight;        // Маска in-flight опов (INFL_UC/UR/UW)

    // Контексты опов: максимум один in-flight оп на направление
    // Op contexts: at most one in-flight op per direction
    EventContext connect_ctx; // CONNECT_UPSTREAM
    EventContext read_ctx;    // READ_UPSTREAM
    EventContext write_ctx;   // WRITE_UPSTREAM
    // POLL_UPSTREAM_IDLE (poll FIN while the slot is idle)
    EventContext idle_ctx;    // POLL_UPSTREAM_IDLE (poll FIN, пока слот idle)

    void reset() {
        fd = -1;
        client_conn = nullptr;
        // New epoch: stale completions will not touch the slot
        generation++; // Новая эпоха: устаревшие completion'ы не тронут слот
        cluster_idx = -1;
        node_idx = -1;
        is_idle = false;
        idle_since_ms = 0;
        inflight = 0;
        connect_ctx = EventContext{OpType::CONNECT_UPSTREAM, this, 0};
        read_ctx = EventContext{OpType::READ_UPSTREAM, this, 0};
        write_ctx = EventContext{OpType::WRITE_UPSTREAM, this, 0};
        idle_ctx = EventContext{OpType::POLL_UPSTREAM_IDLE, this, 0};
    }
};

// Инкрементальный сканер фрейминга ОТВЕТА бэкенда (upstream keep-alive).
// Incremental scanner of the backend RESPONSE framing (upstream keep-alive).
// Сбрасывается на каждый запрос; прогресс хранится в абсолютных офсетах,
// Reset on every request; progress is stored in absolute offsets,
// т.к. окна tx_buffer перезаполняются на каждый READ_UPSTREAM.
// because tx_buffer windows are refilled on every READ_UPSTREAM.
// Фазы: 0=заголовки (только первое окно), 1=CL-тело, 2=chunked-тело,
// Phases: 0=headers (first window only), 1=CL body, 2=chunked body,
// 3=DONE (конец ответа определен), 4=UNKNOWN (фрейминг невозможен:
// 3=DONE (the response end is determined), 4=UNKNOWN (framing impossible:
// соединение бэкенда закроется по EOF, как в старом коде).
// the backend connection will close at EOF, as in the old code).
struct ResponseScanner {
    uint8_t phase;
    // Total response bytes received
    size_t received;   // Всего байт ответа получено
    // Absolute end of the response (CL mode)
    size_t body_total; // Абсолютный конец ответа (CL-режим)
    bool chunked;
    // The backend sent "Connection: close"
    bool conn_close;   // Бэкенд прислал "Connection: close"
    // Framing allows returning the connection to the idle pool
    bool keepable;     // Фрейминг позволяет вернуть соединение в idle-пул
    // chunked-сканер (абсолютные офсеты)
    // chunked scanner (absolute offsets)
    // 0=size, 1=data, 2=CRLF, 3=trailers
    uint8_t chunk_phase;   // 0=размер, 1=данные, 2=CRLF, 3=трейлеры
    size_t chunk_remaining;
    size_t chunk_scan;
};

// Выравнивание по кэш-линии L1 (64 байта) для исключения false sharing
// L1 cache-line alignment (64 bytes) to eliminate false sharing
struct alignas(64) Connection {
    int fd;
    State state;
    uint8_t flags;
    // Mask of in-flight client ops (INFL_*)
    uint8_t inflight;        // Маска in-flight клиентских опов (INFL_*)
    // Monotonic slot epoch: grows on reset() and close(),
    uint32_t generation; // Монотонная эпоха слота: растет на reset() и close(),
                         // отбрасывает устаревшие completion'ы после
                         // discards stale completions after
                         // переиспользования слота пула (ABA)
    // Monotonic time of the last activity (timeouts)
    uint64_t last_activity_ms; // Монотонное время последней активности (таймауты)

    // Zero Allocation: rx/tx буферы выделены ОДНИМ блоком при старте сервера
    // Zero Allocation: the rx/tx buffers are allocated as ONE block at server startup
    // (new char[max_connections * buffer_size * 2]) и розданы слотам пула.
    // (new char[max_connections * buffer_size * 2]) and distributed to the pool slots.
    char* rx_buffer;
    char* tx_buffer;
    uint32_t buffer_size;

    int rx_bytes;
    int tx_bytes;
    // How many bytes of tx_buffer have already gone to the network
    int tx_sent; // Сколько байт из tx_buffer уже ушло в сеть
    // How many bytes of the current rx_buffer window are already sent to the backend
    int rx_sent; // Сколько байт из текущего окна rx_buffer уже отправлено на бэкенд

    // ==== Прокси-стриминг тела запроса (единый автомат, см. server.cpp) ====
    // ==== Request body proxy streaming (unified state machine, see server.cpp) ====
    // Заголовки и длина тела стешиваются из HttpRequest при диспатче
    // Headers and body length are cached from HttpRequest at dispatch
    // Header length (absolute body start)
    size_t proxy_header_len;     // Длина заголовков (абсолютное начало тела)
    // Body Content-Length (0 = none/unknown)
    size_t proxy_content_length; // Content-Length тела (0 = нет/неизвестен)
    // The body arrived as Transfer-Encoding: chunked
    bool proxy_is_chunked;       // Тело пришло как Transfer-Encoding: chunked
    // Абсолютный конец запроса (заголовки + тело) в потоке запроса;
    // Absolute end of the request (headers + body) in the request stream;
    // 0 = пока неизвестен (chunked: выяснит сканер при поступлении 0-чанка)
    // 0 = unknown yet (chunked: the scanner will find it when the 0-chunk arrives)
    size_t proxy_request_end;
    // Absolute request-stream offset at which
    size_t proxy_window_base;    // Абсолютный офсет потока запроса, с которого
                                 // начинается текущее окно rx_buffer
                                 // the current rx_buffer window starts
    // Инкрементальный сканер chunked-фрейминга (координаты ТЕКУЩЕГО окна,
    // Incremental chunked-framing scanner (coordinates of the CURRENT window,
    // сбрасываются при дренаже):
    // reset on drain):
    // 0=size, 1=data, 2=CRLF after data, 3=trailers
    uint8_t proxy_chunk_phase;   // 0=размер, 1=данные, 2=CRLF после данных, 3=трейлеры
    // Bytes left until the end of the current phase
    size_t proxy_chunk_remaining;// Байт до конца текущей фазы
    // Scanner position in the current window
    size_t proxy_chunk_scan;     // Позиция сканера в текущем окне
    // Client method is HEAD: the response has no body
    bool proxy_is_head;          // Клиентский метод HEAD: у ответа нет тела

    // ==== Фрейминг ответа бэкенда (upstream keep-alive) ====
    // ==== Backend response framing (upstream keep-alive) ====
    ResponseScanner resp;

    // ==== Асинхронная статика ====
    // ==== Async static ====
    // READ_STATIC_FILE (буферный путь: READ_FILE в tx_buffer) и SEND_STATIC
    // READ_STATIC_FILE (buffered path: READ_FILE into tx_buffer) and SEND_STATIC
    // (zero-copy путь: READ_FIXED в зарегистрированный tx_buffer + SEND_ZC).
    // (zero-copy path: READ_FIXED into the registered tx_buffer + SEND_ZC).
    // Static file FD (disk read)
    int file_fd;               // FD статического файла (чтение с диска)
    // MIME type (const pointer, 0 allocations)
    const char* file_mime;     // MIME тип (константный указатель, 0 аллокаций)
    EventContext file_read_ctx;

    // ==== Zero-Copy статика (SEND_STATIC) ====
    // ==== Zero-Copy static (SEND_STATIC) ====
    // tx_buffer index in IORING_REGISTER_BUFFERS
    uint32_t buf_index;        // Индекс tx_buffer в IORING_REGISTER_BUFFERS
                               // (выдан один раз при старте, SEND_ZC/READ_FIXED)
                               // (issued once at startup, SEND_ZC/READ_FIXED)
    // File size
    uint64_t file_size;        // Размер файла
    // How many file bytes are already sent
    uint64_t file_offset;      // Сколько байт файла уже отправлено
    // Data bytes of the current chunk in tx_buffer
    uint32_t file_chunk_len;   // Байт данных текущего чанка в tx_buffer
    // SEND_ZC: zero-copy delivery of a chunk to the socket
    EventContext send_zc_ctx;  // SEND_ZC: zero-copy отдача чанка в сокет

    // ==== SPLICE-статика (SEND_SPLICE) ====
    // ==== SPLICE static (SEND_SPLICE) ====
    // Pipe index in the server pool (-1 = not issued)
    int pipe_idx;              // Индекс пайпа в пуле сервера (-1 = не выдан)
    // Total bytes flushed into the pipe (file->pipe);
    uint64_t splice_pipe_bytes;// Всего байт залито в пайп (file->pipe);
                               // остаток пайпа = splice_pipe_bytes - file_offset
                               // the pipe remainder = splice_pipe_bytes - file_offset
    // SPLICE_IN: file -> pipe
    EventContext splice_in_ctx; // SPLICE_IN: file -> pipe
    // SPLICE_OUT: pipe -> socket
    EventContext splice_out_ctx;// SPLICE_OUT: pipe -> socket

    // ==== TLS-статика на «толстом» буфере (SEND_TLS_CHUNK) ====
    // ==== TLS static on the "fat" buffer (SEND_TLS_CHUNK) ====
    // Buffer from the server pool (nullptr = not issued)
    char* large_buf;           // Буфер из пула сервера (nullptr = не выдан)
    // Buffer index in the pool (for release)
    uint32_t large_buf_idx;    // Индекс буфера в пуле (для release)
    // Chunk bytes already encrypted/sent
    uint32_t large_off;        // Байт чанка уже зашифровано/отправлено
    // Chunk bytes in large_buf (after READ_FILE)
    uint32_t large_len;        // Байт чанка в large_buf (после READ_FILE)

    // Контексты клиентских опов: READ_CLIENT/WRITE_CLIENT (plaintext)
    // Client op contexts: READ_CLIENT/WRITE_CLIENT (plaintext)
    // или POLL_IN/POLL_OUT (TLS). Один in-flight на направление.
    // or POLL_IN/POLL_OUT (TLS). One in-flight per direction.
    EventContext read_ctx;
    EventContext write_ctx;
    // Reference to the backend (if proxying)
    UpstreamConnection* upstream; // Ссылка на бэкенд (если проксируем)

    // ==== Двойной буфер прокси-ответа (см. submit_proxy_prefetch) ====
    // ==== Proxy response double buffering (see submit_proxy_prefetch) ====
    // Окно ответа бэкенда приходит в tx_buffer (первое) или в large_buf
    // The backend response window arrives in tx_buffer (the first one) or in large_buf
    // (предзагрузка). Пока клиент дочитывает окно A, READ_UPSTREAM уже
    // (prefetch). While the client finishes window A, READ_UPSTREAM already
    // качает окно B — RTT бэкенда скрыт. Один in-flight READ_UPSTREAM:
    // fetches window B — the backend RTT is hidden. One in-flight READ_UPSTREAM:
    // backpressure прежняя (затык по TCP-окну бэкенда).
    // backpressure stays the same (blocked on the backend TCP window).
    // Buffer of the ACTIVE window (being delivered to the client)
    char* proxy_win_ptr;        // Буфер АКТИВНОГО окна (доставляется клиенту)
    // Buffer of the last submitted READ_UPSTREAM
    char* proxy_read_dst;       // Буфер последнего поданного READ_UPSTREAM
    // Buffer of the prefetched window
    char* prefetch_ptr;         // Буфер предзагруженного окна
    // Bytes of the prefetched window
    uint32_t prefetch_len;      // Байт предзагруженного окна
    // large_buf index of the prefetch
    uint32_t prefetch_large_idx;// Индекс large_buf предзагрузки
                                // (0xFFFFFFFF = не выдан)
                                // (0xFFFFFFFF = not issued)
    // Prefetch READ_UPSTREAM is in flight
    bool prefetch_busy;         // READ_UPSTREAM предзагрузки в полете
    // The window arrived, the client has not freed up yet
    bool prefetch_ready;        // Окно пришло, клиент ещё не освободился

    // Преаллоцированный контекст для криптографии
    // Pre-allocated context for cryptography
    mbedtls_ssl_context ssl;
    // true после mbedtls_ssl_setup(): контекст жив между соединениями
    // true after mbedtls_ssl_setup(): the context lives across connections
    // (session_reset переиспользует его аллокации). НЕ сбрасывается в
    // (session_reset reuses its allocations). NOT reset in
    // reset(): mbedtls_ssl_session_reset() делает безусловный memset(out_buf),
    // reset(): mbedtls_ssl_session_reset() unconditionally memsets out_buf,
    // на неинициализированном контексте это будет сегфолт.
    // on an uninitialized context this would be a segfault.
    bool ssl_setup;

    void reset() {
        fd = -1;
        // Слот помечается свободным: таймаут-скан и stale-события его не тронут,
        // The slot is marked free: the timeout scan and stale events will not touch it,
        // пока accept() не переведет его в State::ACCEPT.
        // until accept() moves it to State::ACCEPT.
        state = State::CLOSE;
        flags = 0;
        inflight = 0;
        // New generation: stale completions will be discarded
        generation++; // Новое поколение: устаревшие completion'ы отбросятся
        last_activity_ms = 0;
        // rx_buffer/tx_buffer/buffer_size/buf_index НЕ трогаем: они выданы
        // Do NOT touch rx_buffer/tx_buffer/buffer_size/buf_index: they are issued
        // один раз при старте сервера и не меняются при переиспользовании
        // once at server startup and never change on slot reuse
        // слота (buf_index = индекс tx_buffer в зарегистрированных буферах).
        // (buf_index = the tx_buffer index among the registered buffers).
        rx_bytes = 0;
        tx_bytes = 0;
        tx_sent = 0;
        rx_sent = 0;
        proxy_header_len = 0;
        proxy_content_length = 0;
        proxy_is_chunked = false;
        proxy_request_end = 0;
        proxy_window_base = 0;
        proxy_chunk_phase = 0;
        proxy_chunk_remaining = 0;
        proxy_chunk_scan = 0;
        proxy_is_head = false;
        resp = ResponseScanner{};
        file_fd = -1;
        file_mime = nullptr;
        file_read_ctx = EventContext{OpType::READ_FILE, this, 0};
        file_size = 0;
        file_offset = 0;
        file_chunk_len = 0;
        send_zc_ctx = EventContext{OpType::SEND_ZC, this, 0};
        pipe_idx = -1;
        splice_pipe_bytes = 0;
        splice_in_ctx = EventContext{OpType::SPLICE_IN, this, 0};
        splice_out_ctx = EventContext{OpType::SPLICE_OUT, this, 0};
        large_buf = nullptr;
        large_buf_idx = 0;
        large_off = 0;
        large_len = 0;
        upstream = nullptr;
        read_ctx = EventContext{OpType::READ_CLIENT, this, 0};
        write_ctx = EventContext{OpType::WRITE_CLIENT, this, 0};
        proxy_win_ptr = nullptr;
        proxy_read_dst = nullptr;
        prefetch_ptr = nullptr;
        prefetch_len = 0;
        prefetch_large_idx = 0xFFFFFFFFu;
        prefetch_busy = false;
        prefetch_ready = false;
        // ssl_setup не сбрасываем: контекст TLS, однажды созданный
        // ssl_setup is not reset: the TLS context, once created
        // (mbedtls_ssl_setup в accept), переиспользуется слотами через
        // (mbedtls_ssl_setup in accept), is reused by slots via
        // mbedtls_ssl_session_reset() в close_connection() — без аллокаций.
        // mbedtls_ssl_session_reset() in close_connection() — with no allocations.
    }
};
