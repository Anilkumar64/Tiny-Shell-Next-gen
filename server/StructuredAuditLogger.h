#pragma once
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace tsh {
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class StructuredAuditLogger {
  std::mutex log_mutex;
  std::string log_file;
  LogLevel min_level = LogLevel::INFO;
  bool mirror_to_stderr = false;
  std::uintmax_t max_size_bytes = 100ULL * 1024ULL * 1024ULL;
  std::uint64_t bytes_written = 0;

  static std::string escape_json(const std::string &value) {
    // BUG-19 FIX: \r was not escaped at all (breaks JSON parsers that treat
    // bare CR as a line ending).  Rewrote with clear if/else chain so each
    // case is unambiguous and \r now maps to \\r.
    std::string out;
    for (char c : value) {
      if (c == '\r') {
        out += "\\r";
      } else if (c == '\n') {
        out += "\\n";
      } else if (c == '"' || c == '\\') {
        out += '\\';
        out += c;
      } else {
        out += c;
      }
    }
    return out;
  }

  static LogLevel parse_level(const char *raw) {
    if (!raw)
      return LogLevel::INFO;
    const std::string level(raw);
    if (level == "DEBUG")
      return LogLevel::DEBUG;
    if (level == "WARN")
      return LogLevel::WARN;
    if (level == "ERROR")
      return LogLevel::ERROR;
    return LogLevel::INFO;
  }

public:
  explicit StructuredAuditLogger(std::string file_path = "tsh_audit.log")
      : log_file(std::move(file_path)),
        min_level(parse_level(std::getenv("TSH_LOG_LEVEL"))),
        mirror_to_stderr(std::getenv("TSH_LOG_STDERR") != nullptr) {
    if (const char *raw = std::getenv("TSH_AUDIT_MAX_SIZE_BYTES")) {
      try {
        max_size_bytes = std::stoull(raw);
      } catch (...) {
      }
    }
  }

  void log_event(const std::string &user, const std::string &action,
                 const std::string &details, const std::string &status) {
    log_event(LogLevel::INFO, user, action, details, status);
  }

  void log_event(LogLevel level, const std::string &user,
                 const std::string &action, const std::string &details,
                 const std::string &status) {
    if (static_cast<int>(level) < static_cast<int>(min_level))
      return;
    std::lock_guard<std::mutex> lock(log_mutex);
    if ((level == LogLevel::ERROR && status == "SUCCESS") ||
        (level == LogLevel::INFO && status == "DENIED")) {
      // BUG: contradictory severity/outcome pairs corrupted audit meaning.
      // FIX: impossible combinations abort before being persisted.
      std::abort();
    }

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");

    const char *level_text = level == LogLevel::DEBUG   ? "DEBUG"
                             : level == LogLevel::WARN  ? "WARN"
                             : level == LogLevel::ERROR ? "ERROR"
                                                        : "INFO";
    // FIX[OBS-4]: Emit structured JSON to file for log collectors.
    std::string json = "{";
    json += "\"timestamp\":\"" + ss.str() + "\",";
    json += "\"level\":\"" + std::string(level_text) + "\",";
    json += "\"user\":\"" + escape_json(user) + "\",";
    json += "\"action\":\"" + escape_json(action) + "\",";
    json += "\"details\":\"" + escape_json(details) + "\",";
    json += "\"status\":\"" + escape_json(status) + "\"";
    json += "}\n";
    if (mirror_to_stderr) {
      std::cerr << json;
    }

    rotate_if_needed(log_file);
    std::ofstream file(log_file, std::ios::app);
    if (file.is_open()) {
      file << json;
      bytes_written += json.size();
    } else {
      const char *home = std::getenv("HOME");
      if (!home) {
        std::cerr << "[Audit Error] Could not write audit log.\n";
        return;
      }
      const auto fallback_dir = std::filesystem::path(home) / ".tsh";
      std::filesystem::create_directories(fallback_dir);
      std::ofstream fallback(fallback_dir / log_file, std::ios::app);
      if (fallback.is_open()) {
        rotate_if_needed((fallback_dir / log_file).string());
        fallback << json;
        bytes_written += json.size();
      } else {
        std::cerr << "[Audit Error] Could not write audit log.\n";
      }
    }
  }

  std::uint64_t audit_log_bytes_written() const { return bytes_written; }

private:
  void rotate_if_needed(const std::string &path) const {
    if (!std::filesystem::exists(path) ||
        std::filesystem::file_size(path) < max_size_bytes) {
      return;
    }
    // BUG: audit logs grew forever and could exhaust disk.
    // FIX: rotate to .1, keep five files, and delete older generations.
    std::error_code ignored;
    std::filesystem::remove(path + ".5", ignored);
    for (int i = 4; i >= 1; --i) {
      const auto from = path + "." + std::to_string(i);
      const auto to = path + "." + std::to_string(i + 1);
      if (std::filesystem::exists(from)) {
        std::filesystem::rename(from, to, ignored);
      }
    }
    std::filesystem::rename(path, path + ".1", ignored);
  }
};
} // namespace tsh
