#pragma once
#include "../common/Ast.h"
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <iostream>

namespace tsh {
    class DifferentialPrivacy {
    public:
        // C-DP1: Differential Privacy Stream Noise Injector
        // Adds Laplace noise to numeric fields to provide statistical privacy guarantees.
        static void apply_noise(ProcessRecord& record, double epsilon, std::mt19937& gen) {
            if (epsilon <= 0.0) return;
            
            // Laplace distribution for differential privacy
            // Scale b = sensitivity / epsilon. Assuming sensitivity of CPU usage is 100.0
            double b = 100.0 / epsilon;
            std::exponential_distribution<double> dist(1.0 / b);
            std::uniform_real_distribution<double> sign_dist(0.0, 1.0);
            
            double noise = dist(gen);
            if (sign_dist(gen) < 0.5) noise = -noise;
            
            record.cpu_usage += noise;
            
            // Clamp to valid range
            if (record.cpu_usage < 0.0) record.cpu_usage = 0.0;
            if (record.cpu_usage > 100.0) record.cpu_usage = 100.0;
        }
    };
}
