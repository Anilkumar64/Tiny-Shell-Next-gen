#pragma once
#include <string>
#include <chrono>
#include <iostream>

namespace tsh {
    class IntentDrift {
        std::chrono::system_clock::time_point last_dangerous_action;
        int dangerous_streak = 0;
    public:
        // J-94: Predicts fatigue-based mistakes. 
        // If a user runs many destructive commands in quick succession, triggers a high-cognitive-load warning.
        bool detect_drift(int risk_score) {
            if (risk_score < 70) return false;
            
            auto now = std::chrono::system_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - last_dangerous_action).count();
            
            if (diff < 10) {
                dangerous_streak++;
            } else {
                dangerous_streak = 1;
            }
            
            last_dangerous_action = now;
            
            if (dangerous_streak >= 3) {
                std::cerr << "\033[1;33m[Cognitive Alert] J-94 Intent-Drift detected. High-risk command streak. Pausing for confirmation.\033[0m\n";
                return true;
            }
            return false;
        }
    };
}
