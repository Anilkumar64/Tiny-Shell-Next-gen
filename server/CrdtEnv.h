#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <iostream>
#include <algorithm>

namespace tsh {
    // D-CRDT: Conflict-Free Replicated Data Type for Env Variables
    // Uses Last-Write-Wins (LWW) Register semantics for eventually consistent global state
    class CrdtEnv {
        struct LWWRegister {
            std::string value;
            uint64_t timestamp_ms;
            std::string node_id;
        };

        std::mutex mtx;
        std::unordered_map<std::string, LWWRegister> state;
        std::string local_node_id;

        uint64_t now_ms() const {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        }

    public:
        explicit CrdtEnv(std::string node_id) : local_node_id(std::move(node_id)) {}

        // Sets a value locally and updates the CRDT timestamp
        void set(const std::string& key, const std::string& value) {
            std::lock_guard<std::mutex> lock(mtx);
            state[key] = {value, now_ms(), local_node_id};
            std::cout << "[CRDT] D-CRDT Write: " << key << "=" << value << " (Time: " << state[key].timestamp_ms << ")\n";
        }

        // Resolves conflicts when receiving state from another node
        void merge(const std::string& key, const std::string& remote_value, uint64_t remote_ts, const std::string& remote_node_id) {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = state.find(key);
            
            bool update = false;
            if (it == state.end()) {
                update = true;
            } else if (remote_ts > it->second.timestamp_ms) {
                update = true;
            } else if (remote_ts == it->second.timestamp_ms && remote_node_id > it->second.node_id) {
                // Tie-breaker: Lexicographical order of node IDs
                update = true;
            }

            if (update) {
                state[key] = {remote_value, remote_ts, remote_node_id};
                std::cout << "[CRDT] Merged Remote State: " << key << "=" << remote_value << "\n";
            }
        }

        std::string get(const std::string& key) {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = state.find(key);
            return (it != state.end()) ? it->second.value : "";
        }
    };
}
