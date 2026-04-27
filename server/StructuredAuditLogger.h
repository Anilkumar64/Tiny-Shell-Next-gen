#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace tsh {
    class StructuredAuditLogger {
        std::mutex log_mutex;
        std::string log_file;
    public:
        explicit StructuredAuditLogger(std::string file_path = "tsh_audit.log") 
            : log_file(std::move(file_path)) {}

        void log_event(const std::string& user, const std::string& action, const std::string& details, const std::string& status) {
            std::lock_guard<std::mutex> lock(log_mutex);
            
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");

            // Simple JSON formatter without external dependencies (RAII via std::ofstream)
            std::string json = "{";
            json += "\"timestamp\":\"" + ss.str() + "\",";
            json += "\"user\":\"" + user + "\",";
            json += "\"action\":\"" + action + "\",";
            json += "\"details\":\"" + details + "\",";
            json += "\"status\":\"" + status + "\"";
            json += "}\n";

            std::ofstream file(log_file, std::ios::app);
            if (file.is_open()) {
                file << json;
            } else {
                std::cerr << "[Audit Error] Critical failure: Could not write to audit log.\n";
            }
        }
    };
}
