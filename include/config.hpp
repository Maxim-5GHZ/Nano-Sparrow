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
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Один бэкенд кластера (балансировка Round-Robin)
// One backend of a cluster (Round-Robin balancing)
struct BackendConfig {
    std::string ip;
    uint16_t port;
};

// Кластер бэкендов: [upstream:имя] в конфиге
// Backend cluster: [upstream:name] in the config
struct UpstreamCluster {
    std::vector<BackendConfig> nodes;
};

// Маршрут из конфига: "/api/ = proxy:api_cluster", "/static/ = static:/var/www"
// Route from the config: "/api/ = proxy:api_cluster", "/static/ = static:/var/www"
struct RouteConfig {
    std::string path;
    std::string handler_type;
};

// Конфигурация передается в конструктор Server явно (никаких глобалов).
// The config is passed to the Server constructor explicitly (no globals).
// Парсится ОДИН раз в main() при старте; в рантайме воркеры только читают
// Parsed ONCE in main() at startup; at runtime the workers only read
// эти структуры по константным ссылкам (Zero Allocation в горячем цикле).
// these structs by const references (Zero Allocation in the hot loop).
struct ServerConfig {
    // [server]
    uint16_t port = 8080;
    // SO_REUSEPORT workers (each with its own Server)
    int worker_threads = 1;          // SO_REUSEPORT воркеры (каждый со своим Server)
    // connection pool per worker
    int max_connections = 10000;     // пул соединений на один воркер
    // rx/tx buffers (one memory block at startup)
    uint32_t buffer_size = 8192;     // rx/tx буферы (один блок памяти при старте)
    // idle timeout (Slowloris protection)
    uint64_t io_timeout_ms = 30000;  // idle-таймаут (Slowloris-защита)
    // pre-allocated pipes per worker for SEND_SPLICE
    int splice_pipes = 512;          // pre-allocated пайпы на воркер для SEND_SPLICE
                                     // (0 = выключить splice-путь)
                                     // (0 = disable the splice path)
    // pool of "fat" TLS buffers per worker
    int tls_large_buffers = 256;     // пул «толстых» TLS-буферов на воркер
                                     // (0 = выключить)
                                     // (0 = disable)
    // size of one TLS buffer
    uint32_t tls_large_buffer_size = 131072; // размер одного TLS-буфера
    // dedicated kernel SQPOLL thread (loses to
    bool enable_sqpoll = false;      // выделенный kernel-поток SQPOLL (проигрывает
                                     // plain-режиму при multi-worker: N потоков ядра)
                                     // plain mode with multi-worker: N kernel threads)
    // pinning workers to CPU cores
    bool enable_affinity = true;     // пиннинг воркеров на ядра CPU
    bool enable_gzip = true;
    // global keep-alive switch
    bool enable_keep_alive = true;   // глобальный рубильник keep-alive

    // [ssl]
    bool enable_ssl = false;
    std::string ssl_cert = "cert.pem";
    std::string ssl_key = "key.pem";

    // [upstream:имя] — кластеры бэкендов для балансировки
    // [upstream:name] — backend clusters for balancing
    std::unordered_map<std::string, UpstreamCluster> upstreams;

    // [routes]
    std::vector<RouteConfig> routes;
};

enum class State : uint16_t {
    ACCEPT = 0,
    SSL_HANDSHAKE,
    WRITE_RESPONSE,
    // async static read from disk (io_uring READ_FILE)
    READ_STATIC_FILE,   // асинхронное чтение статики с диска (io_uring READ_FILE)
    // response headers before zero-copy file delivery
    WRITE_FILE_HEADERS, // заголовки ответа перед zero-copy отдачей файла
    // zero-copy file stream (READ_FIXED + SEND_ZC)
    SEND_STATIC,        // zero-copy поток файла (READ_FIXED + SEND_ZC)
    // plaintext: splice(file -> pipe -> socket), 0 bytes in user-space
    SEND_SPLICE,        // plaintext: splice(file -> pipe -> socket), 0 байт в user-space
    // TLS: delivery of one read file chunk (mbedTLS)
    SEND_TLS_CHUNK,     // TLS: доставка одного прочитанного чанка файла (mbedTLS)
    // connect() to the backend in progress
    PROXY_CONNECTING,   // connect() к бэкенду в процессе
    // streaming the client's request body to the backend (until the body ends)
    PROXY_UPLOADING,    // стримим тело запроса клиента на бэкенд (до конца тела)
    PROXY_STREAMING,
    CLOSE
};

constexpr uint8_t FLAG_USE_GZIP   = 1 << 0;
constexpr uint8_t FLAG_KEEP_ALIVE = 1 << 1;
constexpr uint8_t FLAG_IS_SSL     = 1 << 2;
constexpr uint8_t FLAG_SSL_EOF    = 1 << 3;

// Тип асинхронной операции io_uring
// Type of an async io_uring operation
enum class OpType : uint8_t {
    // multishot accept of a new client
    ACCEPT,            // multishot accept нового клиента
    // recv from the client (plaintext)
    READ_CLIENT,       // recv от клиента (plaintext)
    // send to the client (plaintext)
    WRITE_CLIENT,      // send клиенту (plaintext)
    // connect to the backend
    CONNECT_UPSTREAM,  // connect к бэкенду
    // send the request to the backend
    WRITE_UPSTREAM,    // send запроса на бэкенд
    // recv the response from the backend
    READ_UPSTREAM,     // recv ответа от бэкенда
    // poll POLLIN (TLS: read readiness)
    POLL_IN,           // poll POLLIN (TLS: готовность к чтению)
    // poll POLLOUT (TLS: write readiness)
    POLL_OUT,          // poll POLLOUT (TLS: готовность к записи)
    // async static read from disk (READ/READ_FIXED)
    READ_FILE,         // асинхронное чтение статики с диска (READ/READ_FIXED)
    // zero-copy send from the registered buffer (MSG_ZEROCOPY)
    SEND_ZC,           // zero-copy send из зарегистрированного буфера (MSG_ZEROCOPY)
    // splice(file -> pipe): pages from the page cache into the pipe
    SPLICE_IN,         // splice(file -> pipe): страницы из page cache в пайп
    // splice(pipe -> socket): pipe to the client socket
    SPLICE_OUT,        // splice(pipe -> socket): пайп в сокет клиента
    // poll POLLIN on an idle upstream: backend FIN detection
    POLL_UPSTREAM_IDLE,// poll POLLIN на idle upstream: детект FIN бэкенда
    // eventfd: config hot reload (see app_state.hpp)
    RELOAD,            // eventfd: hot reload конфига (см. app_state.hpp)
    // eventfd: stop signal
    SHUTDOWN           // eventfd: сигнал остановки
};

// Контекст одного поданного SQE. gen = generation слота на момент подачи:
// Context of one submitted SQE. gen = the slot generation at submission time:
// ABA-защита — устаревшие completion'ы переиспользованного слота пула
// ABA protection — stale completions of a reused pool slot
// отбрасываются сравнением gen с текущим generation'ом.
// are discarded by comparing gen with the current generation.
struct EventContext {
    OpType op;
    void* ptr;
    uint32_t gen;
};
