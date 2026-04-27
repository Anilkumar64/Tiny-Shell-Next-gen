#pragma once
#include <vector>
#include <cmath>
#include <array>
#include <string>

namespace tsh {
    class EntropyScore {
    public:
        // G-1: Measures Shannon Entropy to detect obfuscated data in pipelines.
        static double calculate(const std::vector<uint8_t>& data) {
            if (data.empty()) return 0.0;
            std::array<size_t, 256> counts = {0};
            for (auto b : data) counts[b]++;
            
            double entropy = 0.0;
            for (auto count : counts) {
                if (count > 0) {
                    double p = static_cast<double>(count) / data.size();
                    entropy -= p * std::log2(p);
                }
            }
            return entropy; // 0.0 (uniform) to 8.0 (completely random/encrypted)
        }
    };
}
