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
#include "example_handler.hpp"
#include "handler.hpp"
#include "server.hpp"
#include "app_state.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <pthread.h>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_shutdown_fd = -1;

// Асинхронно-безопасный сигнальный хендлер: будит все io_uring-циклы через eventfd
// Async-signal-safe signal handler: wakes all io_uring loops via eventfd
static void shutdown_signal(int sig) {
    uint64_t one = 1;
    ssize_t rc = write(g_shutdown_fd, &one, sizeof(one));
    (void)rc;
    (void)sig;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// INI-парсер с секциями: [server], [ssl], [upstream:имя], [routes].
// INI parser with sections: [server], [ssl], [upstream:name], [routes].
// Комментарии: '#' и ';'. Ошибки фатальны (fail-fast): кривой конфиг
// Comments: '#' and ';'. Errors are fatal (fail-fast): a broken config
// лучше не поднимать, чем молча работать с неверными значениями.
// is better left unstarted than silently running with wrong values.
// Работает и от файла (main), и от строки (hot reload /api/reload).
// Works both from a file (main) and from a string (hot reload /api/reload).
void parse_config_from_string(const std::string& content, ServerConfig& config) {
    std::istringstream file(content);
    // the current section ("", "server", "ssl", "upstream:name", "routes")
    std::string section; // текущая секция ("", "server", "ssl", "upstream:имя", "routes")
    std::string line;
    int lineno = 0;
    auto fail = [&](const std::string& msg) {
        throw std::runtime_error("config:" + std::to_string(lineno) + ": " + msg);
    };

    while (std::getline(file, line)) {
        ++lineno;
        // Inline-комментарии: '#' и ';' (в т.ч. после значения)
        // Inline comments: '#' and ';' (including after a value)
        size_t cmt = line.find_first_of("#;");
        if (cmt != std::string::npos) line = line.substr(0, cmt);
        std::string t = trim(line);
        if (t.empty()) continue;

        if (t.front() == '[') {
            if (t.back() != ']') fail("bad section header");
            section = trim(t.substr(1, t.size() - 2));
            if (section == "server" || section == "ssl" || section == "routes") continue;
            if (section.rfind("upstream:", 0) == 0) {
                std::string name = section.substr(9);
                if (name.empty()) fail("empty upstream name");
                config.upstreams.try_emplace(name);
                continue;
            }
            fail("unknown section [" + section + "]");
        }

        size_t pos = t.find('=');
        if (pos == std::string::npos) fail("expected key = value");
        std::string key = trim(t.substr(0, pos));
        std::string value = trim(t.substr(pos + 1));

        auto to_bool = [&](const std::string& v, const std::string& k) {
            if (v == "true" || v == "on" || v == "1") return true;
            if (v == "false" || v == "off" || v == "0") return false;
            fail("bad bool for '" + k + "': '" + v + "'");
            return false;
        };

        if (section == "server") {
            if (key == "port") config.port = static_cast<uint16_t>(std::stoi(value));
            else if (key == "worker_threads") config.worker_threads = std::max(1, std::stoi(value));
            else if (key == "max_connections") config.max_connections = std::max(128, std::stoi(value));
            else if (key == "buffer_size")
                config.buffer_size = static_cast<uint32_t>(std::max(1024, std::min(65536, std::stoi(value))));
            else if (key == "io_timeout_ms") config.io_timeout_ms = std::max<uint64_t>(1000, std::stoull(value));
            else if (key == "splice_pipes") config.splice_pipes = std::max(0, std::stoi(value));
            else if (key == "tls_large_buffers") config.tls_large_buffers = std::max(0, std::stoi(value));
            else if (key == "tls_large_buffer_size")
                config.tls_large_buffer_size =
                    static_cast<uint32_t>(std::max(8192, std::min(1048576, std::stoi(value))));
            else if (key == "enable_sqpoll") config.enable_sqpoll = to_bool(value, key);
            else if (key == "enable_affinity") config.enable_affinity = to_bool(value, key);
            else if (key == "enable_gzip") config.enable_gzip = to_bool(value, key);
            else if (key == "enable_keep_alive") config.enable_keep_alive = to_bool(value, key);
            else fail("unknown key '" + key + "' in [server]");
        } else if (section == "ssl") {
            if (key == "enable_ssl") config.enable_ssl = to_bool(value, key);
            else if (key == "ssl_cert") config.ssl_cert = value;
            else if (key == "ssl_key") config.ssl_key = value;
            else fail("unknown key '" + key + "' in [ssl]");
        } else if (section.rfind("upstream:", 0) == 0) {
            if (key != "node") fail("unknown key '" + key + "' in [" + section + "]");
            size_t colon = value.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size())
                fail("bad node '" + value + "' (expected ip:port)");
            config.upstreams[section.substr(9)].nodes.push_back(BackendConfig{
                value.substr(0, colon),
                static_cast<uint16_t>(std::stoi(value.substr(colon + 1)))});
        } else if (section == "routes") {
            config.routes.push_back(RouteConfig{key, value});
        } else {
            fail("key '" + key + "' outside any section");
        }
    }
}

