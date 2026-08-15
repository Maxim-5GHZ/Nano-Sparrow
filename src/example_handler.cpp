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
#include "server.hpp"
#include "app_state.hpp"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <unistd.h>

// Внимание: req содержит string_view на rx_buffer соединения. Действителен
// Caution: req contains a string_view into the connection's rx_buffer. Valid
// только на время вызова хендлера. target/mount — строки роутера (живут
// only for the duration of the handler call. target/mount — router strings (they live
// с момента старта, 0 аллокаций в рантайме).
// since startup, 0 allocations at runtime).

static void handle_status(Server& server, Connection* conn, const HttpRequest&,
                          const std::string&, std::string_view) {
    static const char kBody[] = "{\"status\": \"ok\", \"server\": \"zero-allocation-v6\"}";
    server.send_response(conn, kBody, sizeof(kBody) - 1, "application/json", true);
}

// Демонстрация: тело больше 1 КБ, где gzip реально выигрывает
// Demo: a body larger than 1 KB, where gzip really wins
static void handle_big(Server& server, Connection* conn, const HttpRequest&,
                       const std::string&, std::string_view) {
    static const char kBase[] = "{\"data\": \"";
    static const char kFill[] = "the quick brown fox jumps over the lazy dog; ";
    static const char kEnd[] = "\"}";
    char body[1800];
    size_t len = 0;
    std::memcpy(body, kBase, sizeof(kBase) - 1);
    len += sizeof(kBase) - 1;
    while (len + sizeof(kEnd) - 1 < sizeof(body)) {
        size_t n = std::min(sizeof(kFill) - 1, sizeof(body) - (sizeof(kEnd) - 1) - len);
        std::memcpy(body + len, kFill, n);
        len += n;
    }
    std::memcpy(body + len, kEnd, sizeof(kEnd) - 1);
    len += sizeof(kEnd) - 1;
    server.send_response(conn, body, len, "application/json", true);
}

// Прокси на бэкенд-кластер: target = имя кластера из [upstream:имя].
// Proxy to a backend cluster: target = the cluster name from [upstream:name].
// Round-Robin + keep-alive пул выбирает соединение внутри start_proxy().
// The Round-Robin + keep-alive pool picks a connection inside start_proxy().
static void handle_api(Server& server, Connection* conn, const HttpRequest&,
                       const std::string& target, std::string_view) {
    server.start_proxy(conn, target);
}

// Демонстрация поддержки тела запроса (Content-Length / chunked): эхо.
// Demo of request body support (Content-Length / chunked): echo.
// chunked-тело парсер НЕ декодирует автоматически (прокси должен слать бэкенду
// The parser does NOT decode the chunked body automatically (the proxy must send
// оригинальные чанки) — хендлер сам вызывает decode_chunked_in_place().
// the original chunks to the backend) — the handler calls decode_chunked_in_place() itself.
static void handle_echo(Server& server, Connection* conn, const HttpRequest& req,
                        const std::string&, std::string_view) {
    if (req.chunked) {
        std::string_view body;
        if (HttpParser::decode_chunked_in_place(conn, req, body) != ParseResult::OK) {
            server.send_error(conn, 400);
            return;
        }
        server.send_response(conn, body.data(), body.size(), "application/octet-stream", false);
        return;
    }
    server.send_response(conn, req.body.data(), req.body.size(), "application/octet-stream", false);
}

// MIME по расширению файла (static: массив структур, 0 аллокаций)
// MIME by file extension (static: an array of structs, 0 allocations)
static const char* mime_for(std::string_view path) {
    static constexpr struct {
        const char* ext;
        const char* mime;
    } kMimes[] = {
        {".html", "text/html; charset=utf-8"},      {".htm", "text/html; charset=utf-8"},
        {".css", "text/css"},                        {".js", "application/javascript"},
        {".json", "application/json"},               {".png", "image/png"},
        {".jpg", "image/jpeg"},                      {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},                       {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},                    {".txt", "text/plain; charset=utf-8"},
        {".xml", "application/xml"},                 {".wasm", "application/wasm"},
    };
    size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    std::string_view ext = path.substr(dot);
    for (const auto& m : kMimes) {
        if (ext == m.ext) return m.mime;
    }
    return "application/octet-stream";
}

