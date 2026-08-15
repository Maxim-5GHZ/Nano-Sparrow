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
#include "server.hpp"
#include "http_parser.hpp"
#include "app_state.hpp"
#include <libdeflate.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string_view>
#include <vector>

#include <mbedtls/net_sockets.h> // MBEDTLS_ERR_NET_* (mbedTLS 2.28)

// Компрессор на поток: gzip нужен на миллисекунду за запрос (как и раньше —
// Per-thread compressor: gzip is needed for a millisecond per request (as before —
// по одному на воркер, а не на соединение).
// one per worker, not per connection).
static thread_local libdeflate_compressor* g_compressor =
    libdeflate_alloc_compressor(6);

// RAII-гвард: thread_local деструктор освобождает компрессор при выходе из
// RAII guard: the thread_local destructor frees the compressor when the
// потока (без него утечка при динамическом создании/уничтожении потоков).
// thread exits (without it there is a leak on dynamic thread creation/destruction).
struct CompressorGuard {
    libdeflate_compressor* ptr;
    explicit CompressorGuard(libdeflate_compressor* p) : ptr(p) {}
    ~CompressorGuard() {
        if (ptr) libdeflate_free_compressor(ptr);
    }
};
static thread_local CompressorGuard g_compressor_guard(g_compressor);

// Рабочий буфер на поток (gzip-выход + временные заголовки). Размер зависит
// Per-thread scratch buffer (gzip output + temporary headers). Its size depends
// от конфигурируемого buffer_size, поэтому выделяется лениво первым запросом
// on the configured buffer_size, so it is allocated lazily by the first request
// (0 аллокаций в горячем цикле после прогрева).
// (0 allocations in the hot loop after warm-up).
static thread_local char* g_scratch_buffer = nullptr;
static thread_local size_t g_scratch_size = 0;

static void ensure_scratch(size_t need) {
    if (g_scratch_size < need) {
        delete[] g_scratch_buffer;
        g_scratch_size = need;
        g_scratch_buffer = new char[need];
    }
}

struct ScratchGuard {
    ~ScratchGuard() {
        delete[] g_scratch_buffer;
        g_scratch_buffer = nullptr;
        g_scratch_size = 0;
    }
};
static thread_local ScratchGuard g_scratch_guard;

// Монотонные часы (vDSO, ~20ns за вызов) — см. app_state.hpp
// Monotonic clock (vDSO, ~20ns per call) — see app_state.hpp

// Регистронезависимое сравнение (0 аллокаций; для фрейминга ответов бэкенда)
// Case-insensitive comparison (0 allocations; used for backend response framing)
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

// Поиск токена в списке через запятую ("keep-alive, close" -> close)
// Search for a token in a comma-separated list ("keep-alive, close" -> close)
static bool has_token(std::string_view value, std::string_view token) {
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        size_t end = (comma == std::string_view::npos) ? value.size() : comma;
        std::string_view item = value.substr(start, end - start);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (iequals(item, token)) return true;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return false;
}

// Неблокирующие BIO колбеки для mbedTLS
// Non-blocking BIO callbacks for mbedTLS
static int custom_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    auto* conn = static_cast<Connection*>(ctx);
    // MSG_NOSIGNAL обязателен: клиент, закрывший сокет, во время отправки
    // MSG_NOSIGNAL is mandatory: a client that closed the socket during a send
    // вызывает SIGPIPE (процесс умирает со 141). Проверено wrk-нагрузкой.
    // triggers SIGPIPE (the process dies with 141). Verified with a wrk load test.
    ssize_t ret = send(conn->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return static_cast<int>(ret);
}

static int custom_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    auto* conn = static_cast<Connection*>(ctx);
    ssize_t ret = recv(conn->fd, buf, len, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    // EOF (клиент закрыл TCP без close_notify): рукопожатие/чтение не сможет
    // EOF (client closed TCP without close_notify): the handshake/read can never
    // продвинуться, а poll на таком сокете мгновенно «читаем» — без явного
    // make progress, and poll on such a socket is instantly "readable" — without an explicit
    // флага сервер навечно зациклится в WANT_READ -> re-arm poll.
    // flag the server would spin forever in WANT_READ -> re-arm poll.
    if (ret == 0) conn->flags |= FLAG_SSL_EOF;
    return static_cast<int>(ret);
}

Server::Server(const ServerConfig* config, IRequestHandler* handler, int shutdown_fd,
               int reload_fd)
    : config_(config), handler_(handler), shutdown_fd_(shutdown_fd),
      reload_fd_(reload_fd), listen_fd(-1), ssl_ready(false),
      conn_pool(config->max_connections),
      upstream_pool(config->max_connections) {
    // Единственная аллокация при старте: rx/tx буферы ВСЕХ слотов пула одним
    // The only allocation at startup: rx/tx buffers of ALL pool slots in one
    // непрерывным блоком (Zero Allocation: дальше ни одного malloc в рантайме).
    // contiguous block (Zero Allocation: no malloc in the runtime afterwards).
    size_t total = static_cast<size_t>(config->max_connections) *
                   static_cast<size_t>(config->buffer_size) * 2u;
    connection_buffers_ = new char[total];

    // Пул пайпов для SEND_SPLICE: pipe2 при старте (страницы буферов пайпов
    // Pipe pool for SEND_SPLICE: pipe2 at startup (pipe buffer pages are
    // аллоцируются ядром лениво — только при активных передачах).
    // allocated lazily by the kernel — only during active transfers).
    // splice_pipes=0 в конфиге выключает splice-путь (фолбэк на SEND_ZC).
    // splice_pipes=0 in the config disables the splice path (falls back to SEND_ZC).
    splice_pipes_.resize(static_cast<size_t>(std::max(0, config->splice_pipes)));
    pipe_free_.resize(splice_pipes_.size());
    pipe_top_ = static_cast<int>(splice_pipes_.size()) - 1;
    for (int i = 0; i < config->splice_pipes; ++i) {
        int fds[2];
        if (pipe2(fds, O_NONBLOCK) != 0) {
            // Провал (EMFILE/ENFILE): сервер выживает без splice-пути
            // Failure (EMFILE/ENFILE): the server survives without the splice path
            splice_pipes_.resize(static_cast<size_t>(i));
            pipe_top_ = i - 1;
            pipe_free_.resize(splice_pipes_.size());
            break;
        }
        long cap = fcntl(fds[0], F_GETPIPE_SZ);
        splice_pipes_[static_cast<size_t>(i)] = SplicePipe{fds[0], fds[1], cap > 0 ? cap : 65536L};
        pipe_free_[static_cast<size_t>(i)] = i;
    }

    // Пул «толстых» TLS-буферов: один блок памяти (64 байта — выравнивание
    // Pool of "fat" TLS buffers: a single memory block (64 bytes — no alignment
    // не требуется: буферы используются только для I/O и mbedTLS).
    // needed: the buffers are only used for I/O and mbedTLS).
    if (config->tls_large_buffers > 0) {
        large_buffer_size_ = config->tls_large_buffer_size;
        size_t large_total = static_cast<size_t>(config->tls_large_buffers) *
                             large_buffer_size_;
        large_buffers_mem_ = new char[large_total];
        large_free_.resize(static_cast<size_t>(config->tls_large_buffers));
        large_top_ = config->tls_large_buffers - 1;
        for (int i = 0; i < config->tls_large_buffers; ++i) {
            large_free_[static_cast<size_t>(i)] = i;
        }
    }

    // Upstream-кластеры из конфига: узлы + idle-пулы keep-alive соединений
    // Upstream clusters from the config: nodes + idle pools of keep-alive connections
    // (стартовая инициализация; при hot reload перестраиваются через
    // (initial setup; on hot reload they are rebuilt via
    // rebuild_clusters()).
    cluster_index_.reserve(config->upstreams.size());
    clusters_.reserve(config->upstreams.size());
    for (const auto& [name, cluster] : config->upstreams) {
        ClusterState cs;
        cs.nodes = cluster.nodes;
        cs.idle.reserve(static_cast<size_t>(config->max_connections));
        cluster_index_[name] = static_cast<int>(clusters_.size());
        clusters_.push_back(std::move(cs));
    }

    std::vector<Connection*> temp;
    temp.reserve(static_cast<size_t>(config->max_connections));
    char* mem = connection_buffers_;
    for (int i = 0; i < config->max_connections; ++i) {
        Connection* c = conn_pool.acquire();
        // Slots start in the CLOSE state (free)
        c->reset(); // Слоты стартуют в состоянии CLOSE (свободны)
        c->rx_buffer = mem;
        mem += config->buffer_size;
        c->tx_buffer = mem;
        mem += config->buffer_size;
        c->buffer_size = config->buffer_size;
        mbedtls_ssl_init(&c->ssl);
        // Lazy mbedtls_ssl_setup in accept()
        c->ssl_setup = false; // Ленивый mbedtls_ssl_setup в accept()
        temp.push_back(c);
    }
    // ВАЖНО: acquire() выдает слоты в обратном порядке (стеко-пул), поэтому
    // IMPORTANT: acquire() hands out slots in reverse order (stack pool), so
    // buf_index привязываем к ФИЗИЧЕСКОМУ индексу слота — именно в таком
    // buf_index is bound to the PHYSICAL slot index — it is exactly in this
    // порядке tx-буферы регистрируются в IORING_REGISTER_BUFFERS (см. start())
    // order that tx buffers are registered with IORING_REGISTER_BUFFERS (see start())
    for (int i = 0; i < config->max_connections; ++i) {
        conn_pool.at(i)->buf_index = static_cast<uint32_t>(i);
    }
    for (Connection* c : temp) conn_pool.release(c);
}

