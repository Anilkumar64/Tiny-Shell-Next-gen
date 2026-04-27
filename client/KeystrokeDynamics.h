#pragma once
#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <cmath>

class KeystrokeDynamics {
public:
    void record_keypress() {
        auto now = std::chrono::steady_clock::now();
        if (last_press_time.time_since_epoch().count() != 0) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_press_time).count();
            intervals.push_back(static_cast<double>(duration));
            
            if (intervals.size() > 20) {
                validate_rhythm();
            }
        }
        last_press_time = now;
    }

    void validate_rhythm() {
        // Feature 8: Behavioral Biometrics (Typing DNA)
        // Check if the current typing rhythm (variance) deviates too much from the baseline
        double sum = std::accumulate(intervals.begin(), intervals.end(), 0.0);
        double mean = sum / intervals.size();
        
        double sq_sum = std::inner_product(intervals.begin(), intervals.end(), intervals.begin(), 0.0);
        double stdev = std::sqrt(sq_sum / intervals.size() - mean * mean);

        // Simulated baseline: we expect a standard deviation within a certain range
        // If it's too high (random/bot-like) or too low (perfectly timed script), we alert
        if (stdev < 5.0 || stdev > 500.0) {
            std::cerr << "\n\033[1;31m[SECURITY] Typing DNA mismatch detected. Locking session.\033[0m\n";
            exit(1);
        }
        
        // Keep the buffer small for continuous real-time analysis
        if (intervals.size() > 50) intervals.erase(intervals.begin());
    }

private:
    std::chrono::steady_clock::time_point last_press_time;
    std::vector<double> intervals;
};
