#pragma once
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

namespace tsh {
class TimeoutException : public std::runtime_error {
public:
  explicit TimeoutException(const std::string &msg)
      : std::runtime_error("Timeout: " + msg) {}
};

class ExecutionGuard {
public:
  template <typename F>
  static std::string execute_with_timeout(std::chrono::milliseconds timeout_ms,
                                          F &&f) {
    auto future = std::async(std::launch::async, std::forward<F>(f));
    if (future.wait_for(timeout_ms) == std::future_status::timeout) {
      throw TimeoutException("Execution time exceeded hard limit of " +
                             std::to_string(timeout_ms.count()) + "ms");
    }
    return future.get();
  }
};
} // namespace tsh