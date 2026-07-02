  // ring_buffer.h — Lock-free single-producer single-consumer ring buffer
  // Designed for the JACK realtime callback (producer) → network thread (consumer)
  // and network thread (producer) → JACK playout callback (consumer).

  #pragma once
  #include <atomic>
  #include <cstddef>
  #include <cstring>
  #include <vector>

  template <typename T>
  class RingBuffer {
  public:
      explicit RingBuffer(size_t capacity)
          : capacity_(capacity)
          , buffer_(capacity)
          , head_(0)
          , tail_(0)
      {}

      // Returns number of items available to read
      size_t available_read() const {
          size_t h = head_.load(std::memory_order_acquire);
          size_t t = tail_.load(std::memory_order_relaxed);
          return (h - t + capacity_) % capacity_;
      }

      // Returns number of slots available to write
      size_t available_write() const {
          return capacity_ - 1 - available_read();
      }

      // Push a single item. Returns false if full.
      bool push(const T& item) {
          size_t h = head_.load(std::memory_order_relaxed);
          size_t next = (h + 1) % capacity_;
          if (next == tail_.load(std::memory_order_acquire)) {
              return false; // full
          }
          buffer_[h] = item;
          head_.store(next, std::memory_order_release);
          return true;
      }

      // Push multiple items. Returns number of items pushed.
      size_t push_bulk(const T* data, size_t count) {
          size_t written = 0;
          for (size_t i = 0; i < count; ++i) {
              if (!push(data[i])) break;
              ++written;
          }
          return written;
      }

      // Pop a single item. Returns false if empty.
      bool pop(T& item) {
          size_t t = tail_.load(std::memory_order_relaxed);
          if (t == head_.load(std::memory_order_acquire)) {
              return false; // empty
          }
          item = buffer_[t];
          tail_.store((t + 1) % capacity_, std::memory_order_release);
          return true;
      }

      // Pop multiple items. Returns number of items popped.
      size_t pop_bulk(T* data, size_t count) {
          size_t read = 0;
          for (size_t i = 0; i < count; ++i) {
              if (!pop(data[i])) break;
              ++read;
          }
          return read;
      }

      // Peek without consuming. Returns false if empty.
      bool peek(T& item) const {
          size_t t = tail_.load(std::memory_order_relaxed);
          if (t == head_.load(std::memory_order_acquire)) {
              return false;
          }
          item = buffer_[t];
          return true;
      }

      void reset() {
          head_.store(0, std::memory_order_relaxed);
          tail_.store(0, std::memory_order_relaxed);
      }

  private:
      size_t capacity_;
      std::vector<T> buffer_;
      alignas(64) std::atomic<size_t> head_;
      alignas(64) std::atomic<size_t> tail_;
  };

  // Specialization for raw audio frames (float buffers)
  class AudioRingBuffer {
  public:
      explicit AudioRingBuffer(size_t frame_count, size_t frame_size)
          : frame_size_(frame_size)
          , ring_(frame_count * frame_size + 1)  // +1 for SPSC sentinel
      {}

      size_t available_frames() const {
          return ring_.available_read() / frame_size_;
      }

      bool push_frame(const float* frame) {
          if (ring_.available_write() < frame_size_) return false;
          ring_.push_bulk(frame, frame_size_);
          return true;
      }

      bool pop_frame(float* frame) {
          if (ring_.available_read() < frame_size_) return false;
          ring_.pop_bulk(frame, frame_size_);
          return true;
      }

      void reset() { ring_.reset(); }

  private:
      size_t frame_size_;
      RingBuffer<float> ring_;
  };
