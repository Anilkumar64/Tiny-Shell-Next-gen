#pragma once
#include <memory>
#include <string>
#include <vector>

namespace tsh {

enum class OpType {
    COMMAND,  // Source node  — e.g. "ps", "df", "who"
    FILTER,   // Predicate    — e.g. "cpu > 80"
    MAP,      // Projection   — e.g. "select pid,name"
    REDUCE,   // Aggregation  — e.g. "count"
    SORT,     // Ordering     — e.g. "sort cpu desc"
    LIMIT,    // Row cap      — e.g. "limit 10"
};

struct AstNode {
    OpType                   type;
    std::string              name;   // command name or expression string
    std::vector<std::string> args;   // optional parsed arguments
    std::shared_ptr<AstNode> next;   // next stage in the linear pipeline

    explicit AstNode(OpType t, std::string n,
                     std::vector<std::string> a = {})
        : type(t), name(std::move(n)), args(std::move(a)) {}
};

} // namespace tsh
