#pragma once
#include "JobScheduler.h"
#include <string>
#include <map>
#include <functional>
#include <stdexcept>
#include <mutex>

namespace tsh {
    class ConsistentHashRouter {
        std::map<size_t, RemoteNode> ring;
        std::mutex mtx;
    public:
        void add_node(const RemoteNode& node) {
            std::lock_guard<std::mutex> lock(mtx);
            size_t hash = std::hash<std::string>{}(node.host + ":" + std::to_string(node.port));
            ring[hash] = node;
            // In production, add virtual nodes for uniform distribution
        }

        RemoteNode get_route(const std::string& ast_serialization_key) {
            std::lock_guard<std::mutex> lock(mtx);
            if(ring.empty()) throw std::runtime_error("ConsistentHashRouter: No nodes available in cluster.");
            
            size_t hash = std::hash<std::string>{}(ast_serialization_key);
            auto it = ring.lower_bound(hash);
            
            if (it == ring.end()) {
                return ring.begin()->second; // Wrap around to the start of the ring
            }
            return it->second;
        }
        
        size_t size() {
            std::lock_guard<std::mutex> lock(mtx);
            return ring.size();
        }
    };
}
