#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace visioncast {

/**
 * BoundedBlockingQueue is a small real-time queue for media frames.
 *
 * push_drop_oldest() never blocks producers. When the queue is full, it drops
 * the oldest item so downstream stages always work on the newest live frame.
 */
template <typename T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    bool push_drop_oldest(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(item));
        cond_.notify_one();
        return true;
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cond_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<T> queue_;
    bool closed_ = false;
    std::size_t dropped_ = 0;
};

}  // namespace visioncast
