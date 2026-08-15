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
#include <ctime>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

// ===== Hot Reload: общий стейт всех воркеров =====
// ===== Hot Reload: the common state of all workers =====
// Горячий цикл воркера читает g_active_state БЕЗ блокировок (сырые указатели,
// The worker hot loop reads g_active_state WITHOUT locks (raw pointers,
// 0 оверхеда). Меняет пару (config, router) только админский хендлер
// 0 overhead). Only the admin handler changes the (config, router) pair
// /api/reload; старые указатели уходят в g_gc_queue и удаляются GC-тредом
// /api/reload; the old pointers go into g_gc_queue and are deleted by the GC thread
// через >= 10 секунд после релоада (см. main.cpp).
// >= 10 seconds after a reload (see main.cpp).

struct ServerConfig;
class HttpRouter;

struct AppState {
    ServerConfig* config = nullptr;
    HttpRouter* router = nullptr;
};

// the current (config, router) pair
extern AppState g_active_state;              // актуальная пара (config, router)
// one eventfd per worker
extern std::vector<int> g_worker_reload_fds; // eventfd на каждого воркера
// locks: only /api/reload and GC
extern std::mutex g_gc_mutex;                // локи: только /api/reload и GC
extern std::queue<std::pair<uint64_t, AppState>> g_gc_queue;

// Монотонные часы (vDSO, ~20ns за вызов)
// Monotonic clock (vDSO, ~20ns per call)
inline uint64_t now_ms() {
    timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}
