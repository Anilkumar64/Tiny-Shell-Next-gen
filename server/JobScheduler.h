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
    // FIX[SCALE-2]: Partition scheduler state instead of one global mutex.
    return std::hash<std::string>{}(job_id) % SHARD_COUNT;
  }

public:
  void register_node(const std::string &host, int port) {
    const size_t shard = shard_for(host + ":" + std::to_string(port));
    std::lock_guard<std::mutex> lock(shard_mutexes[shard]);
    sharded_nodes[shard].push_back(
        {{host, port, host + ":" + std::to_string(port)}, false, 0});
  }

  void mark_node_healthy(const std::string &id) {
    // BUG: recovered workers had no scheduler-state transition back to active.
    // FIX: successful reconnect explicitly restores HEALTHY scheduling state.
    update_node(id, [](NodeState &node) {
      node.status = WorkerHealthStatus::HEALTHY;
      node.consecutive_failures = 0;
      node.degraded_skip_next = false;
    });
  }

  void mark_node_unreachable(const std::string &id) {
    // BUG: unreachable workers remained in the round-robin scheduling pool.
    // FIX: UNREACHABLE nodes are retained for reconnect but skipped for jobs.
    update_node(id, [](NodeState &node) {
      node.status = WorkerHealthStatus::UNREACHABLE;
      node.busy = false;
    });
  }

  void mark_node_failure(const std::string &id) {
    // BUG: repeated worker failures did not reduce future allocation.
    // FIX: five consecutive job failures trip a DEGRADED circuit breaker.
    update_node(id, [](NodeState &node) {
      ++node.consecutive_failures;
      if (node.consecutive_failures >= 5) {
        node.status = WorkerHealthStatus::DEGRADED;
      }
    });
  }

  // Schedules a job to the next available node using Round Robin
  std::future<std::string> schedule_job(std::shared_ptr<AstNode> ast,
                                        bool use_speculative = false) {
    // BUG-22 FIX: Previously shard_for(ast->name) was used, meaning every
    // pipeline that starts with "ps" hashes to the same shard — defeating
    // sharding for the most common command and serialising all ps jobs through
    // one mutex.  We now shard by a random 64-bit job ID so load is spread
    // evenly across shards regardless of the command name.
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
      // FEATURE D-4: Speculative Fan-out routing
      std::vector<RemoteNode> speculative_cluster = {cluster_nodes[0].node,
                                                     cluster_nodes[1].node};
      std::promise<std::string> p;
      // FIX[SCALE-3]: Keep scheduler bounded; speculative fan-out remains
      // opt-in but not auto-threaded here.
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

    // Return an async task for the job
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

  // FEATURE D-1: Trust-less execution routing
  std::future<std::string> schedule_bft_job(std::shared_ptr<AstNode> ast) {
    // BUG-27 FIX: Previously shard_for(ast->name) caused all BFT jobs for
    // the same command (e.g. "ps") to hash to the same shard — the same
    // concentration bug fixed in schedule_job by BUG-22.  Use a time-based
    // random shard ID instead.
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
    // FIX[SCALE-3]: Avoid unbounded background task creation in scheduler APIs.
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
    // BUG: round-robin selected dead workers without checking live health.
    // FIX: selection scans for HEALTHY/DEGRADED nodes and skips unreachable.
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
