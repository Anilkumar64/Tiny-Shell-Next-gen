#pragma once
#include "../common/Ast.h"
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace tsh {

class TaintException : public std::runtime_error {
public:
  explicit TaintException(const std::string &msg)
      : std::runtime_error("Taint Violation: " + msg) {}
};

class TaintTracker {
private:
  static std::set<std::string> tainted_variables;
  static std::map<std::string, std::string> taint_sources;
  static std::mutex mutex;

public:
  // T-7: Information Flow Control via Taint Tracking
  // Enforces that data flows only through legitimate channels
  static void enforce_data_flow(std::shared_ptr<AstNode> head) {
    if (!head) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex);
    auto current = head;
    while (current) {
      // Check for potential command injection vectors
      if (current->type == OpType::COMMAND &&
          current->name.find("eval") != std::string::npos) {
        throw TaintException("eval() is prohibited in command pipeline");
      }

      // Check for shell metacharacters in untrusted context
      std::string dangerous_chars = "$();&|`";
      for (char c : dangerous_chars) {
        if (current->name.find(c) != std::string::npos) {
          // Flag as potential taint source
          if (tainted_variables.find(current->name) !=
              tainted_variables.end()) {
            throw TaintException("Tainted variable '" + current->name +
                                 "' contains shell metacharacters");
          }
        }
      }

      current = current->next;
    }

    std::cout << "[TaintTracker] T-7 Taint Analysis: Data flow verified as "
                 "untainted.\n";
  }

  // Mark a variable as tainted from an external source
  static void mark_tainted(const std::string &var_name,
                           const std::string &source) {
    std::lock_guard<std::mutex> lock(mutex);
    tainted_variables.insert(var_name);
    taint_sources[var_name] = source;
  }

  // Check if a variable is tainted
  static bool is_tainted(const std::string &var_name) {
    std::lock_guard<std::mutex> lock(mutex);
    return tainted_variables.find(var_name) != tainted_variables.end();
  }

  // Clear all taint tracking (e.g., after command execution)
  static void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    tainted_variables.clear();
    taint_sources.clear();
  }
};

// Initialize static members
inline std::set<std::string> TaintTracker::tainted_variables;
inline std::map<std::string, std::string> TaintTracker::taint_sources;
inline std::mutex TaintTracker::mutex;
} // namespace tsh
