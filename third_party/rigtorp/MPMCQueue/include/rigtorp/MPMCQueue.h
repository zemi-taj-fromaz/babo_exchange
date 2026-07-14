/* Copyright (c) 2018 Erik Rigtorp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace rigtorp {

template <typename T>
class MPMCQueue {
public:
    explicit MPMCQueue(std::size_t capacity)
        : capacity_(capacity < 2 ? 2 : capacity), buffer_(capacity_) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPMCQueue() {
        T value;
        while (try_pop(value)) {
        }
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    void push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        while (!try_push(value)) {
        }
    }

    void push(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        while (!try_push(std::move(value))) {
        }
    }

    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(value));
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        Cell* cell = nullptr;
        auto pos = enqueuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos % capacity_];
            const auto seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        std::construct_at(cell->ptr(), std::forward<Args>(args)...);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        auto pos = dequeuePos_.load(std::memory_order_relaxed);
        Cell& cell = buffer_[pos % capacity_];
        const auto seq = cell.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq) -
                          static_cast<std::intptr_t>(pos + 1);

        if (diff < 0) {
            return false;
        }
        if (diff > 0) {
            // Single-consumer invariant: dequeuePos_ cannot lag because another
            // consumer advanced it. A positive diff can only be a transient view
            // while producers are racing ahead; retry on the next call.
            return false;
        }

        dequeuePos_.store(pos + 1, std::memory_order_relaxed);

        value = std::move(*cell.ptr());
        std::destroy_at(cell.ptr());
        cell.sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    static constexpr std::size_t kCacheLineSize = 64;
    static constexpr std::size_t kStorageAlignment =
        kCacheLineSize > alignof(T) ? kCacheLineSize : alignof(T);

    struct Cell {
        alignas(kCacheLineSize) std::atomic<std::size_t> sequence{};
        alignas(kStorageAlignment) std::byte storage[sizeof(T)];

        [[nodiscard]] T* ptr() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    const std::size_t capacity_;
    std::vector<Cell> buffer_;
    alignas(kCacheLineSize) std::atomic<std::size_t> enqueuePos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeuePos_{0};
};

} // namespace rigtorp