Server::~Server() {
    delete[] connection_buffers_;
    connection_buffers_ = nullptr;
    delete[] large_buffers_mem_;
    large_buffers_mem_ = nullptr;
    for (const SplicePipe& p : splice_pipes_) {
        if (p.in_fd != -1) (void)close(p.in_fd);
        if (p.out_fd != -1) (void)close(p.out_fd);
    }
    if (listen_fd != -1) (void)close(listen_fd);
    if (ring_ready_) io_uring_queue_exit(&ring);
    if (ssl_ready) {
        // Освобождаем преаллоцированные SSL-контексты слотов пула
        // Free the pre-allocated SSL contexts of the pool slots
        for (int i = 0; i < conn_pool.capacity(); ++i) {
            mbedtls_ssl_free(&conn_pool.at(i)->ssl);
        }
        mbedtls_ssl_config_free(&ssl_conf);
        mbedtls_x509_crt_free(&srvcert);
        mbedtls_pk_free(&pkey);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
}

void Server::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// TCP_NODELAY обязателен: с включенным Nagle пачки мелких ответов ждут
// TCP_NODELAY is mandatory: with Nagle enabled, batches of small responses wait
// delayed-ACK клиента (~40 мс на батч — замерено). Для HTTP-сервера это
// for the client's delayed-ACK (~40 ms per batch — measured). For an HTTP server this is
// не оптимизация, а исправление задержки.
// not an optimization but a latency fix.
static void set_tcp_nodelay(int fd) {
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// УМНЫЙ I/O: Авто-выбор между сырым сокетом и SSL (используется TLS-путями)
// SMART I/O: auto-selection between the raw socket and SSL (used by the TLS paths)
ssize_t Server::io_read(Connection* conn, void* buf, size_t count) {
    if (conn->flags & FLAG_IS_SSL) {
        int ret = mbedtls_ssl_read(&conn->ssl, static_cast<unsigned char*>(buf), count);
        if (ret > 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // WANT_READ после EOF (FIN без close_notify, частичный record):
            // WANT_READ after EOF (FIN without close_notify, partial record):
            // mbedTLS ждёт «ещё данных», но их никогда не будет, а poll на
            // mbedTLS waits for "more data" that will never come, and poll on
            // таком сокете мгновенно «читаем» вечно — без этого превратился
            // such a socket is instantly "readable" forever — without this it would turn
            // бы в бесконечный спин. EOF = конец соединения.
            // into an endless spin. EOF = end of connection.
            if (conn->flags & FLAG_SSL_EOF) return 0;
            errno = EAGAIN; return -1;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        // Ошибка mbedTLS (напр. CONN_EOF: FIN без close_notify): errno обязан
        // mbedTLS error (e.g. CONN_EOF: FIN without close_notify): errno must
        // быть != EAGAIN — иначе handle_read примет устаревший EAGAIN за
        // be != EAGAIN — otherwise handle_read would mistake a stale EAGAIN for
        // «нет данных» и вечно переподаст poll на постоянно «читаемом» сокете.
        // "no data" and forever re-arm poll on a permanently "readable" socket.
        errno = ENOTCONN;
        return -1;
    }
    return read(conn->fd, buf, count);
}

ssize_t Server::io_write(Connection* conn, const void* buf, size_t count) {
    if (conn->flags & FLAG_IS_SSL) {
        int ret = mbedtls_ssl_write(&conn->ssl, static_cast<const unsigned char*>(buf), count);
        if (ret > 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            errno = EAGAIN; return -1;
        }
        return -1;
    }
    return write(conn->fd, buf, count);
}

void Server::start() {
    if (config_->enable_ssl) {
        mbedtls_ssl_config_init(&ssl_conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_x509_crt_init(&srvcert);
        mbedtls_pk_init(&pkey);

        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, nullptr, 0) != 0 ||
            mbedtls_x509_crt_parse_file(&srvcert, config_->ssl_cert.c_str()) != 0 ||
            mbedtls_pk_parse_keyfile(&pkey, config_->ssl_key.c_str(), nullptr) != 0) {
            std::cerr << "[TLS] Failed to load certificate/key\n";
            std::exit(1);
        }

        mbedtls_ssl_config_defaults(&ssl_conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_rng(&ssl_conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        mbedtls_ssl_conf_own_cert(&ssl_conf, &srvcert, &pkey);
        ssl_ready = true;

        // mbedtls_ssl_setup() внутри делает calloc (сессии, трансформы, буферы
        // mbedtls_ssl_setup() internally calls calloc (sessions, transforms, buffers
        // in/out 16+16 КБ). Раньше он выполнялся для всех 10000 слотов при
        // in/out 16+16 KB). Previously it ran for all 10000 slots at
        // старте: 316 МБ RSS в TLS-режиме даже без единого клиента.
        // startup: 316 MB RSS in TLS mode even without a single client.
        // Теперь setup делается лениво в handle_accept() при первом
        // Now setup is done lazily in handle_accept() on the first
        // использовании слота; контекст живет до close_connection()
        // use of a slot; the context lives until close_connection()
        // (mbedtls_ssl_session_reset() переиспользует его аллокации).
        // (mbedtls_ssl_session_reset() reuses its allocations).
        // Память TLS-режима в покое: ~125 МБ (10000 x 8 КБ rx + 8 КБ tx).
        // TLS-mode memory at rest: ~125 MB (10000 x 8 KB rx + 8 KB tx).
    }

    // 1. io_uring. По умолчанию plain-режим: при multi-worker каждый SQPOLL
    // 1. io_uring. Plain mode by default: with multi-worker each SQPOLL
    // плодит спиннингующий kernel-поток (iou-sqp), конкурирующий за CPU, —
    // spawns a spinning kernel thread (iou-sqp) competing for CPU —
    // net loss против пакетного plain-режима (один submit на пачку CQE).
    // a net loss against batched plain mode (one submit per batch of CQEs).
    // enable_sqpoll=true — только для однопоточных инсталляций.
    // enable_sqpoll=true — only for single-threaded installations.
    // entries капим 32768 = IORING_MAX_SQ_ENTRIES: io_uring_setup(entries > 32768)
    // We cap entries at 32768 = IORING_MAX_SQ_ENTRIES: io_uring_setup(entries > 32768)
    // возвращает EINVAL, сервер падал при max_connections > 16384.
    // returns EINVAL, the server crashed with max_connections > 16384.
    unsigned entries = std::max(4096u, std::min(32768u,
                 2u * static_cast<unsigned>(std::max(1, config_->max_connections))));
    bool use_sqpoll = false;
    // advanced = accepted kernel 6.0+ flags
    bool advanced = false; // приняты флаги ядер 6.0+
    io_uring_params params{};
    // Ядра 6.0+: SINGLE_ISSUER говорит ядру, что SQE подает один поток
    // Kernels 6.0+: SINGLE_ISSUER tells the kernel that one thread submits SQEs
    // (убирает внутренние локи), COOP_TASKRUN + DEFER_TASKRUN переносят
    // (removes internal locks), COOP_TASKRUN + DEFER_TASKRUN move
    // обработку завершений в контекст нашего потока при wait.
    // completion processing into our thread's context on wait.
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
    params.flags |= IORING_SETUP_COOP_TASKRUN;
    params.flags |= IORING_SETUP_DEFER_TASKRUN;
    if (config_->enable_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
    }
    if (io_uring_queue_init_params(entries, &ring, &params) == 0) {
        advanced = true;
        use_sqpoll = (params.flags & IORING_SETUP_SQPOLL) != 0;
    } else {
        // Фолбэк для старых ядер (< 6.0)
        // Fallback for old kernels (< 6.0)
        std::cerr << "[io_uring] Advanced flags unavailable (kernel < 6.0?), "
                     "falling back to basic setup\n";
        if (io_uring_queue_init(entries, &ring, config_->enable_sqpoll ? IORING_SETUP_SQPOLL : 0) != 0) {
            std::cerr << "[io_uring] Failed to init ring\n";
            std::exit(1);
        }
        use_sqpoll = config_->enable_sqpoll;
    }
    ring_ready_ = true;
    std::cout << "[io_uring] ring entries=" << entries
              << " sqpoll=" << (use_sqpoll ? "yes" : "no")
              << " mode=" << (advanced ? "advanced(6.0+)" : "basic")
              << "\n" << std::flush;

    if (!splice_pipes_.empty()) {
        std::cout << "[splice] " << splice_pipes_.size() << " pipes pre-allocated"
                  << " (SEND_SPLICE static)\n" << std::flush;
    }
    if (large_buffers_mem_) {
        std::cout << "[tls] large buffer pool: "
                  << large_free_.size() << " x " << large_buffer_size_
                  << " bytes\n" << std::flush;
    }

    // Zero-Copy статика: регистрируем tx-буферы всех слотов через
    // Zero-Copy static: register the tx buffers of all slots via
    // IORING_REGISTER_BUFFERS (один syscall при старте, дальше ноль).
    // IORING_REGISTER_BUFFERS (one syscall at startup, zero afterwards).
    // SEND_ZC (ядро 6.0+) шлет страницы этих буферов в сокет напрямую.
    // SEND_ZC (kernel 6.0+) sends the pages of these buffers straight to the socket.
    // Лимит ядра: 16384 буфера; провал (RLIMIT_MEMLOCK) = фолбэк на
    // Kernel limit: 16384 buffers; failure (RLIMIT_MEMLOCK) = fallback to the
    // буферный READ_FILE путь.
    // buffered READ_FILE path.
    if (advanced) {
        size_t reg = std::min<size_t>(static_cast<size_t>(config_->max_connections),
                                      16384u);
        std::vector<struct iovec> bufs;
        bufs.reserve(reg);
        for (size_t i = 0; i < reg; ++i) {
            struct iovec vec{};
            vec.iov_base = conn_pool.at(static_cast<int>(i))->tx_buffer;
            vec.iov_len = config_->buffer_size;
            bufs.push_back(vec);
        }
        if (io_uring_register_buffers(&ring, bufs.data(), reg) == 0) {
            zerocopy_ = true;
            registered_buffers_ = static_cast<uint32_t>(reg);
            std::cout << "[io_uring] registered " << reg
                      << " tx-buffers for SEND_ZC (zero-copy static)\n" << std::flush;
        } else {
            std::cerr << "[io_uring] buffer registration failed (RLIMIT_MEMLOCK?), "
                         "SEND_ZC disabled, using buffered static\n";
        }
    }

    // 2. listen-сокет
    // 2. listen socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(listen_fd);

    int opt = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (config_->worker_threads > 1) {
        // Несколько воркеров слушают один порт: ядро балансирует accept
        // Multiple workers listen on one port: the kernel balances accept
        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_->port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "[Server] bind failed: " << std::strerror(errno) << "\n";
        std::exit(1);
    }
    (void)listen(listen_fd, SOMAXCONN);

    // 3. Первичные асинхронные опы: multishot accept и чтение каналов
    // 3. Initial async ops: multishot accept and reading the event channels
    submit_accept();
    if (shutdown_fd_ != -1) {
        io_uring_sqe* sqe = get_sqe();
        shutdown_ctx_ = EventContext{OpType::SHUTDOWN, nullptr, 0};
        io_uring_prep_read(sqe, shutdown_fd_, &shutdown_buf_, sizeof(shutdown_buf_), 0);
        io_uring_sqe_set_data(sqe, &shutdown_ctx_);
    }
    // Hot Reload: eventfd-чтение, которым /api/reload будит воркера
    // Hot Reload: eventfd read through which /api/reload wakes the worker
    // (см. process_completion, OpType::RELOAD)
    // (see process_completion, OpType::RELOAD)
    if (reload_fd_ != -1) {
        io_uring_sqe* sqe = get_sqe();
        reload_ctx_ = EventContext{OpType::RELOAD, nullptr, 0};
        io_uring_prep_read(sqe, reload_fd_, &reload_buf_, sizeof(reload_buf_), 0);
        io_uring_sqe_set_data(sqe, &reload_ctx_);
    }
}

// SQE из кольца; при переполнении SQ проталкиваем очередь в ядро и повторяем
// Take an SQE from the ring; if the SQ overflows, push the queue to the kernel and retry
io_uring_sqe* Server::get_sqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (sqe) return sqe;
    (void)io_uring_submit(&ring);
    return io_uring_get_sqe(&ring);
}

// Multishot accept (Linux 5.19+): один SQE, ядро непрерывно принимает
// Multishot accept (Linux 5.19+): a single SQE, the kernel keeps accepting
// подключения и шлет CQE на каждого нового клиента. Подается один раз в start()
// connections and sends a CQE for each new client. Submitted once in start()
// и ПЕРЕПОДАЕТСЯ, если ядро завершило стрим (см. handle_accept).
// and RE-SUBMITTED if the kernel ended the stream (see handle_accept).
void Server::submit_accept() {
    io_uring_sqe* sqe = get_sqe();
    accept_ctx_ = EventContext{OpType::ACCEPT, nullptr, 0};
    io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &accept_ctx_);
}

void Server::submit_read(Connection* conn) {
    // the parser would return BAD
    if (conn->rx_bytes >= static_cast<int>(conn->buffer_size)) return; // парсер дал бы BAD
    io_uring_sqe* sqe = get_sqe();
    conn->read_ctx.op = OpType::READ_CLIENT;
    conn->read_ctx.ptr = conn;
    conn->read_ctx.gen = conn->generation;
    conn->inflight |= INFL_R;
    // Читаем прямо в pre-аллоцированный rx_buffer слота (Zero Copy)
    // Read straight into the slot's pre-allocated rx_buffer (Zero Copy)
    io_uring_prep_recv(sqe, conn->fd, conn->rx_buffer + conn->rx_bytes,
                       static_cast<size_t>(conn->buffer_size) -
                           static_cast<size_t>(conn->rx_bytes),
                       0);
    io_uring_sqe_set_data(sqe, &conn->read_ctx);
}

void Server::submit_write(Connection* conn) {
    io_uring_sqe* sqe = get_sqe();
    conn->write_ctx.op = OpType::WRITE_CLIENT;
    conn->write_ctx.ptr = conn;
    conn->write_ctx.gen = conn->generation;
    conn->inflight |= INFL_W;
    // Двойной буфер прокси: окно ответа может жить в large_buf
    // Proxy double buffering: the response window can live in large_buf
    // (см. dispatch_proxy_window). Вне PROXY_STREAMING окно всегда
    // (see dispatch_proxy_window). Outside PROXY_STREAMING the window is always
    // tx_buffer (статика/ошибки/keep-alive).
    // tx_buffer (static/errors/keep-alive).
    const char* win = (conn->state == State::PROXY_STREAMING) ? conn->proxy_win_ptr
                                                              : conn->tx_buffer;
    io_uring_prep_send(sqe, conn->fd, win + conn->tx_sent,
                       static_cast<size_t>(conn->tx_bytes - conn->tx_sent), MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &conn->write_ctx);
}

// TLS-готовность: вместо async recv/send (mbedTLS сам дергает BIO)
// TLS readiness: instead of async recv/send (mbedTLS drives the BIO itself)
// ждем готовность сокета через poll, дальше - синхронный mbedTLS.
// we wait for socket readiness via poll, then - synchronous mbedTLS.
void Server::submit_poll(Connection* conn, OpType op) {
    io_uring_sqe* sqe = get_sqe();
    EventContext& ctx = (op == OpType::POLL_IN) ? conn->read_ctx : conn->write_ctx;
    ctx.op = op;
    ctx.ptr = conn;
    ctx.gen = conn->generation;
    if (op == OpType::POLL_IN) conn->inflight |= INFL_R;
    else conn->inflight |= INFL_W;
    io_uring_prep_poll_add(sqe, conn->fd, (op == OpType::POLL_IN) ? POLLIN : POLLOUT);
    io_uring_sqe_set_data(sqe, &ctx);
    if (conn->state == State::SSL_HANDSHAKE) {
        }
}

void Server::submit_upstream_connect(UpstreamConnection* up, const sockaddr_in& addr) {
    io_uring_sqe* sqe = get_sqe();
    up->connect_ctx.op = OpType::CONNECT_UPSTREAM;
    up->connect_ctx.ptr = up;
    up->connect_ctx.gen = up->generation;
    up->inflight |= INFL_UC;
    // Completion приходит, когда connect() завершился (в т.ч. неблокирующий:
    // The completion arrives when connect() has finished (incl. non-blocking:
    // EINPROGRESS обрабатывает ядро)
    // the kernel handles EINPROGRESS)
    io_uring_prep_connect(sqe, up->fd,
                          reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    io_uring_sqe_set_data(sqe, &up->connect_ctx);
}

// Сколько байт из текущего окна rx_buffer принадлежит запросу (см. ниже)
// How many bytes of the current rx_buffer window belong to the request (see below)
static size_t proxy_sendable(Connection* conn);

void Server::submit_upstream_write(Connection* conn) {
    UpstreamConnection* up = conn->upstream;
    size_t len = proxy_sendable(conn);
    // Nothing to send (the request is already sent / the window is empty)
    if (len == 0) return; // нечего слать (запрос уже отправлен/пустое окно)
    io_uring_sqe* sqe = get_sqe();
    up->write_ctx.op = OpType::WRITE_UPSTREAM;
    up->write_ctx.ptr = up;
    up->write_ctx.gen = up->generation;
    up->inflight |= INFL_UW;
    // Шлем ровно столько, сколько принадлежит запросу: pipelined-хвост
    // Send exactly as many bytes as belong to the request: the pipelined tail
    // (следующие запросы клиента) не утекает на бэкенд.
    // (the client's next requests) must not leak to the backend.
    io_uring_prep_send(sqe, up->fd, conn->rx_buffer + conn->rx_sent, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &up->write_ctx);
}

void Server::submit_upstream_read(UpstreamConnection* up, char* dst, uint32_t len) {
    Connection* conn = up->client_conn;
    if (!conn) return;
    io_uring_sqe* sqe = get_sqe();
    up->read_ctx.op = OpType::READ_UPSTREAM;
    up->read_ctx.ptr = up;
    up->read_ctx.gen = up->generation;
    up->inflight |= INFL_UR;
    // The CQE does not know the buffer: we find it by where the op was submitted
    conn->proxy_read_dst = dst; // CQE не знает буфер: узнаем по месту подачи
    io_uring_prep_recv(sqe, up->fd, dst, len, 0);
    io_uring_sqe_set_data(sqe, &up->read_ctx);
}

// Предзагрузка следующего окна ответа бэкенда (двойной буфер). Читаем в
// Prefetch of the backend's next response window (double buffering). Read into
// буфер, СВОБОДНЫЙ от активного окна: активное в tx_buffer -> prefetch в
// the buffer FREE of the active window: active in tx_buffer -> prefetch into
// large_buf (acquire), активное в large_buf -> prefetch в tx_buffer
// large_buf (acquire), active in large_buf -> prefetch into tx_buffer
// (гарантированно свободен: в него читалось только окно N-1, доставленное
// (guaranteed free: only window N-1 was read into it, delivered
// до подачи чтения окна N). Пока клиент дочитывает окно N, ядро уже
// before window N's read was submitted). While the client finishes window N, the kernel
// качает окно N+1 — RTT бэкенда скрыт. Один READ_UPSTREAM в полете:
// already fetches window N+1 — the backend RTT is hidden. One READ_UPSTREAM in flight:
// backpressure прежняя (TCP-окно бэкенда). Пул large_buf исчерпан —
// backpressure stays the same (backend TCP window). large_buf pool exhausted —
// ping-pong через write_done (корректный фолбэк).
// ping-pong via write_done (a correct fallback).
void Server::submit_proxy_prefetch(Connection* conn) {
    UpstreamConnection* up = conn->upstream;
    if (!up || conn->prefetch_busy || conn->prefetch_ready || conn->resp.phase == 3) return;
    char* dst;
    uint32_t len;
    if (conn->proxy_win_ptr == conn->tx_buffer) {
        if (conn->prefetch_large_idx == 0xFFFFFFFFu) {
            // The pool is empty
            if (!acquire_large_buf(conn->prefetch_large_idx)) return; // пул пуст
        }
        dst = large_buffers_mem_ +
              static_cast<size_t>(conn->prefetch_large_idx) * large_buffer_size_;
        len = large_buffer_size_;
    } else {
        dst = conn->tx_buffer;
        len = conn->buffer_size;
    }
    conn->prefetch_ptr = dst;
    conn->prefetch_busy = true;
    submit_upstream_read(up, dst, len);
}

// Окно (или предзагрузка) в large_buf доставлено/отменено: буфер в пул.
// The window (or prefetch) in large_buf is delivered/cancelled: return the buffer to the pool.
void Server::release_prefetch_large(Connection* conn) {
    if (conn->prefetch_large_idx != 0xFFFFFFFFu) {
        release_large_buf(conn->prefetch_large_idx);
        conn->prefetch_large_idx = 0xFFFFFFFFu;
    }
}

// Main Event Loop: пачка завершенных операций обрабатывается БЕЗ единого
// Main Event Loop: a batch of completed operations is processed WITHOUT a single
// перехода в ядро (кроме самого wait и submit). Все хендлеры вызываются
// kernel transition (except wait and submit themselves). All handlers are called
// напрямую из CQ — никаких очередей и копирований.
// directly from the CQ — no queues or copies.
// ABA-защита: generation захватывается при подаче SQE и сверяется при
// ABA protection: the generation is captured when the SQE is submitted and checked on
// завершении — устаревшие completion'ы переиспользованных слотов пула
// completion — stale completions of reused pool slots
// отбрасываются.
// are discarded.
void Server::run() {
    // Проталкиваем первичные опы (accept, shutdown) в ядро
    // Push the initial ops (accept, shutdown) to the kernel
    (void)io_uring_submit(&ring);

    while (true) {
        // Idle-таймауты: раз в секунду скан пула (Slowloris-защита).
        // Idle timeouts: pool scan once per second (Slowloris protection).
        // Скан на ВЕРХУ цикла + явный submit: close_connection() очередедит
        // Scan at the TOP of the loop + explicit submit: close_connection() queues
        // cancel-опы в SQ, и их нужно протолкнуть в ядро ДО ожидания.
        // cancel ops in the SQ, and they must be pushed to the kernel BEFORE waiting.
        uint64_t now = now_ms();
        if (now - last_scan_ms >= 1000) {
            last_scan_ms = now;
            check_timeouts(now);
            (void)io_uring_submit(&ring);
        }

        struct io_uring_cqe* cqe = nullptr;

        // Быстрый путь: если CQ уже содержит completion'ы — работаем без
        // Fast path: if the CQ already holds completions — work without
        // единого syscall (peek читает shared-память). Тяжелый wait с
        // a single syscall (peek reads shared memory). The heavy wait with
        // таймером ядра — только когда кольцо действительно пусто.
        // a kernel timer — only when the ring is truly empty.
        int ret = io_uring_peek_cqe(&ring, &cqe);
        if (ret < 0) {
            // Кольцо пусто: засыпаем до первого события или таймаута.
            // Ring is empty: sleep until the first event or timeout.
            // Таймаут нужен, чтобы скан idle-таймаутов работал: без него
            // The timeout is needed so the idle-timeout scan works: without it
            // цикл блокировался бы навсегда на пустом ring (у epoll было 2 мс).
            // the loop would block forever on an empty ring (epoll had 2 ms).
            __kernel_timespec ts{};
            ts.tv_sec = 0;
            // 200 ms
            ts.tv_nsec = 200 * 1000 * 1000; // 200 мс
            ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            // No events: keep waiting
            if (ret == -ETIME) continue; // Нет событий: ждем дальше
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }

        // Обрабатываем ВСЕ завершившиеся операции без единого syscall
        // Process ALL completed ops without a single syscall
        unsigned head;
        unsigned completed = 0;
        bool stop = false;
        io_uring_for_each_cqe(&ring, head, cqe) {
            completed++;
            auto* ctx = static_cast<EventContext*>(io_uring_cqe_get_data(cqe));
            if (!ctx) continue;
            if (process_completion(ctx, cqe->res, cqe->flags)) {
                stop = true;
                break;
            }
        }

        // Пакетно освобождаем вычитанные CQE и проталкиваем новые SQE
        // Release the consumed CQEs in a batch and push new SQEs
        io_uring_cq_advance(&ring, completed);
        (void)io_uring_submit(&ring);

        if (stop) break;
    }
}

bool Server::process_completion(EventContext* ctx, int res, uint32_t cqe_flags) {
    switch (ctx->op) {
    case OpType::SHUTDOWN:
        // stop signal: exit the loop
        return true; // сигнал остановки: выходим из цикла

    case OpType::ACCEPT:
        handle_accept(res, cqe_flags);
        break;

    case OpType::READ_CLIENT: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_R;
        on_client_read(conn, res);
        break;
    }
    case OpType::WRITE_CLIENT: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_W;
        on_client_write(conn, res);
        break;
    }
    case OpType::POLL_IN: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_R;
        on_poll_in(conn, res);
        break;
    }
    case OpType::POLL_OUT: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_W;
        on_poll_out(conn, res);
        break;
    }
    case OpType::CONNECT_UPSTREAM: {
        auto* up = static_cast<UpstreamConnection*>(ctx->ptr);
        if (ctx->gen != up->generation) break;
        up->inflight &= ~INFL_UC;
        on_upstream_connect(up, res);
        break;
    }
    case OpType::WRITE_UPSTREAM: {
        auto* up = static_cast<UpstreamConnection*>(ctx->ptr);
        Connection* conn = (ctx->gen == up->generation) ? up->client_conn : nullptr;
        if (!conn || conn->state == State::CLOSE) break;
        if (ctx->gen == up->generation) up->inflight &= ~INFL_UW;
        on_upstream_write(conn, res);
        break;
    }
    case OpType::READ_UPSTREAM: {
        auto* up = static_cast<UpstreamConnection*>(ctx->ptr);
        Connection* conn = (ctx->gen == up->generation) ? up->client_conn : nullptr;
        if (!conn || conn->state == State::CLOSE) break;
        if (ctx->gen == up->generation) up->inflight &= ~INFL_UR;
        on_upstream_read(conn, res);
        break;
    }
    case OpType::READ_FILE: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_F;
        on_file_read(conn, res);
        break;
    }
    case OpType::SEND_ZC: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        // Каждый SEND_ZC шлет второй CQE-нотификацию (IORING_CQE_F_NOTIF):
        // Each SEND_ZC sends a second CQE notification (IORING_CQE_F_NOTIF):
        // буфер зарегистрирован (страницы пинятся ядром), так что нотификация
        // the buffer is registered (pages are pinned by the kernel), so the notification
        // чисто информационная — игнорируем, ничего не переподаем.
        // is purely informational — we ignore it and re-submit nothing.
        if (cqe_flags & IORING_CQE_F_NOTIF) break;
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_Z;
        on_send_zc(conn, res);
        break;
    }
    case OpType::SPLICE_IN: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_SI;
        on_splice_in(conn, res);
        break;
    }
    case OpType::SPLICE_OUT: {
        auto* conn = static_cast<Connection*>(ctx->ptr);
        if (ctx->gen != conn->generation || conn->state == State::CLOSE) break;
        conn->inflight &= ~INFL_SO;
        on_splice_out(conn, res);
        break;
    }
    case OpType::RELOAD: {
        // Hot Reload: воркер меняет сырые указатели (0 оверхеда) и
        // Hot Reload: the worker swaps raw pointers (0 overhead) and
        // перестраивает upstream-кластеры из нового конфига. Старые конфиг/
        // rebuilds the upstream clusters from the new config. The old config/
        // роутер живут еще 10 секунд (GC в main.cpp) — in-flight запросы
        // router stay alive for 10 more seconds (GC in main.cpp) — in-flight requests
        // дорабатывают безопасно.
        // finish safely.
        config_ = g_active_state.config;
        handler_ = g_active_state.router;
        rebuild_clusters();
        // Переподаем слушателя reload-канала
        // Re-submit the reload-channel listener
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_read(sqe, reload_fd_, &reload_buf_, sizeof(reload_buf_), 0);
        io_uring_sqe_set_data(sqe, &reload_ctx_);
        break;
    }
    case OpType::POLL_UPSTREAM_IDLE: {
        auto* up = static_cast<UpstreamConnection*>(ctx->ptr);
        if (ctx->gen != up->generation) break;
        on_upstream_idle_poll(up, res);
        break;
    }
    }
    return false;
}

