#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <array>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsh {

class Metrics {
public:
  void record_command(bool ok, std::chrono::milliseconds duration) {
    // FIX[OBS-2]: Use real counters instead of hardcoded telemetry values.
    if (ok) {
      commands_ok_.fetch_add(1, std::memory_order_relaxed);
    } else {
      commands_error_.fetch_add(1, std::memory_order_relaxed);
    }
    last_duration_ms_.store(duration.count(), std::memory_order_relaxed);
    record_command_duration(static_cast<double>(duration.count()));
  }

  void record_command_duration(double duration_ms) {
    // BUG: p99 latency was reported from the last value, often showing fake 0.
    // FIX: keep a circular sample of real request durations and compute p99.
    std::lock_guard<std::mutex> lock(duration_mutex_);
    durations_[duration_index_ % durations_.size()] = duration_ms;
    ++duration_index_;
    duration_count_ = std::min(duration_count_ + 1, durations_.size());
  }

  void connection_opened() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
  }
  void connection_closed() {
    // BUG-18 FIX: If connection_closed() is called without a matching
    // connection_opened() (e.g. due to an exception thrown before the counter
    // was incremented), the counter would go negative and /healthz would report
    // a nonsensical "clients_connected": -1.
    std::int64_t current = active_connections_.load(std::memory_order_relaxed);
    while (current > 0 &&
           !active_connections_.compare_exchange_weak(
               current, current - 1, std::memory_order_relaxed)) {
      // retry
    }
  }

  std::uint64_t ok_count() const {
    return commands_ok_.load(std::memory_order_relaxed);
  }
  std::uint64_t error_count() const {
    return commands_error_.load(std::memory_order_relaxed);
  }
  std::int64_t active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
  }
  std::int64_t last_duration_ms() const {
    return last_duration_ms_.load(std::memory_order_relaxed);
  }

  void increment_auth_failure(const std::string &ip) {
    // BUG: auth failures were invisible to metrics and brute-force monitoring.
    // FIX: count failures by client IP without storing token material.
    std::lock_guard<std::mutex> lock(auth_mutex_);
    ++auth_failures_by_ip_[ip];
  }

  void set_sandbox_seccomp_active(bool active) {
    sandbox_seccomp_active_.store(active ? 1 : 0, std::memory_order_relaxed);
  }

  double p99_duration_ms() const {
    std::lock_guard<std::mutex> lock(duration_mutex_);
    if (duration_count_ == 0) {
      return 0.0;
    }
    std::vector<double> values(durations_.begin(),
                               durations_.begin() + duration_count_);
    std::sort(values.begin(), values.end());
    const std::size_t index =
        std::min(values.size() - 1,
                 static_cast<std::size_t>(values.size() * 0.99));
    return values[index];
  }

  std::string prometheus(std::size_t queue_depth) const {
    std::ostringstream out;
    out << "tsh_commands_total{status=\"ok\"} " << ok_count() << "\n";
    out << "tsh_commands_total{status=\"error\"} " << error_count() << "\n";
    out << "tsh_command_duration_seconds{quantile=\"0.99\"} "
        << p99_duration_ms() / 1000.0 << "\n";
    out << "tsh_active_connections " << active_connections() << "\n";
    out << "tsh_scheduler_queue_depth " << queue_depth << "\n";
    out << "sandbox_seccomp_active " << sandbox_seccomp_active_.load()
        << "\n";
    {
      std::lock_guard<std::mutex> lock(auth_mutex_);
      for (const auto &[ip, count] : auth_failures_by_ip_) {
        out << "auth_failures_total{ip=\"" << ip << "\"} " << count << "\n";
      }
    }
    return out.str();
  }

private:
  std::atomic<std::uint64_t> commands_ok_{0};
  std::atomic<std::uint64_t> commands_error_{0};
  std::atomic<std::int64_t> active_connections_{0};
  std::atomic<std::int64_t> last_duration_ms_{0};
  std::atomic<int> sandbox_seccomp_active_{1};
  mutable std::mutex duration_mutex_;
  std::array<double, 1000> durations_{};
  std::size_t duration_index_ = 0;
  std::size_t duration_count_ = 0;
  mutable std::mutex auth_mutex_;
  std::unordered_map<std::string, std::uint64_t> auth_failures_by_ip_;
};

} // namespace tsh
