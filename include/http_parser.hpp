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
#include "connection.hpp"

// Результат работы парсера
// Parser result
enum class ParseResult : uint8_t {
    // Data has not fully arrived yet, wait for the next EPOLLIN
    INCOMPLETE = 0, // Данные еще не дошли, ждем следующего EPOLLIN
    // Parsed successfully
    OK,             // Успешно распарсили
    // Protocol error
    BAD_REQUEST,    // Ошибка протокола
    // The body does not fit the buffer (Content-Length > BUFFER_SIZE)
    TOO_LARGE       // Тело не влезает в буфер (Content-Length > BUFFER_SIZE)
};

// Структура-представление запроса (весит копейки, копируется по значению)
// Request representation struct (costs pennies, copied by value)
struct HttpRequest {
    std::string_view method;
    std::string_view path;
    // Request body. For chunked — RAW chunks (not mutated
    std::string_view body; // Тело запроса. Для chunked — СЫРЫЕ чанки (не мутируется
                           // by the parser!); the binary body is obtained via decode_chunked_in_place()
                           // парсером!); бинарное тело получает decode_chunked_in_place()
    // Header length (headers are parsed if > 0)
    size_t header_len = 0;      // Длина заголовков (заголовки распарсены, если > 0)
    // Content-Length (0 if none/not parsed)
    size_t content_length = 0;  // Content-Length (0, если нет/не парсен)
    // Full message size (headers + body) = consumed
    size_t raw_length = 0; // Полный размер сообщения (заголовки + тело) = consumed
    bool keep_alive = false;
    bool support_gzip = false;
    // The body was transferred as Transfer-Encoding: chunked
    bool chunked = false;  // Тело передано как Transfer-Encoding: chunked
};

class HttpParser {
public:
    // Парсит данные прямо из буфера Connection (Zero Copy).
    // Parses data straight from the Connection buffer (Zero Copy).
    // consumed: число байт запроса (заголовки + тело), которое можно удалить
    // consumed: the number of request bytes (headers + body) that can be removed
    // из rx_buffer после обработки. При INCOMPLETE/BAD_REQUEST не значимо.
    // from rx_buffer after processing. Not meaningful on INCOMPLETE/BAD_REQUEST.
    // ВНИМАНИЕ: parse() НЕ мутирует rx_buffer — для проксирования chunked-запроса
    // WARNING: parse() does NOT mutate rx_buffer — for proxying a chunked request
    // буфер должен остаться в первозданном виде (чтобы бэкенд получил валидные
    // the buffer must stay pristine (so the backend receives valid
    // чанки). Если хендлеру нужно бинарное тело, он явно вызывает
    // chunks). If a handler needs a binary body, it explicitly calls
    // decode_chunked_in_place().
    [[nodiscard]] static ParseResult parse(Connection* conn, HttpRequest& out_req,
                                           size_t& consumed);

    // Декодирует chunked-тело НА МЕСТЕ (в rx_buffer) и возвращает непрерывное
    // Decodes the chunked body IN PLACE (in rx_buffer) and returns a contiguous
    // бинарное тело в out_body. Разрушает chunked-фрейминг — вызывать только
    // binary body in out_body. Destroys the chunked framing — call only
    // если запрос не проксируется. Допустимо только для req.chunked == true.
    // if the request is not proxied. Valid only for req.chunked == true.
    [[nodiscard]] static ParseResult decode_chunked_in_place(Connection* conn,
                                                             const HttpRequest& req,
                                                             std::string_view& out_body);
};
