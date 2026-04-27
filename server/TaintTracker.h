#pragma once
#include "../common/Ast.h"
#include <string>
#include <unordered_set>
#include <stdexcept>
#include <iostream>

namespace tsh {
    class TaintException : public std::runtime_error {
    public:
        explicit TaintException(const std::string& msg) : std::runtime_error("Taint Violation: " + msg) {}
    };

    class TaintTracker {
        // Known sources of highly sensitive data
        static inline std::unordered_set<std::string> sources = {"read_vault", "cat_secret", "get_key", "decrypt"};
        // Known sinks that transmit data outside the local context
        static inline std::unordered_set<std::string> sinks = {"curl", "send_net", "nc", "wget", "export"};

    public:
        // C-22: Cryptographic Taint Tracking
        // Statically analyzes the AST before execution to ensure no tainted data flows into a sink.
        static void enforce_data_flow(std::shared_ptr<AstNode> head) {
            bool is_tainted = false;
            auto current = head;
            
            while (current) {
                if (current->type == OpType::COMMAND || current->type == OpType::MAP) {
                    if (sources.find(current->name) != sources.end()) {
                        is_tainted = true;
                        std::cout << "[TaintTracker] Security Notice: Data stream marked as TAINTED (Secret source detected).\n";
                    }
                    
                    if (is_tainted && sinks.find(current->name) != sinks.end()) {
                        throw TaintException("Attempted to pipe TAINTED data to an untrusted network sink (" + current->name + ").");
                    }
                }
                current = current->next;
            }
        }
    };
}
