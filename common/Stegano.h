#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <iostream>
#include <algorithm>
#include <arpa/inet.h>

class Stegano {
public:
    static std::vector<uint8_t> hide(const std::vector<uint8_t>& carrier, const std::vector<uint8_t>& secret) {
        std::cout << "[Stealth] Hiding payload in carrier image (EOF-Tagging technique)...\n";
        std::vector<uint8_t> out = carrier;
        
        std::string marker = "TSH_STEGO_START";
        out.insert(out.end(), marker.begin(), marker.end());
        
        uint32_t size = htonl(secret.size());
        uint8_t* size_bytes = reinterpret_cast<uint8_t*>(&size);
        out.insert(out.end(), size_bytes, size_bytes + 4);
        out.insert(out.end(), secret.begin(), secret.end());
        
        return out;
    }

    static std::vector<uint8_t> extract(const std::vector<uint8_t>& data) {
        std::string marker = "TSH_STEGO_START";
        auto it = std::search(data.begin(), data.end(), marker.begin(), marker.end());
        
        if (it == data.end()) return {};

        it += marker.size();
        uint32_t size;
        std::copy(it, it + 4, reinterpret_cast<uint8_t*>(&size));
        size = ntohl(size);
        it += 4;

        std::cout << "[Stealth] Extracted " << size << " bytes from steganographic carrier.\n";
        return std::vector<uint8_t>(it, it + size);
    }
};
