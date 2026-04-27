#pragma once
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <string>

namespace tsh {
    class TimeoutException : public std::runtime_error {
    public:
        explicit TimeoutException(const std::string& msg) : std::runtime_error("Timeout: " + msg) {}
    };

    class ExecutionGuard {
    public:
        template<typename F>
        static std::string execute_with_timeout(std::chrono::milliseconds timeout_ms, F&& f) {
            // Packages the task and runs it on a detached thread to prevent blocking
            std::packaged_task<std::string()> task(std::forward<F>(f));
            std::future<std::string> future = task.get_future();
            std::thread(std::move(task)).detach();

            if (future.wait_for(timeout_ms) == std::future_status::timeout) {
                throw TimeoutException("Execution time exceeded hard limit of " + std::to_string(timeout_ms.count()) + "ms");
            }
            return future.get();
        }
    };
}
