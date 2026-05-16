#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace tsh {

class ThreadPool {
public:
  explicit ThreadPool(std::size_t worker_count) {
    const std::size_t count = worker_count == 0 ? 2 : worker_count;
    for (std::size_t i = 0; i < count; ++i) {
      workers_.emplace_back([this]() { worker_loop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  bool submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || tasks_.size() >= max_queue_depth_) {
        return false;
      }
      tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_)
        return;
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable())
        worker.join();
    }
  }

  std::size_t queue_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

  bool saturated() const { return queue_depth() >= max_queue_depth_; }

private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      try {
        task();
      } catch (const std::exception &e) {
        std::cerr << "[ThreadPool] task failed: " << e.what() << "\n";
      } catch (...) {
        std::cerr << "[ThreadPool] task failed with unknown exception\n";
      }
    }
  }

  static constexpr std::size_t max_queue_depth_ = 1024;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

} // namespace tsh
