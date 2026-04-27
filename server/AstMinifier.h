#pragma once
#include "../common/Ast.h"
#include <memory>
#include <iostream>

namespace tsh {
    class AstMinifier {
    public:
        // H-75: Compiler-style optimization. Removes redundant AST nodes.
        // E.g., `ps | filter(cpu>0) | filter(cpu>50)` -> `ps | filter(cpu>50)`
        static void minify(std::shared_ptr<AstNode> head) {
            if (!head) return;
            auto current = head;
            while (current && current->next) {
                // Heuristic: Duplicate filters
                if (current->type == OpType::FILTER && current->next->type == OpType::FILTER) {
                    if (current->name == current->next->name) {
                        std::cout << "[Minifier] H-75: Removed redundant duplicate filter node.\n";
                        current->next = current->next->next;
                        continue;
                    }
                }
                current = current->next;
            }
        }
    };
}