void load_config(const std::string& filename, ServerConfig& config) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "[Config] No " << filename << " found, using defaults.\n";
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    parse_config_from_string(content, config);
    std::cout << "[Config] Loaded configuration from " << filename << "\n";
}

// ===== Hot Reload: глобальный стейт и GC =====
// ===== Hot Reload: global state and GC =====

AppState g_active_state;
std::vector<int> g_worker_reload_fds;
std::mutex g_gc_mutex;
std::queue<std::pair<uint64_t, AppState>> g_gc_queue;
static std::atomic<bool> g_gc_stop{false};

// GC-тред hot reload: старые пары (конфиг + роутер) живут 10 секунд после
// Hot reload GC thread: old (config + router) pairs live 10 seconds after
// релоада — гарантия, что ни один воркер не дорабатывает запрос по старым
// a reload — a guarantee that no worker finishes a request on the old
// указателям, — затем удаляются. Мьютекс нужен только здесь и в /api/reload:
// pointers — then they are deleted. The mutex is needed only here and in /api/reload:
// горячий цикл воркеров не блокируется (они лишь читают сырые указатели
// the worker hot loop is never blocked (it only reads the raw pointers
// g_active_state).
// of g_active_state).
static void gc_thread_func() {
    while (true) {
        if (g_gc_stop.load(std::memory_order_relaxed)) break;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (g_gc_stop.load(std::memory_order_relaxed)) break;
        uint64_t now = now_ms();
        std::lock_guard<std::mutex> lock(g_gc_mutex);
        while (!g_gc_queue.empty()) {
            auto& item = g_gc_queue.front();
            if (now - item.first > 10000) {
                delete item.second.config;
                delete item.second.router;
                g_gc_queue.pop();
            } else {
                break;
            }
        }
    }
}

