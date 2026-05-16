#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tsh {

enum class WorkerHealthStatus { HEALTHY, DEGRADED, UNREACHABLE };

struct WorkerNode {
  std::string host;
  int port;
  std::atomic<int> consecutive_failures{0};
  std::atomic<int> consecutive_missed_heartbeats{0};
  std::atomic<int> consecutive_job_failures{0};
  std::atomic<bool> is_healthy{true};
  std::atomic<WorkerHealthStatus> status{WorkerHealthStatus::HEALTHY};
  std::atomic<int> successful_jobs{0};
  std::atomic<int> failed_jobs{0};

  // FIX[C-5]: last_heartbeat and response_time_ms were plain non-atomic fields
  // read and written from multiple threads (the health-check background thread
  // writes them; get_healthy_nodes / health_score / is_responding read them).
  // std::chrono::time_point and double are not atomically readable/writable on
  // all architectures — concurrent access is a data race → undefined behaviour.
  //
  // Fix: guard both fields with a dedicated mutex.  All reads and writes go
  // through the typed accessors below, which hold the lock for the minimum
  // necessary time.  We deliberately do NOT store the mutex inside the atomic
  // wrappers to keep the struct copyable-in-spirit (though it is move-only
  // now because std::mutex is not copyable; callers already use shared_ptr).

  std::chrono::steady_clock::time_point get_last_heartbeat() const {
    std::lock_guard<std::mutex> lk(time_mutex_);
    return last_heartbeat_;
  }

  void set_last_heartbeat(std::chrono::steady_clock::time_point tp) {
    std::lock_guard<std::mutex> lk(time_mutex_);
    last_heartbeat_ = tp;
  }

  double get_response_time_ms() const {
    std::lock_guard<std::mutex> lk(time_mutex_);
    return response_time_ms_;
  }

  void set_response_time_ms(double ms) {
    std::lock_guard<std::mutex> lk(time_mutex_);
    response_time_ms_ = ms;
  }

  WorkerNode(const std::string &h, int p)
      : host(h), port(p), last_heartbeat_(std::chrono::steady_clock::now()),
        response_time_ms_(0.0) {}

  std::string identifier() const { return host + ":" + std::to_string(port); }

  bool is_responding() const {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             now - get_last_heartbeat())
                             .count();
    return elapsed < 30; // 30 second timeout
  }

  bool is_schedulable() const {
    const auto current = status.load();
    return current == WorkerHealthStatus::HEALTHY ||
           current == WorkerHealthStatus::DEGRADED;
  }

  double health_score() const {
    if (status.load() == WorkerHealthStatus::UNREACHABLE || !is_responding())
      return 0.0;

    const int total_jobs = successful_jobs.load() + failed_jobs.load();
    if (total_jobs == 0)
      return 50.0; // Neutral for new nodes

    const double success_rate =
        static_cast<double>(successful_jobs.load()) / total_jobs * 100.0;
    const double load_penalty = std::min(get_response_time_ms() / 100.0, 20.0);
    const double degradation_penalty =
        status.load() == WorkerHealthStatus::DEGRADED ? 50.0 : 0.0;
    return std::max(0.0, success_rate - load_penalty - degradation_penalty);
  }

private:
  mutable std::mutex
      time_mutex_; // guards last_heartbeat_ and response_time_ms_
  std::chrono::steady_clock::time_point last_heartbeat_;
  double response_time_ms_;
};

class ClusterHealthManager {
public:
  static constexpr int MAX_CONSECUTIVE_FAILURES = 3;
  static constexpr int DEGRADED_JOB_FAILURES = 5;
  static constexpr int HEARTBEAT_INTERVAL_MS = 5000;
  static constexpr int DEAD_NODE_REMOVAL_TIMEOUT_S = 60;
  static constexpr int RECONNECT_INTERVAL_S = 30;

  ClusterHealthManager() : running_(false) {}

  ~ClusterHealthManager() { stop(); }

  void add_node(const std::string &host, int port) {
    auto node = std::make_shared<WorkerNode>(host, port);
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    nodes_.emplace(node->identifier(), node);
  }

  void remove_node(const std::string &host, int port) {
    std::string id = host + ":" + std::to_string(port);
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    nodes_.erase(id);
  }

