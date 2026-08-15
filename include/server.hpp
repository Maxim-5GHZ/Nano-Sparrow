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
#include "handler.hpp"
#include "memory_pool.hpp"
#include <liburing.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <vector>

// Транспортное ядро: TCP/TLS, io_uring (async recv/send/poll/connect/read-file),
// The transport core: TCP/TLS, io_uring (async recv/send/poll/connect/read-file),
// keep-alive (клиентский и upstream-пул бэкендов), gzip, балансировка
// keep-alive (client-side and upstream backend pool), gzip, Round-Robin
// Round-Robin по кластерам, прокси-стриминг с backpressure. Вся HTTP-логика
// balancing across clusters, proxy streaming with backpressure. All HTTP logic
// вынесена в IRequestHandler.
// is extracted into IRequestHandler.
// Plaintext-соединения: полностью асинхронный I/O (0 read/write syscalls
// Plaintext connections: fully async I/O (0 read/write syscalls
// в горячем цикле). TLS-соединения: io_uring-poll готовности + синхронный
// in the hot loop). TLS connections: io_uring poll readiness + synchronous
// mbedTLS (криптография CPU-bound, как в nginx).
// mbedTLS (crypto is CPU-bound, like in nginx).
// Один экземпляр = один поток. Для многопоточности создается N экземпляров
// One instance = one thread. For multi-threading, N instances are created
// (SO_REUSEPORT), каждый со своим ring и пулом.
// (SO_REUSEPORT), each with its own ring and pool.
class Server {
public:
    // Результат drain: CLOSED означает, что соединение уже закрыто
    // Drain result: CLOSED means the connection is already closed
    // и использовать conn больше нельзя.
    // and conn must not be used anymore.
    enum class DrainResult : uint8_t { DONE, WOULD_BLOCK, CLOSED };

    Server(const ServerConfig* config, IRequestHandler* handler, int shutdown_fd = -1,
           int reload_fd = -1);
    ~Server();

    void start();
    // Main Event Loop (point of no return)
    void run(); // Main Event Loop (точка невозврата)

    // Публичный API для хендлеров
    // Public API for handlers
    void start_proxy(Connection* conn, const std::string& cluster_name);
    void serve_static_file_async(Connection* conn, const std::string& full_path,
                                 const char* mime_type);
    uint32_t get_buffer_size() const { return config_->buffer_size; }
    const ServerConfig* get_config() const { return config_; }
    // SEND_ZC is active
    bool zerocopy_enabled() const { return zerocopy_; } // SEND_ZC активен
    void send_response(Connection* conn, const char* body, size_t body_len,
                       const char* content_type, bool gzip_ok);
    void send_error(Connection* conn, int status);
    void close_connection(Connection* conn);

private:
    // Состояние одного upstream-кластера (строится при старте)
    // State of one upstream cluster (built at startup)
    struct ClusterState {
        std::vector<BackendConfig> nodes;
        // Round-Robin counter (1 worker = 1 counter)
        uint32_t rr_counter = 0; // Round-Robin счетчик (1 воркер = 1 counter)
        // keep-alive connection pool
        std::vector<UpstreamConnection*> idle; // пул keep-alive соединений
    };

    // Сырой указатель, НЕ владение: конфиг живет в куче (main.cpp / hot reload
    // Raw pointer, NOT ownership: the config lives on the heap (main.cpp / hot reload
    // GC). Воркер меняет его на RELOAD-сигнале (см. app_state.hpp) — в горячем
    // GC). The worker swaps it on the RELOAD signal (see app_state.hpp) — in the hot
    // цикле только чтение по указателю, ноль лочей.
    // loop it is read-only through the pointer, zero locks.
    const ServerConfig* config_;
    IRequestHandler* handler_;
    int shutdown_fd_;
    // eventfd: hot reload (CQE OpType::RELOAD)
    int reload_fd_; // eventfd: hot reload (CQE OpType::RELOAD)

    struct io_uring ring;
    bool ring_ready_ = false;
    int listen_fd;
    uint64_t last_scan_ms = 0;
    bool ssl_ready = false;
    // SEND_ZC is available (kernel 6.0+ + buffers
    bool zerocopy_ = false;            // SEND_ZC доступен (ядро 6.0+ + буферы
                                       // зарегистрированы в IORING_REGISTER_BUFFERS)
    // how many tx buffers are actually registered
    uint32_t registered_buffers_ = 0;  // сколько tx-буферов реально зарегистрировано

