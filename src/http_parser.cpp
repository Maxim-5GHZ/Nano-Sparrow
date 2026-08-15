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
#include "http_parser.hpp"
#include "picohttpparser.h"
#include <cstring>

// Максимум заголовков за запрос (защита от заголовочного флуда;
// Maximum headers per request (protection against header flooding;
// phr_parse_request вернет -1 при превышении -> BAD_REQUEST)
// phr_parse_request returns -1 on overflow -> BAD_REQUEST)
constexpr size_t MAX_HEADERS = 32;

// Регистронезависимое сравнение (без аллокаций)
// Case-insensitive comparison (no allocations)
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

// Поиск токена в списке через запятую ("deflate, gzip, br" -> gzip), без аллокаций
// Token search in a comma-separated list ("deflate, gzip, br" -> gzip), no allocations
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

// Сканирование chunked-тела прямо в rx_buffer (Zero Allocation).
// Chunked body scanning right in rx_buffer (Zero Allocation).
// При decode_in_place=false буфер НЕ мутируется: только валидация структуры
// With decode_in_place=false the buffer is NOT mutated: only structure validation
// и поиск конца сообщения (прокси должен отправить бэкенду оригинальные чанки).
// and end-of-message search (the proxy must send the original chunks to the backend).
// При decode_in_place=true чанки декодируются на месте (фрейминг разрушается!)
// With decode_in_place=true chunks are decoded in place (the framing is destroyed!)
// и decoded_len содержит длину непрерывного бинарного тела.
// and decoded_len holds the length of the contiguous binary body.
// Возвращает ParseResult; consumed = конец всего сообщения (заголовки + чанки + трейлеры).
// Returns ParseResult; consumed = the end of the whole message (headers + chunks + trailers).
static ParseResult chunked_scan(Connection* conn, size_t header_total,
                                size_t& consumed, size_t& decoded_len,
                                bool decode_in_place) {
    std::string_view buffer(conn->rx_buffer, static_cast<size_t>(conn->rx_bytes));
    size_t src = header_total;
    // Decoding in place: dst never outruns src
    size_t dst = header_total; // Декодируем на месте: dst никогда не обгоняет src
    decoded_len = 0;

    while (true) {
        // 1. Строка размера чанка: "<hex>[;extensions]\r\n"
        // 1. Chunk size line: "<hex>[;extensions]\r\n"
        size_t eol = buffer.find("\r\n", src);
        if (eol == std::string_view::npos) return ParseResult::INCOMPLETE;
        std::string_view size_line = buffer.substr(src, eol - src);

        size_t chunk_size = 0;
        size_t i = 0;
        for (; i < size_line.size() && size_line[i] != ';'; ++i) {
            char c = size_line[i];
            if (c >= '0' && c <= '9') chunk_size = chunk_size * 16u + static_cast<size_t>(c - '0');
            else if (c >= 'a' && c <= 'f') chunk_size = chunk_size * 16u + static_cast<size_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk_size = chunk_size * 16u + static_cast<size_t>(c - 'A' + 10);
            else return ParseResult::BAD_REQUEST;
            // Проверка на КАЖДОЙ итерации: исключает целочисленное переполнение
            // Check on EVERY iteration: rules out integer overflow
            // (chunk_size <= buffer_size до умножения) и чанки больше буфера
            // (chunk_size <= buffer_size before multiplication) and chunks larger than the buffer
            if (chunk_size > static_cast<size_t>(conn->buffer_size)) return ParseResult::TOO_LARGE;
        }
        // Empty size line
        if (i == 0) return ParseResult::BAD_REQUEST; // Пустая строка размера
        src = eol + 2;

        if (chunk_size == 0) {
            // 2. Последний чанк: трейлеры заканчиваются "\r\n\r\n"
            // 2. Last chunk: trailers end with "\r\n\r\n"
            //    или пустые трейлеры - просто "\r\n"
            //    or empty trailers - just "\r\n"
            size_t trailers = buffer.find("\r\n\r\n", src);
            size_t end = (trailers == std::string_view::npos) ? src + 2 : trailers + 4;
            if (end > buffer.size()) return ParseResult::INCOMPLETE;
            consumed = end;
            return ParseResult::OK;
        }

        // 3. Данные чанка + завершающий CRLF (без переполнения size_t)
        // 3. Chunk data + trailing CRLF (without size_t overflow)
        if (buffer.size() - src < chunk_size) return ParseResult::INCOMPLETE;
        if (buffer.size() - src - chunk_size < 2) return ParseResult::INCOMPLETE;
        if (buffer[src + chunk_size] != '\r' || buffer[src + chunk_size + 1] != '\n')
            return ParseResult::BAD_REQUEST;

        if (decode_in_place) {
            std::memmove(conn->rx_buffer + dst, conn->rx_buffer + src, chunk_size);
            decoded_len += chunk_size;
            dst += chunk_size;
        }
        src += chunk_size + 2;
    }
}

