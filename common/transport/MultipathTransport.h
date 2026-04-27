#pragma once
#include "ITransport.h"
#include <vector>
#include <memory>

class MultipathTransport : public ITransport {
public:
    void add_transport(std::unique_ptr<ITransport> t) {
        transports.push_back(std::move(t));
    }

    bool send(const void* buffer, size_t length) override {
        if (transports.empty()) return false;
        
        std::cout << "[Transport] Fragmenting payload across " << transports.size() << " paths (Quantum-Jump).\n";
        
        // Feature 10: Multi-path fragmentation
        size_t frag_size = length / transports.size();
        const uint8_t* ptr = static_cast<const uint8_t*>(buffer);

        for (size_t i = 0; i < transports.size(); ++i) {
            size_t cur_size = (i == transports.size() - 1) ? (length - i * frag_size) : frag_size;
            if (!transports[i]->send(ptr + i * frag_size, cur_size)) {
                return false;
            }
        }
        return true;
    }

    bool recv(void* buffer, size_t length) override {
        // Reassembly logic
        if (transports.empty()) return false;
        size_t frag_size = length / transports.size();
        uint8_t* ptr = static_cast<uint8_t*>(buffer);

        for (size_t i = 0; i < transports.size(); ++i) {
            size_t cur_size = (i == transports.size() - 1) ? (length - i * frag_size) : frag_size;
            if (!transports[i]->recv(ptr + i * frag_size, cur_size)) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<std::unique_ptr<ITransport>> transports;
};
