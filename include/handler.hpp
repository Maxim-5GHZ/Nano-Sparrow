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
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "http_parser.hpp"

class Server;
struct Connection;

// target — аргумент маршрута из конфига (имя upstream-кластера, папка статики
// target — the route argument from the config (upstream cluster name, static folder
// и т.п.); mount — точка монтирования маршрута ("/api/"). Обе строки живут в
// etc.); mount — the route mount point ("/api/"). Both strings live in
// роутере с момента старта: в рантайме хендлер получает только ссылки
// the router since startup: at runtime the handler receives only references
// (0 аллокаций при обработке запроса).
// (0 allocations while processing a request).
using RouteHandler = void (*)(Server& server, Connection* conn, const HttpRequest& req,
                              const std::string& target, std::string_view mount);

// Интерфейс приложения (паттерн Reactor / Inversion of Control):
// Application interface (Reactor pattern / Inversion of Control):
// Server вызывает его после парсинга HTTP-запроса, вся сеть и стейт-машина
// Server calls it after parsing an HTTP request; all networking and the
// TCP/TLS остаются внутри Server.
// TCP/TLS state machine stay inside Server.
class IRequestHandler {
public:
    virtual ~IRequestHandler() = default;

    // Внимание: req содержит string_view на rx_buffer соединения.
    // Caution: req contains a string_view into the connection's rx_buffer.
    // Действителен только на время вызова.
    // Valid only for the duration of the call.
    virtual void on_request(Server& server, Connection* conn, const HttpRequest& req) = 0;

    // Возвращает true, если маршрут умеет обрабатывать запрос с НЕПОЛНЫМ телом
    // Returns true if the route can handle a request with an INCOMPLETE body
    // (стриминговый прокси). В этом случае on_request может быть вызван сразу
    // (streaming proxy). In this case on_request may be called right
    // после заголовков: req.body пуст, а content_length/chunked/header_len
    // after the headers: req.body is empty, while content_length/chunked/header_len
    // заполнены. Такой хендлер обязан вызвать start_proxy() (или закрыть
    // are filled. Such a handler must call start_proxy() (or close
    // соединение). По умолчанию false: хендлер получает только полные запросы,
    // the connection). Defaults to false: the handler receives only complete requests,
    // а тело больше буфера соединения отклоняется 413.
    // and a body larger than the connection buffer is rejected with 413.
    virtual bool wants_streaming(const HttpRequest& /*req*/) { return false; }
};

// Роутер без runtime-аллокаций: маршруты регистрируются до run() (при старте),
// Router without runtime allocations: routes are registered before run() (at startup),
// в рантайме — только find() по уже распарсенному пути.
// at runtime — only find() over the already parsed path.
class HttpRouter : public IRequestHandler {
public:
    // Маршрут + флаг стриминга (маршруты add_proxy обрабатывают неполное тело)
    // Route + streaming flag (add_proxy routes handle an incomplete body)
    struct RouteEntry {
        RouteHandler fn;
        bool streaming;
        // Handler argument (cluster name / static folder)
        std::string target; // Аргумент хендлера (имя кластера / папка статики)
        // Route mount point (for static)
        std::string mount;  // Точка монтирования маршрута (для статики)
    };

    void reserve(size_t n) { routes_.reserve(n); }

    // Точное совпадение пути (query string игнорируется)
    // Exact path match (the query string is ignored)
    void add(std::string_view path, RouteHandler handler, std::string target = "") {
        routes_.emplace(path, RouteEntry{handler, false, std::move(target), std::string(path)});
    }

    // Прокси-маршрут: хендлеру может быть отдано неполное тело (стриминг)
    // Proxy route: the handler may receive an incomplete body (streaming)
    void add_proxy(std::string_view path, RouteHandler handler, std::string target = "") {
        routes_.emplace(path, RouteEntry{handler, true, std::move(target), std::string(path)});
    }

    // Префиксное совпадение: /api/* и т.п.
    // Prefix match: /api/* etc.
    void add_prefix(std::string_view path, RouteHandler handler, std::string target = "") {
        prefixes_.emplace_back(path, RouteEntry{handler, false, std::move(target), std::string(path)});
    }

    void add_prefix_proxy(std::string_view path, RouteHandler handler, std::string target = "") {
        prefixes_.emplace_back(path, RouteEntry{handler, true, std::move(target), std::string(path)});
    }

    void on_request(Server& server, Connection* conn, const HttpRequest& req) override {
        const RouteEntry* entry = lookup(req.path);
        if (entry) {
            entry->fn(server, conn, req, entry->target, entry->mount);
            return;
        }
        on_not_found(server, conn, req);
    }

    bool wants_streaming(const HttpRequest& req) override {
        const RouteEntry* entry = lookup(req.path);
        return entry && entry->streaming;
    }

    virtual void on_not_found(Server& server, Connection* conn, const HttpRequest& req);

private:
    // Поиск маршрута: точное совпадение, затем первое префиксное
    // Route lookup: exact match first, then the first prefix match
    // (как и в старом on_request)
    // (as in the old on_request)
    const RouteEntry* lookup(std::string_view path) const {
        size_t q = path.find('?');
        if (q != std::string_view::npos) path = path.substr(0, q);

        auto it = routes_.find(path);
        if (it != routes_.end()) return &it->second;
        for (const auto& p : prefixes_) {
            if (path.size() >= p.first.size() &&
                path.compare(0, p.first.size(), p.first) == 0) {
                return &p.second;
            }
        }
        return nullptr;
    }

    std::unordered_map<std::string_view, RouteEntry> routes_;
    std::vector<std::pair<std::string_view, RouteEntry>> prefixes_;
};
