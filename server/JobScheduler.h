#pragma once
#include "DistributedOrchestrator.h"
#include "SpeculativeFanOut.h"
#include "BftExecutor.h"
#include <vector>
#include <queue>
#include <mutex>
#include <map>

namespace tsh {
    class JobScheduler {
        struct NodeState {
            RemoteNode node;
            bool busy = false;
            int jobs_completed = 0;
        };

        std::vector<NodeState> cluster_nodes;
        std::mutex scheduler_mutex;
        size_t next_node_idx = 0;

    public:
        void register_node(const std::string& host, int port) {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            cluster_nodes.push_back({{host, port}, false, 0});
        }

        // Schedules a job to the next available node using Round Robin
        std::future<std::string> schedule_job(std::shared_ptr<AstNode> ast, bool use_speculative = false) {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            if (cluster_nodes.empty()) {
                std::promise<std::string> p;
                p.set_value("[Error] No nodes registered in cluster.");
                return p.get_future();
            }

            if (use_speculative && cluster_nodes.size() >= 2) {
                // FEATURE D-4: Speculative Fan-out routing
                std::vector<RemoteNode> speculative_cluster = {cluster_nodes[0].node, cluster_nodes[1].node};
                return std::async(std::launch::async, [ast, speculative_cluster]() {
                    return SpeculativeFanOut::execute_fastest(ast, speculative_cluster);
                });
            }

            // Simple Round-Robin Selection
            auto& selected = cluster_nodes[next_node_idx];
            next_node_idx = (next_node_idx + 1) % cluster_nodes.size();

            // Return an async task for the job
            return std::async(std::launch::async, [ast, node = selected.node]() {
                auto results = DistributedOrchestrator::fan_out(ast, {node});
                return results.empty() ? "[Error] Job execution failed." : results[0];
            });
        }

        // FEATURE D-1: Trust-less execution routing
        std::future<std::string> schedule_bft_job(std::shared_ptr<AstNode> ast) {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            if (cluster_nodes.size() < 3) {
                std::promise<std::string> p;
                p.set_value("[BFT Error] Requires minimum 3 nodes for Byzantine fault tolerance.");
                return p.get_future();
            }
            std::vector<RemoteNode> bft_cluster = {cluster_nodes[0].node, cluster_nodes[1].node, cluster_nodes[2].node};
            return std::async(std::launch::async, [ast, bft_cluster]() {
                return BftExecutor::execute_with_bft(ast, bft_cluster, 3);
            });
        }

        size_t cluster_size() const { return cluster_nodes.size(); }
    };
}
