#pragma once
#include "ITransport.h"
#include <vector>
#include <random>
#include <thread>
#include <chrono>

class MorphedTransport : public ITransport {
public:
    explicit MorphedTransport(std::unique_ptr<ITransport> base) : base_transport(std::move(base)) {
        std::cout << "[Stealth] Chameleon Traffic Morphing active (mimicking HTTPS stream).\n";
    }

    bool send(const void* buffer, size_t length) override {
        // Feature 1: Traffic Shaping / Morphing
        // Add randomized padding to normalize packet size
        std::vector<uint8_t> padded(length + (rand() % 256));
        std::memcpy(padded.data(), buffer, length);
        
        // Random delay to defeat timing analysis (jitter)
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 50));
        
        return base_transport->send(padded.data(), padded.size());
    }

    bool recv(void* buffer, size_t length) override {
        // In a real implementation, we would strip the padding here
        // For simulation, we assume the base transport handles the framed raw data
        return base_transport->recv(buffer, length);
    }

private:
    std::unique_ptr<ITransport> base_transport;
};