void Server::handle_accept(int res, uint32_t cqe_flags) {
    if (res >= 0) {
        Connection* conn = conn_pool.acquire();
        if (!conn) {
            // No memory: drop the connection (Zero RAM growth)
            (void)close(res); // Нет памяти, дропаем соединение (Zero RAM growth)
        } else {
            conn->reset();
            conn->state = State::ACCEPT;
            conn->fd = res;
            conn->last_activity_ms = now_ms();
            set_nonblocking(res);
            set_tcp_nodelay(res);

            if (config_->enable_ssl) {
                conn->flags |= FLAG_IS_SSL;
                // Ленивый mbedtls_ssl_setup(): calloc буферов in/out (16+16 КБ
                // Lazy mbedtls_ssl_setup(): calloc of the in/out buffers (16+16 KB
                // каждая) только для реально подключающихся клиентов. Обычный
                // each) only for actually connecting clients. A normal
                // keep-alive слот в покое держит только rx/tx (8+8 КБ).
                // keep-alive slot holds only rx/tx at rest (8+8 KB).
                bool ssl_ok = true;
                if (!conn->ssl_setup) {
                    if (mbedtls_ssl_setup(&conn->ssl, &ssl_conf) != 0) {
                        // OOM: slot without an SSL context
                        ssl_ok = false; // OOM: слот без SSL-контекста
                    } else {
                        conn->ssl_setup = true;
                    }
                }
                if (ssl_ok) {
                    // В accept() - только привязка BIO и рукопожатие, без аллокаций.
                    // In accept() - only BIO binding and handshake, no allocations.
                    mbedtls_ssl_set_bio(&conn->ssl, conn, custom_bio_send, custom_bio_recv, nullptr);
                    conn->state = State::SSL_HANDSHAKE;
                    handle_ssl_handshake(conn);
                } else {
                    conn->state = State::CLOSE;
                    close_connection(conn);
                }
            } else {
                // Сразу просим ядро читать данные из нового клиента
                // Immediately ask the kernel to read data from the new client
                submit_read(conn);
            }
        }
    }

    // Ядро молча снимает multishot accept при EMFILE/ENFILE, переполнении
    // The kernel silently disarms multishot accept on EMFILE/ENFILE, io_uring
    // внутреннего буфера io_uring и т.п.: CQE приходит с ошибкой и БЕЗ флага
    // internal buffer overflow, etc.: the CQE arrives with an error and WITHOUT the
    // IORING_CQE_F_MORE (флаг остаётся только при транзиентных ошибках, когда
    // IORING_CQE_F_MORE flag (the flag stays only on transient errors, when
    // стрим жив). Если не переподать — сервер навсегда перестанет принимать
    // the stream is alive). If not re-submitted — the server stops accepting
    // соединения. Busy-loop невозможен: мультишот завершается только на событие.
    // connections forever. No busy-loop is possible: multishot completes only on an event.
    // Во время шатдауна не переподаём (ring скоро уничтожается).
    // Do not re-submit during shutdown (the ring is about to be destroyed).
    if (!(cqe_flags & IORING_CQE_F_MORE) && shutdown_buf_ == 0) {
        submit_accept();
    }
}

void Server::handle_ssl_handshake(Connection* conn) {
    int ret = mbedtls_ssl_handshake(&conn->ssl);
    if (ret == 0) {
        conn->state = State::ACCEPT;
        // В последних handshake-записях может быть буферизовано прикладных
        // The last handshake records may buffer application
        // данных — пробуем прочитать сразу (не ждем нового poll-события).
        // data — try to read immediately (do not wait for a new poll event).
        handle_read(conn);
        if (conn->state == State::ACCEPT) submit_poll(conn, OpType::POLL_IN);
    } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        // WANT_READ после EOF = клиент ушёл по TCP без close_notify: poll
        // WANT_READ after EOF = the client left over TCP without close_notify: poll
        // будет мгновенно «читаем» вечно, а рукопожатие никогда не продвинется.
        // will be instantly "readable" forever, and the handshake will never progress.
        if (conn->flags & FLAG_SSL_EOF) {
            close_connection(conn);
            return;
        }
        submit_poll(conn, OpType::POLL_IN);
    } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (conn->flags & FLAG_SSL_EOF) {
            close_connection(conn);
            return;
        }
        submit_poll(conn, OpType::POLL_OUT);
    } else {
        close_connection(conn);
    }
}

// Парсинг и обработка накопленных в rx_buffer данных (общий для plaintext
// Parsing and processing of the data accumulated in rx_buffer (common for plaintext
// и TLS). Keep-alive + pipelining: после ответа потребленные байты удаляются
// and TLS). Keep-alive + pipelining: after a response the consumed bytes are removed
// из буфера (memmove), остаток обрабатывается в следующем цикле.
// from the buffer (memmove), the remainder is processed in the next loop.
// Ответ теперь уходит АСИНХРОННО: хендлер только заполняет tx_buffer и
// The response now goes out ASYNCHRONOUSLY: the handler only fills tx_buffer and
// подает WRITE_CLIENT — завершение доставки обрабатывает write_done().
// submits WRITE_CLIENT — delivery completion is handled by write_done().
// Прокси-маршруты (wants_streaming): при неполном теле (INCOMPLETE) или теле
// Proxy routes (wants_streaming): on an incomplete body (INCOMPLETE) or a body
// больше буфера (TOO_LARGE) хендлер вызывается сразу после заголовков —
// larger than the buffer (TOO_LARGE) the handler is called right after the headers —
// остаток тела доставит прокси-стриминг (PROXY_UPLOADING).
// the rest of the body will be delivered by proxy streaming (PROXY_UPLOADING).
void Server::process_buffered(Connection* conn) {
    while (true) {
        if (conn->rx_bytes > 0) {
            HttpRequest req;
            size_t consumed = 0;
            ParseResult res = HttpParser::parse(conn, req, consumed);
            if (res == ParseResult::BAD_REQUEST) {
                send_error(conn, 400); // 400 + Connection: close
                return;
            }
            if (res == ParseResult::OK) {
                // Глобальный рубильник keep-alive: при enable_keep_alive=false
                // Global keep-alive switch: with enable_keep_alive=false
                // соединение закрывается после ответа, несмотря на желание клиента
                // the connection is closed after the response, despite the client's wishes
                if (req.keep_alive && config_->enable_keep_alive) {
                    conn->flags |= FLAG_KEEP_ALIVE;
                } else {
                    conn->flags &= ~FLAG_KEEP_ALIVE;
                }
                if (req.support_gzip && config_->enable_gzip) conn->flags |= FLAG_USE_GZIP;

                // Границы прокси-запроса стешируются сейчас: start_proxy()
                // The proxy request boundaries are cached now: start_proxy()
                // определит конец запроса (заголовки + тело) без pipelined-хвоста.
                // will determine the end of the request (headers + body) without the pipelined tail.
                conn->proxy_header_len = req.header_len;
                conn->proxy_content_length = req.content_length;
                conn->proxy_is_chunked = req.chunked;
                conn->proxy_is_head = (req.method == "HEAD");

                if (!handler_) {
                    send_error(conn, 500);
                    return;
                }
                // Длина текущего запроса: нужна start_proxy(), чтобы не утечь
                // Current request length: needed by start_proxy() so the
                // pipelined-хвост буфера на бэкенд.
                // pipelined tail of the buffer does not leak to the backend.
                handler_->on_request(*this, conn, req);
                if (conn->state == State::CLOSE) return;

                // Потребляем байты запроса ДО выхода. Если этого не сделать
                // Consume the request bytes BEFORE exiting. If this is not done
                // при state == WRITE_RESPONSE (ответ уже подан в ring),
                // with state == WRITE_RESPONSE (the response is already in the ring),
                // повторный вход из write_done() заново распарсит этот же
                // a re-entry from write_done() would re-parse this same
                // запрос и отправит дубликат ответа.
                // request and send a duplicate response.
                // READ_STATIC_FILE/WRITE_FILE_HEADERS/SEND_STATIC: rx_buffer
                // еще понадобится для pipelined запросов (файл читается в
                // will still be needed for pipelined requests (the file is read into
                // tx_buffer), потребляем сейчас. SEND_STATIC достижим прямо
                // tx_buffer), so consume now. SEND_STATIC is reachable directly
                // в on_request: TLS-чанки, заголовки ушли инлайн-дренажем.
                // in on_request: TLS chunks, headers went out via inline drain.
                if (conn->state == State::ACCEPT || conn->state == State::WRITE_RESPONSE ||
                    conn->state == State::READ_STATIC_FILE ||
                    conn->state == State::WRITE_FILE_HEADERS ||
                    conn->state == State::SEND_STATIC ||
                    conn->state == State::SEND_SPLICE) {
                    if (consumed < static_cast<size_t>(conn->rx_bytes)) {
                        std::memmove(conn->rx_buffer, conn->rx_buffer + consumed,
                                     static_cast<size_t>(conn->rx_bytes - consumed));
                    }
                    conn->rx_bytes -= static_cast<int>(consumed);
                }

                if (conn->state != State::ACCEPT) {
                    // WRITE_RESPONSE: tx_buffer занят неотправленным ответом,
                    // WRITE_RESPONSE: tx_buffer holds an unsent response,
                    // дальше парсить нельзя (следующий ответ перезапишет его).
                    // parsing further is impossible (the next response would overwrite it).
                    // Повторный вход произойдет по завершению WRITE_CLIENT.
                    // Re-entry will happen on WRITE_CLIENT completion.
                    // PROXY_*/READ_STATIC_FILE: буферы принадлежат прокси/диску.
                    // PROXY_*/READ_STATIC_FILE: the buffers belong to the proxy/disk.
                    return;
                }
                // Pipelining: parse the next request
                continue; // pipelining: парсим следующий запрос
            }
            // INCOMPLETE или TOO_LARGE. Если заголовки уже распарсены
            // INCOMPLETE or TOO_LARGE. If the headers are already parsed
            // (req.header_len > 0) и маршрут стриминговый — диспатчим хендлеру
            // (req.header_len > 0) and the route is streaming — dispatch to the handler
            // сразу: тело (возможно, больше буфера) докачает прокси-стриминг.
            // immediately: the body (possibly larger than the buffer) will be fetched by proxy streaming.
            // TOO_LARGE без стриминга ниже уходит в 413 (как раньше).
            // TOO_LARGE without streaming goes to 413 below (as before).
            if (conn->state == State::ACCEPT && req.header_len > 0 && handler_ &&
                handler_->wants_streaming(req)) {
                conn->flags &= ~FLAG_KEEP_ALIVE;
                if (req.keep_alive && config_->enable_keep_alive) conn->flags |= FLAG_KEEP_ALIVE;
                conn->proxy_header_len = req.header_len;
                conn->proxy_content_length = req.content_length;
                conn->proxy_is_chunked = req.chunked;
                conn->proxy_is_head = (req.method == "HEAD");

                handler_->on_request(*this, conn, req);
                if (conn->state == State::CLOSE) return;
                // НЕ потребляем байты: rx_buffer теперь принадлежит
                // Do NOT consume bytes: rx_buffer now belongs to
                // прокси-стримингу (PROXY_UPLOADING), который вышлет их на
                // proxy streaming (PROXY_UPLOADING), which will send them to
                // бэкенд и продолжит дочитывать тело.
                // the backend and continue reading the body.
                return;
            }
            if (res == ParseResult::TOO_LARGE) {
                send_error(conn, 413);
                return;
            }
            // INCOMPLETE: ждем остаток заголовков/тела
            // INCOMPLETE: wait for the rest of the headers/body
        }
        return;
    }
}

// TLS: синхронный дренаж mbedTLS (recv через BIO, неблокирующий).
// TLS: synchronous mbedTLS drain (recv via BIO, non-blocking).
// Вызывается по POLL_IN-готовности; цикл до EAGAIN (как ET).
// Called on POLL_IN readiness; loops until EAGAIN (like ET).
// В PROXY_UPLOADING: вычитываем тело запроса и форвардим на бэкенд
// In PROXY_UPLOADING: read the request body and forward it to the backend
// (backpressure: чтение останавливается, пока WRITE_UPSTREAM в полете).
// (backpressure: reading stops while WRITE_UPSTREAM is in flight).
void Server::handle_read(Connection* conn) {
    if (conn->state == State::PROXY_UPLOADING) {
        while (true) {
            // buffer full
            if (conn->rx_bytes >= static_cast<int>(conn->buffer_size)) return; // буфер полон
            ssize_t n = io_read(conn, conn->rx_buffer + conn->rx_bytes,
                                static_cast<size_t>(conn->buffer_size) -
                                    static_cast<size_t>(conn->rx_bytes));
            if (n > 0) {
                conn->rx_bytes += static_cast<int>(n);
                conn->last_activity_ms = now_ms();
                proxy_on_client_data(conn);
                // Прокси решил судьбу соединения (ответ/закрытие) или подал
                // The proxy decided the connection's fate (response/close) or submitted
                // WRITE_UPSTREAM (rx_sent < rx_bytes): дальше не читаем,
                // WRITE_UPSTREAM (rx_sent < rx_bytes): stop reading,
                // следующее чтение запустит on_upstream_write после дренажа.
                // the next read will be started by on_upstream_write after the drain.
                if (conn->state != State::PROXY_UPLOADING) return;
                if (conn->rx_sent < conn->rx_bytes) return;
                continue;
            }
            if (n == 0 || (n == -1 && errno != EAGAIN)) {
                close_connection(conn);
                return;
            }
            // EAGAIN: no more data, wait for the next POLL_IN
            return; // EAGAIN: данных больше нет, ждем следующего POLL_IN
        }
    }
    while (true) {
        process_buffered(conn);
        if (conn->state != State::ACCEPT) return;

        // Protection
        if (conn->rx_bytes >= static_cast<int>(conn->buffer_size)) { // Защита
            close_connection(conn);
            return;
        }

        ssize_t n = io_read(conn, conn->rx_buffer + conn->rx_bytes,
                            static_cast<size_t>(conn->buffer_size) -
                                static_cast<size_t>(conn->rx_bytes));
        if (n > 0) {
            conn->rx_bytes += static_cast<int>(n);
            conn->last_activity_ms = now_ms();
            continue;
        }
        if (n == 0 || (n == -1 && errno != EAGAIN)) {
            close_connection(conn);
            return;
        }
        // EAGAIN: no more data
        return; // EAGAIN: данных больше нет
    }
}

// READ_CLIENT (plaintext) завершен: порция данных в rx_buffer
// READ_CLIENT (plaintext) completed: a chunk of data is in rx_buffer
void Server::on_client_read(Connection* conn, int res) {
    if (res <= 0) {
        // 0: клиент закрыл сокет, <0: ошибка
        // 0: the client closed the socket, <0: error
        close_connection(conn);
        return;
    }
    conn->rx_bytes += res;
    conn->last_activity_ms = now_ms();

    if (conn->state == State::PROXY_UPLOADING) {
        // Стримим тело запроса на бэкенд (минуя парсер)
        // Stream the request body to the backend (bypassing the parser)
        proxy_on_client_data(conn);
        return;
    }
    process_buffered(conn);
    if (conn->state == State::ACCEPT) {
        if (conn->flags & FLAG_IS_SSL) {
            submit_poll(conn, OpType::POLL_IN);
        } else if (conn->rx_bytes < static_cast<int>(conn->buffer_size)) {
            submit_read(conn);
        }
    }
}

// WRITE_CLIENT (plaintext) завершен: порция tx_buffer ушла в сеть
// WRITE_CLIENT (plaintext) completed: a chunk of tx_buffer went to the network
void Server::on_client_write(Connection* conn, int res) {
    if (res <= 0) {
        close_connection(conn);
        return;
    }
    conn->tx_sent += res;
    conn->last_activity_ms = now_ms();
    if (conn->tx_sent < conn->tx_bytes) {
        // Не всё отправили — засылаем остаток
        // Not everything was sent — send the remainder
        submit_write(conn);
        return;
    }
    write_done(conn);
}

// TLS: сокет готов к чтению (poll POLLIN)
// TLS: the socket is ready for reading (poll POLLIN)
void Server::on_poll_in(Connection* conn, int res) {
    // error/close
    if (res <= 0) { // ошибка/закрытие
        close_connection(conn);
        return;
    }
    if (conn->state == State::SSL_HANDSHAKE) {
        handle_ssl_handshake(conn);
        return;
    }
    if (conn->state == State::PROXY_UPLOADING) {
        // Стримим тело запроса: вычитываем TLS-поток и форвардим на бэкенд
        // Stream the request body: read the TLS stream and forward it to the backend
        handle_read(conn);
        if (conn->state != State::PROXY_UPLOADING) return;
        // Переподаем poll, только если нет WRITE_UPSTREAM в полете:
        // Re-arm poll only if there is no WRITE_UPSTREAM in flight:
        // иначе следующее чтение запустит on_upstream_write после дренажа.
        // otherwise the next read will be started by on_upstream_write after the drain.
        if (conn->rx_sent >= conn->rx_bytes) submit_poll(conn, OpType::POLL_IN);
        return;
    }
    handle_read(conn);
    if (conn->state == State::ACCEPT) submit_poll(conn, OpType::POLL_IN);
}

// TLS: сокет готов к записи (poll POLLOUT)
// TLS: the socket is ready for writing (poll POLLOUT)
void Server::on_poll_out(Connection* conn, int res) {
    if (res <= 0) {
        close_connection(conn);
        return;
    }
    if (conn->state == State::SSL_HANDSHAKE) {
        handle_ssl_handshake(conn);
        return;
    }
    if (conn->state == State::SEND_TLS_CHUNK) {
        if (conn->large_buf) {
            continue_large_tls_chunk(conn);
        } else {
            continue_tls_chunk(conn);
        }
        return;
    }
    if (conn->state == State::SEND_SPLICE) {
        // Сокет освободился: досливаем остаток пайпа (solo SPLICE_OUT)
        // The socket freed up: drain the pipe remainder (solo SPLICE_OUT)
        submit_splice(conn);
        return;
    }
    // PROXY_STREAMING: окно в proxy_win_ptr (tx_buffer или large_buf);
    // PROXY_STREAMING: the window is in proxy_win_ptr (tx_buffer or large_buf);
    // WRITE_RESPONSE: статика/ошибки всегда в tx_buffer.
    // WRITE_RESPONSE: static/errors are always in tx_buffer.
    char* win = (conn->state == State::PROXY_STREAMING) ? conn->proxy_win_ptr
                                                        : conn->tx_buffer;
    DrainResult r = drain_client(conn, win);
    if (r == DrainResult::CLOSED) return;
    if (r == DrainResult::WOULD_BLOCK) {
        submit_poll(conn, OpType::POLL_OUT);
        return;
    }
    write_done(conn);
}

