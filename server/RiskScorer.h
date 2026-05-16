#pragma once
#include "../common/Ast.h"
#include <string>
#include <unordered_map>

namespace tsh {
    class RiskScorer {
        static inline std::unordered_map<std::string, int> destructive_weights = {
            {"kill", 80}, {"rm", 95}, {"format", 100}, {"shutdown", 90}, {"dd", 85}, {"memfd_exec", 70}
        };
    public:
        // A-7: Quantifies the "Blast Radius" of an AST before execution.
        static int calculate_score(std::shared_ptr<AstNode> head) {
            int total_risk = 0;
            auto current = head;
            while (current) {
                if (destructive_weights.count(current->name)) {
                    total_risk += destructive_weights[current->name];
                }
                current = current->next;
            }
            return total_risk > 100 ? 100 : total_risk;
        }
    };
}
