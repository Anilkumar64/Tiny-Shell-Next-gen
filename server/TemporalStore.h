#pragma once
#include <map>
#include <vector>
#include <string>
#include <chrono>

namespace tsh {
    struct HistoricalValue {
        std::string val;
        std::chrono::system_clock::time_point timestamp;
    };

    class TemporalStore {
        std::map<std::string, std::vector<HistoricalValue>> timeline;
    public:
        // B-15: Stores historical states of variables for time-travel queries.
        void update(const std::string& key, const std::string& val) {
            timeline[key].push_back({val, std::chrono::system_clock::now()});
        }

        std::string get_at(const std::string& key, std::chrono::system_clock::time_point tp) {
            if (!timeline.count(key)) return "";
            for (auto it = timeline[key].rbegin(); it != timeline[key].rend(); ++it) {
                if (it->timestamp <= tp) return it->val;
            }
            return "";
        }
    };
}