// Keep-alive-сброс после полной доставки ответа: tx сброшен, pipelined-хвост
// Keep-alive reset after the response is fully delivered: tx is reset, the pipelined tail
// из rx_buffer разбирается заново, читаем следующий запрос. Общий для
// in rx_buffer is re-parsed, and the next request is read. Shared by
// write_done(), finish_proxy_response() и zero-copy статики.
// write_done(), finish_proxy_response() and zero-copy static.
void Server::reset_for_keep_alive(Connection* conn) {
    conn->tx_bytes = 0;
    conn->tx_sent = 0;
    // Preserve the SSL flag on reset
    conn->flags &= FLAG_IS_SSL; // Сохраняем флаг SSL при сбросе
    conn->state = State::ACCEPT;

    if (conn->rx_bytes > 0) {
        process_buffered(conn);
        if (conn->state == State::ACCEPT) {
            if (conn->flags & FLAG_IS_SSL) {
                submit_poll(conn, OpType::POLL_IN);
            } else if (conn->rx_bytes < static_cast<int>(conn->buffer_size)) {
                submit_read(conn);
            }
        }
    } else if (conn->flags & FLAG_IS_SSL) {
        submit_poll(conn, OpType::POLL_IN);
    } else {
        submit_read(conn);
    }
}

// tx_buffer доставлен клиенту целиком: keep-alive-сброс, pipelined-разбор
// tx_buffer fully delivered to the client: keep-alive reset, pipelined parsing
// или возврат к прокси-стримингу (backpressure снята).
// or a return to proxy streaming (backpressure released).
void Server::write_done(Connection* conn) {
    // Заголовки файла ушли в сеть: стартуем отдачу тела (splice или zero-copy)
    // The file headers went to the network: start delivering the body (splice or zero-copy)
    if (conn->state == State::WRITE_FILE_HEADERS) {
        if (conn->pipe_idx != -1) {
            conn->state = State::SEND_SPLICE;
            if (conn->file_offset >= conn->file_size) {
                // empty file: nothing to stream
                finish_splice_stream(conn); // пустой файл: качать нечего
                return;
            }
            submit_splice(conn);
            return;
        }
        conn->state = State::SEND_STATIC;
        if (conn->file_offset >= conn->file_size) {
            // empty file: nothing to stream
            finish_send_static(conn); // пустой файл: качать нечего
            return;
        }
        submit_file_chunk(conn);
        return;
    }

    if (conn->state == State::PROXY_STREAMING) {
        // Клиент освободился. Если фрейминг ответа определил конец —
        // The client freed up. If the response framing determined the end —
        // бэкенд в idle-пул (upstream keep-alive) / закрытие; иначе
        // the backend goes to the idle pool (upstream keep-alive) / is closed; otherwise
        // предзагруженное окно ждет диспатча (двойной буфер: RTT бэкенда
        // the prefetched window waits for dispatch (double buffering: the backend RTT
        // уже скрыт) или подаем предзагрузку следующего окна.
        // is already hidden) or we submit the next window prefetch.
        if (conn->resp.phase == 3) {
            finish_proxy_response(conn);
        } else if (conn->prefetch_ready) {
            conn->prefetch_ready = false;
            dispatch_proxy_window(conn, static_cast<int>(conn->prefetch_len),
                                  conn->prefetch_ptr);
        } else {
            // ping-pong fallback when the pool is empty
            submit_proxy_prefetch(conn); // ping-pong фолбэк при пустом пуле
        }
        return;
    }

    if (conn->flags & FLAG_KEEP_ALIVE) {
        // rx_bytes НЕ трогаем: в буфере может лежать pipelined запрос,
        // Do NOT touch rx_bytes: the buffer may hold a pipelined request,
        // его потребление и сдвиг делает process_buffered() (иначе - потеря)
        // its consumption and shifting is done by process_buffered() (otherwise - data loss)
        reset_for_keep_alive(conn);
    } else {
        close_connection(conn);
    }
}

// ===== Upstream keep-alive: idle-пулы =====
// ===== Upstream keep-alive: idle pools =====

// Взятие idle-соединения кластера (Zero RTT: без connect к бэкенду).
// Taking an idle connection of the cluster (Zero RTT: no connect to the backend).
// Пул per-кластер, но соединение принадлежит КОНКРЕТНОМУ узлу (Round-Robin
// The pool is per-cluster, but the connection belongs to a SPECIFIC node (Round-Robin
// выбрал его): выдаем только слот с подходящим node_idx, иначе RR
// chose it): hand out only a slot with the matching node_idx, otherwise RR
// разбалансируется (запросы пойдут не на выбранный бэкенд).
// gets unbalanced (requests would not go to the chosen backend).
UpstreamConnection* Server::get_idle_upstream(int cluster_idx, int node_idx) {
    auto& pool = clusters_[cluster_idx].idle;
    for (size_t i = 0; i < pool.size(); ++i) {
        UpstreamConnection* up = pool[i];
        if (up->node_idx != node_idx) continue;
        pool[i] = pool.back();
        pool.pop_back();
        if (up->fd == -1) {
            // Мертвый слот (страховка): освобождаем в пул
            // Dead slot (safety net): release it to the pool
            up->generation++;
            up->client_conn = nullptr;
            upstream_pool.release(up);
            // do not try other slots: the request is fresh, RR stays honest
            return nullptr; // не пробуем другие слоты: запрос свежий, RR честен
        }
        // Idle-poll НЕ отменяем: слот снова активен, но poll продолжает висеть
        // The idle-poll is NOT cancelled: the slot is active again, but the poll keeps hanging
        // до первого события (данные/FIN с бэкенда). Его completion придет с
        // until the first event (data/FIN from the backend). Its completion will arrive with
        // устаревшим gen (bump ниже) и будет отброшен в process_completion;
        // a stale gen (bump below) and will be discarded in process_completion;
        // on_upstream_idle_poll дополнительно защищен `!up->is_idle` return.
        // on_upstream_idle_poll is additionally protected by a `!up->is_idle` return.
        // Отмена здесь была бы холостым IORING_OP_CANCEL (-ENOENT) на каждое
        // Cancelling here would be an idle IORING_OP_CANCEL (-ENOENT) on every
        // переиспользование keep-alive соединения — лишний скан опов кольца.
        // keep-alive connection reuse — an extra scan of the ring's ops.
        // a stale idle-poll completion will not touch the slot
        up->generation++; // устаревший completion idle-полла не тронет слот
        up->is_idle = false;
        return up;
    }
    return nullptr;
}

// Возврат бэкенда в idle-пул после полного ответа: вешаем poll POLLIN,
// Return the backend to the idle pool after a complete response: arm poll POLLIN,
// чтобы сразу поймать FIN бэкенда и не копить мертвые сокеты.
// to catch the backend's FIN immediately and not accumulate dead sockets.
void Server::release_upstream_to_idle(UpstreamConnection* up) {
    if (!up || up->fd == -1) return;
    

    up->client_conn = nullptr;
    up->is_idle = true;
    up->idle_since_ms = now_ms();
    if (up->cluster_idx >= 0 && up->cluster_idx < static_cast<int>(clusters_.size())) {
        clusters_[up->cluster_idx].idle.push_back(up);
    } else {
        // Изолированный слот (не кластерный) — закрываем
        // Isolated slot (not cluster-based) — close it
        (void)close(up->fd);
        up->fd = -1;
        up->generation++;
        upstream_pool.release(up);
        return;
    }
    // Следим за сокетом: если бэкенд закроет соединение в простое (FIN),
    // Watch the socket: if the backend closes the connection while idle (FIN),
    // io_uring доставит POLLIN — почистим слот без ожидания таймаута.
    // io_uring will deliver POLLIN — we clean the slot without waiting for the timeout.
    io_uring_sqe* sqe = get_sqe();
    up->idle_ctx.op = OpType::POLL_UPSTREAM_IDLE;
    up->idle_ctx.ptr = up;
    up->idle_ctx.gen = up->generation;
    io_uring_prep_poll_add(sqe, up->fd, POLLIN);
    io_uring_sqe_set_data(sqe, &up->idle_ctx);
}

// Закрытие idle-соединения: вынимаем из пула, отменяем idle-poll,
// Closing an idle connection: remove from the pool, cancel the idle-poll,
// освобождаем слот upstream-пула.
// release the upstream pool slot.
void Server::discard_idle_upstream(UpstreamConnection* up) {
    // already reused/closed
    if (!up->is_idle) return; // уже переиспользован/закрыт
    // Вынимаем из idle-пула кластера (swap-pop по указателю)
    // Remove from the cluster's idle pool (swap-pop by pointer)
    if (up->cluster_idx >= 0 && up->cluster_idx < static_cast<int>(clusters_.size())) {
        auto& pool = clusters_[up->cluster_idx].idle;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i] == up) {
                pool[i] = pool.back();
                pool.pop_back();
                break;
            }
        }
    }
    up->is_idle = false;
    // Idle-poll может быть в полете (таймаут-скан): отменяем, иначе poll
    // The idle-poll may be in flight (timeout scan): cancel it, otherwise the poll
    // удержит file в ядре до своего завершения.
    // would hold the file in the kernel until it completes.
    io_uring_sqe* sqe = get_sqe();
    io_uring_prep_cancel(sqe, &up->idle_ctx, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    if (up->fd != -1) {
        (void)close(up->fd);
        up->fd = -1;
    }
    // new slot epoch: stale completions will be discarded
    up->generation++; // новая эпоха слота: stale completion'ы отбросятся
    up->client_conn = nullptr;
    upstream_pool.release(up);
}

// Освобождение слота под новый коннект: выбрасываем старейшее idle-соединение
// Freeing a slot for a new connection: evict the oldest idle connection
// (иначе заполненный idle-пул заблокирует проксирование).
// (otherwise a full idle pool would block proxying).
bool Server::evict_oldest_idle() {
    UpstreamConnection* oldest = nullptr;
    uint64_t oldest_since = UINT64_MAX;
    for (const auto& cs : clusters_) {
        for (UpstreamConnection* up : cs.idle) {
            if (up->idle_since_ms < oldest_since) {
                oldest_since = up->idle_since_ms;
                oldest = up;
            }
        }
    }
    if (!oldest) return false;
    discard_idle_upstream(oldest);
    return true;
}

// Бэкенд закрыл (FIN) или ошибка на idle-соединении: убираем из пула.
// The backend closed (FIN) or errored on the idle connection: remove it from the pool.
void Server::on_upstream_idle_poll(UpstreamConnection* up, int res) {
    (void)res;
    // the slot was reused earlier (ABA safeguard)
    if (!up->is_idle) return; // слот переиспользован раньше (ABA-страховка)
    // POLLIN на idle-сокете = FIN/данные (бэкенд не должен слать данные
    // POLLIN on an idle socket = FIN/data (the backend must not send data
    // без запроса) — считаем соединение мертвым.
    // without a request) — treat the connection as dead.
    discard_idle_upstream(up);
}

// ===== Прокси-стриминг =====
// ===== Proxy streaming =====

void Server::handle_proxy_connect(Connection* conn, int cluster_idx, int node_idx) {
    // 1. Upstream keep-alive: сначала ищем живое idle-соединение кластера
    // 1. Upstream keep-alive: first look for a live idle connection of the cluster
    // (Zero RTT: без socket()/connect() и SYN-SYN/ACK-ACK к бэкенду)
    // (Zero RTT: no socket()/connect() and SYN-SYN/ACK-ACK to the backend)
    if (UpstreamConnection* up = get_idle_upstream(cluster_idx, node_idx)) {
        

        up->client_conn = conn;
        conn->upstream = up;
        conn->rx_sent = 0;
        conn->state = State::PROXY_UPLOADING;
        // Весь запрос уже в rx_buffer: Fast-Exit (единственный WRITE_UPSTREAM)
        // The whole request is already in rx_buffer: Fast-Exit (the only WRITE_UPSTREAM)
        proxy_on_client_data(conn);
        return;
    }

    // 2. Idle нет: создаем новое соединение с бэкендом
    // 2. No idle: create a new connection to the backend
    UpstreamConnection* up = upstream_pool.acquire();
    if (!up) {
        // Пул исчерпан: закрываем старейшее idle-соединение и пробуем снова
        // Pool exhausted: close the oldest idle connection and retry
        if (!evict_oldest_idle()) {
            // No backend resources
            send_error(conn, 502); // Нет ресурсов на бэкенд
            return;
        }
        up = upstream_pool.acquire();
        if (!up) {
            send_error(conn, 502);
            return;
        }
    }

    up->reset();
    up->cluster_idx = cluster_idx;
    up->node_idx = node_idx;
    up->client_conn = conn;
    conn->upstream = up;

    const BackendConfig& backend = clusters_[cluster_idx].nodes[static_cast<size_t>(node_idx)];
    up->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (up->fd < 0) {
        close_connection(conn);
        return;
    }
    set_nonblocking(up->fd);
    set_tcp_nodelay(up->fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend.port);
    if (inet_pton(AF_INET, backend.ip.c_str(), &addr.sin_addr) != 1) {
        (void)close(up->fd);
        up->fd = -1;
        close_connection(conn);
        return;
    }

    conn->state = State::PROXY_CONNECTING;
    submit_upstream_connect(up, addr);
}

// Единый автомат прокси: PROXY_CONNECTING -> PROXY_UPLOADING -> PROXY_STREAMING.
// The unified proxy state machine: PROXY_CONNECTING -> PROXY_UPLOADING -> PROXY_STREAMING.
// PROXY_UPLOADING доставляет на бэкенд ВЕСЬ запрос (заголовки + тело) с
// PROXY_UPLOADING delivers the ENTIRE request (headers + body) to the backend with
// потоковой докачкой тела любого размера:
// streaming of a body of any size:
//   - малые запросы (тело уже в rx_buffer): Fast-Exit — один WRITE_UPSTREAM,
//   - small requests (body already in rx_buffer): Fast-Exit — one WRITE_UPSTREAM,
//     после которого сразу PROXY_STREAMING (0 лишних SQE против старого кода);
//     after which PROXY_STREAMING follows immediately (0 extra SQEs vs. the old code);
//   - большие тела: READ_CLIENT -> WRITE_UPSTREAM по мере поступления
//   - large bodies: READ_CLIENT -> WRITE_UPSTREAM as data arrives
//     (backpressure: следующее чтение только после дренажа окна).
//     (backpressure: the next read only after the window is drained).
// Границы запроса (proxy_request_end) защищают от утечки pipelined-хвоста
// The request boundaries (proxy_request_end) prevent the pipelined tail from leaking
// на бэкенд; для chunked конец находит инкрементальный сканер.
// to the backend; for chunked the end is found by an incremental scanner.

// Сколько байт из текущего окна rx_buffer принадлежит запросу
// How many bytes of the current rx_buffer window belong to the request
static size_t proxy_sendable(Connection* conn) {
    size_t len = static_cast<size_t>(conn->rx_bytes - conn->rx_sent);
    if (conn->proxy_request_end != 0) {
        size_t sent_abs = conn->proxy_window_base + static_cast<size_t>(conn->rx_sent);
        // the request is already sent
        if (sent_abs >= conn->proxy_request_end) return 0; // запрос уже отправлен
        size_t room = conn->proxy_request_end - sent_abs;
        // clip: the pipelined tail must not leak
        if (len > room) len = room; // клип: pipelined-хвост не утекает
    }
    return len;
}

// Вся порция данных клиента -> бэкенд: сканер chunked, Fast-Exit или
// The whole client data chunk -> backend: chunked scanner, Fast-Exit or
// подача WRITE_UPSTREAM с клипом по концу запроса.
// submitting WRITE_UPSTREAM clipped at the end of the request.
void Server::proxy_on_client_data(Connection* conn) {
    if (conn->proxy_is_chunked && conn->proxy_request_end == 0) {
        // Ищем конец chunked-тела (0-чанк + трейлеры). Структурная ошибка -> 400.
        // Look for the end of the chunked body (0-chunk + trailers). Structural error -> 400.
        if (!scan_chunked(conn)) {
            send_error(conn, 400);
            return;
        }
    }

    // Fast-Exit: весь запрос уже ушел на бэкенд — стримим ответ
    // Fast-Exit: the whole request already went to the backend — stream the response
    if (conn->proxy_request_end != 0 &&
        conn->proxy_window_base + static_cast<size_t>(conn->rx_sent) >=
            conn->proxy_request_end) {
        conn->state = State::PROXY_STREAMING;
        submit_upstream_read(conn->upstream, conn->tx_buffer, conn->buffer_size);
        return;
    }

    if (conn->rx_sent < conn->rx_bytes) {
        conn->state = State::PROXY_UPLOADING;
        // send what has accumulated (clipped)
        submit_upstream_write(conn); // шлем накопленное (с клипом)
        return;
    }
    // Буфер пуст, запрос не дописан: читать дальше будет on_upstream_write()
    // The buffer is empty and the request is incomplete: on_upstream_write() will read more
    // после дренажа (здесь недостижимо: вызывают с только что прочитанными
    // after the drain (unreachable here: callers pass freshly read
    // данными или заголовками, которые всегда > 0 байт).
    // data or headers, which are always > 0 bytes).
}

