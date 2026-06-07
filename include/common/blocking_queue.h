/**
 * @file blocking_queue.h
 * @brief VisionCast Bounded Blocking Queue
 * 
 * 本文件定义了 VisionCast 项目中用于媒体帧（音频/视频）传输的有界阻塞队列 BoundedBlockingQueue。
 * 该队列通过互斥锁（std::mutex）和条件变量（std::condition_variable）实现线程安全的生产者-消费者模型。
 * 在实时流媒体处理中，当队列满时，会丢弃最老的数据帧（push_drop_oldest），以确保下游阶段处理的总是最新的实时帧，
 * 避免累积延迟。
 */

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
 * 
 * BoundedBlockingQueue 是一个用于媒体帧实时传输的有界阻塞队列。
 * push_drop_oldest() 永远不会阻塞生产者。当队列满时，会自动丢弃队列头部（最老）的元素，
 * 以便下游处理单元（消费者）能及时消费最新的实时帧。
 */
template <typename T>
class BoundedBlockingQueue {
public:
    /**
     * @brief 构造函数，初始化有界阻塞队列的容量限制。
     * @param capacity 队列的最大容量，若传入 0 则会自动调整为 1。
     */
    explicit BoundedBlockingQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    /**
     * @brief 向队列尾部插入一个元素。如果队列已满，则丢弃最老的一个元素。
     * 
     * 该函数不会阻塞写入者，适用于视频帧捕获或网络数据接收等实时性要求极高的生产者线程。
     * 使用 mutex_ 保证对队列的互斥访问。
     * 
     * @param item 要插入的元素，通过移动语义（std::move）传入。
     * @return 若队列已关闭返回 false；插入/丢弃并插入成功返回 true。
     */
    bool push_drop_oldest(T item) {
        std::lock_guard<std::mutex> lock(mutex_); // 获取锁，保护临界区
        if (closed_) {
            return false;
        }
        if (queue_.size() >= capacity_) {
            queue_.pop_front(); // 队列满时，直接从头部弹出并丢弃最老的数据帧
            ++dropped_;         // 增加已丢弃元素计数
        }
        queue_.push_back(std::move(item));
        cond_.notify_one(); // 唤醒一个可能在等待的消费者线程
        return true;
    }

    /**
     * @brief 从队列头部取出一个元素。如果队列为空，则阻塞等待。
     * 
     * 消费者线程会通过该接口提取数据。若队列为空，线程会进入休眠状态直到被唤醒。
     * 
     * @param item 输出参数，取出的元素会通过移动赋值给此变量。
     * @return 若成功取出元素返回 true；若队列关闭且为空则返回 false。
     */
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_); // 使用 unique_lock 配合条件变量
        // 等待条件：队列被关闭或者队列不为空
        cond_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false; // 如果队列为空，说明队列已被关闭，返回 false
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    /**
     * @brief 关闭队列，通知所有等待的消费者线程。
     * 
     * 关闭后，push 将不再接收新元素，pop 将在消费完队列内剩余元素后返回 false。
     */
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cond_.notify_all(); // 唤醒所有在 cond_.wait 中挂起的消费者线程
    }

    /**
     * @brief 清空队列中的所有元素。
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    /**
     * @brief 获取当前队列中的元素数量。
     * @return 当前存储的元素个数。
     */
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 获取由于队列溢出而被丢弃的帧数计数。
     * @return 丢弃的总元素（帧）数量。
     */
    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    const std::size_t capacity_;           ///< 队列的最大容量限制
    mutable std::mutex mutex_;             ///< 保护队列内部数据的互斥锁，由于 const 成员函数中可能需要加锁，标记为 mutable
    std::condition_variable cond_;         ///< 用于等待队列非空或已关闭的条件变量
    std::deque<T> queue_;                  ///< 底层存储容器（双端队列）
    bool closed_ = false;                  ///< 队列是否已关闭的标志，初始为 false
    std::size_t dropped_ = 0;              ///< 被丢弃的元素（数据帧）计数器
};

}  // namespace visioncast
