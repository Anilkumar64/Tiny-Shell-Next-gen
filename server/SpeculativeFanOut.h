#pragma once
#include "DistributedOrchestrator.h"
#include <future>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>

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

            std::promise<std::string> result_promise;
            std::atomic<bool> resolved{false};
            auto serialized_ast = AstSerializer::serialize(ast);

            // Spawn 2 detached C++20 coroutines (threads)
            for (int i = 0; i < 2; ++i) {
                std::thread([&, node = cluster[i], serialized_ast]() {
                    auto res = DistributedOrchestrator::fan_out(ast, {node});
                    
                    // The first node to finish atomically claims the promise
                    if (!res.empty() && !resolved.exchange(true)) {
                        std::cout << "[SpeculativeFanOut] Winner: " << node.host << "\n";
                        result_promise.set_value(res[0]);
                    }
                }).detach();
            }

            // Wait for the first node to complete
            return result_promise.get_future().get();
        }
    };
}
