#pragma once
// =============================================================================
// LOCK-FREE RING BUFFER
//
// Used between the Core Audio callback thread and the network/codec thread.
// Core Audio callbacks run at realtime priority — they CANNOT block, allocate,
// or take locks. This single-producer/single-consumer ring buffer is
// the only safe way to pass audio between threads.
//
// Properties:
//   - Lock-free (uses atomic read/write positions)
//   - Single producer, single consumer (SPSC)
//   - Fixed capacity (set at construction)
//   - Overflow: silently drops oldest samples (keeps latency bounded)
//   - Underrun: returns zeros (silence)
// =============================================================================

#include <atomic>
#include <cstring>
#include <vector>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : m_capacity(capacity), m_buffer(capacity), m_writePos(0), m_readPos(0) {}

    void resize(size_t capacity) {
        m_capacity = capacity;
        m_buffer.assign(capacity, T{});
        clear();
    }

    // Returns number of samples currently buffered
    size_t available() const {
        size_t w = m_writePos.load(std::memory_order_acquire);
        size_t r = m_readPos.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (m_capacity - r + w);
    }

    size_t freeSpace() const {
        return m_capacity - 1 - available();  // -1 to distinguish full from empty
    }

    // Write samples into the buffer.
    // If buffer is full, drops OLDEST samples to make room (bounds latency).
    void write(const T* data, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            size_t nextWrite = (m_writePos.load(std::memory_order_relaxed) + 1) % m_capacity;

            // If full, advance read position (drop oldest)
            if (nextWrite == m_readPos.load(std::memory_order_acquire)) {
                m_readPos.store((m_readPos.load(std::memory_order_relaxed) + 1) % m_capacity, std::memory_order_release);
            }

            m_buffer[m_writePos.load(std::memory_order_relaxed)] = data[i];
            m_writePos.store(nextWrite, std::memory_order_release);
        }
    }

    // Read samples from the buffer.
    // If buffer is empty, fills output with zeros (silence).
    // Returns number of real samples read (vs zeros filled).
    size_t read(T* output, size_t count) {
        size_t read = 0;
        for (size_t i = 0; i < count; ++i) {
            if (m_readPos.load(std::memory_order_acquire) == m_writePos.load(std::memory_order_acquire)) {
                // Underrun — fill with silence
                output[i] = T(0);
            } else {
                output[i] = m_buffer[m_readPos.load(std::memory_order_relaxed)];
                m_readPos.store((m_readPos.load(std::memory_order_relaxed) + 1) % m_capacity, std::memory_order_release);
                read++;
            }
        }
        return read;
    }

    // Reset buffer to empty state
    void clear() {
        m_readPos.store(0, std::memory_order_release);
        m_writePos.store(0, std::memory_order_release);
    }

    size_t capacity() const { return m_capacity; }

private:
    size_t m_capacity;
    std::vector<T> m_buffer;
    std::atomic<size_t> m_writePos;
    std::atomic<size_t> m_readPos;
};