ParseResult HttpParser::parse(Connection* conn, HttpRequest& out_req,
                              size_t& consumed) {
    consumed = 0;
    // Marker: headers not parsed yet (see below)
    out_req.header_len = 0; // Маркер: заголовки пока не распарсены (см. ниже)

    // SIMD-парсер (SSE4.2): request line и все заголовки за 1-2 прохода.
    // SIMD parser (SSE4.2): request line and all headers in 1-2 passes.
    // Массив заголовков на стеке: 0 аллокаций.
    // Header array on the stack: 0 allocations.
    const char* method_str = nullptr;
    size_t method_len = 0;
    const char* path_str = nullptr;
    size_t path_len = 0;
    int minor_version = 0;

    phr_header headers[MAX_HEADERS];
    size_t num_headers = MAX_HEADERS;

    int res = phr_parse_request(
        conn->rx_buffer, static_cast<size_t>(conn->rx_bytes),
        &method_str, &method_len,
        &path_str, &path_len,
        &minor_version,
        headers, &num_headers, 0);

    if (res == -2) {
        // Неполный запрос. Защита от переполнения буфера: если шлют мусор
        // Incomplete request. Buffer overflow protection: if garbage is sent
        // без конца заголовков — рвем по размеру буфера.
        // without an end of headers — break by the buffer size.
        if (conn->rx_bytes >= static_cast<int>(conn->buffer_size)) return ParseResult::BAD_REQUEST;
        return ParseResult::INCOMPLETE;
    }
    // Protocol error
    if (res == -1) return ParseResult::BAD_REQUEST; // Ошибка протокола

    size_t header_total = static_cast<size_t>(res);

    out_req.method = std::string_view(method_str, method_len);
    out_req.path = std::string_view(path_str, path_len);

    // 3. Разбор заголовков из готового массива указателей от phr_parse_request
    // 3. Header parsing from the ready pointer array of phr_parse_request
    //    (регистронезависимо, без хэш-мап)
    //    (case-insensitively, without a hash map)
    bool connection_close = false;
    bool support_gzip = false;
    bool has_content_length = false;
    bool chunked = false;
    size_t content_length = 0;
    for (size_t i = 0; i < num_headers; ++i) {
        std::string_view name(headers[i].name, headers[i].name_len);
        std::string_view value(headers[i].value, headers[i].value_len);

        if (iequals(name, "connection") && has_token(value, "close")) {
            connection_close = true;
        } else if (iequals(name, "accept-encoding") && has_token(value, "gzip")) {
            support_gzip = true;
        } else if (iequals(name, "content-length")) {
            // Защита от request smuggling: дубликаты Content-Length - ошибка
            // Request smuggling protection: duplicate Content-Length - an error
            if (has_content_length || value.empty()) return ParseResult::BAD_REQUEST;
            size_t cl = 0;
            for (char c : value) {
                if (c < '0' || c > '9') return ParseResult::BAD_REQUEST;
                // Сатурация вместо раннего TOO_LARGE: решение о размере тела
                // Saturation instead of an early TOO_LARGE: the body size decision
                // принимается ПОСЛЕ разбора заголовков (стриминговые маршруты
                // is made AFTER header parsing (streaming routes
                // могут принимать тело больше buffer_size).
                // may accept a body larger than buffer_size).
                if (cl > (SIZE_MAX - 9u) / 10u) cl = SIZE_MAX;
                else cl = cl * 10u + static_cast<size_t>(c - '0');
            }
            content_length = cl;
            has_content_length = true;
        } else if (iequals(name, "transfer-encoding")) {
            if (!has_token(value, "identity")) chunked = true;
        }
    }

    // HTTP/1.1 по умолчанию keep-alive; закрываем только при явном "close"
    // HTTP/1.1 defaults to keep-alive; we close only on an explicit "close"
    out_req.keep_alive = (minor_version >= 1) && !connection_close;
    out_req.support_gzip = support_gzip;

    // Заголовки распарсены. header_len > 0 сигнализирует серверу, что при
    // Headers are parsed. header_len > 0 signals the server that on
    // INCOMPLETE/TOO_LARGE можно предложить стриминг тела (прокси-маршруты).
    // INCOMPLETE/TOO_LARGE body streaming can be offered (proxy routes).
    out_req.header_len = header_total;
    out_req.content_length = has_content_length ? content_length : 0;

    // 4. Тело запроса. Content-Length + chunked одновременно - smuggling-вектор.
    // 4. Request body. Content-Length + chunked at once - a smuggling vector.
    if (has_content_length && chunked) return ParseResult::BAD_REQUEST;

    // Флаг ставится ДО сканирования тела: при INCOMPLETE (тело больше буфера)
    // The flag is set BEFORE body scanning: on INCOMPLETE (body larger than the buffer)
    // сервер диспатчит стриминговому прокси, который обязан знать про chunked.
    // the server dispatches to the streaming proxy, which must know about chunked.
    out_req.chunked = chunked;
    if (chunked) {
        // Только валидация и поиск конца сообщения: буфер НЕ мутируем,
        // Only validation and end-of-message search: the buffer is NOT mutated,
        // чтобы прокси мог переслать бэкенду оригинальные чанки.
        // so the proxy can forward the original chunks to the backend.
        size_t decoded_len = 0;
        ParseResult r = chunked_scan(conn, header_total, consumed, decoded_len, false);
        if (r != ParseResult::OK) return r;
        out_req.body = std::string_view(conn->rx_buffer + header_total,
                                        // Raw chunks
                                        consumed - header_total); // Сырые чанки
    } else if (has_content_length) {
        // Без переполнения при cl == SIZE_MAX (сатурация выше)
        // No overflow with cl == SIZE_MAX (saturation above)
        if (content_length > static_cast<size_t>(conn->buffer_size) - header_total)
            return ParseResult::TOO_LARGE;
        if (static_cast<size_t>(conn->rx_bytes) < header_total + content_length)
            // Waiting for the rest of the body
            return ParseResult::INCOMPLETE; // Ждем остаток тела
        out_req.body = std::string_view(conn->rx_buffer + header_total, content_length);
        consumed = header_total + content_length;
    } else {
        out_req.body = std::string_view();
        consumed = header_total;
    }

    out_req.raw_length = consumed;
    return ParseResult::OK;
}

ParseResult HttpParser::decode_chunked_in_place(Connection* conn, const HttpRequest& req,
                                                std::string_view& out_body) {
    if (!req.chunked) return ParseResult::BAD_REQUEST;

    // Заголовки все еще на своих местах (хендлер вызывается до сдвига буфера)
    // Headers are still in place (the handler is called before the buffer shift)
    std::string_view buffer(conn->rx_buffer, static_cast<size_t>(conn->rx_bytes));
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return ParseResult::BAD_REQUEST;
    size_t header_total = header_end + 4;

    size_t consumed = 0;
    size_t decoded_len = 0;
    ParseResult r = chunked_scan(conn, header_total, consumed, decoded_len, true);
    if (r != ParseResult::OK) return r;

    out_body = std::string_view(conn->rx_buffer + header_total, decoded_len);
    return ParseResult::OK;
}