  void mark_success(const std::string &host, int port,
                    double response_time = 0.0) {
    auto node = get_node(host, port);
    if (!node)
      return;

    node->consecutive_failures.store(0);
    node->consecutive_missed_heartbeats.store(0);
    node->consecutive_job_failures.store(0);
    node->successful_jobs++;
    node->is_healthy.store(true);
    node->status.store(WorkerHealthStatus::HEALTHY);
    // FIX[C-5]: Use thread-safe accessor instead of direct field write.
    node->set_response_time_ms(response_time);
    node->set_last_heartbeat(std::chrono::steady_clock::now());
  }

  void mark_failure(const std::string &host, int port) {
    auto node = get_node(host, port);
    if (!node)
      return;

    node->consecutive_failures.fetch_add(1);
    const int job_failures = node->consecutive_job_failures.fetch_add(1) + 1;
    node->failed_jobs++;
    // FIX[C-5]: Use thread-safe accessor instead of direct field write.
    node->set_response_time_ms(10000.0); // Mark as slow

    if (job_failures >= DEGRADED_JOB_FAILURES) {
      // BUG: workers with repeated job failures kept receiving full traffic and
      // FIX: the circuit breaker marks them DEGRADED so schedulers halve load.
      node->status.store(WorkerHealthStatus::DEGRADED);
      node->is_healthy.store(false);
    }
  }

  void mark_heartbeat_missed(const std::string &host, int port) {
    auto node = get_node(host, port);
    if (!node)
      return;
    const int missed = node->consecutive_missed_heartbeats.fetch_add(1) + 1;
    if (missed >= MAX_CONSECUTIVE_FAILURES &&
        node->status.load() != WorkerHealthStatus::UNREACHABLE) {
      // BUG: dead workers stayed in the active pool after heartbeat loss.
      // FIX: three missed heartbeats immediately marks the worker unreachable.
      node->is_healthy.store(false);
      node->status.store(WorkerHealthStatus::UNREACHABLE);
      std::cerr << "WORKER " << node->identifier()
                << " marked unreachable after 3 missed heartbeats\n";
    }
  }

  void mark_reconnect_success(const std::string &host, int port,
                              double response_time = 0.0) {
    // BUG: unreachable workers had no path back into service after recovery.
    // FIX: a successful reconnect resets heartbeat/failure state and restores
    // scheduling eligibility.
    mark_success(host, port, response_time);
  }

