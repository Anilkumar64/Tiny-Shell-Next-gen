#pragma once
#include "DistributedOrchestrator.h"
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace tsh {
    class SpeculativeFanOut {
    public:
        // D-4: Trades bandwidth for massive latency reduction. 
        // Sends identical ASTs to 2 nodes simultaneously. The orchestrator returns the first response.
        static std::string execute_fastest(std::shared_ptr<AstNode> ast, const std::vector<RemoteNode>& cluster) {
            if (cluster.size() < 2) {
                // Fallback to normal execution if the cluster is too small
                auto res = DistributedOrchestrator::fan_out(ast, cluster);
                return res.empty() ? "[Error] Speculative Node failed." : res[0];
            }

            std::cout << "[SpeculativeFanOut] D-4 Racing Nodes: " << cluster[0].host << " vs " << cluster[1].host << "\n";

            std::atomic<bool> resolved{false};
            std::string result = "[Error] Speculative Node failed.";
            std::mutex result_mutex;

            // FIX[SCALE-3]: Join speculative threads; do not leave detached work behind.
            std::vector<std::thread> workers;
            for (int i = 0; i < 2; ++i) {
                workers.emplace_back([&, node = cluster[i]]() {
                    auto res = DistributedOrchestrator::fan_out(ast, {node});
                    
                    // The first node to finish atomically claims the promise
                    if (!res.empty() && !resolved.exchange(true)) {
                        std::lock_guard<std::mutex> lock(result_mutex);
                        std::cout << "[SpeculativeFanOut] Winner: " << node.host << "\n";
                        result = res[0];
                    }
                });
            }

            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
            return result;
        }
    };
}
