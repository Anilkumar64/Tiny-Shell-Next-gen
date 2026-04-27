#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace tsh {
    class ResourceLimiter {
        std::string cgroup_path;
        bool active = false;

    public:
        explicit ResourceLimiter(const std::string& group_name = "tinyshell") {
            // Cgroups v2 path
            cgroup_path = "/sys/fs/cgroup/" + group_name;
            
            try {
                if (!std::filesystem::exists(cgroup_path)) {
                    std::filesystem::create_directory(cgroup_path);
                }
                active = true;
            } catch (...) {
                std::cerr << "[Resource] Warning: Failed to initialize cgroup. Resource limits disabled.\n";
            }
        }

        void set_limits(size_t memory_limit_bytes, int cpu_weight) {
            if (!active) return;
            
            // Set Memory Hard Limit (memory.max)
            std::ofstream mem_file(cgroup_path + "/memory.max");
            if (mem_file.is_open()) mem_file << memory_limit_bytes;

            // Set CPU Weight (cpu.weight)
            std::ofstream cpu_file(cgroup_path + "/cpu.weight");
            if (cpu_file.is_open()) cpu_file << cpu_weight;
        }

        void attach_process(pid_t pid) {
            if (!active) return;
            std::ofstream proc_file(cgroup_path + "/cgroup.procs");
            if (proc_file.is_open()) proc_file << pid;
        }
    };
}
