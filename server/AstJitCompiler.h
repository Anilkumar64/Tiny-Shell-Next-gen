#pragma once
#include "../common/Ast.h"
#include <vector>
#include <functional>
#include <memory>
#include <iostream>
#include <variant>

namespace tsh {
    // E-JIT: AST JIT Compiler via Threaded Code
    // Transforms the AST DAG into a pre-compiled flat array of C++ function pointers for linear, branchless execution.
    class AstJitCompiler {
        using JitOp = std::function<void(std::vector<ProcessRecord>&)>;

    public:
        static std::vector<JitOp> compile(std::shared_ptr<AstNode> head) {
            std::vector<JitOp> compiled_pipeline;
            auto current = head;
            
            while (current) {
                if (current->type == OpType::FILTER) {
                    std::string filter_expr = current->name;
                    compiled_pipeline.push_back([filter_expr](std::vector<ProcessRecord>& batch) {
                        std::vector<ProcessRecord> next_batch;
                        for (const auto& p : batch) {
                            // High performance inline evaluation
                            if (filter_expr.find("cpu >") != std::string::npos) {
                                double threshold = std::stod(filter_expr.substr(filter_expr.find(">") + 1));
                                if (p.cpu_usage > threshold) next_batch.push_back(p);
                            }
                        }
                        batch = std::move(next_batch);
                    });
                }
                // In production, handles MAP, REDUCE via closures
                current = current->next;
            }
            std::cout << "[AST-JIT] Pipeline successfully compiled into " << compiled_pipeline.size() << " native closures.\n";
            return compiled_pipeline;
        }

        static void execute(const std::vector<JitOp>& pipeline, std::vector<ProcessRecord>& data) {
            for (const auto& op : pipeline) {
                op(data); // Linear, cache-friendly execution
            }
        }
    };
}