// Инкрементальный сканер chunked-фрейминга (стриминг, координаты текущего
// Incremental chunked-framing scanner (streaming, coordinates of the current
// окна rx_buffer, состояние переживает дренаж). В отличие от chunked_scan()
// rx_buffer window, state survives the drain). Unlike chunked_scan()
// в парсере НЕ требует полного тела в буфере и НЕ отклоняет чанки больше
// in the parser it does NOT require the full body in the buffer and does NOT reject chunks
// буфера (для прокси размер чанка не важен — байты льются потоком).
// larger than the buffer (for the proxy the chunk size is irrelevant — bytes just flow).
// При обнаружении последнего чанка + трейлеров устанавливает
// When the last chunk + trailers are found it sets
// conn->proxy_request_end = абсолютный конец запроса.
// conn->proxy_request_end = the absolute end of the request.
// Возвращает false при структурной ошибке (-> 400).
// Returns false on a structural error (-> 400).
bool Server::scan_chunked(Connection* conn) {
    const char* buf = conn->rx_buffer;
    while (true) {
        size_t avail = static_cast<size_t>(conn->rx_bytes) - conn->proxy_chunk_scan;
        switch (conn->proxy_chunk_phase) {
        // size line: "<hex>[;extensions]\r\n"
        case 0: { // строка размера: "<hex>[;extensions]\r\n"
            size_t pos = conn->proxy_chunk_scan;
            const char* cr = static_cast<const char*>(std::memchr(buf + pos, '\r', avail));
            if (!cr || cr + 1 >= buf + static_cast<size_t>(conn->rx_bytes) || cr[1] != '\n')
                // wait for the rest of the line
                return true; // ждем остаток строки
            size_t len = static_cast<size_t>(cr - (buf + pos));
            size_t chunk_size = 0;
            size_t i = 0;
            for (; i < len; ++i) {
                char c = buf[pos + i];
                size_t d;
                if (c >= '0' && c <= '9') d = static_cast<size_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<size_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<size_t>(c - 'A' + 10);
                // ignore extensions
                else if (c == ';') break; // extensions игнорируем
                // garbage in the size line
                else return false;        // мусор в строке размера
                // Сатурация: чанк больше SIZE_MAX/16 никогда не закончится
                // Saturation: a chunk larger than SIZE_MAX/16 will never end
                if (chunk_size > static_cast<size_t>(-1) / 16) {
                    chunk_size = static_cast<size_t>(-1);
                } else {
                    chunk_size = chunk_size * 16u + d;
                }
            }
            // empty size line
            if (i == 0) return false; // пустая строка размера
            // after "\r\n"
            conn->proxy_chunk_scan = pos + len + 2; // после "\r\n"
            if (chunk_size == 0) {
                // trailers
                conn->proxy_chunk_phase = 3; // трейлеры
            } else {
                conn->proxy_chunk_phase = 1;
                conn->proxy_chunk_remaining = chunk_size;
            }
            continue;
        }
        // chunk data
        case 1: { // данные чанка
            size_t take = conn->proxy_chunk_remaining < avail
                              ? conn->proxy_chunk_remaining
                              : avail;
            conn->proxy_chunk_remaining -= take;
            conn->proxy_chunk_scan += take;
            if (conn->proxy_chunk_remaining == 0) {
                conn->proxy_chunk_phase = 2;
                // wait for CRLF
                conn->proxy_chunk_remaining = 2; // ждем CRLF
            }
            // there is more to scan
            if (take < avail) continue; // есть что сканировать дальше
            // wait for data
            return true;                // ждем данные
        }
        // CRLF after chunk data
        case 2: { // CRLF после данных чанка
            if (avail < 2) return true;
            if (buf[conn->proxy_chunk_scan] != '\r' ||
                buf[conn->proxy_chunk_scan + 1] != '\n')
                // garbage instead of CRLF
                return false; // мусор вместо CRLF
            conn->proxy_chunk_scan += 2;
            conn->proxy_chunk_phase = 0;
            continue;
        }
        // trailers: empty "\r\n" or "Header: x\r\n...\r\n\r\n"
        case 3: { // трейлеры: пустые "\r\n" или "Заголовок: x\r\n...\r\n\r\n"
            if (avail >= 2 && buf[conn->proxy_chunk_scan] == '\r' &&
                buf[conn->proxy_chunk_scan + 1] == '\n') {
                // Пустые трейлеры: конец запроса сразу после "0\r\n"
                // Empty trailers: the request ends right after "0\r\n"
                conn->proxy_request_end =
                    conn->proxy_window_base + conn->proxy_chunk_scan + 2;
                return true;
            }
            std::string_view rest(buf + conn->proxy_chunk_scan, avail);
            size_t end = rest.find("\r\n\r\n");
            if (end != std::string_view::npos) {
                conn->proxy_request_end =
                    conn->proxy_window_base + conn->proxy_chunk_scan + end + 4;
            }
            // found, or wait for the rest of the trailers
            return true; // найден или ждем остаток трейлеров
        }
        }
    }
}

void Server::on_upstream_connect(UpstreamConnection* up, int res) {
    Connection* conn = up->client_conn;
    if (!conn || conn->state == State::CLOSE) return;
    // The backend is down / connect rejected
    if (res < 0) { // Бэкенд лежит / connect отклонен
        send_error(conn, 502); // Bad Gateway
        return;
    }
    conn->rx_sent = 0;
    conn->state = State::PROXY_UPLOADING;
    // В rx_buffer лежат заголовки (+ первая часть тела): сканируем и шлем.
    // The headers (+ the first part of the body) are in rx_buffer: scan and send.
    // Для малых запросов это единственный WRITE_UPSTREAM (Fast-Exit в
    // For small requests this is the only WRITE_UPSTREAM (Fast-Exit in
    // on_upstream_write).
    proxy_on_client_data(conn);
}

void Server::on_upstream_write(Connection* conn, int res) {
    if (res <= 0) {
        close_connection(conn);
        return;
    }
    conn->rx_sent += res;
    conn->last_activity_ms = now_ms();

    // Fast-Exit: конец запроса (заголовки + тело) уже отправлен на бэкенд —
    // Fast-Exit: the end of the request (headers + body) is already sent to the backend —
    // начинаем стримить ответ, не читая больше у клиента.
    // start streaming the response without reading more from the client.
    if (conn->proxy_request_end != 0 &&
        conn->proxy_window_base + static_cast<size_t>(conn->rx_sent) >=
            conn->proxy_request_end) {
        conn->state = State::PROXY_STREAMING;
        submit_upstream_read(conn->upstream, conn->tx_buffer, conn->buffer_size);
        return;
    }
    if (conn->rx_sent < conn->rx_bytes) {
        // incomplete window send — finish it
        submit_upstream_write(conn); // неполная отправка окна — досылаем
        return;
    }

    // Окно исчерпано: дренаж, сканер продолжит с начала нового окна
    // Window exhausted: drain, the scanner continues from the start of the new window
    conn->proxy_window_base += static_cast<size_t>(conn->rx_bytes);
    conn->rx_bytes = 0;
    conn->rx_sent = 0;
    conn->proxy_chunk_scan = 0;

    // Конец запроса мог лечь ровно на границу окна: он уже отправлен
    // The end of the request could lie exactly on the window boundary: it is already sent
    if (conn->proxy_request_end != 0 && conn->proxy_window_base >= conn->proxy_request_end) {
        conn->state = State::PROXY_STREAMING;
        submit_upstream_read(conn->upstream, conn->tx_buffer, conn->buffer_size);
        return;
    }

    // Тело не дописано: вычитываем следующий кусок у клиента
    // The body is incomplete: read the next chunk from the client
    if (conn->flags & FLAG_IS_SSL) {
        submit_poll(conn, OpType::POLL_IN);
    } else {
        submit_read(conn);
    }
}

// Бэкенд -> клиент (одно копирование через окно tx_buffer/large_buf).
// Backend -> client (one copy through the tx_buffer/large_buf window).
// Двойной буфер: пока клиент дочитывает активное окно (WRITE_CLIENT в
// Double buffering: while the client finishes the active window (WRITE_CLIENT in
// полете / недодренаж TLS), предзагруженное окно ждет в prefetch_ptr;
// flight / TLS not fully drained), the prefetched window waits in prefetch_ptr;
// write_done диспатчит его без ожидания бэкенда.
// write_done dispatches it without waiting for the backend.
void Server::on_upstream_read(Connection* conn, int res) {
    if (res <= 0) {
        // 0 или ошибка: бэкенд закрыл соединение. Для фреймированных ответов
        // 0 or an error: the backend closed the connection. For framed responses
        // READ_UPSTREAM после концовки (phase == 3) не подается; предзагрузка
        // READ_UPSTREAM is not submitted after the ending (phase == 3); the prefetch
        // могла успеть уйти ДО установки phase 3 — её EOF игнорируем, а
        // may have gone out BEFORE phase 3 was set — we ignore its EOF, and
        // финализация идет через write_done.
        // finalization happens via write_done.
        if (conn->resp.phase != 3) close_connection(conn);
        return;
    }
    conn->last_activity_ms = now_ms();

    // Клиент ещё не дочитал активное окно: держим предзагруженное
    // The client has not finished the active window yet: keep the prefetched one
    if (conn->inflight & INFL_W || conn->tx_sent < conn->tx_bytes) {
        conn->prefetch_ptr = conn->proxy_read_dst;
        conn->prefetch_len = static_cast<uint32_t>(res);
        conn->prefetch_busy = false;
        conn->prefetch_ready = true;
        return;
    }
    conn->prefetch_busy = false;
    conn->prefetch_ready = false;
    dispatch_proxy_window(conn, res, conn->proxy_read_dst);
}

// Окно ответа стало активным: сканер, доставка клиенту, предзагрузка
// The response window became active: scan, deliver to the client, prefetch
// следующего окна (после полной передачи окна в сокет).
// the next window (after the window is fully transferred to the socket).
void Server::dispatch_proxy_window(Connection* conn, int res, char* buf) {
    conn->proxy_win_ptr = buf;
    conn->tx_bytes = res;
    conn->tx_sent = 0;
    conn->last_activity_ms = now_ms();

    // Инкрементальный фрейминг ответа: определяет конец ответа и решает,
    // Incremental response framing: determines the end of the response and decides,
    // вернется ли соединение в idle-пул (upstream keep-alive)
    // whether the connection returns to the idle pool (upstream keep-alive)
    scan_response(conn, res, buf);

    if (conn->flags & FLAG_IS_SSL) {
        // TLS-клиент: оптимистичная синхронная запись через mbedTLS
        // TLS client: optimistic synchronous write through mbedTLS
        DrainResult r = drain_client(conn, buf);
        if (r == DrainResult::CLOSED) return;
        if (r == DrainResult::WOULD_BLOCK) {
            submit_poll(conn, OpType::POLL_OUT);
            return;
        }
        // Окно целиком в сокете: large_buf можно вернуть в пул. write_done
        // The whole window is in the socket: large_buf can return to the pool. write_done
        // сам решит: финиш (phase 3), диспатч предзагруженного или prefetch.
        // will decide itself: finish (phase 3), dispatch the prefetched one or prefetch.
        release_prefetch_large(conn);
        write_done(conn);
    } else {
        submit_write(conn);
        // Данные окна скопированы в skb при обработке SQE: буфер свободен.
        // The window data was copied into skb when the SQE was processed: the buffer is free.
        release_prefetch_large(conn);
        // fetch the next window while the client reads
        submit_proxy_prefetch(conn); // качаем следующее окно, пока клиент читает
    }
}

// Полная доставка окна (buf + tx_sent) в сокет клиента (синхронно, только TLS).
// Full delivery of the window (buf + tx_sent) to the client socket (synchronous, TLS only).
// Возвращает CLOSED, если соединение уже закрыто (нельзя трогать conn).
// Returns CLOSED if the connection is already closed (conn must not be touched).
Server::DrainResult Server::drain_client(Connection* conn, char* buf) {
    while (conn->tx_sent < conn->tx_bytes) {
        ssize_t n = io_write(conn, buf + conn->tx_sent,
                             static_cast<size_t>(conn->tx_bytes - conn->tx_sent));
        if (n > 0) {
            conn->tx_sent += static_cast<int>(n);
            conn->last_activity_ms = now_ms();
        } else if (n == -1 && errno == EAGAIN) {
            // Сокет не готов: caller подает POLL_OUT и ждет готовности
            // Socket not ready: the caller submits POLL_OUT and waits for readiness
            return DrainResult::WOULD_BLOCK;
        } else {
            // Ошибка/EOF: соединение мертво. БЕЗ return цикл не завершится:
            // Error/EOF: the connection is dead. WITHOUT the return the loop would not finish:
            // close_connection() обнуляет fd и ставит state=CLOSE, но
            // close_connection() zeroes fd and sets state=CLOSE, but
            // tx_sent < tx_bytes остаётся истинным, и следующая итерация
            // tx_sent < tx_bytes stays true, and the next iteration
            // снова позовёт io_write на закрытом соединении (fd=-1) —
            // would call io_write again on the closed connection (fd=-1) —
            // вечный спин на 100% CPU (пойман wrk-нагрузкой: DRAIN fd=-1).
            // an endless spin at 100% CPU (caught by wrk load: DRAIN fd=-1).
            close_connection(conn);
            return DrainResult::CLOSED;
        }
    }
    return DrainResult::DONE;
}

// TLS: продолжение доставки файла чанками. Чанк уже прочитан в tx_buffer
// TLS: continuing file delivery in chunks. The chunk is already read into tx_buffer
// (file_chunk_len байт), tx_bytes/tx_sent выставлены — дренажим его через
// (file_chunk_len bytes), tx_bytes/tx_sent are set — drain it through
// mbedTLS; при EAGAIN ждем POLL_OUT (tx_sent при этом НЕ трогаем — часть
// mbedTLS; on EAGAIN wait for POLL_OUT (tx_sent is NOT touched then — part of
// чанка уже ушла). После полной доставки чанка: следующий READ_FILE или
// the chunk already went out). After the chunk is fully delivered: next READ_FILE or
// финиш (keep-alive-сброс + переподача POLL_IN в finish_send_static).
// finish (keep-alive reset + POLL_IN re-arm in finish_send_static).
void Server::continue_tls_chunk(Connection* conn) {
    DrainResult r = drain_client(conn, conn->tx_buffer);
    if (r == DrainResult::CLOSED) return;
    if (r == DrainResult::WOULD_BLOCK) {
        submit_poll(conn, OpType::POLL_OUT);
        return;
    }
    conn->file_offset += static_cast<uint64_t>(conn->file_chunk_len);
    if (conn->file_offset >= conn->file_size) {
        finish_send_static(conn);
    } else {
        conn->state = State::SEND_STATIC;
        submit_file_chunk(conn);
    }
}

