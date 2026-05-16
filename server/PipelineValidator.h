#pragma once
#include "../common/Ast.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <stdexcept>

namespace tsh {
    class ValidationException : public std::runtime_error {
    public:
        explicit ValidationException(const std::string& msg) : std::runtime_error("Validation Error: " + msg) {}
    };

    class PipelineValidator {
    private:
        std::unordered_set<std::string> allowed_commands = {
            // FIX[M1]: Keep validator aligned with parser's default-deny command allowlist.
            "ps", "ls", "df", "du", "netstat", "top", "who", "uptime"
        };
        
    public:
        // Statically analyzes the AST graph before any execution occurs
        void validate(const std::shared_ptr<AstNode>& head) const {
            if (!head) return;
            
            auto current = head;
            while (current) {
                if (current->type == OpType::COMMAND) {
                    if (allowed_commands.find(current->name) == allowed_commands.end()) {
                        throw ValidationException("Command '" + current->name + "' is not in the strict AST allowlist.");
                    }
                }
                current = current->next;
            }
        }
    };
}
