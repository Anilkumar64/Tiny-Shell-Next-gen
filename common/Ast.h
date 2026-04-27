#pragma once
#include <string>
#include <vector>
#include <variant>
#include <map>
#include <memory>
#include <stdexcept>
#include <cstdint>

namespace tsh {

    // 1. Typed Data Structures (replacing text pipes)
    using Value = std::variant<
        std::monostate,
        int64_t,
        double,
        bool,
        std::string,
        std::vector<std::string>
    >;

    struct ProcessRecord {
        int64_t pid;
        std::string name;
        double cpu_usage;
    };

    struct FileRecord {
        std::string path;
        int64_t size;
    };

    // 2. Immutable Execution Graph (AST) Nodes
    enum class OpType {
        COMMAND,
        FILTER,
        MAP,
        REDUCE
    };

    struct AstNode {
        OpType type;
        std::string name; // "ps", "ls", "cpu > 80"
        std::vector<std::string> args;
        std::shared_ptr<AstNode> next; // Next stage in pipeline
        
        AstNode(OpType t, std::string n) : type(t), name(std::move(n)) {}
    };

}