// Статика с диска: target = корневая папка (база), mount = точка монтирования.
// Static from disk: target = the root folder (base), mount = the mount point.
// Асинхронное чтение io_uring (READ_FILE) прямо в tx_buffer, доставка без
// Async io_uring read (READ_FILE) straight into tx_buffer, delivery without
// единой копии данных (send_response умеет aliased-буфер).
// a single data copy (send_response supports an aliased buffer).
static void handle_static(Server& server, Connection* conn, const HttpRequest& req,
                          const std::string& target, std::string_view mount) {
    // Путь запроса без query string, относительный точки монтирования
    // Request path without the query string, relative to the mount point
    std::string_view rel = req.path;
    size_t q = rel.find('?');
    if (q != std::string_view::npos) rel = rel.substr(0, q);
    if (!mount.empty() && rel.size() >= mount.size() &&
        rel.compare(0, mount.size(), mount) == 0) {
        rel = rel.substr(mount.size());
    }
    while (!rel.empty() && rel.front() == '/') rel.remove_prefix(1);
    if (rel.empty()) rel = "index.html";

    // Защита от path traversal: ни один компонент не может быть ".." или "."
    // Path traversal protection: no component may be ".." or "."
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        size_t end = (slash == std::string_view::npos) ? rel.size() : slash;
        std::string_view comp = rel.substr(start, end - start);
        if (comp == ".." || comp == ".") {
            server.send_error(conn, 403);
            return;
        }
        start = end + 1;
    }

    // target + '/' + rel в фиксированный стековый буфер (0 аллокаций)
    // target + '/' + rel into a fixed stack buffer (0 allocations)
    char path[2048];
    if (target.size() + 1 + rel.size() + 1 > sizeof(path)) {
        server.send_error(conn, 413);
        return;
    }
    std::memcpy(path, target.data(), target.size());
    size_t len = target.size();
    if (len == 0 || path[len - 1] != '/') path[len++] = '/';
    std::memcpy(path + len, rel.data(), rel.size());
    len += rel.size();
    path[len] = '\0';

    server.serve_static_file_async(conn, path, mime_for(rel));
}

// Маршруты из конфига: "/api/ = proxy:api_cluster", "/static/ = static:/var/www",
// Routes from the config: "/api/ = proxy:api_cluster", "/static/ = static:/var/www",
// "/status = status", "/big = big", "/echo = echo". Прокси-маршруты
// "/status = status", "/big = big", "/echo = echo". Proxy routes
// регистрируются и точно, и по префиксу: работают и "/api", и "/api/...".
// are registered both exactly and by prefix: both "/api" and "/api/..." work.
void setup_routes_from_config(HttpRouter& router, const ServerConfig& config) {
    router.reserve(config.routes.size() * 2 + 1);
    for (const RouteConfig& r : config.routes) {
        size_t colon = r.handler_type.find(':');
        // string_view от ЖИВОЙ строки конфига (substr() от временной = UB)
        // string_view from the LIVE config string (substr() of a temporary = UB)
        std::string_view type(r.handler_type);
        if (colon != std::string::npos) type = type.substr(0, colon);
        std::string target = (colon == std::string::npos)
                                 ? std::string()
                                 : r.handler_type.substr(colon + 1);

        if (type == "status") {
            router.add(r.path, handle_status);
        } else if (type == "big") {
            router.add(r.path, handle_big);
        } else if (type == "echo") {
            router.add(r.path, handle_echo);
        } else if (type == "proxy") {
            router.add_proxy(r.path, handle_api, target);
            router.add_prefix_proxy(r.path, handle_api, target);
        } else if (type == "static") {
            router.add_prefix(r.path, handle_static, target);
        }
    }
}

// ===== Web-панель (Hot Reload): системные роуты =====
// ===== Web panel (Hot Reload): system routes =====