    // multishot accept: one SQE for the whole life of the loop
    EventContext accept_ctx_;   // multishot accept: один SQE на всю жизнь цикла
    // eventfd read: stop signal
    EventContext shutdown_ctx_; // eventfd-чтение: сигнал остановки
    // eventfd read: hot reload signal
    EventContext reload_ctx_;   // eventfd-чтение: сигнал hot reload
    uint64_t shutdown_buf_ = 0;
    uint64_t reload_buf_ = 0;

    MemoryPool<Connection> conn_pool;
    MemoryPool<UpstreamConnection> upstream_pool;

    // ОДИН блок памяти для всех rx/tx буферов (Zero Allocation при старте)
    // ONE memory block for all rx/tx buffers (Zero Allocation at startup)
    char* connection_buffers_ = nullptr;

    // Пул pre-allocated пайпов для SEND_SPLICE (splice file->pipe->socket).
    // Pool of pre-allocated pipes for SEND_SPLICE (splice file->pipe->socket).
    // Пайпы создаются при старте (pipe2), страницы буферов ленивые — память
    // Pipes are created at startup (pipe2), buffer pages are lazy — memory
    // только при активных передачах. Стек свободных индексов (O(1) acquire).
    // only during active transfers. A stack of free indexes (O(1) acquire).
    struct SplicePipe {
        int in_fd;  // read end (file -> pipe)
        int out_fd; // write end (pipe -> socket)
        // F_GETPIPE_SZ at creation (may be shrunk by the kernel)
        long capacity = 65536; // F_GETPIPE_SZ при создании (может быть урезан ядром)
    };
    std::vector<SplicePipe> splice_pipes_;
    std::vector<int> pipe_free_;
    int pipe_top_ = -1;

    // Пул «толстых» TLS-буферов: один блок памяти, выдается соединению на
    // Pool of "fat" TLS buffers: one memory block, handed to a connection for the
    // время доставки одного чанка файла (TLS шифрует из user-space).
    // delivery of one file chunk (TLS encrypts from user-space).
    char* large_buffers_mem_ = nullptr;
    size_t large_buffer_size_ = 0;
    std::vector<int> large_free_;
    int large_top_ = -1;

    // Upstream-кластеры: узлы + RR-счетчик + idle-пул
    // Upstream clusters: nodes + RR counter + idle pool
    std::vector<ClusterState> clusters_;
    std::unordered_map<std::string, int> cluster_index_;

    // TLS-состояние инстанса (не глобалы: несколько воркеров не конфликтуют)
    // Instance TLS state (not globals: multiple workers do not conflict)
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt srvcert;
    mbedtls_pk_context pkey;

    void set_nonblocking(int fd);
    io_uring_sqe* get_sqe();

    // Подача асинхронных операций в ring
    // Submitting async ops to the ring
    // multishot
    void submit_accept(); // multishot
    // plaintext recv from the client
    void submit_read(Connection* conn);  // plaintext recv от клиента
    // plaintext send to the client
    void submit_write(Connection* conn); // plaintext send клиенту
    // TLS readiness
    void submit_poll(Connection* conn, OpType op); // TLS-готовность
    void submit_upstream_connect(UpstreamConnection* up, const sockaddr_in& addr);
    void submit_upstream_write(Connection* conn);
    void submit_upstream_read(UpstreamConnection* up, char* dst, uint32_t len);
    // prefetch of the next response window
    void submit_proxy_prefetch(Connection* conn); // предзагрузка след. окна ответа
    void release_prefetch_large(Connection* conn);

