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

// Регистрация маршрутов приложения из конфига (вызывается 1 раз при старте
// Registration of application routes from the config (called once at startup
// и в каждом /api/reload — см. handle_reload)
// and on every /api/reload — see handle_reload)
void setup_routes_from_config(HttpRouter& router, const ServerConfig& config);

// Системные роуты веб-панели (/api/stats, /api/reload): регистрируются при
// Web panel system routes (/api/stats, /api/reload): registered at
// старте и ПОСЛЕ каждого hot reload, чтобы панель не отваливалась
// startup and AFTER each hot reload, so the panel never disappears
void register_system_routes(HttpRouter& router);

// INI-парсер (определен в main.cpp): главный load_config и админский
// INI parser (defined in main.cpp): the main load_config and the admin
// /api/reload. Бросает std::runtime_error на любую ошибку конфига (fail-fast)
// /api/reload. Throws std::runtime_error on any config error (fail-fast)
void parse_config_from_string(const std::string& content, ServerConfig& config);