// GET /api/stats: сводка о работающем сервере из живого конфига.
// GET /api/stats: a summary of the running server from the live config.
// Тело собирается snprintf'ом в стековый буфер — 0 аллокаций.
// The body is assembled with snprintf into a stack buffer — 0 allocations.
static void handle_stats(Server& server, Connection* conn, const HttpRequest&,
                         const std::string&, std::string_view) {
    const ServerConfig& c = *server.get_config();
    char body[1024];
    int len = std::snprintf(body, sizeof(body),
        "{\"status\":\"ok\",\"server\":\"zero-allocation-v6\","
        "\"port\":%u,\"workers\":%d,\"max_connections\":%d,\"buffer_size\":%u,"
        "\"gzip\":%s,\"keep_alive\":%s,\"ssl\":%s,\"sqpoll\":%s,\"affinity\":%s,"
        "\"timeout_ms\":%" PRIu64 ",\"zerocopy\":%s,\"upstreams\":%zu,\"routes\":%zu}",
        c.port, c.worker_threads, c.max_connections, c.buffer_size,
        c.enable_gzip ? "true" : "false",
        c.enable_keep_alive ? "true" : "false",
        c.enable_ssl ? "true" : "false",
        c.enable_sqpoll ? "true" : "false",
        c.enable_affinity ? "true" : "false",
        c.io_timeout_ms,
        server.zerocopy_enabled() ? "true" : "false",
        c.upstreams.size(), c.routes.size());
    if (len < 0 || static_cast<size_t>(len) >= sizeof(body)) len = 0;
    server.send_response(conn, body, static_cast<size_t>(len), "application/json", true);
}

// POST /api/reload: тело = новый INI-конфиг. Полная валидация (fail-fast),
// POST /api/reload: body = a new INI config. Full validation (fail-fast),
// swap пары (config, router) в глобальном стейте, все воркеры будятся
// swap of the (config, router) pair in the global state, all workers are woken
// eventfd'ом (CQE OpType::RELOAD), старые указатели уходят в GC-очередь
// via eventfd (CQE OpType::RELOAD), the old pointers go into the GC queue
// (удаление через 10 секунд). Аллокации допустимы: это редкий админ-путь.
// (deletion after 10 seconds). Allocations are allowed: this is a rare admin path.
static void handle_reload(Server& server, Connection* conn, const HttpRequest& req,
                          const std::string&, std::string_view) {
    if (req.method != "POST") {
        server.send_error(conn, 405);
        return;
    }
    // the full request is already in rx_buffer
    std::string new_text(req.body.data(), req.body.size()); // полный запрос уже в rx_buffer

    ServerConfig* new_config = nullptr;
    HttpRouter* new_router = nullptr;
    try {
        new_config = new ServerConfig();
        parse_config_from_string(new_text, *new_config);
        new_router = new HttpRouter();
        setup_routes_from_config(*new_router, *new_config);
        // the web panel survives a reload
        register_system_routes(*new_router); // веб-панель переживает reload
    } catch (const std::exception&) {
        delete new_router;
        delete new_config;
        server.send_error(conn, 400);
        return;
    }

    // Воркеры не блокируются (они лишь читают сырые указатели g_active_state):
    // Workers are never blocked (they only read the raw pointers of g_active_state):
    // мьютекс нужен только против GC-треда. 10-секундная форточка покрывает
    // the mutex is needed only against the GC thread. The 10-second window covers
    // torn-read (конфиг уже новый, роутер еще старый — и наоборот).
    // a torn read (the config is already new, the router still old — and vice versa).
    {
        std::lock_guard<std::mutex> lock(g_gc_mutex);
        g_gc_queue.emplace(now_ms(), g_active_state);
        g_active_state.config = new_config;
        g_active_state.router = new_router;
    }

    // Будим всех воркеров: единичка в eventfd -> CQE OpType::RELOAD
    // Waking all workers: a one in the eventfd -> CQE OpType::RELOAD
    const uint64_t one = 1;
    for (int fd : g_worker_reload_fds) {
        ssize_t rc = write(fd, &one, sizeof(one));
        (void)rc;
    }

    char body[512];
    int len = std::snprintf(body, sizeof(body),
        "{\"status\":\"reloaded\",\"port\":%u,\"workers\":%d,\"routes\":%zu}",
        new_config->port, new_config->worker_threads, new_config->routes.size());
    if (len < 0 || static_cast<size_t>(len) >= sizeof(body)) len = 0;
    server.send_response(conn, body, static_cast<size_t>(len), "application/json", true);
}

void register_system_routes(HttpRouter& router) {
    router.add("/api/stats", handle_stats);
    router.add("/api/reload", handle_reload);
}
