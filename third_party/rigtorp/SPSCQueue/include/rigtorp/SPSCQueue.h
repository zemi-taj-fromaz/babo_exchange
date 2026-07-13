/* Copyright (c) 2018-2020 Erik Rigtorp
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

#include <cassert>
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace rigtorp {

template <typename T, typename Allocator = std::allocator<T>>
class SPSCQueue {
public:
    explicit SPSCQueue(std::size_t capacity,
                       const Allocator& allocator = Allocator())
        : capacity_(capacity < 1 ? 2 : capacity + 1),
          allocator_(allocator),
          slots_(std::allocator_traits<Allocator>::allocate(
              allocator_, capacity_ + 2 * kPadding)) {}

    ~SPSCQueue() {
        while (front()) {
            pop();
        }
        std::allocator_traits<Allocator>::deallocate(
            allocator_, slots_, capacity_ + 2 * kPadding);
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    template <typename... Args>
    void emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        static_assert(std::is_constructible_v<T, Args&&...>);

        const auto write_idx = writeIdx_.load(std::memory_order_relaxed);
        auto next_write_idx = write_idx + 1;
        if (next_write_idx == capacity_) {
            next_write_idx = 0;
        }

        while (next_write_idx == readIdxCache_) {
            readIdxCache_ = readIdx_.load(std::memory_order_acquire);
        }

        new (&slots_[write_idx + kPadding]) T(std::forward<Args>(args)...);
        writeIdx_.store(next_write_idx, std::memory_order_release);
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args&&...>) {
        static_assert(std::is_constructible_v<T, Args&&...>);

        const auto write_idx = writeIdx_.load(std::memory_order_relaxed);
        auto next_write_idx = write_idx + 1;
        if (next_write_idx == capacity_) {
            next_write_idx = 0;
        }

        if (next_write_idx == readIdxCache_) {
            readIdxCache_ = readIdx_.load(std::memory_order_acquire);
            if (next_write_idx == readIdxCache_) {
                return false;
            }
        }

        new (&slots_[write_idx + kPadding]) T(std::forward<Args>(args)...);
        writeIdx_.store(next_write_idx, std::memory_order_release);
        return true;
    }

    void push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        emplace(value);
    }

    void push(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        emplace(std::move(value));
    }

    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(value));
    }

    [[nodiscard]] T* front() noexcept {
        const auto read_idx = readIdx_.load(std::memory_order_relaxed);
        if (read_idx == writeIdxCache_) {
            writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
            if (writeIdxCache_ == read_idx) {
                return nullptr;
            }
        }
        return &slots_[read_idx + kPadding];
    }

    void pop() noexcept {
        static_assert(std::is_nothrow_destructible_v<T>);

        const auto read_idx = readIdx_.load(std::memory_order_relaxed);
        assert(writeIdx_.load(std::memory_order_acquire) != read_idx);
        slots_[read_idx + kPadding].~T();

        auto next_read_idx = read_idx + 1;
        if (next_read_idx == capacity_) {
            next_read_idx = 0;
        }
        readIdx_.store(next_read_idx, std::memory_order_release);
    }

    [[nodiscard]] bool empty() const noexcept {
        return writeIdx_.load(std::memory_order_acquire) ==
               readIdx_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_ - 1;
    }

private:
    static constexpr std::size_t kCacheLineSize = 64;
    static constexpr std::size_t kPadding =
        (kCacheLineSize - 1) / sizeof(T) + 1;

    std::size_t capacity_;
    Allocator allocator_;
    T* slots_;

    alignas(kCacheLineSize) std::atomic<std::size_t> writeIdx_{0};
    alignas(kCacheLineSize) std::size_t readIdxCache_ = 0;
    alignas(kCacheLineSize) std::atomic<std::size_t> readIdx_{0};
    alignas(kCacheLineSize) std::size_t writeIdxCache_ = 0;
};

} // namespace rigtorp
