#pragma once
#include "../common/Ast.h"
#include <vector>
#include <ranges>
#include <functional>
#include <concepts>
#include <iostream>
#include <string>

namespace tsh {

    // Helper to evaluate basic filter expressions like "cpu > 80"
    bool eval_filter(const ProcessRecord& p, const std::string& expr) {
        if (expr.find("cpu >") != std::string::npos) {
            double threshold = std::stod(expr.substr(expr.find(">") + 1));
            return p.cpu_usage > threshold;
        }
        return true;
    }

    // Concept for valid pipeline data
    template<typename T>
    concept PipelineData = std::is_default_constructible_v<T> || std::is_move_constructible_v<T>;

    // Pipeline Stream
    template<PipelineData T>
    class TypedStream {
        std::vector<T> data;
    public:
        explicit TypedStream(std::vector<T> d) : data(std::move(d)) {}

        auto filter(std::function<bool(const T&)> predicate) {
            auto filtered = data | std::views::filter(predicate);
            std::vector<T> result;
            for (const auto& v : filtered) result.push_back(v);
            return TypedStream<T>(std::move(result));
        }

        const std::vector<T>& collect() const { return data; }
    };

    // The Execution Engine that traverses the AST
    class Executor {
    public:
        static std::string execute(std::shared_ptr<AstNode> head) {
            if (!head) return "Empty pipeline";

            // Stage 1: Source (Command)
            std::vector<ProcessRecord> source_data;
            if (head->type == OpType::COMMAND && head->name == "ps") {
                source_data = {
                    {1, "systemd", 0.1},
                    {402, "nginx", 85.5},
                    {992, "ssh", 0.01}
                };
            } else {
                return "Unknown command or unsupported source: " + head->name;
            }

            TypedStream<ProcessRecord> stream(std::move(source_data));

            // Stage 2: Transformations (Filter, Map, Reduce)
            auto current = head->next;
            bool reduced = false;
            size_t count_val = 0;

            while (current) {
                if (current->type == OpType::FILTER) {
                    stream = stream.filter([&](const ProcessRecord& p) {
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
            for (const auto& record : stream.collect()) {
                result += "  PID: " + std::to_string(record.pid) + 
                          ", Name: " + record.name + 
                          ", CPU: " + std::to_string(record.cpu_usage) + "%\n";
            }
            return result;
        }
    };
}