  std::vector<std::shared_ptr<WorkerNode>> get_healthy_nodes() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    std::vector<std::shared_ptr<WorkerNode>> healthy;
    for (const auto &[id, node] : nodes_) {
      if (node->is_schedulable() && node->is_responding()) {
        healthy.push_back(node);
      }
    }
    // Sort by health score (descending)
    std::sort(healthy.begin(), healthy.end(), [](const auto &a, const auto &b) {
      return a->health_score() > b->health_score();
    });
    return healthy;
  }

  std::vector<std::shared_ptr<WorkerNode>> get_all_nodes() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    std::vector<std::shared_ptr<WorkerNode>> all;
    for (const auto &[id, node] : nodes_) {
      all.push_back(node);
    }
    return all;
  }

  void start_health_check() {
    if (running_.exchange(true))
      return;

    health_check_thread_ = std::make_unique<std::thread>([this]() {
      while (running_.load()) {
        check_node_health();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
      }
    });
  }

  void stop() {
    running_.store(false);
    if (health_check_thread_ && health_check_thread_->joinable()) {
      health_check_thread_->join();
    }
  }

  // BUG-24 FIX: Lock nodes_mutex_ before accessing nodes_.size() — concurrent
  // add_node/remove_node/check_node_health calls mutate the map from the
  // health-check thread, creating a data race without the lock.
  int get_cluster_size() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return static_cast<int>(nodes_.size());
  }

  int get_healthy_node_count() const {
    return static_cast<int>(get_healthy_nodes().size());
  }

  // BUG-24 FIX: Both nodes_.empty() and nodes_.size() are now read under the
  // lock via get_cluster_size() to avoid the data race described above.
  double get_cluster_health_percentage() const {
    const int total = get_cluster_size();
    if (total == 0)
      return 0.0;
    return static_cast<double>(get_healthy_node_count()) / total * 100.0;
  }

  std::string get_cluster_status_report() const {
    std::string report;
    report += "=== Cluster Health Report ===\n";
    report += "Total Nodes: " + std::to_string(get_cluster_size()) + "\n";
    report +=
        "Healthy Nodes: " + std::to_string(get_healthy_node_count()) + "\n";
    report +=
        "Cluster Health: " + std::to_string(get_cluster_health_percentage()) +
        "%\n";
    report += "\nNode Details:\n";

    for (const auto &node : get_all_nodes()) {
      report += "  " + node->identifier() + ":\n";
      report += "    Status: " + status_name(node->status.load()) + "\n";
      report += "    Score: " + std::to_string(node->health_score()) + "\n";
      report +=
          "    Successes: " + std::to_string(node->successful_jobs.load()) +
          "\n";
      report +=
          "    Failures: " + std::to_string(node->failed_jobs.load()) + "\n";
      report += "    Response Time: " +
                std::to_string(node->get_response_time_ms()) + "ms\n";
    }

    return report;
  }

  // Retry mechanism with exponential backoff
  template <typename F>
  bool execute_with_retry(const std::string &host, int port, F &&func,
                          int max_retries = 3) {
    auto node = get_node(host, port);
    if (!node) {
      std::cerr << "Node not found: " << host << ":" << port << "\n";
      return false;
    }

    int retry_count = 0;
    while (retry_count < max_retries) {
      try {
        auto start = std::chrono::steady_clock::now();
        if (!node->is_schedulable()) {
          std::cerr << "Skipping unschedulable node: " << host << ":" << port
                    << "\n";
          return false;
        }
        bool success = func();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();

        if (success) {
          mark_success(host, port, duration);
          return true;
        } else {
          mark_failure(host, port);
          retry_count++;

          // Exponential backoff
          int backoff_ms = 100 * (1 << retry_count); // 200ms, 400ms, 800ms
          std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        }
      } catch (const std::exception &e) {
        mark_failure(host, port);
        retry_count++;
        std::cerr << "Retry attempt " << retry_count << " failed: " << e.what()
                  << "\n";
      }
    }

    return false;
  }

private:
  std::map<std::string, std::shared_ptr<WorkerNode>> nodes_;
  mutable std::mutex nodes_mutex_; // BUG-7 FIX: protects nodes_ from data races
  std::unique_ptr<std::thread> health_check_thread_;
  std::atomic<bool> running_;

  static std::string status_name(WorkerHealthStatus status) {
    switch (status) {
    case WorkerHealthStatus::HEALTHY:
      return "HEALTHY";
    case WorkerHealthStatus::DEGRADED:
      return "DEGRADED";
    case WorkerHealthStatus::UNREACHABLE:
      return "UNREACHABLE";
    }
    return "UNKNOWN";
  }

  std::shared_ptr<WorkerNode> get_node(const std::string &host, int port) {
    std::string id = host + ":" + std::to_string(port);
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    auto it = nodes_.find(id);
    return it != nodes_.end() ? it->second : nullptr;
  }

  void check_node_health() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> dead_nodes;

    std::lock_guard<std::mutex> lock(nodes_mutex_); // BUG-7 FIX
    for (auto &[id, node] : nodes_) {
      // Check if node has timed out
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         now - node->get_last_heartbeat())
                         .count();

      if (elapsed > 30 && node->status.load() != WorkerHealthStatus::UNREACHABLE) {
        const int missed =
            node->consecutive_missed_heartbeats.fetch_add(1) + 1;
        if (missed >= MAX_CONSECUTIVE_FAILURES) {
          // BUG: timed-out heartbeat checks only removed old unhealthy nodes,
          // leaving new dead workers schedulable during the failure window.
          // FIX: heartbeat expiry itself marks workers UNREACHABLE.
          node->is_healthy.store(false);
          node->status.store(WorkerHealthStatus::UNREACHABLE);
          std::cerr << "WORKER " << id
                    << " marked unreachable after 3 missed heartbeats\n";
        }
      }

      // Reset consecutive failures for healthy nodes that haven't failed
      // recently
      if (node->status.load() == WorkerHealthStatus::HEALTHY && elapsed > 10) {
        node->consecutive_failures.store(0);
      }
    }

    // Remove dead nodes
    for (const auto &id : dead_nodes) {
      nodes_.erase(id);
      std::cout << "Removed dead node: " << id << "\n";
    }
  }
};

} // namespace tsh
