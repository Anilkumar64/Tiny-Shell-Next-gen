#pragma once
#include "BftExecutor.h"
#include "ClusterHealthManager.h"
#include "DistributedOrchestrator.h"
#include "SpeculativeFanOut.h"
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace tsh {
class JobScheduler {
  struct NodeState {
    RemoteNode node;
    bool busy = false;
    int jobs_completed = 0;
    WorkerHealthStatus status = WorkerHealthStatus::HEALTHY;
    int consecutive_failures = 0;
    bool degraded_skip_next = false;
  };

  static constexpr size_t SHARD_COUNT = 16;
  std::array<std::vector<NodeState>, SHARD_COUNT> sharded_nodes;
  mutable std::array<std::mutex, SHARD_COUNT> shard_mutexes;
  std::array<size_t, SHARD_COUNT> next_node_idx{};

  size_t shard_for(const std::string &job_id) const {
    return std::hash<std::string>{}(job_id) % SHARD_COUNT;
  }

public:
  void register_node(const std::string &host, int port) {
    const size_t shard = shard_for(host + ":" + std::to_string(port));
    std::lock_guard<std::mutex> lock(shard_mutexes[shard]);
    sharded_nodes[shard].push_back(
        {{host, port, host + ":" + std::to_string(port)}, false, 0});
    // FIX: also register into health_manager_ so the background heartbeat
    // thread actually polls this node.
    health_manager_.add_node(host, port);
  }

  void mark_node_healthy(const std::string &id) {
    update_node(id, [](NodeState &node) {
      node.status = WorkerHealthStatus::HEALTHY;
      node.consecutive_failures = 0;
      node.degraded_skip_next = false;
    });
  }

  void mark_node_unreachable(const std::string &id) {
    update_node(id, [](NodeState &node) {
      node.status = WorkerHealthStatus::UNREACHABLE;
      node.busy = false;
    });
  }

  void mark_node_failure(const std::string &id) {
    update_node(id, [](NodeState &node) {
      ++node.consecutive_failures;
      if (node.consecutive_failures >= 5) {
        node.status = WorkerHealthStatus::DEGRADED;
      }
    });
  }

  bool is_node_healthy(const std::string &id) const {
    for (std::size_t s = 0; s < SHARD_COUNT; ++s) {
      for (const auto &node : sharded_nodes[s]) {
        if (worker_node_id(node.node) == id)
          return node.status != WorkerHealthStatus::UNREACHABLE;
      }
    }
    return false; // unknown node — treat as unhealthy
  }

  // FIX: expose start/stop so Server::run() can drive the health-check thread.
  void start_health_check() { health_manager_.start_health_check(); }
  void stop_health_check() { health_manager_.stop(); }

  std::future<std::string> schedule_job(std::shared_ptr<AstNode> ast,
                                        bool use_speculative = false) {
    // Shard by time-based random ID to avoid command-name concentration
    // (BUG-22).
    const size_t shard =
        shard_for(std::to_string(std::hash<uint64_t>{}(static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()))));
    std::lock_guard<std::mutex> lock(shard_mutexes[shard]);
    auto &cluster_nodes = sharded_nodes[shard];

    if (cluster_nodes.empty()) {
      std::promise<std::string> p;
      p.set_value("[Error] No nodes registered in cluster.");
      return p.get_future();
    }

    if (use_speculative && cluster_nodes.size() >= 2) {
      std::vector<RemoteNode> speculative_cluster = {cluster_nodes[0].node,
                                                     cluster_nodes[1].node};
      std::promise<std::string> p;
      p.set_value(SpeculativeFanOut::execute_fastest(ast, speculative_cluster));
      return p.get_future();
    }

    const auto selected_index = select_schedulable_index(cluster_nodes, shard);
    if (!selected_index.has_value()) {
      std::promise<std::string> p;
      p.set_value("[Error] No healthy workers available in cluster.");
      return p.get_future();
    }

    auto &selected = cluster_nodes[*selected_index];
    selected.busy = true;

    std::promise<std::string> p;
    auto results = DistributedOrchestrator::fan_out(ast, {selected.node});
    selected.busy = false;
    const std::string result =
        results.empty() ? "[Error] Job execution failed." : results[0];
    if (result.rfind("[Error]", 0) == 0) {
      ++selected.consecutive_failures;
      if (selected.consecutive_failures >= 5) {
        selected.status = WorkerHealthStatus::DEGRADED;
      }
    } else {
      selected.status = WorkerHealthStatus::HEALTHY;
      selected.consecutive_failures = 0;
      selected.degraded_skip_next = false;
      ++selected.jobs_completed;
    }
    p.set_value(result);
    return p.get_future();
  }

  std::future<std::string> schedule_bft_job(std::shared_ptr<AstNode> ast) {
    // Shard by time-based random ID to avoid command-name concentration
    // (BUG-27).
    const size_t shard =
        shard_for(std::to_string(std::hash<uint64_t>{}(static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()))));
    std::lock_guard<std::mutex> lock(shard_mutexes[shard]);
    auto &cluster_nodes = sharded_nodes[shard];

    if (cluster_nodes.size() < 3) {
      std::promise<std::string> p;
      p.set_value("[BFT Error] Requires minimum 3 nodes for Byzantine fault "
                  "tolerance.");
      return p.get_future();
    }
    std::vector<RemoteNode> bft_cluster = {
        cluster_nodes[0].node, cluster_nodes[1].node, cluster_nodes[2].node};
    std::promise<std::string> p;
    p.set_value(BftExecutor::execute_with_bft(ast, bft_cluster, 3));
    return p.get_future();
  }

  size_t cluster_size() const {
    size_t total = 0;
    for (size_t i = 0; i < SHARD_COUNT; ++i) {
      std::lock_guard<std::mutex> lock(shard_mutexes[i]);
      total += sharded_nodes[i].size();
    }
    return total;
  }

  size_t schedulable_cluster_size() const {
    size_t total = 0;
    for (size_t i = 0; i < SHARD_COUNT; ++i) {
      std::lock_guard<std::mutex> lock(shard_mutexes[i]);
      for (const auto &node : sharded_nodes[i]) {
        if (node.status != WorkerHealthStatus::UNREACHABLE) {
          ++total;
        }
      }
    }
    return total;
  }

private:
  // FIX: declared here so start_health_check()/stop_health_check() and
  // register_node() all operate on the same instance.
  ClusterHealthManager health_manager_;

  template <typename F> void update_node(const std::string &id, F &&fn) {
    for (size_t i = 0; i < SHARD_COUNT; ++i) {
      std::lock_guard<std::mutex> lock(shard_mutexes[i]);
      for (auto &node : sharded_nodes[i]) {
        if (worker_node_id(node.node) == id) {
          fn(node);
          return;
        }
      }
    }
  }

  std::optional<std::size_t>
  select_schedulable_index(std::vector<NodeState> &nodes, std::size_t shard) {
    for (std::size_t attempts = 0; attempts < nodes.size(); ++attempts) {
      auto index = next_node_idx[shard] % nodes.size();
      next_node_idx[shard] = (next_node_idx[shard] + 1) % nodes.size();
      auto &candidate = nodes[index];
      if (candidate.status == WorkerHealthStatus::UNREACHABLE) {
        continue;
      }
      if (candidate.status == WorkerHealthStatus::DEGRADED) {
        candidate.degraded_skip_next = !candidate.degraded_skip_next;
        if (candidate.degraded_skip_next) {
          continue;
        }
      }
      return index;
    }
    return std::nullopt;
  }
};
} // namespace tsh