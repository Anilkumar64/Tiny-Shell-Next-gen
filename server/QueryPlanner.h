#pragma once
#include "../common/Ast.h"
#include <memory>
#include <iostream>

namespace tsh {
    class QueryPlanner {
    public:
        // Optimization Pass 1: Predicate Pushdown
        // Analyzes the AST DAG and moves FILTER nodes ahead of MAP nodes
        // to reduce the total number of objects processed by expensive transformations.
        static void optimize(std::shared_ptr<AstNode> head) {
            if (!head) return;
            
            auto current = head;
            while (current && current->next) {
                // Heuristic: If we see a MAP followed by a FILTER, swap them.
                // This pushes the predicate closer to the data source (COMMAND).
                if (current->type == OpType::MAP && current->next->type == OpType::FILTER) {
                    std::cout << "[QueryPlanner] IR Optimization: Predicate Pushdown applied (Swapped MAP -> FILTER to FILTER -> MAP).\n";
                    
                    // In a production DAG, we rewire edges. For this linear AST prototype, we swap node contents.
                    auto filter_node = current->next;
                    
                    OpType temp_type = current->type;
                    std::string temp_name = current->name;
                    std::vector<std::string> temp_args = current->args;

                    current->type = filter_node->type;
                    current->name = filter_node->name;
                    current->args = filter_node->args;

                    filter_node->type = temp_type;
                    filter_node->name = temp_name;
                    filter_node->args = temp_args;
                }
                current = current->next;
            }
        }
    };
}