// Отмена in-flight опов слота. ВАЖНО (отличие от epoll): pending SQE в
// Cancelling the slot's in-flight ops. IMPORTANT (unlike epoll): a pending SQE in
// io_uring держит ссылку на file, поэтому close(fd) сам по себе НЕ освобождает
// io_uring holds a reference to the file, so close(fd) by itself does NOT free the
// сокет — соединение висит в ESTAB до завершения/отмены опы. Cancel по
// socket — the connection stays in ESTAB until the op completes/is cancelled. Cancel by
// user_data (адресу контекста): не зависит от номеров fd (которые могут быть
// user_data (context address): independent of fd numbers (which may be
// переиспользованы раньше, чем ядро обработает cancel).
// reused before the kernel processes the cancel).
// Безопасность при переиспользовании слота: SQE обрабатываются ядром строго
// Safety on slot reuse: SQEs are processed by the kernel strictly
// по порядку — cancel'ы, поданные в той же пачке, всегда идут ДО опов
// in order — cancels submitted in the same batch always go BEFORE the ops
// переиспользованного слота, так что новые опы не пострадают. Устаревшие
// of the reused slot, so the new ops are unaffected. Stale
// completion'ы (-ECANCELED) отбрасываются по generation.
// completions (-ECANCELED) are discarded by generation.
// Отменяем ТОЛЬКО опы из inflight-маски: холостой cancel (-ENOENT) заставляет
// We cancel ONLY the ops from the inflight mask: an idle cancel (-ENOENT) forces the
// ядро просканировать все in-flight опы кольца — при Connection: close
// kernel to scan all in-flight ops of the ring — with Connection: close
// (ни read, ни write не висят) это превращалось в ~10k бесполезных
// (neither read nor write in flight) this used to become ~10k useless
// сканирований/сек и резало RPS до 5k.
// scans/sec and cut RPS to 5k.
void Server::cancel_ops(Connection* conn) {
    if (conn->inflight & INFL_R) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->read_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }
    if (conn->inflight & INFL_W) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->write_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    // Пендинг чтение статики с диска удержит file_fd: отменяем (READ_FIXED
    // A pending static read from disk would hold file_fd: cancel it (READ_FIXED
    // и SEND_ZC могут быть оба в полете — zero-copy путь SEND_STATIC)
    // and SEND_ZC can both be in flight — the SEND_STATIC zero-copy path)
    if (conn->inflight & INFL_Z) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->send_zc_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }
    if (conn->inflight & INFL_F) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->file_read_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    // Пендинг splice-опы держат пайп: отменяем до его пересоздания/возврата
    // Pending splice ops hold the pipe: cancel them before the pipe is recreated/returned
    if (conn->inflight & INFL_SI) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->splice_in_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }
    if (conn->inflight & INFL_SO) {
        io_uring_sqe* sqe = get_sqe();
        io_uring_prep_cancel(sqe, &conn->splice_out_ctx, 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    if (conn->upstream) {
        UpstreamConnection* up = conn->upstream;
        if (up->inflight & INFL_UC) {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_cancel(sqe, &up->connect_ctx, 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
        if (up->inflight & INFL_UR) {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_cancel(sqe, &up->read_ctx, 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
        if (up->inflight & INFL_UW) {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_cancel(sqe, &up->write_ctx, 0);
            io_uring_sqe_set_data(sqe, nullptr);
        }
    }
}

void Server::close_connection(Connection* conn) {
    if (!conn || conn->state == State::CLOSE) return;
    conn->state = State::CLOSE;

    // Мгновенный сброс TLS-сессии: освобождает трансформы и состояние
    // Instant TLS session reset: frees the transforms and handshake state
    // рукопожатия прошлого соединения, чтобы переиспользуемый слот пула
    // of the previous connection, so a reused pool slot
    // стартовал новое SSL-соединение с чистого листа. Только при лениво
    // starts a new SSL connection from scratch. Only with a lazily
    // созданном контексте: session_reset безусловно memset'ит out_buf,
    // created context: session_reset unconditionally memsets out_buf,
    // на слоте без setup это был бы сегфолт.
    // on a slot without setup this would be a segfault.
    if ((conn->flags & FLAG_IS_SSL) && conn->ssl_setup) {
        (void)mbedtls_ssl_session_reset(&conn->ssl);
    }

    // Сначала отменяем in-flight опы (иначе pending SQE удержит сокет живым),
    // First cancel the in-flight ops (otherwise a pending SQE keeps the socket alive),
    // потом закрываем fd. Completion'ы придут с -ECANCELED и будут отброшены
    // then close fd. The completions will arrive with -ECANCELED and be discarded
    // по generation.
    // by generation.
    cancel_ops(conn);

    if (conn->fd != -1) {
        (void)close(conn->fd);
        conn->fd = -1;
    }

    // Незакрытый FD статики (чтение прервано/не завершено)
    // An unclosed static FD (read interrupted/not completed)
    if (conn->file_fd != -1) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
    }

    // «Толстый» TLS-буфер: возврат в пул (in-flight READ_FILE отменен выше;
    // "Fat" TLS buffer: return to the pool (the in-flight READ_FILE was cancelled above;
    // устаревшее завершение отбросится по generation, как и для tx_buffer)
    // the stale completion will be discarded by generation, as with tx_buffer)
    if (conn->large_buf) {
        release_large_buf(conn->large_buf_idx);
        conn->large_buf = nullptr;
        conn->large_buf_idx = 0;
    }
    // large_buf двойного буфера прокси (предзагрузка окна): в пул
    // The proxy double-buffer large_buf (window prefetch): back to the pool
    if (conn->prefetch_large_idx != 0xFFFFFFFFu) {
        release_large_buf(conn->prefetch_large_idx);
        conn->prefetch_large_idx = 0xFFFFFFFFu;
    }

    // Пайп splice-передачи. Прерванная передача могла оставить данные в
    // The splice-transfer pipe. An interrupted transfer may have left data in
    // пайпе, а отменяемый splice-оп — дописать их ПОСЛЕ возврата в пул
    // the pipe, and the cancelled splice op may write more AFTER the return to the pool
    // (ring держит ссылку на fd до завершения опы). Переиспользование
    // (the ring holds a reference to fd until the op completes). Reusing
    // грязного пайпа испортит чужой поток — пересоздаем (close + pipe2).
    // a dirty pipe would corrupt another stream — recreate it (close + pipe2).
    if (conn->pipe_idx != -1) {
        int idx = conn->pipe_idx;
        SplicePipe& p = splice_pipes_[static_cast<size_t>(idx)];
        (void)close(p.in_fd);
        (void)close(p.out_fd);
        p = SplicePipe{-1, -1};
        int fds[2];
        if (pipe2(fds, O_NONBLOCK) == 0) {
            long cap = fcntl(fds[0], F_GETPIPE_SZ);
            splice_pipes_[static_cast<size_t>(idx)] =
                SplicePipe{fds[0], fds[1], cap > 0 ? cap : 65536L};
            release_pipe(idx);
        }
        conn->pipe_idx = -1;
    }

    // Очистка бэкенда, если он был
    // Clean up the backend, if there was one
    if (conn->upstream) {
        if (conn->upstream->fd != -1) {
            (void)close(conn->upstream->fd);
            conn->upstream->fd = -1;
        }
        // Новая эпоха слота бэкенда: stale completion'ы (connect/write/read)
        // New epoch of the backend slot: stale completions (connect/write/read)
        // переиспользованного слота отбросятся по generation
        // of the reused slot will be discarded by generation
        conn->upstream->generation++;
        conn->upstream->client_conn = nullptr;
        upstream_pool.release(conn->upstream);
        conn->upstream = nullptr;
    }

    conn_pool.release(conn);

    // Новая эпоха слота клиента: любые устаревшие completion'ы в текущей
    // New epoch of the client slot: any stale completions in the current
    // пачке CQ (READ/WRITE/POLL) больше не смогут задеть переиспользуемый
    // CQ batch (READ/WRITE/POLL) can no longer touch the reused
    // слот (см. ABA-проверку в process_completion()).
    // slot (see the ABA check in process_completion()).
    conn->generation++;
}

// Idle-таймаут: O(N) скан пула раз в секунду (N=10000 — это ~10k тривиальных
// Idle timeout: an O(N) pool scan once per second (N=10000 — ~10k trivial
// проверок, дешевле timing wheel). Покрывает Slowloris, зависшие SSL-рукопожатия,
// checks, cheaper than a timing wheel). Covers Slowloris, stuck SSL handshakes,
// зависшие connect() к бэкенду, полузакрытые прокси-соединения и простаивающие
// stuck connect() to the backend, half-closed proxy connections and idle
// upstream-соединения (бэкенды рвут их своими таймаутами).
// upstream connections (backends break them with their own timeouts).
void Server::check_timeouts(uint64_t now) {
    for (int i = 0; i < conn_pool.capacity(); ++i) {
        Connection* conn = conn_pool.at(i);
        if (conn->state == State::CLOSE) continue;
        if (now - conn->last_activity_ms > config_->io_timeout_ms) {
            close_connection(conn);
        }
    }

    // Простаивающие upstream-соединения в idle-пулах
    // Idle upstream connections in the idle pools
    for (auto& cs : clusters_) {
        for (size_t i = 0; i < cs.idle.size();) {
            UpstreamConnection* up = cs.idle[i];
            if (now - up->idle_since_ms > config_->io_timeout_ms) {
                // it removes itself from the pool
                discard_idle_upstream(up); // сама вынимает из пула
            } else {
                ++i;
            }
        }
    }
}

// Hot Reload: перестройка upstream-кластеров из нового конфига.
// Hot Reload: rebuild the upstream clusters from the new config.
// Idle-соединения старого конфига утилизируем сразу (отмена idle-poll +
// Idle connections of the old config are disposed of immediately (idle-poll cancel +
// закрытие + возврат слота в пул). In-flight прокси-соединения безопасны:
// close + slot return to the pool). In-flight proxy connections are safe:
// их опы ходят по up->fd, а не по кластерам; при освобождении слот уйдет в
// their ops run on up->fd, not on clusters; when freed, the slot goes into the
// idle-пул нового кластера того же индекса (node_idx из старого конфига
// idle pool of the new cluster at the same index (the node_idx from the old config
// не совпадет с новым — RR его не выдаст, таймаут-скан почистит).
// will not match the new one — RR will not hand it out, the timeout scan will clean it).
void Server::rebuild_clusters() {
    for (auto& cs : clusters_) {
        while (!cs.idle.empty()) {
            UpstreamConnection* up = cs.idle.back();
            // it removes itself from the pool
            discard_idle_upstream(up); // сама вынимает из пула
        }
    }
    cluster_index_.clear();
    clusters_.clear();
    for (const auto& [name, cluster] : config_->upstreams) {
        ClusterState cs;
        cs.nodes = cluster.nodes;
        cs.idle.reserve(static_cast<size_t>(config_->max_connections));
        cluster_index_[name] = static_cast<int>(clusters_.size());
        clusters_.push_back(std::move(cs));
    }
}

// ===== Публичный API для хендлеров =====
// ===== Public API for handlers =====

void Server::start_proxy(Connection* conn, const std::string& cluster_name) {
    if (!conn || conn->state == State::CLOSE) return;

    auto it = cluster_index_.find(cluster_name);
    if (it == cluster_index_.end() || clusters_[it->second].nodes.empty()) {
        // Bad Gateway: cluster not found/empty
        send_error(conn, 502); // Bad Gateway: кластер не найден/пуст
        return;
    }
    int cluster_idx = it->second;
    ClusterState& cs = clusters_[cluster_idx];

    // Round-Robin по узлам кластера. Счетчик на кластер на воркер:
    // Round-Robin over the cluster nodes. The counter is per-cluster per-worker:
    // Share-Nothing (1 поток = 1 Server), синхронизация не нужна.
    // Share-Nothing (1 thread = 1 Server), no synchronization needed.
    int node_idx = static_cast<int>(cs.rr_counter++ % static_cast<uint32_t>(cs.nodes.size()));

    // Границы прокси-запроса: абсолютный конец (заголовки + тело) в потоке
    // Proxy request boundaries: the absolute end (headers + body) in the request
    // запроса. Для chunked конец пока неизвестен — его найдет scan_chunked()
    // stream. For chunked the end is unknown yet — scan_chunked() will find it
    // по 0-чанку. Поля header_len/content_length/chunked стешил process_buffered().
    // at the 0-chunk. The header_len/content_length/chunked fields were cached by process_buffered().
    conn->proxy_request_end = conn->proxy_is_chunked
                                  ? 0
                                  : conn->proxy_header_len + conn->proxy_content_length;
    conn->proxy_window_base = 0;
    conn->proxy_chunk_phase = 0;
    conn->proxy_chunk_remaining = 0;
    conn->proxy_chunk_scan = conn->proxy_header_len;
    // Сброс сканера фрейминга ответа (новая эпоха ответа бэкенда)
    // Reset the response framing scanner (new epoch of the backend response)
    conn->resp = ResponseScanner{};

    handle_proxy_connect(conn, cluster_idx, node_idx);
}

void Server::serve_static_file_async(Connection* conn, const std::string& full_path,
                                     const char* mime_type) {
    if (!conn || conn->state == State::CLOSE) return;

    // open+fstat синхронно: только FD и метаданные (O_NONBLOCK, без чтения
    // open+fstat synchronously: only FD and metadata (O_NONBLOCK, no disk
    // диска). ОС кэширует популярную статику в Page Cache — это микросекунды.
    // reads). The OS caches popular static in the Page Cache — this takes microseconds.
    int fd = open(full_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        send_error(conn, 404);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        send_error(conn, 404);
        return;
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);

    // Zero-Copy путь (только plaintext, ядро 6.0+): файл читается через
    // Zero-Copy path (plaintext only, kernel 6.0+): the file is read via
    // READ_FIXED в зарегистрированный tx_buffer (DMA из page cache) и
    // READ_FIXED into the registered tx_buffer (DMA from the page cache) and
    // отдается через SEND_ZC (MSG_ZEROCOPY: страницы буфера летят в сокет
    // delivered via SEND_ZC (MSG_ZEROCOPY: the buffer pages fly to the socket
    // напрямую, без копий через user-space). Файлы ЛЮБОГО размера — чанки
    // directly, with no copies through user-space). Files of ANY size — chunks
    // по buffer_size. Gzip-клиентам и TLS — буферный путь (gzip на лету;
    // of buffer_size. For gzip clients and TLS — the buffered path (gzip on the fly;
    // TLS шифрует из user-буфера, SEND_ZC для него неприменим).
    // TLS encrypts from the user buffer, SEND_ZC does not apply to it).
    // Пути доставки статики (по приоритету для больших файлов):
    // Static delivery paths (in priority order for large files):
    //   1. SPLICE (plaintext, не gzip): splice(file -> pipe -> socket),
    //   1. SPLICE (plaintext, not gzip): splice(file -> pipe -> socket),
    //      0 байт через user-space, чанки по 64 КБ (емкость пайпа).
    //      0 bytes through user-space, 64 KB chunks (pipe capacity).
    //   2. SEND_ZC (plaintext, ядро 6.0+): READ_FIXED + MSG_ZEROCOPY.
    //   2. SEND_ZC (plaintext, kernel 6.0+): READ_FIXED + MSG_ZEROCOPY.
    //   3. TLS large buffer (128 КБ чанки из преаллоцированного пула).
    //   3. TLS large buffer (128 KB chunks from the pre-allocated pool).
    //   4. Буферный READ_FILE (малые файлы / фолбэк).
    //   4. Buffered READ_FILE (small files / fallback).
    const bool gzip_static = (conn->flags & FLAG_USE_GZIP) && file_size > 150 &&
                             file_size + 512 <= conn->buffer_size;
    const bool big_file = file_size + 512 > conn->buffer_size;
    bool use_splice = !(conn->flags & FLAG_IS_SSL) && !gzip_static && big_file &&
                      config_->splice_pipes > 0;
    if (use_splice) {
        conn->pipe_idx = acquire_pipe();
        // pool exhausted: fallback
        if (conn->pipe_idx == -1) use_splice = false; // пул исчерпан: фолбэк
    }
    const bool use_zerocopy = zerocopy_ && !(conn->flags & FLAG_IS_SSL) &&
                              !gzip_static && !use_splice &&
                              conn->buf_index < registered_buffers_;

    if (use_splice) {
        // SPLICE-путь: заголовки уходят обычным send'ом, тело качает ядро
        // SPLICE path: the headers go out via a plain send, the kernel pumps the body
        // (file -> pipe -> socket). Пайп возвращается в пул в
        // (file -> pipe -> socket). The pipe returns to the pool in
        // finish_splice_stream() (пустой) или пересоздается в
        // finish_splice_stream() (empty) or is recreated in
        // close_connection() (обрыв передачи).
        // close_connection() (aborted transfer).
        conn->file_fd = fd;
        conn->file_size = file_size;
        conn->file_offset = 0;
        conn->file_chunk_len = 0;
        conn->splice_pipe_bytes = 0;
        conn->file_mime = mime_type;

        int header_len = std::snprintf(conn->tx_buffer, conn->buffer_size,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %" PRIu64 "\r\n"
            "Connection: %s\r\n\r\n",
            mime_type, file_size,
            (conn->flags & FLAG_KEEP_ALIVE) ? "keep-alive" : "close");
        if (header_len < 0 || static_cast<size_t>(header_len) >= conn->buffer_size) {
            release_pipe(conn->pipe_idx);
            conn->pipe_idx = -1;
            (void)close(fd);
            conn->file_fd = -1;
            send_error(conn, 500);
            return;
        }
        conn->tx_bytes = header_len;
        conn->tx_sent = 0;
        conn->state = State::WRITE_FILE_HEADERS;
        submit_write(conn); // write_done -> SEND_SPLICE -> submit_splice()
        return;
    }

    if (!use_zerocopy) {
        // Резерв 512 байт под HTTP-заголовки ответа
        // Reserve 512 bytes for the response HTTP headers
        if (file_size + 512 > conn->buffer_size) {
            if (conn->flags & FLAG_IS_SSL) {
                // TLS-чанки: файл ЛЮБОГО размера. Заголовки с Content-Length
                // TLS chunks: a file of ANY size. The headers with Content-Length
                // шлём синхронным дренажем, тело — чанками: READ_FILE ->
                // are sent with a synchronous drain, the body — in chunks: READ_FILE ->
                // mbedTLS-дренаж -> следующий чанк. «Толстый» буфер из пула
                // mbedTLS drain -> next chunk. The "fat" buffer from the pool
                // (чанки по tls_large_buffer_size, меньше итераций); фолбэк —
                // (chunks of tls_large_buffer_size, fewer iterations); fallback —
                // tx_buffer по buffer_size (как раньше). SEND_ZC для TLS
                // tx_buffer of buffer_size (as before). SEND_ZC for TLS
                // неприменим (шифрование из user-буфера).
                // does not apply (encryption from the user buffer).
                conn->file_fd = fd;
                conn->file_size = file_size;
                conn->file_offset = 0;
                conn->file_mime = mime_type;
                conn->file_chunk_len = 0;
                conn->large_buf = (config_->tls_large_buffers > 0)
                                      ? acquire_large_buf(conn->large_buf_idx)
                                      : nullptr;
                if (conn->large_buf) {
                    conn->large_off = 0;
                    conn->large_len = 0;
                }

                int header_len = std::snprintf(conn->tx_buffer, conn->buffer_size,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %" PRIu64 "\r\n"
                    "Connection: %s\r\n\r\n",
                    mime_type, file_size,
                    (conn->flags & FLAG_KEEP_ALIVE) ? "keep-alive" : "close");
                if (header_len < 0 || static_cast<size_t>(header_len) >= conn->buffer_size) {
                    (void)close(fd);
                    conn->file_fd = -1;
                    send_error(conn, 500);
                    return;
                }
                conn->tx_bytes = header_len;
                conn->tx_sent = 0;
                conn->state = State::WRITE_FILE_HEADERS;
                DrainResult r = drain_client(conn, conn->tx_buffer);
                if (r == DrainResult::CLOSED) return;
                if (r == DrainResult::WOULD_BLOCK) {
                    // on_poll_out -> write_done -> chunks
                    submit_poll(conn, OpType::POLL_OUT); // on_poll_out -> write_done -> чанки
                    return;
                }
                // Заголовки ушли инлайн: стартуем поток чанков (как write_done)
                // The headers went out inline: start the chunk stream (like write_done)
                conn->state = State::SEND_STATIC;
                submit_file_chunk(conn);
                return;
            }
            (void)close(fd);
            send_error(conn, 413);
            return;
        }

        conn->file_fd = fd;
        conn->file_mime = mime_type;
        conn->state = State::READ_STATIC_FILE;

        // Асинхронное чтение с диска прямо в tx_buffer: rx_buffer занят
        // Asynchronous disk read straight into tx_buffer: rx_buffer is busy
        // (pipelined-хвост запросов клиента), tx_buffer свободен (ответа еще нет).
        // (the client's pipelined request tail), tx_buffer is free (no response yet).
        io_uring_sqe* sqe = get_sqe();
        conn->file_read_ctx.op = OpType::READ_FILE;
        conn->file_read_ctx.ptr = conn;
        conn->file_read_ctx.gen = conn->generation;
        conn->inflight |= INFL_F;
        io_uring_prep_read(sqe, fd, conn->tx_buffer, static_cast<size_t>(file_size), 0);
        io_uring_sqe_set_data(sqe, &conn->file_read_ctx);
        return;
    }

    // Zero-Copy: сначала формируем заголовки в tx_buffer, отдаем их обычным
    // Zero-Copy: first build the headers in tx_buffer, deliver them with a plain
    // send'ом, дальше тело качает ядро (READ_FIXED + SEND_ZC).
    // send, then the kernel pumps the body (READ_FIXED + SEND_ZC).
    conn->file_fd = fd;
    conn->file_size = file_size;
    conn->file_offset = 0;
    conn->file_chunk_len = 0;
    conn->file_mime = mime_type;

    int header_len = std::snprintf(conn->tx_buffer, conn->buffer_size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %" PRIu64 "\r\n"
        "Connection: %s\r\n\r\n",
        mime_type, file_size,
        (conn->flags & FLAG_KEEP_ALIVE) ? "keep-alive" : "close");
    if (header_len < 0 || static_cast<size_t>(header_len) >= conn->buffer_size) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
        send_error(conn, 500);
        return;
    }
    conn->tx_bytes = header_len;
    conn->tx_sent = 0;
    conn->state = State::WRITE_FILE_HEADERS;
    // Send the headers
    submit_write(conn); // Отправляем заголовки
}

// Общая доставка готового tx_buffer (TLS: синхронный дренаж; plaintext: WRITE_CLIENT).
// Generic delivery of the ready tx_buffer (TLS: synchronous drain; plaintext: WRITE_CLIENT).
void Server::dispatch_response(Connection* conn) {
    // prev_state: инлайн-путь (ACCEPT — вызывающий process_buffered ещё не
    // prev_state: inline path (ACCEPT — the calling process_buffered has not yet
    // потребил текущий запрос из rx_buffer) vs асинхронный путь (статик-файл:
    // consumed the current request from rx_buffer) vs the async path (static file:
    // запрос уже потреблён). От этого зависит keep-alive-сброс: пере-разбор
    // the request is already consumed). The keep-alive reset depends on this: re-parsing
    // НЕпотреблённого запроса = дубликат ответа = рекурсия (stack overflow).
    // an unconsumed request = duplicate response = recursion (stack overflow).
    State prev_state = conn->state;
    conn->tx_sent = 0;
    conn->state = State::WRITE_RESPONSE;

    if (conn->flags & FLAG_IS_SSL) {
        // TLS: оптимистичная синхронная запись; при EAGAIN ждем POLL_OUT.
        // TLS: optimistic synchronous write; on EAGAIN wait for POLL_OUT.
        // ВНИМАНИЕ: здесь НЕ вызываем write_done() — его process_buffered()
        // WARNING: we do NOT call write_done() here — its process_buffered()
        // увидел бы еще НЕ ПОТРЕБЛЕННЫЙ rx_buffer (потребление делает
        // would see a NOT YET CONSUMED rx_buffer (consumption is done by the
        // внешний process_buffered ПОСЛЕ возврата из хендлера) и зациклился
        // outer process_buffered AFTER the handler returns) and would loop
        // бы на том же запросе. Делаем keep-alive-сброс инлайн — повторный
        // on the same request. We do the keep-alive reset inline — the re-
        // вход в разбор выполнит внешний цикл process_buffered (как в epoll).
        // entry into parsing will be done by the outer process_buffered loop (as in epoll).
        DrainResult r = drain_client(conn, conn->tx_buffer);
        if (r == DrainResult::CLOSED) return;
        if (r == DrainResult::WOULD_BLOCK) {
            submit_poll(conn, OpType::POLL_OUT);
            return;
        }
        if (conn->flags & FLAG_KEEP_ALIVE) {
            if (prev_state == State::ACCEPT) {
                // Инлайн-путь: текущий запрос ещё в rx_buffer. Сброс без
                // Inline path: the current request is still in rx_buffer. Reset without
                // пере-разбора — потребление и разбор pipelined-хвоста
                // re-parsing — the consumption and parsing of the pipelined tail
                // сделает read-цикл process_buffered (который вернется в
                // will be done by the process_buffered read loop (which will return to
                // loop после нашего возврата), а он же переподаст POLL_IN.
                // the loop after we return), and it will also re-arm POLL_IN.
                conn->tx_bytes = 0;
                conn->tx_sent = 0;
                conn->flags &= FLAG_IS_SSL;
                conn->state = State::ACCEPT;
            } else {
                // Асинхронный путь (READ_STATIC_FILE и т.п.): запрос уже
                // Async path (READ_STATIC_FILE etc.): the request is already
                // потреблён, нужно переподать POLL_IN на следующий запрос
                // consumed, POLL_IN must be re-armed for the next request
                // и разобрать pipelined-хвост rx_buffer. Было упущено —
                // and the rx_buffer pipelined tail parsed. This was once missed —
                // keep-alive соединения после первого ответа навсегда
                // keep-alive connections went silent forever after the first response
                // замолкали (wrk: 1 запрос на коннект, 3 rps вместо 96k).
                // (wrk: 1 request per connection, 3 rps instead of 96k).
                reset_for_keep_alive(conn);
            }
        } else {
            close_connection(conn);
        }
    } else {
        // Async delivery: continues in write_done()
        submit_write(conn); // Асинхронная доставка: продолжение в write_done()
    }
}

void Server::send_response(Connection* conn, const char* body, size_t body_len,
                           const char* content_type, bool gzip_ok) {
    if (!conn || conn->state == State::CLOSE) return;

    const char* final_body = body;
    bool is_gzipped = false;

    // Gzip "On The Fly" через thread_local scratch. Порог 150 байт: сжатие
    // Gzip "On The Fly" through the thread_local scratch. 150-byte threshold: compressing
    // крошечных тел не окупается — заголовки/словари gzip больше самого тела.
    // tiny bodies does not pay off — gzip headers/dictionaries exceed the body itself.
    if (gzip_ok && (conn->flags & FLAG_USE_GZIP) && g_compressor && body_len > 150 &&
        body_len + 512u <= conn->buffer_size) {
        size_t need = libdeflate_gzip_compress_bound(g_compressor, body_len);
        ensure_scratch(need);
        size_t comp_size = libdeflate_gzip_compress(g_compressor, body, body_len,
                                                    g_scratch_buffer, need);
        if (comp_size > 0 && comp_size < body_len) {
            final_body = g_scratch_buffer;
            body_len = comp_size;
            is_gzipped = true;
        }
    }

    // Тело может лежать прямо в tx_buffer (асинхронная статика): тогда
    // The body may live right in tx_buffer (async static): then
    // заголовки строим в scratch и сдвигаем тело вправо (иначе заголовки
    // we build the headers in scratch and shift the body right (otherwise the headers
    // перезапишут начало тела).
    // would overwrite the beginning of the body).
    uintptr_t b = reinterpret_cast<uintptr_t>(body);
    uintptr_t tb = reinterpret_cast<uintptr_t>(conn->tx_buffer);
    bool aliased = !is_gzipped && b >= tb && b < tb + conn->buffer_size;

    int header_len;
    if (aliased) {
        ensure_scratch(1024);
        header_len = std::snprintf(g_scratch_buffer, g_scratch_size,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n\r\n",
            content_type, body_len,
            (conn->flags & FLAG_KEEP_ALIVE) ? "keep-alive" : "close");
        if (header_len < 0 || static_cast<size_t>(header_len) >= g_scratch_size ||
            static_cast<size_t>(header_len) + body_len > static_cast<size_t>(conn->buffer_size)) {
            send_error(conn, 500);
            return;
        }
        std::memmove(conn->tx_buffer + header_len, conn->tx_buffer, body_len);
        std::memcpy(conn->tx_buffer, g_scratch_buffer, static_cast<size_t>(header_len));
    } else {
        header_len = std::snprintf(conn->tx_buffer, conn->buffer_size,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "%s"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n\r\n",
            content_type,
            is_gzipped ? "Content-Encoding: gzip\r\n" : "",
            body_len,
            (conn->flags & FLAG_KEEP_ALIVE) ? "keep-alive" : "close");
        // Защита от переполнения: если заголовки + тело не влезают в буфер,
        // Overflow protection: if the headers + body do not fit into the buffer,
        // отдаем минимальную ошибку, чтобы не записать данные за границы.
        // send a minimal error so no data is written past the bounds.
        if (header_len < 0 ||
            static_cast<size_t>(header_len) + body_len > static_cast<size_t>(conn->buffer_size)) {
            send_error(conn, 500);
            return;
        }
        std::memcpy(conn->tx_buffer + header_len, final_body, body_len);
    }

    conn->tx_bytes = header_len + static_cast<int>(body_len);
    dispatch_response(conn);
}

void Server::send_error(Connection* conn, int status) {
    if (!conn || conn->state == State::CLOSE) return;

    const char* reason = (status == 404) ? "Not Found"
                       : (status == 403) ? "Forbidden"
                       : (status == 400) ? "Bad Request"
                       : (status == 413) ? "Payload Too Large"
                       : (status == 502) ? "Bad Gateway"
                       : "Internal Server Error";

    char body[64];
    int body_len = std::snprintf(body, sizeof(body),
                                 "{\"error\": %d, \"message\": \"%s\"}", status, reason);

    int header_len = std::snprintf(conn->tx_buffer, conn->buffer_size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, reason, body_len);

    if (header_len < 0 ||
        static_cast<size_t>(header_len) + static_cast<size_t>(body_len) >
            static_cast<size_t>(conn->buffer_size)) {
        // Полный отказ: минимальный ответ
        // Total failure: minimal response
        static const char kErr[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        int err_len = static_cast<int>(sizeof(kErr) - 1);
        std::memcpy(conn->tx_buffer, kErr, err_len);
        conn->tx_bytes = err_len;
    } else {
        std::memcpy(conn->tx_buffer + header_len, body, static_cast<size_t>(body_len));
        conn->tx_bytes = header_len + body_len;
    }

    conn->flags &= ~FLAG_KEEP_ALIVE;
    dispatch_response(conn);
}

// ===== Фрейминг ответа бэкенда (upstream keep-alive) =====
// ===== Backend response framing (upstream keep-alive) =====

// Инкрементальный сканер фрейминга ОТВЕТА бэкенда. Ответ течет окнами
// Incremental scanner of the backend RESPONSE framing. The response flows in windows
// (tx_buffer / large_buf — двойной буфер, каждое окно целиком принадлежит
// (tx_buffer / large_buf — double buffering, each window belongs entirely
// текущему ответу: бэкенд не шлет данных без нашего запроса);
// to the current response: the backend sends no data without our request);
// scan_response вызывается на каждом окне ДО его отправки клиенту.
// scan_response is called on each window BEFORE it is sent to the client.
// Концы ответа: CL (received >= header_end + Content-Length), chunked (сканер),
// Response ends: CL (received >= header_end + Content-Length), chunked (scanner),
// HEAD/204/304 (сразу после заголовков). Если фрейминг невозможен (заголовки
// HEAD/204/304 (right after the headers). If framing is impossible (headers
// не влезли в окно, нет CL и не chunked, 1xx, битый парсинг) — phase=4:
// do not fit the window, no CL and not chunked, 1xx, broken parsing) — phase=4:
// стрим до EOF и закрытие соединения бэкенда (как в старом коде).
// stream to EOF and close the backend connection (as in the old code).
void Server::scan_response(Connection* conn, int res, const char* win_buf) {
    ResponseScanner& rs = conn->resp;
    rs.received += static_cast<size_t>(res);
    if (rs.phase == 1) {
        // the CL body is over
        if (rs.received >= rs.body_total) rs.phase = 3; // CL-тело закончилось
        return;
    }
    if (rs.phase == 2) {
        scan_response_chunked(conn, res, win_buf);
        return;
    }
    if (rs.phase != 0) return; // 3 (done) / 4 (unknown)

    // ---- Заголовки ответа: только в ПЕРВОМ окне ----
    // ---- Response headers: only in the FIRST window ----
    const char* buf = win_buf;
    size_t n = static_cast<size_t>(res);

    // Конец заголовков "\r\n\r\n"
    // End of headers "\r\n\r\n"
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end == 0) {
        // Заголовки не влезли в первое окно: границу ответа не определить
        // The headers did not fit into the first window: the response boundary is undeterminable
        rs.phase = 4;
        rs.keepable = false;
        return;
    }

    // Статус-лайн: "HTTP/1.1 200 OK\r\n" -> код ответа
    // Status line: "HTTP/1.1 200 OK\r\n" -> response code
    size_t eol = 0;
    while (eol + 1 < hdr_end && !(buf[eol] == '\r' && buf[eol + 1] == '\n')) ++eol;
    int status = 0;
    bool has_status = false;
    // "HTTP/x.y " at minimum
    if (eol >= 8) { // "HTTP/x.y " минимум
        size_t sp = 0;
        while (sp < eol && buf[sp] != ' ') ++sp;
        if (sp + 3 <= eol) {
            has_status = true;
            for (size_t k = 0; k < 3; ++k) {
                char c = buf[sp + 1 + k];
                if (c < '0' || c > '9') { has_status = false; break; }
                status = status * 10 + (c - '0');
            }
        }
    }
    if (!has_status) {
        rs.phase = 4;
        rs.keepable = false;
        return;
    }

    // 1xx (Continue/Switching Protocols и т.п.): повторное использование
    // 1xx (Continue/Switching Protocols etc.): connection reuse
    // соединения не поддерживаем — стрим до EOF.
    // is not supported — stream to EOF.
    if (status >= 100 && status < 200) {
        rs.phase = 4;
        rs.keepable = false;
        return;
    }

    // Ответы без тела по спецификации: конец сразу после заголовков.
    // Spec-wise bodyless responses: the end is right after the headers.
    // Соединение НЕ переиспользуем: битый бэкенд может прислать тело —
    // We do NOT reuse the connection: a broken backend may send a body —
    // не дадим ему утечь в следующий ответ.
    // we must not let it leak into the next response.
    if (status == 204 || status == 304 || conn->proxy_is_head) {
        rs.phase = 3;
        rs.keepable = false;
        return;
    }

    // Заголовки: Content-Length / Transfer-Encoding: chunked / Connection.
    // Headers: Content-Length / Transfer-Encoding: chunked / Connection.
    // Строка заголовка не может начинаться с '\r': пустая строка = терминатор
    // A header line cannot start with '\r': an empty line = the "\r\n\r\n"
    // "\r\n\r\n" — разбор заголовков закончен.
    // terminator — header parsing is finished.
    size_t pos = eol + 2;
    size_t content_length = 0;
    bool has_cl = false;
    bool chunked = false;
    bool conn_close = false;
    bool malformed = false;
    while (pos + 2 <= hdr_end && !(buf[pos] == '\r' && buf[pos + 1] == '\n')) {
        size_t colon = pos;
        while (colon < hdr_end && buf[colon] != ':' && buf[colon] != '\r') ++colon;
        if (colon >= hdr_end || buf[colon] != ':' || colon == pos) { malformed = true; break; }
        size_t name_len = colon - pos;
        size_t val_start = colon + 1;
        while (val_start < hdr_end && (buf[val_start] == ' ' || buf[val_start] == '\t')) ++val_start;
        size_t val_end = val_start;
        while (val_end < hdr_end && buf[val_end] != '\r') ++val_end;
        std::string_view name(buf + pos, name_len);
        std::string_view value(buf + val_start, val_end - val_start);

        if (iequals(name, "content-length")) {
            size_t cl = 0;
            bool digits = !value.empty();
            for (char c : value) {
                if (c < '0' || c > '9') { digits = false; break; }
                if (cl > (SIZE_MAX - 9u) / 10u) cl = SIZE_MAX;
                else cl = cl * 10u + static_cast<size_t>(c - '0');
            }
            if (digits) {
                content_length = cl;
                has_cl = true;
            }
        } else if (iequals(name, "transfer-encoding")) {
            if (has_token(value, "chunked")) chunked = true;
        } else if (iequals(name, "connection")) {
            if (has_token(value, "close")) conn_close = true;
        }

        pos = val_end;
        if (pos + 1 < hdr_end && buf[pos] == '\r' && buf[pos + 1] == '\n') pos += 2;
        else break;
    }
    if (malformed) {
        rs.phase = 4;
        rs.keepable = false;
        return;
    }
    rs.conn_close = conn_close;

    if (chunked) {
        rs.chunked = true;
        rs.keepable = true;
        rs.phase = 2;
        rs.chunk_phase = 0;
        rs.chunk_remaining = 0;
        // the body starts after the headers
        rs.chunk_scan = hdr_end; // тело начинается после заголовков
        // the first window may already contain body data
        scan_response_chunked(conn, res, win_buf); // в первом окне может быть и тело
    } else if (has_cl) {
        rs.body_total = hdr_end + content_length;
        rs.phase = 1;
        rs.keepable = true;
        if (rs.received >= rs.body_total) rs.phase = 3;
    } else {
        // Нет ни Content-Length, ни chunked: конец только по EOF
        // Neither Content-Length nor chunked: the end is only at EOF
        rs.phase = 4;
        rs.keepable = false;
    }
}

// Инкрементальный сканер chunked-фрейминга ОТВЕТА (по окнам tx_buffer).
// Incremental scanner of the RESPONSE chunked framing (over tx_buffer windows).
// rs.chunk_scan — абсолютный офсет в потоке ответа; окно покрывает
// rs.chunk_scan is an absolute offset in the response stream; the window covers
// [win_base, win_base + res]. Токены (строки размера, CRLF, трейлеры) должны
// [win_base, win_base + res]. Tokens (size lines, CRLF, trailers) must
// укладываться в одно окно: если сканер отстал от окна, токен был разорван —
// fit into one window: if the scanner fell behind the window, the token was torn —
// соединение не переиспользуем (phase=4), это безопасно.
// we do not reuse the connection (phase=4), this is safe.
void Server::scan_response_chunked(Connection* conn, int res, const char* win_buf) {
    ResponseScanner& rs = conn->resp;
    size_t win_base = rs.received - static_cast<size_t>(res);
    while (true) {
        if (rs.chunk_scan < win_base || rs.chunk_scan > win_base + static_cast<size_t>(res)) {
            // Незавершенный токен предыдущего окна потерян (окно перезаписано)
            // An unfinished token of the previous window was lost (the window was overwritten)
            rs.keepable = false;
            rs.phase = 4;
            return;
        }
        size_t avail = win_base + static_cast<size_t>(res) - rs.chunk_scan;
        if (avail == 0) return;
        const char* buf = win_buf + (rs.chunk_scan - win_base);

        switch (rs.chunk_phase) {
        // size line: "<hex>[;extensions]\r\n"
        case 0: { // строка размера: "<hex>[;extensions]\r\n"
            const char* cr = static_cast<const char*>(std::memchr(buf, '\r', avail));
            if (!cr || static_cast<size_t>(cr - buf) + 1 >= avail || cr[1] != '\n')
                // wait for the rest of the line (within the window)
                return; // ждем остаток строки (в пределах окна)
            size_t len = static_cast<size_t>(cr - buf);
            size_t chunk_size = 0;
            size_t i = 0;
            for (; i < len; ++i) {
                char c = buf[i];
                size_t d;
                if (c >= '0' && c <= '9') d = static_cast<size_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<size_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<size_t>(c - 'A' + 10);
                // ignore extensions
                else if (c == ';') break; // extensions игнорируем
                else { rs.keepable = false; rs.phase = 4; return; }
                if (chunk_size > static_cast<size_t>(-1) / 16) chunk_size = static_cast<size_t>(-1);
                else chunk_size = chunk_size * 16u + d;
            }
            if (i == 0) { rs.keepable = false; rs.phase = 4; return; }
            rs.chunk_scan += len + 2;
            if (chunk_size == 0) {
                // trailers
                rs.chunk_phase = 3; // трейлеры
            } else {
                rs.chunk_phase = 1;
                rs.chunk_remaining = chunk_size;
            }
            continue;
        }
        // chunk data
        case 1: { // данные чанка
            size_t take = rs.chunk_remaining < avail ? rs.chunk_remaining : avail;
            rs.chunk_remaining -= take;
            rs.chunk_scan += take;
            if (rs.chunk_remaining == 0) {
                rs.chunk_phase = 2;
                // wait for CRLF
                rs.chunk_remaining = 2; // ждем CRLF
            }
            // there is more to scan
            if (take < avail) continue; // есть что сканировать дальше
            // window exhausted
            return;                     // окно исчерпано
        }
        // CRLF after chunk data
        case 2: { // CRLF после данных чанка
            if (avail < 2) return;
            if (buf[0] != '\r' || buf[1] != '\n') { rs.keepable = false; rs.phase = 4; return; }
            rs.chunk_scan += 2;
            rs.chunk_phase = 0;
            continue;
        }
        // trailers: empty "\r\n" or "Header: x\r\n...\r\n\r\n"
        case 3: { // трейлеры: пустые "\r\n" или "Заголовок: x\r\n...\r\n\r\n"
            if (avail >= 2 && buf[0] == '\r' && buf[1] == '\n') {
                rs.chunk_scan += 2;
                // end of the response
                rs.phase = 3; // конец ответа
                return;
            }
            std::string_view rest(buf, avail);
            size_t end = rest.find("\r\n\r\n");
            if (end != std::string_view::npos) {
                rs.chunk_scan += end + 4;
                rs.phase = 3;
            }
            // wait for the rest of the trailers (within the window)
            return; // ждем остаток трейлеров (в пределах окна)
        }
        }
    }
}

// Ответ бэкенда полностью получен и доставлен клиенту.
// The backend response is fully received and delivered to the client.
// Бэкенд: в idle-пул (upstream keep-alive) или закрыть. Клиент: keep-alive
// Backend: to the idle pool (upstream keep-alive) or close. Client: keep-alive
// сброс (pipelined-разбор) или закрытие.
// reset (pipelined parsing) or close.
void Server::finish_proxy_response(Connection* conn) {
    UpstreamConnection* up = conn->upstream;
    conn->upstream = nullptr;
    if (up) {
        if (config_->enable_keep_alive && conn->resp.keepable && !conn->resp.conn_close &&
            up->fd != -1) {
            release_upstream_to_idle(up);
        } else {
            if (up->fd != -1) {
                (void)close(up->fd);
                up->fd = -1;
            }
            up->generation++;
            up->client_conn = nullptr;
            upstream_pool.release(up);
        }
    }

    if (conn->flags & FLAG_KEEP_ALIVE) {
        // Потребляем байты запроса из rx_buffer: прокси УЖЕ отправил их на
        // Consume the request bytes from rx_buffer: the proxy ALREADY sent them to
        // бэкенд (rx_sent/окна), но физически они все еще лежат в буфере.
        // the backend (rx_sent/windows), but physically they still sit in the buffer.
        // Без этого сдвига повторный process_buffered() распарсил бы старый
        // Without this shift a repeated process_buffered() would re-parse the old
        // запрос заново и отправил его на бэкенд второй раз. Абсолютный конец
        // request and send it to the backend a second time. The absolute end
        // запроса — proxy_request_end; байты за ним в текущем окне —
        // of the request is proxy_request_end; bytes after it in the current window —
        // pipelined-хвост (следующие запросы клиента): сохраняем их.
        // the pipelined tail (the client's next requests): keep them.
        int keep = 0;
        if (conn->proxy_request_end > conn->proxy_window_base) {
            size_t req_in_win = conn->proxy_request_end - conn->proxy_window_base;
            if (req_in_win < static_cast<size_t>(conn->rx_bytes)) {
                keep = conn->rx_bytes - static_cast<int>(req_in_win);
            }
        }
        if (keep > 0) {
            std::memmove(conn->rx_buffer,
                         conn->rx_buffer + (conn->rx_bytes - keep),
                         static_cast<size_t>(keep));
        }
        conn->rx_bytes = keep;

        // Общий keep-alive-сброс: pipelined-хвост уже сохранен выше
        // Common keep-alive reset: the pipelined tail is already preserved above
        reset_for_keep_alive(conn);
    } else {
        close_connection(conn);
    }
}

// Асинхронное чтение статики завершено (READ_FILE / READ_FIXED).
// Async static read completed (READ_FILE / READ_FIXED).
// Буферный путь (READ_STATIC_FILE): res = число прочитанных байт, файл в
// Buffered path (READ_STATIC_FILE): res = number of bytes read, the file
// буфер гарантированно влезает (проверено в serve_static_file_async).
// is guaranteed to fit the buffer (checked in serve_static_file_async).
// Zero-Copy путь (SEND_STATIC): прочитан очередной чанк — отдаем SEND_ZC.
// Zero-Copy path (SEND_STATIC): the next chunk was read — deliver via SEND_ZC.
void Server::on_file_read(Connection* conn, int res) {
    if (conn->state == State::SEND_STATIC) {
        if (res <= 0) {
            // file truncated/error: abort the delivery
            close_connection(conn); // файл обрезан/ошибка: оборвать доставку
            return;
        }
        conn->file_chunk_len = static_cast<uint32_t>(res);
        if (conn->flags & FLAG_IS_SSL) {
            if (conn->large_buf) {
                // TLS: чанк прочитан в «толстый» буфер — шифруем и шлем его
                // TLS: the chunk was read into the "fat" buffer — encrypt and send it
                conn->large_off = 0;
                conn->large_len = static_cast<uint32_t>(res);
                conn->state = State::SEND_TLS_CHUNK;
                continue_large_tls_chunk(conn);
                return;
            }
            // TLS: чанк прочитан в tx_buffer — доставляем его через mbedTLS
            // TLS: the chunk was read into tx_buffer — deliver it through mbedTLS
            conn->tx_bytes = static_cast<int>(conn->file_chunk_len);
            conn->tx_sent = 0;
            conn->state = State::SEND_TLS_CHUNK;
            continue_tls_chunk(conn);
            return;
        }
        submit_send_zc(conn, 0, static_cast<uint32_t>(res));
        return;
    }
    if (conn->file_fd != -1) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
    }
    if (res < 0) {
        send_error(conn, 500);
        return;
    }
    send_response(conn, conn->tx_buffer, static_cast<size_t>(res), conn->file_mime, true);
}

// Чтение следующего чанка файла. TLS с «толстым» буфером: обычный READ в
// Reading the next file chunk. TLS with the "fat" buffer: a plain READ into
// large_buf (чанки по tls_large_buffer_size). SEND_ZC: READ_FIXED в
// large_buf (chunks of tls_large_buffer_size). SEND_ZC: READ_FIXED into the
// зарегистрированный tx_buffer (ядро пишет прямо в зарегистрированные
// registered tx_buffer (the kernel writes directly into the registered
// страницы — ноль копий через user-space).
// pages — zero copies through user-space).
void Server::submit_file_chunk(Connection* conn) {
    uint64_t remaining = conn->file_size - conn->file_offset;
    void* dst;
    uint32_t chunk;
    if (conn->large_buf) {
        chunk = static_cast<uint32_t>(std::min<uint64_t>(large_buffer_size_, remaining));
        dst = conn->large_buf;
    } else {
        chunk = static_cast<uint32_t>(std::min<uint64_t>(conn->buffer_size, remaining));
        dst = conn->tx_buffer;
    }
    io_uring_sqe* sqe = get_sqe();
    conn->file_read_ctx.op = OpType::READ_FILE;
    conn->file_read_ctx.ptr = conn;
    conn->file_read_ctx.gen = conn->generation;
    conn->inflight |= INFL_F;
    if (!conn->large_buf && conn->buf_index < registered_buffers_) {
        io_uring_prep_read_fixed(sqe, conn->file_fd, dst, chunk,
                                 static_cast<__u64>(conn->file_offset), conn->buf_index);
    } else {
        io_uring_prep_read(sqe, conn->file_fd, dst, chunk,
                           static_cast<__u64>(conn->file_offset));
    }
    io_uring_sqe_set_data(sqe, &conn->file_read_ctx);
}

// Zero-Copy отдача чанка из зарегистрированного tx_buffer (SEND_ZC =
// Zero-Copy delivery of a chunk from the registered tx_buffer (SEND_ZC =
// MSG_ZEROCOPY): ядро шлет страницы буфера в сокет напрямую, без копий.
// MSG_ZEROCOPY): the kernel sends the buffer pages straight to the socket, no copies.
// buf_off — смещение внутри буфера (досыл частичного чанка).
// buf_off — offset inside the buffer (re-sending part of a chunk).
void Server::submit_send_zc(Connection* conn, uint32_t buf_off, uint32_t len) {
    io_uring_sqe* sqe = get_sqe();
    conn->send_zc_ctx.op = OpType::SEND_ZC;
    conn->send_zc_ctx.ptr = conn;
    conn->send_zc_ctx.gen = conn->generation;
    conn->inflight |= INFL_Z;
    io_uring_prep_send_zc_fixed(sqe, conn->fd, conn->tx_buffer + buf_off, len,
                                MSG_NOSIGNAL, 0, conn->buf_index);
    io_uring_sqe_set_data(sqe, &conn->send_zc_ctx);
}

// SEND_ZC завершен (главный CQE; нотификации отбрасываются в
// SEND_ZC completed (the main CQE; notifications are discarded in
// process_completion по IORING_CQE_F_NOTIF): продвигаем офсет, досылаем
// process_completion by IORING_CQE_F_NOTIF): advance the offset, re-send
// остаток чанка или читаем следующий.
// the chunk remainder or read the next one.
void Server::on_send_zc(Connection* conn, int res) {
    if (res <= 0) {
        // socket dead / zero progress
        close_connection(conn); // сокет умер / нулевой прогресс
        return;
    }
    conn->file_offset += static_cast<uint64_t>(res);
    conn->last_activity_ms = now_ms();

    if (static_cast<uint32_t>(res) < conn->file_chunk_len) {
        // Частичная отправка: досылаем остаток чанка (буфер не перечитываем)
        // Partial send: re-send the chunk remainder (do not re-read the buffer)
        submit_send_zc(conn, static_cast<uint32_t>(res),
                       conn->file_chunk_len - static_cast<uint32_t>(res));
        return;
    }
    if (conn->file_offset < conn->file_size) {
        submit_file_chunk(conn);
        return;
    }
    finish_send_static(conn);
}

// Файл отдан целиком: закрываем FD, возвращаем пулы, keep-alive-сброс
// The file is fully delivered: close the FD, return the pools, keep-alive reset
// (pipelined-разбор следующего запроса) или закрытие соединения.
// (pipelined parsing of the next request) or closing the connection.
void Server::finish_send_static(Connection* conn) {
    if (conn->file_fd != -1) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
    }
    if (conn->large_buf) {
        release_large_buf(conn->large_buf_idx);
        conn->large_buf = nullptr;
    }
    if (conn->flags & FLAG_KEEP_ALIVE) {
        reset_for_keep_alive(conn);
    } else {
        close_connection(conn);
    }
}

// ===== SPLICE-статика (SEND_SPLICE) =====
// ===== SPLICE static (SEND_SPLICE) =====

// O(1) выдача пайпа из пула (индекс слота; -1 = пул исчерпан).
// O(1) pipe allocation from the pool (slot index; -1 = pool exhausted).
int Server::acquire_pipe() {
    if (pipe_top_ < 0) return -1;
    int idx = pipe_free_[static_cast<size_t>(pipe_top_)];
    --pipe_top_;
    return idx;
}

void Server::release_pipe(int idx) {
    assert(idx >= 0 && idx < static_cast<int>(splice_pipes_.size()));
    assert(pipe_top_ < static_cast<int>(pipe_free_.size()) - 1 &&
           "Pipe pool overflow (double release?)");
    pipe_free_[++pipe_top_] = idx;
}

// Подача splice: linked-цепочка file->pipe->socket при пустом пайпе;
// Splice submission: a linked file->pipe->socket chain when the pipe is empty;
// solo pipe->socket, пока в пайпе лежит остаток (сокет был полон —
// solo pipe->socket while a remainder sits in the pipe (the socket was full —
// короткий SPLICE_OUT, FIFO-остаток дочитается до нового чтения файла).
// short SPLICE_OUT, the FIFO remainder is drained before the next file read).
// Чанк = min(64 КБ, остаток файла): емкость стандартного пайпа, чтобы
// Chunk = min(64 KB, file remainder): the standard pipe capacity, so that
// SPLICE_IN всегда заполнял пайп целиком без EAGAIN.
// SPLICE_IN always fills the pipe entirely without EAGAIN.
void Server::submit_splice(Connection* conn) {
    SplicePipe& p = splice_pipes_[static_cast<size_t>(conn->pipe_idx)];
    uint64_t remaining = conn->file_size - conn->file_offset;
    // Чанк не может превышать реальную ёмкость пайпа: ядро может урезать
    // The chunk cannot exceed the real pipe capacity: the kernel may shrink
    // емкость (pipe-user-pages-soft), и короткий SPLICE_IN в linked-цепочке
    // it (pipe-user-pages-soft), and a short SPLICE_IN in the linked chain
    // ломает следующий SPLICE_OUT (kernel отменяет его с -ECANCELED).
    // breaks the next SPLICE_OUT (the kernel cancels it with -ECANCELED).
    uint64_t cap = p.capacity > 0 ? static_cast<uint64_t>(p.capacity) : 65536u;
    uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>({65536u, cap, remaining}));

    if (conn->splice_pipe_bytes > conn->file_offset) {
        // В пайпе остаток неотправленного: досливаем его без чтения файла
        // There is an unsent remainder in the pipe: drain it without reading the file
        io_uring_sqe* sqe = get_sqe();
        conn->splice_out_ctx.op = OpType::SPLICE_OUT;
        conn->splice_out_ctx.ptr = conn;
        conn->splice_out_ctx.gen = conn->generation;
        conn->inflight |= INFL_SO;
        io_uring_prep_splice(sqe, p.in_fd, -1, conn->fd, -1, 65536u, 0);
        io_uring_sqe_set_data(sqe, &conn->splice_out_ctx);
        return;
    }

    // Пайп пуст: одна пачка, два SQE (IOSQE_IO_LINK — строгий порядок).
    // Pipe empty: one batch, two SQEs (IOSQE_IO_LINK — strict order).
    // Офсеты файла передаются значениями и бейкаются в SQE ядром.
    // File offsets are passed by value and baked into the SQEs by the kernel.
    io_uring_sqe* sqe = get_sqe();
    conn->splice_in_ctx.op = OpType::SPLICE_IN;
    conn->splice_in_ctx.ptr = conn;
    conn->splice_in_ctx.gen = conn->generation;
    conn->inflight |= INFL_SI;
    io_uring_prep_splice(sqe, conn->file_fd, static_cast<int64_t>(conn->file_offset),
                         p.out_fd, -1, chunk, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe, &conn->splice_in_ctx);

    sqe = get_sqe();
    conn->splice_out_ctx.op = OpType::SPLICE_OUT;
    conn->splice_out_ctx.ptr = conn;
    conn->splice_out_ctx.gen = conn->generation;
    conn->inflight |= INFL_SO;
    io_uring_prep_splice(sqe, p.in_fd, -1, conn->fd, -1, chunk, 0);
    io_uring_sqe_set_data(sqe, &conn->splice_out_ctx);
}

