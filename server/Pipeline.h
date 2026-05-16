#pragma once
#include "../common/Ast.h"
#include "../common/ProcReader.h"
#include <array>
#include <concepts>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/statvfs.h>
#include <type_traits>
#include <utmpx.h>
#include <vector>

namespace tsh {

// Helper to evaluate basic filter expressions like "cpu > 80"
// inline: defined in a header included by multiple TUs; without inline the
// linker sees a duplicate symbol from every .cpp that includes Pipeline.h.
inline bool eval_filter(const ProcessRecord &p, const std::string &expr) {
  if (expr.find("cpu >") != std::string::npos) {
    double threshold = std::stod(expr.substr(expr.find(">") + 1));
    return p.cpu_usage > threshold;
  }
  // BUG-21 FIX: Previously any unrecognised expression returned true,
  // silently turning e.g. filter(pid > 100) into a no-op that passes
  // every record. Now we reject records when the expression is unknown
  // so the user gets an empty result set rather than a misleading one.
  return false;
}

// Concept for valid pipeline data
template <typename T,
          typename = std::enable_if_t<std::is_copy_constructible_v<T> &&
                                      std::is_move_constructible_v<T>>>

class TypedStream {
  std::vector<T> data;

public:
  explicit TypedStream(std::vector<T> d) : data(std::move(d)) {}

  TypedStream<T> filter(std::function<bool(const T &)> predicate) {
    std::vector<T> result;
    for (const auto &item : data)
      if (predicate(item))
        result.push_back(item);
    return TypedStream<T>(std::move(result));
  }

  const std::vector<T> &collect() const { return data; }
};

// The Execution Engine that traverses the AST
class Executor {
public:
  static std::string execute(std::shared_ptr<AstNode> head) {
    if (!head)
      return "Empty pipeline";

    // Stage 1: Source (Command)
    std::vector<ProcessRecord> source_data;
    if (head->type == OpType::COMMAND && head->name == "ps") {
      source_data = ProcReader::read_all_processes();
    } else {
      if (!head->next) {
        return execute_simple_command(head->name);
      }
      return "Command does not support pipeline transforms: " + head->name +
             "\n";
    }

    TypedStream<ProcessRecord> stream(std::move(source_data));

    // Stage 2: Transformations (Filter, Map, Reduce)
    auto current = head->next;
    bool reduced = false;
    size_t count_val = 0;

    while (current) {
      if (current->type == OpType::FILTER) {
        stream = stream.filter([&](const ProcessRecord &p) {
          return eval_filter(p, current->name);
        });
      } else if (current->type == OpType::REDUCE) {
        if (current->name == "count") {
          count_val = stream.collect().size();
          reduced = true;
        }
      }
      current = current->next;
    }

    // Terminal Stage: Formatting output
    if (reduced) {
      return "Pipeline Result (Reduced): " + std::to_string(count_val) + "\n";
    }

    std::string result = "Pipeline Output:\n";
    size_t emitted = 0;
    constexpr size_t max_process_rows = 200;
    for (const auto &record : stream.collect()) {
      if (emitted++ >= max_process_rows) {
        result += "  ... output truncated at " +
                  std::to_string(max_process_rows) + " processes\n";
        break;
      }
      result += "  PID: " + std::to_string(record.pid) +
                ", Name: " + record.name +
                ", CPU: " + std::to_string(record.cpu_usage) + "%\n";
    }
    return result;
  }

private:
  // Allowed bare command names for the HTTP /exec path.
  // Arguments are not supported here; they come through the gRPC spine.
  static const std::set<std::string> &allowed_commands() {
    static const std::set<std::string> kAllowed = {
        "uptime", "who", "df", "ls", "du",
    };
    return kAllowed;
  }

  // Resolve a bare name to the canonical absolute binary path.
  // Returns empty string if not found.
  static std::string resolve_binary(const std::string &name) {
    for (const char *prefix : {"/usr/bin/", "/bin/"}) {
      std::string path = prefix + name;
      if (std::filesystem::exists(path))
        return path;
    }
    return {};
  }

  // FIX: execute_simple_command now runs the real host binary via popen()
  // instead of reimplementing each command in C++.  This means the output
  // the client sees is identical to what a local shell would produce.
  //
  // Security properties maintained:
  //   • Only commands in allowed_commands() can reach popen().
  //   • The absolute path is resolved from a fixed prefix list — the bare
  //     name is never passed to sh -c, so shell injection is impossible.
  //   • No user-supplied arguments are forwarded (the HTTP /exec path
  //     strips arguments; argument-bearing commands go through the signed
  //     gRPC spine instead).
  //   • Output is capped at 1 MiB to prevent runaway responses.
  static std::string execute_simple_command(const std::string &name) {
    if (!allowed_commands().count(name)) {
      return "Unknown command or unsupported source: " + name + "\n";
    }

    const std::string binary = resolve_binary(name);
    if (binary.empty()) {
      return "Command binary not found on this host: " + name + "\n";
    }

    // popen() with the absolute path — no shell metacharacters possible
    // because binary is a fixed /usr/bin/<name> string.
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(binary.c_str(), "r"),
                                                  &pclose);
    if (!pipe) {
      return "Failed to execute " + name + ": popen() failed\n";
    }

    std::string output;
    constexpr std::size_t kMaxOutput = 1024ULL * 1024ULL; // 1 MiB
    std::array<char, 4096> buf{};
    while (output.size() < kMaxOutput) {
      const std::size_t n = fread(buf.data(), 1, buf.size(), pipe.get());
      if (n == 0)
        break;
      output.append(buf.data(), n);
    }
    if (output.size() >= kMaxOutput) {
      output += "\n[output truncated at 1 MiB]\n";
    }
    return output;
  }
};
} // namespace tsh