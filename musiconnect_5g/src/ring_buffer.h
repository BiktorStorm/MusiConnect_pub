#pragma once
// Lock-free SPSC ring buffer for realtime audio.
// Writer: network/decode thread. Reader: ASIO callback thread.
// No locks, no allocations, cache-line friendly.

#include <atomic>
#include <cstring>
#include <cstddef>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_capacity(capacity + 1)  // +1 to distinguish full from empty
        , m_buffer(new T[capacity + 1]{})
        , m_write(0)
        , m_read(0) {}

    ~RingBuffer() { delete[] m_buffer; }

    RingBuffer(RingBuffer&& o) noexcept
        : m_capacity(o.m_capacity)
        , m_buffer(o.m_buffer)
        , m_write(o.m_write.load())
        , m_read(o.m_read.load()) {
        o.m_buffer = nullptr;
    }

    RingBuffer& operator=(RingBuffer&& o) noexcept {
        delete[] m_buffer;
        m_capacity = o.m_capacity;
        m_buffer = o.m_buffer;
        m_write.store(o.m_write.load());
        m_read.store(o.m_read.load());
        o.m_buffer = nullptr;
        return *this;
    }

    size_t available() const {
        size_t w = m_write.load(std::memory_order_acquire);
        size_t r = m_read.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (m_capacity - r + w);
    }

    size_t freeSpace() const {
        return m_capacity - 1 - available();
    }

    // Write count samples. Returns number actually written.
    // Does NOT overwrite old data — caller must handle overflow.
    size_t write(const T* data, size_t count) {
        size_t free = freeSpace();
        if (count > free) count = free;
        if (count == 0) return 0;

        size_t w = m_write.load(std::memory_order_relaxed);
        size_t first = m_capacity - w;

        if (first >= count) {
            std::memcpy(m_buffer + w, data, count * sizeof(T));
        } else {
            std::memcpy(m_buffer + w, data, first * sizeof(T));
            std::memcpy(m_buffer, data + first, (count - first) * sizeof(T));
        }

        m_write.store((w + count) % m_capacity, std::memory_order_release);
        return count;
    }

    // Read count samples. Fills with zero if underrun.
    // Returns number of real samples read.
    size_t read(T* output, size_t count) {
        size_t avail = available();
        size_t real = (count <= avail) ? count : avail;

        size_t r = m_read.load(std::memory_order_relaxed);

        if (real > 0) {
            size_t first = m_capacity - r;
            if (first >= real) {
                std::memcpy(output, m_buffer + r, real * sizeof(T));
            } else {
                std::memcpy(output, m_buffer + r, first * sizeof(T));
                std::memcpy(output + first, m_buffer, (real - first) * sizeof(T));
            }
            m_read.store((r + real) % m_capacity, std::memory_order_release);
        }

        // Fill remainder with silence
        if (real < count) {
            std::memset(output + real, 0, (count - real) * sizeof(T));
        }

        return real;
    }

    void clear() {
        m_read.store(0, std::memory_order_release);
        m_write.store(0, std::memory_order_release);
    }

private:
    size_t m_capacity;
    T* m_buffer;
    alignas(64) std::atomic<size_t> m_write;
    alignas(64) std::atomic<size_t> m_read;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
};
