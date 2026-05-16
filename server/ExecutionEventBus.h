#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tsh {

enum class ExecutionEventType {
  ClientConnected,
  RequestReceived,
  Authenticated,
  AstParsed,
  RbacPassed,
  AllowlistPassed,
  TaintPassed,
  WorkerAssigned,
  ExecutionStarted,
  StdoutChunk,
  StderrChunk,
  ExecutionCompleted,
  ExecutionFailed,
  SecurityViolation
};

struct ExecutionEvent {
  std::uint64_t sequence = 0;
  std::string timestamp;
  ExecutionEventType type = ExecutionEventType::RequestReceived;
  std::string request_id;
  std::string user;
  std::string tenant;
  std::string client_ip;
  std::string command;
  std::string worker;
  std::string state;
  std::string detail;
  std::string result;
  long long duration_ms = 0;
};

class ExecutionEventBus {
public:
  ExecutionEventBus() {
    if (const char *raw = std::getenv("TSH_EVENT_BUS_MAX")) {
      try {
        max_events_ = std::max<std::size_t>(1, std::stoull(raw));
      } catch (...) {
      }
    }
  }

  void publish(ExecutionEvent event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      event.sequence = ++sequence_;
      if (event.timestamp.empty()) {
        event.timestamp = now_iso8601();
      }
      events_.push_back(std::move(event));
      while (events_.size() > max_events_) {
        // BUG: the event bus had a tiny fixed buffer and no eviction metric.
        // FIX: cap is configurable and every dropped oldest event is counted.
        events_.pop_front();
        ++evictions_;
      }
    }
    cv_.notify_all();
  }

  std::vector<ExecutionEvent> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {events_.begin(), events_.end()};
  }

  std::vector<ExecutionEvent> snapshot_since(std::uint64_t since_sequence,
                                             std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_since_locked(since_sequence, limit);
  }

  std::vector<ExecutionEvent>
  wait_snapshot_since(std::uint64_t since_sequence, std::size_t limit,
                      std::chrono::milliseconds timeout) const {
    // BUG: dashboard polling repeatedly serialized the whole event history.
    // FIX: long-poll waits for newer sequence numbers and returns a capped set.
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return sequence_ > since_sequence; });
    return snapshot_since_locked(since_sequence, limit);
  }

  std::uint64_t latest_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
  }

  std::uint64_t evictions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evictions_;
  }

  static const char *type_name(ExecutionEventType type) {
    switch (type) {
    case ExecutionEventType::ClientConnected:
      return "client_connected";
    case ExecutionEventType::RequestReceived:
      return "request_received";
    case ExecutionEventType::Authenticated:
      return "authenticated";
    case ExecutionEventType::AstParsed:
      return "ast_parsed";
    case ExecutionEventType::RbacPassed:
      return "rbac_passed";
    case ExecutionEventType::AllowlistPassed:
      return "allowlist_passed";
    case ExecutionEventType::TaintPassed:
      return "taint_passed";
    case ExecutionEventType::WorkerAssigned:
      return "worker_assigned";
    case ExecutionEventType::ExecutionStarted:
      return "execution_started";
    case ExecutionEventType::StdoutChunk:
      return "stdout";
    case ExecutionEventType::StderrChunk:
      return "stderr";
    case ExecutionEventType::ExecutionCompleted:
      return "execution_completed";
    case ExecutionEventType::ExecutionFailed:
      return "execution_failed";
    case ExecutionEventType::SecurityViolation:
      return "security_violation";
    }
    return "unknown";
  }

  static std::string now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return out.str();
  }

private:
  std::vector<ExecutionEvent>
  snapshot_since_locked(std::uint64_t since_sequence, std::size_t limit) const {
    std::vector<ExecutionEvent> out;
    out.reserve(std::min(limit, events_.size()));
    for (const auto &event : events_) {
      if (event.sequence > since_sequence) {
        out.push_back(event);
        if (out.size() >= limit) {
          break;
        }
      }
    }
    return out;
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::deque<ExecutionEvent> events_;
  std::uint64_t sequence_ = 0;
  std::size_t max_events_ = 10000;
  std::uint64_t evictions_ = 0;
};

} // namespace tsh