// SPLICE_IN завершен: страницы файла залиты в пайп. Прогресс считаем по
// SPLICE_IN completed: the file pages are flushed into the pipe. Progress is counted by
// SPLICE_OUT (байты, дошедшие до сокета) — счётчик pipe_bytes нужен только
// SPLICE_OUT (bytes that reached the socket) — the pipe_bytes counter is only needed
// для определения FIFO-остатка.
// to detect the FIFO remainder.
void Server::on_splice_in(Connection* conn, int res) {
    if (res == -EAGAIN) {
        // Пайп оказался полон (чанк > емкости пайпа на нестандартном ядре):
        // The pipe turned out to be full (chunk > pipe capacity on a non-standard kernel):
        // не ошибка — дренажим пайп в сокет (solo SPLICE_OUT)
        // not an error — drain the pipe into the socket (solo SPLICE_OUT)
        submit_splice(conn);
        return;
    }
    if (res <= 0) {
        // file truncated/error: abort the delivery
        close_connection(conn); // файл обрезан/ошибка: оборвать доставку
        return;
    }
    conn->splice_pipe_bytes += static_cast<uint64_t>(res);
}

// SPLICE_OUT завершен: байты пайпа ушли в сокет клиента.
// SPLICE_OUT completed: the pipe bytes went to the client socket.
void Server::on_splice_out(Connection* conn, int res) {
    if (res == -EAGAIN) {
        // Сокет полностью полон: не ошибка — ждем готовности к записи
        // The socket is completely full: not an error — wait for write readiness
        // (POLL_OUT вернет нас в submit_splice с тем же остатком пайпа)
        // (POLL_OUT will bring us back to submit_splice with the same pipe remainder)
        submit_poll(conn, OpType::POLL_OUT);
        return;
    }
    if (res <= 0) {
        // socket dead / zero progress
        close_connection(conn); // сокет умер / нулевой прогресс
        return;
    }
    conn->file_offset += static_cast<uint64_t>(res);
    conn->last_activity_ms = now_ms();

    if (conn->file_offset >= conn->file_size) {
        finish_splice_stream(conn);
        return;
    }
    submit_splice(conn);
}

