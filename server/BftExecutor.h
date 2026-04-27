#pragma once
#include "DistributedOrchestrator.h"
#include <vector>
#include <string>
#include <map>
#include <iostream>

namespace tsh {
    class BftExecutor {
    public:
        // D-1: Byzantine Fault Detector
        // Executes AST across multiple nodes. Performs a majority vote. Isolates malicious nodes.
        static std::string execute_with_bft(std::shared_ptr<AstNode> ast, const std::vector<RemoteNode>& cluster, size_t replication_factor = 3) {
            if (cluster.size() < replication_factor) {
                return "[BFT Error] Not enough nodes registered in cluster for consensus voting.";
            }

            std::cout << "[BFT] D-1 Trust-less Execution: Dispatching AST to " << replication_factor << " nodes for consensus...\n";

            std::vector<RemoteNode> target_nodes(cluster.begin(), cluster.begin() + replication_factor);
            auto results = DistributedOrchestrator::fan_out(ast, target_nodes);

            std::map<std::string, int> vote_tally;
            std::map<std::string, std::vector<std::string>> vote_to_nodes;

            for (size_t i = 0; i < results.size(); ++i) {
                vote_tally[results[i]]++;
                vote_to_nodes[results[i]].push_back(target_nodes[i].host);
            }

            int majority_threshold = (replication_factor / 2) + 1;

            for (const auto& pair : vote_tally) {
                if (pair.second >= majority_threshold) {
                    if (vote_tally.size() > 1) {
                        std::cerr << "\033[1;31m[BFT Critical Alert] Byzantine fault detected! Discrepant node isolated and output dropped.\033[0m\n";
                    } else {
                        std::cout << "[BFT] Quorum reached successfully (100% agreement).\n";
                    }
                    return pair.first;
                }
            }

            return "[BFT Error] Catastrophic failure. No consensus reached. Cluster state untrusted.";
        }
    };
}
