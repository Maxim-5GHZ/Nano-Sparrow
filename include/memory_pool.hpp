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
#include <cassert>
#include <cstddef>

// O(1) аллокатор без использования кучи в рантайме.
// O(1) allocator without using the heap at runtime.
// Вся память выделяется один раз в конструкторе при старте приложения.
// All memory is allocated once in the constructor at application startup.
template <typename T>
class MemoryPool {
private:
    T* data;
    int* free_indices;
    int capacity_;
    int top;

public:
    explicit MemoryPool(int capacity)
        : data(nullptr), free_indices(nullptr), capacity_(capacity), top(capacity - 1) {
        // The only allocation at application startup
        data = new T[capacity];           // Единственная аллокация при старте приложения
        free_indices = new int[capacity];
        for (int i = 0; i < capacity; ++i) {
            free_indices[i] = i;
        }
    }

    ~MemoryPool() {
        delete[] data;
        delete[] free_indices;
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // O(1) доступ к свободному слоту; nullptr означает, что пул исчерпан
    // O(1) access to a free slot; nullptr means the pool is exhausted
    [[nodiscard]] T* acquire() {
        // The pool is exhausted
        if (top < 0) return nullptr; // Пул исчерпан
        return &data[free_indices[top--]];
    }

    void release(T* item) {
        // Защита от мусорных/чужих указателей (снимается в Release)
        // Protection against garbage/foreign pointers (stripped in Release)
        assert(item >= data && item < data + capacity_);
        // Защита от двойного освобождения: повторный release переполнит top
        // Protection against double free: a repeated release would overflow top
        assert(top < capacity_ - 1 && "Pool overflow (double free?)");
        // Вычисляем индекс по указателю (Zero Overhead)
        // Computing the index from the pointer (Zero Overhead)
        int idx = static_cast<int>(item - data);
        free_indices[++top] = idx;
    }

    int capacity() const { return capacity_; }
    T* at(int i) { return &data[i]; }
};
