#pragma once
#include "Ast.h"
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace tsh {

// Parser converts a command string into a linked list of AstNode stages.
//
// Supported syntax
// ================
//   <command>                       — simple command (ps, df, who, uptime …)
//   <command> | filter(<expr>)      — pipeline with filter stage
//   <command> | filter(<expr>) | count   — pipeline ending in reduce
//
// The parser applies a strict default-deny allowlist for source commands.
// Unknown commands return nullptr so the caller can emit an appropriate error
// without ever touching the execution engine.

class Parser {
public:
  // Parse a pipeline string. Throws if the input is empty or any stage is
  // outside the allowlist.
  static std::shared_ptr<AstNode> parse_pipeline(const std::string &input) {
    static constexpr std::size_t kMaxPipelineBytes = 4096;
    static constexpr std::size_t kMaxStages = 16;
    const std::string trimmed = trim(input);
    if (trimmed.empty()) {
      throw std::runtime_error("empty pipeline");
    }
    if (trimmed.size() > kMaxPipelineBytes) {
      throw std::runtime_error("pipeline exceeds maximum length");
    }

    // Split on '|'
    const auto stages = split_pipe(trimmed);
    if (stages.empty()) {
      throw std::runtime_error("empty pipeline");
    }
    if (stages.size() > kMaxStages) {
      throw std::runtime_error("pipeline has too many stages");
    }

    // --- Source stage ---
    const std::string src_cmd = trim(stages[0]);
    if (!is_allowed_command(src_cmd)) {
      throw std::runtime_error("command is outside parser allowlist: " +
                               source_root(src_cmd));
    }

    auto head = std::make_shared<AstNode>(OpType::COMMAND, src_cmd);
    auto tail = head;

    // --- Subsequent stages ---
    for (std::size_t i = 1; i < stages.size(); ++i) {
      const std::string stage = trim(stages[i]);
      auto node = parse_stage(stage);
      if (!node) {
        throw std::runtime_error("unsupported pipeline stage: " + stage);
      }
      tail->next = node;
      tail = node;
    }

    return head;
  }

private:
  static const std::unordered_set<std::string> &allowed_commands() {
    static const std::unordered_set<std::string> s{
        "ps", "ls", "df", "du", "who", "uptime", "top", "netstat"};
    return s;
  }

  static bool is_allowed_command(const std::string &cmd) {
    return allowed_commands().count(source_root(cmd)) > 0 &&
           cmd == source_root(cmd);
  }

  static std::string source_root(const std::string &cmd) {
    std::istringstream in(cmd);
    std::string root;
    in >> root;
    return root;
  }

  // Parse a single non-source pipeline stage token.
  static std::shared_ptr<AstNode> parse_stage(const std::string &token) {
    if (token.empty())
      return nullptr;

    // filter(<expr>)
    if (token.rfind("filter(", 0) == 0 && token.back() == ')') {
      const std::string expr = token.substr(7, token.size() - 8);
      return std::make_shared<AstNode>(OpType::FILTER, trim(expr));
    }
    // count / count() / reduce
    if (token == "count" || token == "count()") {
      return std::make_shared<AstNode>(OpType::REDUCE, "count");
    }
    // sort <field> [asc|desc]
    if (token.rfind("sort", 0) == 0) {
      return std::make_shared<AstNode>(OpType::SORT, trim(token.substr(4)));
    }
    // limit <n>
    if (token.rfind("limit", 0) == 0) {
      return std::make_shared<AstNode>(OpType::LIMIT, trim(token.substr(5)));
    }
    // map(<expr>) / select <fields>
    if (token.rfind("map(", 0) == 0 || token.rfind("select", 0) == 0) {
      return std::make_shared<AstNode>(OpType::MAP, token);
    }

    return nullptr; // unrecognised — caller skips
  }

  // Split on '|' but not inside parentheses.
  static std::vector<std::string> split_pipe(const std::string &s) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : s) {
      if (c == '(')
        ++depth;
      else if (c == ')') {
        --depth;
        if (depth < 0) {
          throw std::runtime_error("unbalanced pipeline expression");
        }
      }
      if (c == '|' && depth == 0) {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (depth != 0) {
      throw std::runtime_error("unbalanced pipeline expression");
    }
    parts.push_back(cur);
    return parts;
  }

  static std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
      return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }
};

} // namespace tsh
