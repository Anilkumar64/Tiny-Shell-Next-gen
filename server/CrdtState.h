#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

namespace tsh {
    class CrdtState {
        struct LwwEntry { std::string value; uint64_t timestamp; };
        std::unordered_map<std::string, LwwEntry> state;
        std::mutex mtx;
    public:
        void merge(const std::string& key, const std::string& val, uint64_t ts) {
            std::lock_guard<std::mutex> lock(mtx);
            if (state[key].timestamp < ts) state[key] = {val, ts};
        }
        std::string get(const std::string& key) {
            std::lock_guard<std::mutex> lock(mtx);
            return state[key].value;
        }
        uint64_t now() const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        void set_local(const std::string& key, const std::string& val) {
            merge(key, val, now());
        }
    };
}
