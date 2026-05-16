#pragma once
#include <map>
#include <string>
#include <vector>
#include <iostream>

extern char** environ;

class StateSnapshot {
public:
    static std::map<std::string, std::string> take_snapshot() {
        std::cout << "[Time-Travel] Taking micro-snapshot of system state...\n";
        std::map<std::string, std::string> snapshot;
        
        for (char** env = environ; *env != nullptr; ++env) {
            std::string entry = *env;
            size_t pos = entry.find('=');
            if (pos != std::string::npos) {
                snapshot[entry.substr(0, pos)] = entry.substr(pos + 1);
            }
        }
        return snapshot;
    }

    static void restore(const std::map<std::string, std::string>& snapshot) {
        std::cout << "[Time-Travel] Reverting shell environment to previous snapshot (!rewind)...\n";
        clearenv();
        for (const auto& pair : snapshot) {
            setenv(pair.first.c_str(), pair.second.c_str(), 1);
        }
    }
};
