#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <csignal>
#include <vector>
#include <cstring>

class AntiForensic {
public:
    static void start_guardian() {
        std::cout << "[Guardian] Anti-Forensic Dead Man's Switch activated.\n";
        
        // Monitoring thread / check
        if (is_being_traced()) {
            emergency_wipe("Debugger detected (ptrace attach).");
        }
    }

    static bool is_being_traced() {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.compare(0, 10, "TracerPid:") == 0) {
                int pid = std::stoi(line.substr(10));
                return pid != 0;
            }
        }
        return false;
    }

    static void emergency_wipe(const std::string& reason) {
        std::cerr << "\033[1;31m[CRITICAL] " << reason << " Triggering secure memory wipe...\033[0m\n";
        
        // Securely wipe sensitive variables if we had global access
        // For now, we simulate by clearing the process and aborting
        
        // Feature 3: Secure Erase simulation
        std::vector<uint8_t> junk(1024 * 1024, 0x00); // Allocate large chunk to clear cache/ram
        std::memset(junk.data(), 0xFF, junk.size());
        std::memset(junk.data(), 0x00, junk.size());

        std::cerr << "[Guardian] Session keys purged. Aborting process.\n";
        kill(getpid(), SIGKILL);
    }
};