    // Диспетчеризация завершенных операций (возвращает true = стоп цикла)
    // Dispatch of completed ops (returns true = stop the loop)
    bool process_completion(EventContext* ctx, int res, uint32_t cqe_flags);
    void handle_accept(int res, uint32_t cqe_flags);
    void handle_ssl_handshake(Connection* conn);
    // rx_buffer parsing + handlers
    void process_buffered(Connection* conn); // парсинг rx_buffer + хендлеры
    // TLS: synchronous mbedTLS drain
    void handle_read(Connection* conn);      // TLS: синхронный дренаж mbedTLS
    // tx fully delivered
    void write_done(Connection* conn);       // tx доставлен целиком
    void handle_proxy_connect(Connection* conn, int cluster_idx, int node_idx);
    // client read -> backend (streaming)
    void proxy_on_client_data(Connection* conn); // чтение клиента -> бэкенд (стриминг)
    // incremental chunked scanner
    bool scan_chunked(Connection* conn);     // инкрементальный chunked-сканер
    void on_client_read(Connection* conn, int res);
    void on_client_write(Connection* conn, int res);
    void on_poll_in(Connection* conn, int res);
    void on_poll_out(Connection* conn, int res);
    void on_upstream_connect(UpstreamConnection* up, int res);
    void on_upstream_write(Connection* conn, int res);
    void on_upstream_read(Connection* conn, int res);
    void dispatch_proxy_window(Connection* conn, int res, char* buf);
    // READ_FILE completed
    void on_file_read(Connection* conn, int res);        // READ_FILE завершен
    // FIN on an idle connection
    void on_upstream_idle_poll(UpstreamConnection* up, int res); // FIN на idle
    // backend response framing
    void scan_response(Connection* conn, int res, const char* win_buf);      // фрейминг ответа бэкенда
    void scan_response_chunked(Connection* conn, int res, const char* win_buf);
    // response end: pool/close
    void finish_proxy_response(Connection* conn);      // конец ответа: пул/закрытие
    UpstreamConnection* get_idle_upstream(int cluster_idx, int node_idx);
    void release_upstream_to_idle(UpstreamConnection* up);
    void discard_idle_upstream(UpstreamConnection* up);
    bool evict_oldest_idle();
    // delivery of the ready tx_buffer
    void dispatch_response(Connection* conn); // доставка готового tx_buffer
    [[nodiscard]] DrainResult drain_client(Connection* conn, char* buf);
    // TLS: delivery of a file chunk (mbedTLS)
    void continue_tls_chunk(Connection* conn); // TLS: доставка чанка файла (mbedTLS)
    // cancel in-flight ops before close()
    void cancel_ops(Connection* conn); // отмена in-flight опов перед close()
    ssize_t io_read(Connection* conn, void* buf, size_t count);
    ssize_t io_write(Connection* conn, const void* buf, size_t count);
    void check_timeouts(uint64_t now);

    // Hot Reload: перестройка upstream-кластеров из нового конфига
    // Hot Reload: rebuild the upstream clusters from the new config
    void rebuild_clusters();

    // Keep-alive-сброс после полной доставки ответа (общий для write_done,
    // Keep-alive reset after the response is fully delivered (shared by write_done,
    // finish_proxy_response и zero-copy статики)
    // finish_proxy_response and zero-copy static)
    void reset_for_keep_alive(Connection* conn);

    // Zero-Copy статика (SEND_STATIC): READ_FIXED чанка в зарегистрированный
    // Zero-Copy static (SEND_STATIC): READ_FIXED of a chunk into the registered
    // tx_buffer, SEND_ZC отдача, финиш по концу файла
    // tx_buffer, SEND_ZC delivery, finish at the end of the file
    void submit_file_chunk(Connection* conn);
    void submit_send_zc(Connection* conn, uint32_t buf_off, uint32_t len);
    void on_send_zc(Connection* conn, int res);
    void finish_send_static(Connection* conn);

    // SPLICE-статика (SEND_SPLICE): splice(file -> pipe -> socket).
    // SPLICE static (SEND_SPLICE): splice(file -> pipe -> socket).
    // Linked-цепочка SPLICE_IN+SPLICE_OUT при пустом пайпе; solo SPLICE_OUT
    // A linked SPLICE_IN+SPLICE_OUT chain when the pipe is empty; solo SPLICE_OUT
    // пока в пайпе лежит остаток (сокет был полон). 0 байт user-space памяти.
    // while a remainder sits in the pipe (the socket was full). 0 bytes of user-space memory.
    // pipe slot index; -1 = pool exhausted
    int acquire_pipe(); // индекс слота пайпа; -1 = пул исчерпан
    void release_pipe(int idx);
    void submit_splice(Connection* conn);
    void on_splice_in(Connection* conn, int res);
    void on_splice_out(Connection* conn, int res);
    void finish_splice_stream(Connection* conn);

    // TLS-статика на «толстом» буфере (Large Buffer Pool)
    // TLS static on the "fat" buffer (Large Buffer Pool)
    char* acquire_large_buf(uint32_t& idx);
    void release_large_buf(uint32_t idx);
    void continue_large_tls_chunk(Connection* conn);
};