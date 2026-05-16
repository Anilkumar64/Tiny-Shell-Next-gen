#pragma once
#include <string>
#include <unordered_set>
#include <stdexcept>
#include <iostream>

namespace tsh {
    class CapabilityException : public std::runtime_error {
    public:
        explicit CapabilityException(const std::string& cap) 
            : std::runtime_error("TinyShell Permission Denied: " + cap) {}
    };

    class CapabilityManager {
    private:
        std::unordered_set<std::string> granted_caps;
    public:
        void grant(const std::string& cap) {
            std::cout << "[Auth] Capability Granted: " << cap << "\n";
            granted_caps.insert(cap);
        }

        void require(const std::string& cap) const {
            if (granted_caps.find(cap) == granted_caps.end()) {
                throw CapabilityException(cap);
            }
        }
    };
}