// Файл отдан целиком: пайп пуст (все байты дошли до сокета, опов нет) —
// The file is fully delivered: the pipe is empty (all bytes reached the socket, no ops) —
// возвращаем в пул как есть. Keep-alive-сброс или закрытие.
// return it to the pool as is. Keep-alive reset or close.
void Server::finish_splice_stream(Connection* conn) {
    if (conn->file_fd != -1) {
        (void)close(conn->file_fd);
        conn->file_fd = -1;
    }
    release_pipe(conn->pipe_idx);
    conn->pipe_idx = -1;
    if (conn->flags & FLAG_KEEP_ALIVE) {
        reset_for_keep_alive(conn);
    } else {
        close_connection(conn);
    }
}

// ===== TLS-статика на «толстом» буфере (Large Buffer Pool) =====
// ===== TLS static on the "fat" buffer (Large Buffer Pool) =====

char* Server::acquire_large_buf(uint32_t& idx) {
    // pool exhausted
    if (large_top_ < 0) return nullptr; // пул исчерпан
    int i = large_free_[static_cast<size_t>(large_top_)];
    --large_top_;
    idx = static_cast<uint32_t>(i);
    return large_buffers_mem_ + static_cast<size_t>(i) * large_buffer_size_;
}

void Server::release_large_buf(uint32_t idx) {
    assert(idx < large_free_.size() && "Large buffer out of pool");
    assert(large_top_ < static_cast<int>(large_free_.size()) - 1 &&
           "Large buffer pool overflow (double release?)");
    large_free_[++large_top_] = static_cast<int>(idx);
}

// TLS: продолжение доставки файла «толстым» чанком. Чанк прочитан в
// TLS: continuing file delivery with a "fat" chunk. The chunk was read into
// large_buf (large_len байт), large_off — сколько уже зашифровано/отправлено.
// large_buf (large_len bytes), large_off — how much is already encrypted/sent.
// mbedtls_ssl_write шифрует record'ами (до 16 КБ) и возвращает потребленные
// mbedtls_ssl_write encrypts in records (up to 16 KB) and returns the consumed
// plaintext-байты — цикл до конца чанка; при EAGAIN ждем POLL_OUT
// plaintext bytes — loop until the chunk ends; on EAGAIN wait for POLL_OUT
// (on_poll_out вернется сюда). После чанка — следующий READ_FILE или финиш.
// (on_poll_out will return here). After the chunk — the next READ_FILE or finish.
void Server::continue_large_tls_chunk(Connection* conn) {
    while (conn->large_off < conn->large_len) {
        ssize_t n = io_write(conn, conn->large_buf + conn->large_off,
                             static_cast<size_t>(conn->large_len - conn->large_off));
        if (n > 0) {
            conn->large_off += static_cast<uint32_t>(n);
            conn->last_activity_ms = now_ms();
        } else if (n == -1 && errno == EAGAIN) {
            submit_poll(conn, OpType::POLL_OUT);
            return;
        } else {
            close_connection(conn);
            return;
        }
    }
    conn->file_offset += static_cast<uint64_t>(conn->large_len);
    if (conn->file_offset >= conn->file_size) {
        finish_send_static(conn);
    } else {
        conn->state = State::SEND_STATIC;
        submit_file_chunk(conn);
    }
}