int main(int argc, char** argv) {
    std::string conf_file = (argc > 1) ? argv[1] : "server.conf";

    // Конфиг и роутер живут в куче: воркеры читают их по сырым указателям,
    // The config and the router live on the heap: workers read them by raw pointers,
    // /api/reload меняет пару целиком, GC удаляет старую через 10 секунд.
    // /api/reload replaces the pair entirely, GC deletes the old one after 10 seconds.
    ServerConfig* config = new ServerConfig();
    try {
        load_config(conf_file, *config);
    } catch (const std::exception& e) {
        std::cerr << "[Config] FATAL: " << e.what() << "\n";
        delete config;
        return 1;
    }

    // Fail-fast: proxy-маршрут, ссылающийся на несуществующий кластер
    // Fail-fast: a proxy route referencing a nonexistent cluster
    for (const RouteConfig& r : config->routes) {
        size_t colon = r.handler_type.find(':');
        if (r.handler_type.substr(0, colon) == "proxy") {
            std::string target = (colon == std::string::npos) ? std::string() : r.handler_type.substr(colon + 1);
            if (!config->upstreams.count(target)) {
                std::cerr << "[Config] FATAL: route '" << r.path
                          << "' references unknown upstream '" << target << "'\n";
                delete config;
                return 1;
            }
        }
    }

    // Shared-Nothing: каждый воркер получает свою долю пула соединений
    // Shared-Nothing: each worker gets its share of the connection pool
    config->max_connections = std::max(128, config->max_connections / config->worker_threads);

    HttpRouter* router = new HttpRouter();
    setup_routes_from_config(*router, *config);
    // /api/stats + /api/reload (the web panel)
    register_system_routes(*router); // /api/stats + /api/reload (веб-панель)

    g_active_state.config = config;
    g_active_state.router = router;

    // GC-тред: отложенное удаление старых конфигов после hot reload
    // GC thread: deferred deletion of old configs after a hot reload
    std::thread gc_thread(gc_thread_func);

    g_shutdown_fd = eventfd(0, EFD_NONBLOCK);
    struct sigaction sa{};
    sa.sa_handler = shutdown_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
    // Все сетевые write идут с MSG_NOSIGNAL (io_uring + mbedTLS BIO); SIGPIPE
    // All network writes go with MSG_NOSIGNAL (io_uring + mbedTLS BIO); SIGPIPE
    // игнорируем как страховку от любых синхронных писателей
    // is ignored as a safety net against any synchronous writers
    (void)signal(SIGPIPE, SIG_IGN);

    // eventfd на каждого воркера: /api/reload пишет в них единицу, воркеры
    // One eventfd per worker: /api/reload writes a one into them, the workers
    // ловят CQE OpType::RELOAD и перечитывают g_active_state
    // catch the CQE OpType::RELOAD and re-read g_active_state
    g_worker_reload_fds.resize(static_cast<size_t>(config->worker_threads));
    for (int i = 0; i < config->worker_threads; ++i) {
        g_worker_reload_fds[static_cast<size_t>(i)] = eventfd(0, EFD_NONBLOCK);
    }

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(config->worker_threads));
    const unsigned num_cores = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 0; i < config->worker_threads; ++i) {
        workers.emplace_back([config, router, i]() {
            Server server(g_active_state.config, g_active_state.router, g_shutdown_fd,
                          g_worker_reload_fds[static_cast<size_t>(i)]);
            server.start();
            if (i == 0) {
                std::cout << "Server started on port " << config->port
                          << " [Zero Allocation Mode, workers=" << config->worker_threads
                          << ", max_connections/worker=" << config->max_connections
                          << ", buffer_size=" << config->buffer_size
                          << ", splice_pipes=" << config->splice_pipes
                          << ", tls_large_buffers=" << config->tls_large_buffers
                          << ", keep_alive=" << (config->enable_keep_alive ? "on" : "off")
                          << "]\n" << std::flush;
            }
            // The point of no return. Not a single malloc after this.
            server.run(); // Точка невозврата. Дальше ни одного malloc.
        });

        if (config->enable_affinity) {
            // Пиннинг воркера на ядро: пулы/буферы не мигрируют между L1/L2
            // Pinning the worker to a core: pools/buffers do not migrate between L1/L2
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(static_cast<int>(i) % static_cast<int>(num_cores), &cpuset);
            int rc = pthread_setaffinity_np(workers.back().native_handle(),
                                            sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "[worker " << i << "] affinity failed: "
                          << std::strerror(rc) << "\n";
            }
        }
    }
    for (auto& t : workers) t.join();
    std::cout << "Server stopped.\n";

    g_gc_stop.store(true, std::memory_order_relaxed);
    gc_thread.join();
    for (int fd : g_worker_reload_fds) (void)close(fd);
    (void)close(g_shutdown_fd);
    delete router;
    delete config;
    return 0;
}
