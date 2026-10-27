/**
 * @file lerobot_worker_pool.hpp
 * @brief The worker pool the LeRobot converters prepare episodes on.
 *
 * Episodes are prepared in parallel and committed to the dataset one at a time, in input
 * order.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_WORKER_POOL_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_WORKER_POOL_HPP_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace trossen::io::backends {

/// @brief Largest worker count default_jobs() returns.
constexpr unsigned kMaxDefaultJobs = 8;

/// @brief Prepared items held beyond one per worker.
constexpr std::ptrdiff_t kPrefetchSlack = 2;

/**
 * @brief Default worker count: the hardware thread count, at most kMaxDefaultJobs.
 *
 * @param hardware_threads Hardware thread count, as std::thread::hardware_concurrency()
 *   reports it; 0 (unknown) counts as 1.
 * @return Worker count, from 1 to kMaxDefaultJobs.
 */
int default_jobs(unsigned hardware_threads);

/**
 * @brief Prepare items on worker threads and commit them on the calling thread in order.
 *
 * Calls prepare(i) for every i in [0, count) on `jobs` worker threads, and commit(i, result)
 * on the calling thread for i = 0, 1, 2 and so on, each once its item is prepared. At most
 * jobs + kPrefetchSlack prepared items are held at once. When commit returns false, no
 * further item is committed or handed to a worker; items already being prepared finish and
 * are destroyed unused. Returns once every worker has exited. Neither callable may throw; a
 * throw terminates the process.
 *
 * @tparam Prepare Callable as `Result prepare(std::size_t i)`. Result must be
 *   default-constructible and movable.
 * @tparam Commit Callable as `bool commit(std::size_t i, Result& result)`.
 * @param count Number of items.
 * @param jobs Worker threads to run, at least 1.
 * @param prepare Called concurrently from the worker threads, once per item.
 * @param commit Called on the calling thread in increasing i; returns false to stop.
 */
template <typename Prepare, typename Commit>
void run_ordered(std::size_t count, int jobs, Prepare prepare, Commit commit) {
  using Result = std::invoke_result_t<Prepare&, std::size_t>;
  std::vector<Result> slots(count);
  std::vector<std::uint8_t> ready(count, 0);
  std::mutex mtx;
  std::condition_variable ready_cv;
  std::atomic<std::size_t> next_index{0};

  // A window of at least `jobs` permits guarantees progress: indices are handed out in
  // increasing order, so the item the calling thread waits for is always already on some
  // worker, which holds its permit until the item is committed.
  std::counting_semaphore<> slots_free(static_cast<std::ptrdiff_t>(jobs) + kPrefetchSlack);

  auto worker = [&]() {
    while (true) {
      slots_free.acquire();
      const std::size_t i = next_index.fetch_add(1);
      if (i >= count) {
        slots_free.release();  // nothing to do with this permit
        break;
      }
      Result result = prepare(i);
      {
        std::lock_guard<std::mutex> lk(mtx);
        slots[i] = std::move(result);
        ready[i] = 1;
      }
      ready_cv.notify_all();
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(jobs));
  for (int t = 0; t < jobs; ++t) pool.emplace_back(worker);

  for (std::size_t i = 0; i < count; ++i) {
    Result result;
    {
      std::unique_lock<std::mutex> lk(mtx);
      ready_cv.wait(lk, [&] { return ready[i] != 0; });
      result = std::move(slots[i]);
    }
    const bool keep_going = commit(i, result);
    if (!keep_going) {
      // Hand out no more work, and give every worker a permit so none stays blocked.
      next_index.store(count);
      slots_free.release(static_cast<std::ptrdiff_t>(jobs));
      break;
    }
    slots_free.release();
  }

  for (auto& th : pool) th.join();
}

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_WORKER_POOL_HPP_
