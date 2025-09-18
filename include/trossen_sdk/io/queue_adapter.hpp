/**
 * @file queue_adapter.hpp
 * @brief Queue abstraction for sink producer/consumer decoupling.
 *
 * Provides an interface (`QueueAdapter`) that abstracts the underlying
 * multi-producer single-consumer queue implementation. The default
 * implementation wraps moodycamel::ConcurrentQueue.
 */

#ifndef TROSSEN_SDK__IO__QUEUE_ADAPTER_HPP
#define TROSSEN_SDK__IO__QUEUE_ADAPTER_HPP

#include <memory>
#include <utility>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/third_party/concurrentqueue.h"

namespace trossen::io {

/**
 * @brief Abstract MPSC queue used by sinks
 *
 * Implementations must be thread-safe for multiple producers and a single draining consumer.
 * Ownership of enqueued records is shared via std::shared_ptr.
 */
class QueueAdapter {
public:
  virtual ~QueueAdapter() = default;

  /**
   * @brief Enqueue a record (non-blocking)
   *
   * @param rec Shared pointer to the record to enqueue
   */
  virtual void enqueue(std::shared_ptr<data::RecordBase> rec) = 0;

  /**
   * @brief Try to dequeue one record
   * @param rec Output parameter for the dequeued record
   *
   * @return true on success, false if empty
   */
  virtual bool try_dequeue(std::shared_ptr<data::RecordBase>& rec) = 0;
};

/**
 * @brief Queue adapter using moodycamel::ConcurrentQueue
 */
class MoodyCamelQueueAdapter final : public QueueAdapter {
public:
  /**
   * @brief Enqueue a record (non-blocking)
   *
   * @param rec Shared pointer to the record to enqueue
   */
  void enqueue(std::shared_ptr<data::RecordBase> rec) override {
    queue_.enqueue(std::move(rec));
  }

  /**
   * @brief Try to dequeue one record
   *
   * @param rec Output parameter for the dequeued record
   * @return true on success, false if empty
   */
  bool try_dequeue(std::shared_ptr<data::RecordBase>& rec) override {
    return queue_.try_dequeue(rec);
  }
private:
  moodycamel::ConcurrentQueue<std::shared_ptr<data::RecordBase>> queue_;
};

/** @brief Owning pointer type for queue adapters */
using QueueAdapterPtr = std::unique_ptr<QueueAdapter>;

} // namespace trossen::io

#endif // TROSSEN_SDK__IO__QUEUE_ADAPTER_HPP
